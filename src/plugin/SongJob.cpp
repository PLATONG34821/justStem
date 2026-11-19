#include "SongJob.h"
#include "../core/Separator.h"
#include "../io/AudioFiles.h"
#include "../io/YouTube.h"

#include <juce_events/juce_events.h>

#include <stdexcept>

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
} // namespace

SongJob::SongJob (Kind k, juce::String src, juce::File model, Finished done)
    : juce::Thread ("justStem song job"), kind (k), source (std::move (src)), modelFile (std::move (model)), onFinished (std::move (done))
{
}

SongJob::~SongJob()
{
    cancelled = true;
    stopThread (30000);
}

SongJob::Status SongJob::status() const
{
    const juce::ScopedLock sl (statusLock);
    return current;
}

juce::StringArray SongJob::log() const
{
    const juce::ScopedLock sl (statusLock);
    return logLines;
}

void SongJob::addLog (const juce::String& line)
{
    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
    const int t = (int) seconds;
    const juce::ScopedLock sl (statusLock);
    logLines.add ("[" + juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2) + "]  " + line);
}

void SongJob::setStatus (const juce::String& step, double fraction)
{
    // Log each new step once, and long steps again at every 25 %.
    const auto base = step.upToFirstOccurrenceOf (" (", false, false);
    bool newStep = false, newQuarter = false;
    int quarter = 0;
    {
        const juce::ScopedLock sl (statusLock);
        current = { step, fraction };
        if (base != lastStep)
        {
            lastStep = base;
            lastQuarter = 0;
            newStep = true;
        }
        quarter = fraction >= 0 ? (int) std::floor (fraction * 4.0 + 1e-9) : 0;
        if (quarter > lastQuarter)
        {
            lastQuarter = quarter;
            newQuarter = true;
        }
    }
    if (newStep) addLog (base);
    if (newQuarter) addLog (base + " " + juce::String (quarter * 25) + "%" + (step.contains (" (") ? "  " + step.fromFirstOccurrenceOf (" (", false, false).dropLastCharacters (1) : ""));
}

void SongJob::run()
{
    SongResultPtr result;
    juce::String error;
    try
    {
        result = process();
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    if (cancelled) error = "cancelled";
    if (error == "cancelled") addLog ("Cancelled");
    else if (error.isNotEmpty()) addLog ("Error: " + error);

    // The job may be destroyed before this runs, so only captured copies are used.
    juce::MessageManager::callAsync ([done = onFinished, result, error] { done (result, error); });
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
    auto mmss = [] (double s) { const int t = (int) s; return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2); };
    if (kind == Kind::YouTube)
    {
        addLog ("Source: " + source);
        const auto dl = downloadYouTubeAudio (source, [this] (const juce::String& s, double f) { setStatus (s, f); }, cancelled);
        addLog ("Downloaded \"" + dl.title + "\" (" + juce::File::descriptionOfSizeInBytes (dl.file.getSize()) + ")");
        setStatus ("Reading audio", -1);
        mix = loadAudioFile (dl.file);
        dl.file.deleteFile(); // only the stems are kept
        song->title = dl.title;
    }
    else
    {
        const juce::File file (source);
        addLog ("Source: " + file.getFullPathName() + " (" + juce::File::descriptionOfSizeInBytes (file.getSize()) + ")");
        setStatus ("Reading audio", -1);
        mix = loadAudioFile (file);
        song->title = file.getFileNameWithoutExtension();
    }
    if (cancelled) throw std::runtime_error ("cancelled");
    song->durationSeconds = (double) mix.size() / sampleRate;
    addLog ("Decoded " + mmss (song->durationSeconds) + " of audio (stereo, 44.1 kHz)");

    // On macOS the first run also compiles the model for CoreML (about 25 s); later runs reuse it.
    setStatus ("Preparing AI model", -1);
    if (! modelFile.existsAsFile()) throw std::runtime_error ("The AI model is missing from the plug-in. Please reinstall justStem.");
    addLog ("Model: " + modelFile.getFileName() + " (HT-Demucs, 6 stems)");
    Separator separator (modelFile.getFullPathName().toStdString(), paths::coremlCache().getFullPathName().toStdString());
    const auto splitStart = juce::Time::getMillisecondCounterHiRes();
    bool backendLogged = false;
    Backend lastBackend {};
    song->stems = separator.separate (mix, [&] (const SeparationProgress& p) {
        if (! backendLogged || p.backend != lastBackend)
        {
            addLog (juce::String (backendLogged ? "Fell back to " : "Running on ") + backendName (p.backend)
                    + ", " + juce::String (p.totalSegments) + " chunks of 7.8 s");
            backendLogged = true;
            lastBackend = p.backend;
        }
        setStatus ("Splitting stems (" + juce::String (p.segment) + "/" + juce::String (p.totalSegments) + ", about "
                       + juce::String (juce::roundToInt (p.etaSeconds)) + " s left)",
                   (double) p.segment / p.totalSegments);
    }, cancelled);
    const double splitSeconds = (juce::Time::getMillisecondCounterHiRes() - splitStart) / 1000.0;
    addLog ("Split 6 stems in " + juce::String (splitSeconds, 1) + " s ("
            + juce::String (song->durationSeconds / std::max (0.01, splitSeconds), 1) + "x realtime)");

    setStatus ("Detecting key, tempo and chords", -1);
    song->analysis = analyzeSong (mix, song->stems);
    const auto& a = song->analysis;
    addLog ("Key " + juce::String (a.key.name()) + " (" + juce::String (a.key.votes) + "/3 profiles agree), "
            + juce::String (a.tempo.bpm, 1) + " BPM, " + juce::String (a.loudness.integratedLufs, 1) + " LUFS");
    addLog (juce::String ((int) a.chords.bars (song->durationSeconds).size()) + " bars, "
            + juce::String ((int) a.chords.segments.size()) + " chord changes");

    setStatus ("Saving stems", -1);
    song->folder = newSongFolder (song->title);
    saveSong (*song);
    addLog ("Saved 6 stems + instrumental to " + song->folder.getFullPathName());
    addLog ("Done in " + mmss ((juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0));
    return song;
}
} // namespace juststem
