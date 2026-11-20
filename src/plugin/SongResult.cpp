#include "SongResult.h"
#include "../core/PitchShift.h"
#include "../io/AudioFiles.h"

#include <algorithm>
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
        writeWav (song.stemFile (stem), s);
        if (stem != Stem::Vocals) addInto (instrumental, s);
    }
    writeWav (song.instrumentalFile(), instrumental);
    saveInfo (song);
}

bool copyStems (const SongResult& song, const juce::File& dir, const std::function<bool (double)>& progress)
{
    if (! dir.createDirectory()) throw std::runtime_error ("Couldn't create " + dir.getFullPathName().toStdString());
    std::vector<juce::File> files;
    for (auto stem : stemOrder) files.push_back (song.stemFile (stem));
    files.push_back (song.instrumentalFile());
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        if (! progress ((double) i / (double) files.size())) return false;
        if (! files[i].copyFileTo (dir.getChildFile (files[i].getFileName())))
            throw std::runtime_error ("Couldn't copy " + files[i].getFileName().toStdString() + " to " + dir.getFullPathName().toStdString());
    }
    progress (1.0);
    return true;
}

bool saveTransposedStems (const SongResult& song, int semitones, const juce::File& dir, const std::function<bool (double)>& progress)
{
    if (! dir.createDirectory()) throw std::runtime_error ("Couldn't create " + dir.getFullPathName().toStdString());
    StereoAudio instrumental;
    for (std::size_t i = 0; i < stemOrder.size(); ++i)
    {
        const auto stem = stemOrder[i];
        const auto shifted = pitchShift (song.stems.at (stem), (float) semitones, sampleRate, [&] (double f) {
            return progress (((double) i + f) / (double) stemOrder.size());
        });
        if (shifted.size() == 0 && song.stems.at (stem).size() > 0) return false; // cancelled
        writeWav (dir.getChildFile (song.stemFile (stem).getFileName()), shifted);
        if (stem != Stem::Vocals) addInto (instrumental, shifted);
    }
    writeWav (dir.getChildFile (song.instrumentalFile().getFileName()), instrumental);
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
    a.key.tonic = (int) info["keyTonic"];
    a.key.minor = (bool) info["keyMinor"];
    a.key.confidence = (float) (double) info["keyConfidence"];
    a.key.votes = (int) info["keyVotes"];
    a.key.tuningCents = (float) (double) info["tuningCents"];
    a.tempo.bpm = (float) (double) info["bpm"];
    a.loudness.integratedLufs = (float) (double) info["lufs"];
    a.loudness.samplePeakDb = (float) (double) info["peakDb"];
    if (const auto* beats = info["beats"].getArray())
        for (const auto& b : *beats) a.chords.beats.push_back ((double) b);
    a.chords.firstDownbeat = (int) info["firstDownbeat"];
    if (const auto* chords = info["chords"].getArray())
        for (const auto& c : *chords)
            if (c.size() == 5)
                a.chords.segments.push_back ({ (double) c[0], (double) c[1],
                                               { (int) c[2], (ChordQuality) juce::jlimit (0, numChordQualities - 1, (int) c[3]), (int) c[4] } });

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
