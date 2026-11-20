#include "SongResult.h"
#include "../core/Pitch.h"
#include "../core/PitchShift.h"
#include "../io/AudioFiles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <vector>

namespace juststem
{
namespace
{
/** Adds `stem` into `sum` (the instrumental), growing it to fit. */
void addInto (StereoAudio& sum, const StereoAudio& stem)
{
    sum.left.resize (std::max (sum.size(), stem.size()), 0.0f);
    sum.right.resize (sum.left.size(), 0.0f);
    for (std::size_t i = 0; i < stem.size(); ++i)
    {
        sum.left[i] += stem.left[i];
        sum.right[i] += stem.right[i];
    }
}
} // namespace

void saveSong (const SongResult& song)
{
    StereoAudio instrumental;
    for (auto stem : stemOrder)
    {
        const auto& s = song.stems.at (stem);
        writeFlac (song.folder.getChildFile (juce::String (stemFileName (stem))), s);
        if (stem != Stem::Vocals) addInto (instrumental, s);
    }
    writeFlac (song.folder.getChildFile ("instrumental.flac"), instrumental);
    saveInfo (song);
}

bool copyStems (const SongResult& song, const juce::File& dir, const std::function<bool (double)>& progress)
{
    if (! dir.createDirectory()) throw std::runtime_error ("Couldn't create " + dir.getFullPathName().toStdString());
    // From the stems in memory (the saved ones are FLAC; WAV opens in every DAW).
    constexpr double files = numStems + 1;
    StereoAudio instrumental;
    for (std::size_t i = 0; i < stemOrder.size(); ++i)
    {
        if (! progress ((double) i / files)) return false;
        const auto stem = stemOrder[i];
        const auto& s = song.stems.at (stem);
        writeWav (dir.getChildFile (juce::String (stemWavName (stem))), s);
        if (stem != Stem::Vocals) addInto (instrumental, s);
    }
    if (! progress ((double) numStems / files)) return false;
    writeWav (dir.getChildFile ("instrumental.wav"), instrumental);
    progress (1.0);
    return true;
}

bool saveTransposedStems (const SongResult& song, int semitones, const juce::File& dir, const std::function<bool (double)>& progress)
{
    if (! dir.createDirectory()) throw std::runtime_error ("Couldn't create " + dir.getFullPathName().toStdString());

    // The stems are independent, so a few are shifted at once (three at a time, which keeps the extra
    // memory to three shifted stems). They're written in order on this thread, which also reports the
    // progress and passes a cancel on to the threads. The files are the same as one at a time.
    constexpr std::size_t atOnce = 3;
    std::array<std::atomic<double>, numStems> fraction {};
    std::atomic<bool> cancelled { false };
    std::array<std::future<StereoAudio>, numStems> shifting;
    // However this returns (done, cancelled, or an error writing), shifting still going stops before
    // `shifting` waits for it.
    struct StopOnExit
    {
        std::atomic<bool>& flag;
        ~StopOnExit() { flag = true; }
    } stopOnExit { cancelled };
    auto start = [&] (std::size_t i) {
        shifting[i] = std::async (std::launch::async, [&, i] {
            return pitchShift (song.stems.at (stemOrder[i]), (float) semitones, sampleRate, [&, i] (double f) {
                fraction[i] = f;
                return ! cancelled.load();
            });
        });
    };
    auto report = [&] {
        double sum = 0;
        for (const auto& f : fraction) sum += f.load();
        if (! progress (sum / (double) numStems)) cancelled = true;
    };
    for (std::size_t i = 0; i < std::min (atOnce, stemOrder.size()); ++i) start (i);

    StereoAudio instrumental;
    for (std::size_t i = 0; i < stemOrder.size(); ++i)
    {
        while (shifting[i].wait_for (std::chrono::milliseconds (100)) != std::future_status::ready) report();
        const auto shifted = shifting[i].get();
        if (cancelled) return false; // the other threads stop at their next progress check
        if (i + atOnce < stemOrder.size()) start (i + atOnce);
        const auto stem = stemOrder[i];
        writeWav (dir.getChildFile (juce::String (stemWavName (stem))), shifted);
        if (stem != Stem::Vocals) addInto (instrumental, shifted);
        report();
    }
    writeWav (dir.getChildFile ("instrumental.wav"), instrumental);
    progress (1.0);
    return true;
}

void saveInfo (const SongResult& song)
{
    const auto& a = song.analysis;
    auto info = std::make_unique<juce::DynamicObject>();
    info->setProperty ("version", 1);
    info->setProperty ("title", song.title);
    info->setProperty ("durationSeconds", song.durationSeconds);
    info->setProperty ("model", song.model);
    info->setProperty ("keyTonic", a.key.tonic);
    info->setProperty ("keyMinor", a.key.minor);
    info->setProperty ("keyConfidence", a.key.confidence);
    info->setProperty ("keyVotes", a.key.votes);
    info->setProperty ("tuningCents", a.key.tuningCents);
    info->setProperty ("bpm", a.tempo.bpm);
    info->setProperty ("lufs", a.loudness.integratedLufs);
    info->setProperty ("peakDb", a.loudness.samplePeakDb);
    juce::Array<juce::var> beats, chords;
    for (double b : a.chords.beats) beats.add (std::round (b * 1000.0) / 1000.0);
    for (const auto& seg : a.chords.segments) // [start, end, root, quality, bass]; root -1 = no chord
        chords.add (juce::Array<juce::var> { std::round (seg.start * 1000.0) / 1000.0, std::round (seg.end * 1000.0) / 1000.0,
                                             seg.chord.root, (int) seg.chord.quality, seg.chord.bass });
    info->setProperty ("beats", beats);
    info->setProperty ("firstDownbeat", a.chords.firstDownbeat);
    info->setProperty ("chords", chords);
    song.infoFile().replaceWithText (juce::JSON::toString (juce::var (info.release())));
}

std::shared_ptr<SongResult> loadSong (const juce::File& folder)
{
    auto song = std::make_shared<SongResult>();
    song->folder = folder;
    const auto info = juce::JSON::parse (song->infoFile());
    if (! info.isObject()) throw std::runtime_error ("The saved stems for this session were moved or deleted");

    auto& a = song->analysis;
    song->title = info["title"].toString();
    song->durationSeconds = (double) info["durationSeconds"];
    song->model = info["model"].toString();
    // info.json can be edited or damaged: values used as indices are brought into range.
    const auto pitchClass = [] (int p) { return p < 0 ? -1 : wrapPitchClass (p); }; // -1: none
    a.key.tonic = wrapPitchClass ((int) info["keyTonic"]);
    a.key.minor = (bool) info["keyMinor"];
    a.key.confidence = (float) (double) info["keyConfidence"];
    a.key.votes = juce::jlimit (0, 3, (int) info["keyVotes"]);
    a.key.tuningCents = (float) (double) info["tuningCents"];
    a.tempo.bpm = (float) (double) info["bpm"];
    a.loudness.integratedLufs = (float) (double) info["lufs"];
    a.loudness.samplePeakDb = (float) (double) info["peakDb"];
    if (const auto* beats = info["beats"].getArray())
        for (const auto& b : *beats) a.chords.beats.push_back ((double) b);
    a.chords.firstDownbeat = juce::jlimit (0, ChordResult::beatsPerBar - 1, (int) info["firstDownbeat"]);
    if (const auto* chords = info["chords"].getArray())
        for (const auto& c : *chords)
            if (c.size() == 5)
                a.chords.segments.push_back ({ (double) c[0], (double) c[1],
                                               { pitchClass ((int) c[2]), (ChordQuality) juce::jlimit (0, numChordQualities - 1, (int) c[3]),
                                                 pitchClass ((int) c[4]) } });

    for (auto stem : stemOrder)
        song->stems[stem] = loadAudioFile (song->stemFile (stem));
    // A stem edited outside justStem (trimmed, or bounced again) can come back shorter or longer than
    // the others: pad them all to the longest, so they still play and export in step.
    std::size_t longest = 0;
    for (const auto& [stem, audio] : song->stems) longest = std::max (longest, audio.size());
    for (auto& [stem, audio] : song->stems)
    {
        audio.left.resize (longest, 0.0f);
        audio.right.resize (longest, 0.0f);
    }
    song->durationSeconds = (double) longest / sampleRate;
    return song;
}
} // namespace juststem
