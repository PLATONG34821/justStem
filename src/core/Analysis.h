#pragma once

#include "Audio.h"
#include "Chords.h"

#include <array>
#include <string>
#include <vector>

namespace juststem
{
struct KeyResult
{
    int tonic = 0;          // pitch class, 0 = C
    bool minor = false;
    float confidence = 0;   // 0..1: correlation with the key profile, scaled by how many profiles agreed
    int votes = 0;          // how many of the 3 key profiles chose this key
    float tuningCents = 0;  // detected tuning offset from A = 440 Hz

    std::string name() const;      // "E major", "C# minor"
    std::string shortName() const; // "E", "C#m"
    std::string camelot() const;   // "12B"
    KeyResult relative() const;    // C major <-> A minor
    KeyResult transposed (int semitones) const;
};

struct TempoResult
{
    float bpm = 0;          // the tempo octave is a guess; show bpm / 2 and bpm * 2 as alternatives
};

struct LoudnessResult
{
    float integratedLufs = -70;  // EBU R128 / ITU-R BS.1770-4
    float samplePeakDb = -100;
};

/** Key from mono audio (use the mix of non-drum stems for best results). */
KeyResult detectKey (const std::vector<float>& mono, int rate);

/** Tempo from mono audio (the drums stem works best). Searches 60-200 BPM. */
TempoResult detectTempo (const std::vector<float>& mono, int rate);

/** Integrated loudness of a stereo signal. */
LoudnessResult measureLoudness (const std::vector<float>& left, const std::vector<float>& right, int rate);

struct SongAnalysis
{
    KeyResult key;
    TempoResult tempo;
    LoudnessResult loudness;
    ChordResult chords;
};

/** Key (from the non-drum stems), tempo (from the drums), loudness (from the mix) and chords.
    Pass an empty StemMap to analyse the mix alone. */
SongAnalysis analyzeSong (const StereoAudio& mix, const StemMap& stems);

/** K-weighting biquad coefficients for a sample rate: {b0,b1,b2,a1,a2} for the shelf, then the high-pass. */
std::array<std::array<double, 5>, 2> kWeightingCoefficients (double rate);
} // namespace juststem
