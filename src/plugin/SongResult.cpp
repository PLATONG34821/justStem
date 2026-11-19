#include "SongResult.h"
#include "../io/AudioFiles.h"

#include <stdexcept>

namespace covertools
{
void saveSong (const SongResult& song)
{
    StereoAudio instrumental;
    for (auto stem : stemOrder)
    {
        const auto& s = song.stems.at (stem);
        writeWav (song.stemFile (stem), s);
        if (stem == Stem::Vocals) continue;
        instrumental.left.resize (s.size(), 0.0f);
        instrumental.right.resize (s.size(), 0.0f);
        for (size_t i = 0; i < s.size(); ++i)
        {
            instrumental.left[i] += s.left[i];
            instrumental.right[i] += s.right[i];
        }
    }
    writeWav (song.instrumentalFile(), instrumental);

    const auto& a = song.analysis;
    auto info = std::make_unique<juce::DynamicObject>();
    info->setProperty ("version", 1);
    info->setProperty ("title", song.title);
    info->setProperty ("durationSeconds", song.durationSeconds);
    info->setProperty ("keyTonic", a.key.tonic);
    info->setProperty ("keyMinor", a.key.minor);
    info->setProperty ("keyConfidence", a.key.confidence);
    info->setProperty ("keyVotes", a.key.votes);
    info->setProperty ("tuningCents", a.key.tuningCents);
    info->setProperty ("bpm", a.tempo.bpm);
    info->setProperty ("lufs", a.loudness.integratedLufs);
    info->setProperty ("peakDb", a.loudness.samplePeakDb);
    song.infoFile().replaceWithText (juce::JSON::toString (juce::var (info.release())));
}

SongResultPtr loadSong (const juce::File& folder)
{
    auto song = std::make_shared<SongResult>();
    song->folder = folder;
    const auto info = juce::JSON::parse (song->infoFile());
    if (! info.isObject()) throw std::runtime_error ("The saved stems for this session were moved or deleted");

    auto& a = song->analysis;
    song->title = info["title"].toString();
    song->durationSeconds = (double) info["durationSeconds"];
    a.key.tonic = (int) info["keyTonic"];
    a.key.minor = (bool) info["keyMinor"];
    a.key.confidence = (float) (double) info["keyConfidence"];
    a.key.votes = (int) info["keyVotes"];
    a.key.tuningCents = (float) (double) info["tuningCents"];
    a.tempo.bpm = (float) (double) info["bpm"];
    a.loudness.integratedLufs = (float) (double) info["lufs"];
    a.loudness.samplePeakDb = (float) (double) info["peakDb"];

    for (auto stem : stemOrder)
        song->stems[stem] = loadAudioFile (song->stemFile (stem));
    return song;
}
} // namespace covertools
