#pragma once

#include "../core/Analysis.h"
#include "../core/Separator.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace juststem
{
/** A split song: its stems (in memory and as WAVs in `folder`) and its analysis.
    Immutable once published, so the audio thread can read it without locks. */
struct SongResult
{
    juce::String title;
    juce::File folder;
    StemMap stems; // 44.1 kHz stereo
    SongAnalysis analysis;
    double durationSeconds = 0;
    juce::String model; // ModelSpec::name that made the stems (empty for songs from before HQ mode)

    bool isHQ() const { return model == juce::String (std::string (model::hq.name)); }

    juce::File stemFile (Stem s) const { return folder.getChildFile (juce::String (std::string (stemName (s))) + ".wav"); }
    juce::File instrumentalFile() const { return folder.getChildFile ("instrumental.wav"); } // everything except vocals
    juce::File infoFile() const { return folder.getChildFile ("info.json"); }
};

using SongResultPtr = std::shared_ptr<const SongResult>;

/** Write the stem WAVs, the instrumental and info.json into song.folder. Throws std::runtime_error. */
void saveSong (const SongResult& song);

/** Write the stems and the instrumental pitch-shifted by `semitones` (same names, same length)
    into `dir`. `progress` gets 0..1 and returns false to cancel. Returns false if cancelled.
    Throws std::runtime_error. */
bool saveTransposedStems (const SongResult& song, int semitones, const juce::File& dir, const std::function<bool (double)>& progress);

/** Write (or rewrite) song.folder/info.json: title, analysis, beats and chords. */
void saveInfo (const SongResult& song);

/** Reload a song written by saveSong (e.g. when a DAW project is reopened). Songs saved before
    chord detection existed come back with no chord segments. Throws std::runtime_error. */
std::shared_ptr<SongResult> loadSong (const juce::File& folder);
} // namespace juststem
