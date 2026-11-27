#include "SongJob.h"
#include "Text.h"
#include "../core/Separator.h"
#include "../io/AudioFiles.h"
#include "../io/ToolInstaller.h"
#include "../io/YouTube.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace juststem
{
namespace
{
/** <Music>/justStem/<title>, numbered if that folder already exists. */
juce::File newSongFolder (const juce::String& title)
{
    auto name = juce::File::createLegalFileName (title).trim().substring (0, 80);
    if (name.isEmpty()) name = "Song";
    auto dir = paths::stems().getChildFile (name);
    for (int i = 2; dir.exists(); ++i) dir = paths::stems().getChildFile (name + " (" + juce::String (i) + ")");
    dir.createDirectory();
    return dir;
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

/** Jobs whose owner has gone (SongJob::abandon), kept until their thread ends. Message thread.
    Heap-allocated so process exit / DLL unload never runs ~SongJob() -> killThread() while an
    uninterruptible step (e.g. CoreML compilation) is still finishing. */
std::vector<std::unique_ptr<SongJob>>& abandonedJobs()
{
    static auto* jobs = new std::vector<std::unique_ptr<SongJob>>();
    return *jobs;
}

juce::String seconds (double ms)
{
    return (ms < 10000.0 ? juce::String (ms / 1000.0, 1) : juce::String (juce::roundToInt (ms / 1000.0))) + " s";
}

/** RMS level of a stem in dBFS (both channels). */
float rmsDb (const StereoAudio& a)
{
    if (a.size() == 0) return -100.0f;
    double sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += (double) a.left[i] * a.left[i] + (double) a.right[i] * a.right[i];
    return (float) juce::Decibels::gainToDecibels (std::sqrt (sum / (2.0 * (double) a.size())), -100.0);
}
} // namespace

SongJob::SongJob (Kind k, juce::String src, juce::File model, MidiModels midi, Finished done, bool downloadModel)
    : juce::Thread ("justStem song job"), kind (k), source (std::move (src)), modelFile (std::move (model)),
      midiModels (std::move (midi)), onFinished (std::move (done)), allowModelDownload (downloadModel)
{
}

void SongJob::abandon (std::unique_ptr<SongJob> job)
{
    if (job == nullptr) return;
    pinPluginBinary();
    job->cancel();
    job->abandoned = true;
    abandonedJobs().push_back (std::move (job));
}

int SongJob::numAbandoned() { return (int) abandonedJobs().size(); }

SongJob::~SongJob()
{
    cancelled = true;
    stopThread (-1); // never force-kill (TerminateThread / pthread_cancel corrupts locks and C++ state)
}

SongJob::Status SongJob::status() const
{
    const juce::ScopedLock sl (statusLock);
    auto s = current;
    if (timedSeconds > 0)
    {
        // Never reaches 100 %: the real length varies from Mac to Mac.
        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - timedStartMs) / 1000.0;
        s.fraction = std::min (0.97, elapsed / timedSeconds);
        s.secondsLeft = std::max (0.0, timedSeconds - elapsed);
    }
    return s;
}

void SongJob::setTimedStatus (const juce::String& step, double expectedSeconds)
{
    setStatus (step, -1);
    const juce::ScopedLock sl (statusLock);
    timedStartMs = juce::Time::getMillisecondCounterHiRes();
    timedSeconds = expectedSeconds;
}

juce::StringArray SongJob::log() const
{
    const juce::ScopedLock sl (statusLock);
    return logLines;
}

void SongJob::addLog (const juce::String& line)
{
    const auto stamp = text::minutesSeconds ((juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0);
    const juce::ScopedLock sl (statusLock);
    logLines.add ("[" + stamp + "]  " + line);
}

void SongJob::setStatus (const juce::String& step, double fraction)
{
    // Log each new step once, and long steps again at every 10 %.
    const auto base = step.upToFirstOccurrenceOf (" (", false, false);
    bool newStep = false, newTenth = false;
    int tenth = 0;
    {
        const juce::ScopedLock sl (statusLock);
        current = { step, fraction };
        timedSeconds = 0;
        if (base != lastStep)
        {
            lastStep = base;
            lastTenth = 0;
            newStep = true;
        }
        tenth = fraction >= 0 ? (int) std::floor (fraction * 10.0 + 1e-9) : 0;
        if (tenth > lastTenth)
        {
            lastTenth = tenth;
            newTenth = true;
        }
    }
    if (newStep) addLog (base);
    if (newTenth) addLog (base + " " + juce::String (tenth * 10) + "%" + (step.contains (" (") ? "  " + step.fromFirstOccurrenceOf (" (", false, false).dropLastCharacters (1) : ""));
}

void SongJob::run()
{
    SongResultPtr result;
    juce::String error;
    try
    {
        result = process();
    }
    catch (const std::bad_alloc&)
    {
        error = "Not enough memory to split this song. Close other apps or try Fast mode.";
    }
    catch (const std::exception& e)
    {
        error = juce::String::fromUTF8 (e.what()); // file names, titles and paths in it can be any language
    }
    if (cancelled) error = "cancelled";
    if (error == "cancelled") addLog ("Cancelled");
    else if (error.isNotEmpty()) addLog ("Error: " + error);

    // Runs on the message thread after this thread has ended (or is about to). The job still exists:
    // only `done` deletes it, or, once abandoned, this.
    juce::MessageManager::callAsync ([this, done = onFinished, result, error] {
        if (! abandoned)
        {
            done (result, error);
            return;
        }
        auto& jobs = abandonedJobs();
        const auto it = std::find_if (jobs.begin(), jobs.end(), [this] (const auto& j) { return j.get() == this; });
        if (it != jobs.end())
        {
            const auto self = std::move (*it); // deleted at the end of this scope, its thread done
            jobs.erase (it);
        }
    });
}

SongResultPtr SongJob::process()
{
    if (kind == Kind::Reload)
    {
        addLog ("Reloading the stems saved with this project: " + source);
        setStatus ("Loading saved stems", -1);
        auto song = loadSong (juce::File (source));
        addLog ("Loaded \"" + song->title + "\"");
        if (song->analysis.chords.segments.empty()) // saved before chord detection existed
        {
            setStatus ("Finding chords", -1);
            song->analysis.chords = detectSongChords ({}, song->stems, song->analysis.tempo.bpm, song->analysis.key);
            saveInfo (*song);
            addLog ("Found " + juce::String ((int) song->analysis.chords.segments.size()) + " chord changes (saved for next time)");
        }
        return song;
    }

    auto song = std::make_shared<SongResult>();
    StereoAudio mix;
    double decodeMs = 0;
    if (kind == Kind::YouTube)
    {
        addLog ("Source: " + source);
        const auto dlStart = nowMs();
        const auto dl = downloadYouTubeAudio (source, [this] (const juce::String& s, double f) { setStatus (s, f); }, cancelled);
        addLog ("Downloaded \"" + dl.title + "\" in " + seconds (nowMs() - dlStart) + " ("
                + juce::File::descriptionOfSizeInBytes (dl.file.getSize()) + ", " + describeAudioFile (dl.file) + ")");
        setStatus ("Reading audio", -1);
        const auto t0 = nowMs();
        mix = loadAudioFile (dl.file);
        decodeMs = nowMs() - t0;
        addLog ("The downloaded audio is deleted now (only the stems are kept)"); // with `dl`, at the end of this block
        song->title = dl.title;
    }
    else
    {
        const juce::File file (source);
        addLog ("Source: " + file.getFullPathName());
        addLog ("Format: " + describeAudioFile (file) + ", " + juce::File::descriptionOfSizeInBytes (file.getSize()));
        setStatus ("Reading audio", -1);
        const auto t0 = nowMs();
        mix = loadAudioFile (file);
        decodeMs = nowMs() - t0;
        song->title = file.getFileNameWithoutExtension();
    }
    if (cancelled) throw std::runtime_error ("cancelled");
    song->durationSeconds = (double) mix.size() / sampleRate;
    addLog ("Decoded " + text::minutesSeconds (song->durationSeconds) + " of audio in " + seconds (decodeMs)
            + " (working at 44.1 kHz stereo, " + juce::String (rmsDb (mix), 1) + " dB RMS)");

    if (! modelFile.existsAsFile())
    {
        if (! allowModelDownload)
            throw std::runtime_error ("The AI model is missing from the plug-in. Please reinstall justStem.");
        addLog ("Downloading " + modelFile.getFileName() + " from GitHub (saved for future runs)");
        ensureModel (modelFile, [this] (const juce::String& s, double f) { setStatus (s, f); }, cancelled);
    }
    const auto& spec = model::forFile (modelFile.getFullPathName().toStdString());

    // The same song split before with this model: reuse its stems instead of splitting (and storing) it again.
    setStatus ("Looking for this song in saved stems", -1);
    song->audioHash = audioFingerprint (mix);
    if (const auto saved = findSavedSong (song->audioHash, text::from (spec.name)); saved != juce::File())
    {
        addLog ("Already split earlier, so its saved stems are used: " + saved.getFullPathName());
        setStatus ("Loading saved stems", -1);
        auto existing = loadSong (saved);
        addLog ("Loaded \"" + existing->title + "\"");
        makeMidi (*existing); // split before justStem made MIDI, or a stem's MIDI failed then
        return existing;
    }

    // On macOS the first run of each model also compiles it for CoreML; later runs reuse it.
    setStatus ("Preparing AI model", -1);
    addLog ("Model: " + modelFile.getFileName() + " (" + text::from (spec.name) + ", 6 stems, "
            + juce::File::descriptionOfSizeInBytes (modelFile.getSize()) + ")");
    song->model = text::from (spec.name);
#if JUCE_MAC
    // CoreML keeps the compiled model in one cache folder per model file version, so "already
    // compiled" is known for each. Other OSes have no compile step and no cache.
    const std::string coremlBase = paths::coremlCache().getFullPathName().toStdString();
#else
    const std::string coremlBase;
#endif
    // Freed once the stems are split, before the MIDI models load.
    auto separator = std::make_unique<Separator> (modelFile.getFullPathName().toStdString(), coremlBase);
#if JUCE_MAC
    const juce::File coremlCache (juce::String::fromUTF8 (separator->cacheFolder().c_str()));
    const bool cached = coremlCache.isDirectory()
                        && coremlCache.getNumberOfChildFiles (juce::File::findFilesAndDirectories) > 0;
    const double compileSeconds = spec.coremlCompileSeconds;
    addLog (cached ? "Using the CoreML model compiled on an earlier run"
                   : "First run on this Mac: compiling the model for CoreML (only once; about " + text::roughDuration (compileSeconds) + ")");
    // Compiling happens when the first chunk runs; the first progress report ends the countdown.
    if (! cached) setTimedStatus ("Compiling AI model", compileSeconds);
#endif

    // The model is loaded when the first chunk runs, so the first progress report marks it ready.
    const auto splitStart = nowMs();
    double modelMs = 0;
    bool backendLogged = false;
    Backend lastBackend {};
    song->stems = separator->separate (mix, [&] (const SeparationProgress& p) {
        if (! backendLogged || p.backend != lastBackend)
        {
            if (! backendLogged) modelMs = nowMs() - splitStart;
            addLog (juce::String (backendLogged ? "Fell back to " : "Running on ") + backendName (p.backend)
                    + (backendLogged ? juce::String() : " (model loaded in " + seconds (modelMs) + ")")
                    + ", " + juce::String (p.totalSegments) + " chunks of "
                    + juce::String ((double) spec.chunkSamples / sampleRate, 1) + " s");
#if JUCE_MAC || JUCE_WINDOWS
            if (p.backend == Backend::CPU)
                addLog ("The GPU couldn't run the model, so this split runs on the CPU and will take much longer");
#endif
            backendLogged = true;
            lastBackend = p.backend;
        }
        // On a Mac or PC the CPU only runs the model when the GPU failed: say so on the gauge.
#if JUCE_MAC || JUCE_WINDOWS
        const juce::String splitting = p.backend == Backend::CPU ? "Splitting on CPU" : "Splitting stems";
#else
        const juce::String splitting = "Splitting stems";
#endif
        setStatus (splitting + " (" + juce::String (p.segment) + "/" + juce::String (p.totalSegments) + ", about "
                       + juce::String (juce::roundToInt (p.etaSeconds)) + " s left)",
                   (double) p.segment / p.totalSegments);
    }, cancelled);
    const double splitMs = nowMs() - splitStart - modelMs; // the stems themselves, without loading the model
    separator.reset();
    addLog ("Split 6 stems in " + seconds (splitMs) + " ("
            + juce::String (song->durationSeconds / std::max (0.01, splitMs / 1000.0), 1) + "x realtime)");

    // Saved as 24-bit files, anything over full scale would be clipped: lower all the stems together.
    if (const float db = fitUnderFullScale (song->stems); db < 0)
        addLog ("Lowered all stems by " + juce::String (-db, 2) + " dB, so the stems and mixes saved don't clip");

    // How much of the song ended up in each stem; a near-silent one usually means the instrument isn't there.
    juce::StringArray levels, quiet;
    for (auto stem : stemOrder)
    {
        const float db = rmsDb (song->stems[stem]);
        levels.add (text::stemLabel (stem) + " " + juce::String (db, 1));
        if (db < -45.0f) quiet.add (text::stemLabel (stem).toLowerCase());
    }
    addLog ("Stem levels (dB RMS): " + levels.joinIntoString (", "));
    if (! quiet.isEmpty()) addLog ("Almost silent: " + quiet.joinIntoString (", ") + " (probably not in this song)");

    setStatus ("Analysing song", -1); // key, tempo and chords
    const auto analysisStart = nowMs();
    const auto loudness = measureLoudness (mix.left, mix.right, sampleRate);
    mix = {}; // free the 44.1 kHz mix before building SongParts and saving stems
    song->analysis = analyzeSong (song->stems, loudness);
    const double analysisMs = nowMs() - analysisStart;
    const auto& a = song->analysis;
    addLog ("Key " + juce::String (a.key.name()) + " (Camelot " + juce::String (a.key.camelot()) + "), "
            + juce::String (juce::roundToInt (a.key.confidence * 100)) + "% confidence, "
            + juce::String (a.key.votes) + "/3 profiles agree, "
            + (std::abs (a.key.tuningCents) < 1.0f ? juce::String ("tuned to A = 440 Hz")
                                                   : "tuned " + juce::String (a.key.tuningCents > 0 ? "+" : "")
                                                         + juce::String (juce::roundToInt (a.key.tuningCents)) + " cents from A = 440 Hz"));
    addLog (a.tempo.bpm > 0 ? "Tempo " + text::bpm (a.tempo.bpm) + " BPM (could also be " + text::bpm (a.tempo.bpm / 2) + " or "
                                  + text::bpm (a.tempo.bpm * 2) + ")"
                            : juce::String ("No tempo found (too short, or no beat)"));
    addLog ("Loudness " + juce::String (a.loudness.integratedLufs, 1) + " LUFS integrated, sample peak "
            + (a.loudness.samplePeakDb > 0 ? "+" : "") + juce::String (a.loudness.samplePeakDb, 1) + " dBFS"
            + (a.loudness.samplePeakDb > 0 ? " (the source clips)" : ""));
    std::set<std::tuple<int, int, int>> distinct;
    for (const auto& seg : a.chords.segments)
        if (! seg.chord.isNone()) distinct.insert ({ seg.chord.root, (int) seg.chord.quality, seg.chord.bass });
    addLog ("Chords: " + juce::String ((int) a.chords.bars (song->durationSeconds).size()) + " bars, "
            + juce::String ((int) a.chords.segments.size()) + " changes, " + juce::String ((int) distinct.size())
            + " different chords (analysis took " + seconds (analysisMs) + ")");

    setStatus ("Saving stems", -1);
    const auto saveStart = nowMs();
    song->folder = newSongFolder (song->title);
    try
    {
        saveSong (*song);
    }
    catch (...)
    {
        // Half a song (e.g. the disk filled up): without its info.json, Storage could neither list nor free it.
        song->folder.deleteRecursively();
        throw;
    }
    juce::int64 bytes = 0;
    for (const auto& f : song->folder.findChildFiles (juce::File::findFiles, false, "*.flac")) bytes += f.getSize();
    addLog ("Saved 6 stems + instrumental + full mix (24-bit FLAC, " + juce::File::descriptionOfSizeInBytes (bytes) + ") in "
            + seconds (nowMs() - saveStart));
    addLog ("Folder: " + song->folder.getFullPathName());

    const auto midiStart = nowMs();
    makeMidi (*song);
    addLog ("Done in " + text::minutesSeconds ((nowMs() - startMs) / 1000.0) + " (decode " + seconds (decodeMs) + ", model " + seconds (modelMs)
            + ", split " + seconds (splitMs) + ", analysis " + seconds (analysisMs) + ", MIDI " + seconds (nowMs() - midiStart) + ")");
    return song;
}

void SongJob::makeMidi (const SongResult& song)
{
    // The stems are saved by now: a stem whose MIDI fails is logged and left without it.
    for (const auto& m : midiStems)
    {
        const auto file = song.midiFile (m.stem);
        const auto it = song.stems.find (m.stem);
        if (file.existsAsFile() || it == song.stems.end()) continue;
        const auto name = text::stemLabel (m.stem).toLowerCase();
        if (rmsDb (it->second) < -45.0f) continue; // not in this song (logged with the stem levels)
        const auto start = nowMs();
        try
        {
            Transcription notes;
            if (m.stem == Stem::Piano)
            {
                const auto piano = midiModels.piano;
                for (const auto part : { model::pianoFile, model::pianoHeadsFile, model::pianoFrontEndFile })
                {
                    const auto f = piano.getSiblingFile (text::from (part));
                    if (f.existsAsFile()) continue;
                    if (! allowModelDownload) throw std::runtime_error ("the piano MIDI model isn't installed");
                    if (part == model::pianoFile) addLog ("Downloading the piano MIDI model from GitHub (saved for future runs)");
                    ensureModel (f, [this] (const juce::String& s, double fr) { setStatus (s, fr); }, cancelled);
                }
                setStatus ("Making piano MIDI", 0);
                notes = transcribePiano (piano.getFullPathName().toStdString(), it->second,
                                         [this] (double f) { setStatus ("Making piano MIDI", f); }, cancelled);
            }
            else
            {
                setStatus ("Making " + name + " MIDI", -1);
                notes = transcribeNotes (midiModels.notes.getFullPathName().toStdString(), it->second, m.minHz, m.maxHz, cancelled);
            }
            writeMidi (file, notes, song.analysis.tempo.bpm, text::stemLabel (m.stem), m.program);
            addLog ("Made " + name + " MIDI: " + juce::String ((int) notes.notes.size()) + " notes"
                    + (notes.pedals.empty() ? juce::String() : ", " + juce::String ((int) notes.pedals.size()) + " pedal presses")
                    + " in " + seconds (nowMs() - start));
        }
        catch (const std::bad_alloc&)
        {
            file.deleteFile();
            if (cancelled) throw std::runtime_error ("cancelled");
            addLog ("No " + name + " MIDI: Not enough memory");
        }
        catch (const std::exception& e)
        {
            file.deleteFile();
            if (cancelled) throw std::runtime_error ("cancelled");
            addLog ("No " + name + " MIDI: " + juce::String::fromUTF8 (e.what()));
        }
    }
}
} // namespace juststem
