#pragma once

#include "../core/Analysis.h"
#include "../core/Separator.h"
#include "Text.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace juststem
{
/** A split song: its stems (in memory and as FLACs in `folder`) and its analysis.
    Immutable once published, so the audio thread can read it without locks. */
struct SongResult
{
    juce::String title;
    juce::File folder;
    StemMap stems; // 44.1 kHz stereo
    SongAnalysis analysis;
    double durationSeconds = 0;
    juce::String model; // ModelSpec::name that made the stems (empty for songs from before HQ mode)
    juce::String audioHash; // audioFingerprint of the song as split (empty for songs saved before 0.0.3)

    bool isHQ() const { return model == text::from (model::hq.name); }

    /** The stem's FLAC, or its WAV in a song saved before stems were FLAC. */
    juce::File stemFile (Stem s) const { return savedFile (juce::String (std::string (stemName (s)))); }
    juce::File instrumentalFile() const { return savedFile ("instrumental"); } // everything except vocals
    juce::File fullMixFile() const { return savedFile ("full mix"); }          // every stem together: the whole song
    juce::File infoFile() const { return folder.getChildFile ("info.json"); }
    /** The stem's notes (midiStems only; missing when the stem was almost silent, or the song was
        split before justStem made MIDI). */
    juce::File midiFile (Stem s) const { return folder.getChildFile (juce::String (stemMidiName (s))); }

private:
    juce::File savedFile (const juce::String& name) const
    {
        const auto flac = folder.getChildFile (name + ".flac");
        const auto wav = folder.getChildFile (name + ".wav");
        return ! flac.existsAsFile() && wav.existsAsFile() ? wav : flac;
    }
};

using SongResultPtr = std::shared_ptr<const SongResult>;

/** SHA-256 of decoded 44.1 kHz audio, to tell the same song when it's split again (from the same
    link or file, or a copy of it). */
juce::String audioFingerprint (const StereoAudio& audio);

/** A saved song made from the same audio (audioFingerprint) with the same model, whose stems are all
    still there. Empty if there's none. */
juce::File findSavedSong (const juce::String& audioHash, const juce::String& model);

/** Write the stem FLACs, the instrumental, the full mix (every stem together) and info.json into song.folder.
    Throws std::runtime_error. */
void saveSong (const SongResult& song);

/** Write the stems, the instrumental and the full mix into `dir` (created) as 24-bit WAVs, and the MIDI files. `progress` gets 0..1 and returns false
    to cancel. Returns false if cancelled. Throws std::runtime_error. */
bool copyStems (const SongResult& song, const juce::File& dir, const std::function<bool (double)>& progress);

/** Write the stems, the instrumental and the full mix pitch-shifted by `semitones` (same names, same length),
    and the MIDI files moved by as much, into `dir`. `progress` gets 0..1 and returns false to cancel. Returns false if cancelled.
    Throws std::runtime_error. */
bool saveTransposedStems (const SongResult& song, int semitones, const juce::File& dir, const std::function<bool (double)>& progress);

/** Write (or rewrite) song.folder/info.json: title, analysis, beats and chords. */
void saveInfo (const SongResult& song);

/** Reload a song written by saveSong (e.g. when a DAW project is reopened). Songs saved before
    chord detection existed come back with no chord segments. Stems edited since (a different
    length) are padded with silence to the longest. Throws std::runtime_error. */
std::shared_ptr<SongResult> loadSong (const juce::File& folder);
} // namespace juststem
