#pragma once

#include "Audio.h"
#include "Dsp.h"

#include <string>
#include <vector>

namespace juststem
{
struct KeyResult;

enum class ChordQuality { Major, Minor, Dominant7, Major7, Minor7, Sus2, Sus4, Diminished };
constexpr int numChordQualities = 8;

struct Chord
{
    int root = -1;                               // pitch class (0 = C); -1 = no chord
    ChordQuality quality = ChordQuality::Major;
    int bass = -1;                               // a chord tone other than the root in the bass ("C/E"), or -1

    bool isNone() const { return root < 0; }
    bool operator== (const Chord&) const = default;
};

struct ChordSegment
{
    double start = 0, end = 0; // seconds
    Chord chord;
};

struct ChordResult
{
    static constexpr int beatsPerBar = 4;  // 4/4 is assumed
    std::vector<double> beats;             // seconds, covering the whole song
    int firstDownbeat = 0;                 // index into beats of the first bar line
    std::vector<ChordSegment> segments;    // consecutive, covering the whole song

    struct Bar
    {
        double start = 0, end = 0;
        std::vector<int> segments; // indices of the segments sounding in this bar, in order
    };
    /** The song in bars (the last ends at `length`). Beats before the first downbeat form a pickup bar. */
    std::vector<Bar> bars (double length) const;
    /** Index of the segment playing at `seconds`, or -1. */
    int segmentAt (double seconds) const;
};

/** "F#m7", "Bb/D", "N.C.", spelled for the key (flats in flat keys) after transposing. */
std::string chordName (const Chord&, const KeyResult& key, int transpose = 0);

/** A plain-text chord chart, `barsPerLine` bars per line: "| C   | Am7 | F  G | C |". */
std::string chordChart (const ChordResult&, double length, const KeyResult& key, int transpose = 0, int barsPerLine = 4);

/** Beat times (seconds) following a known tempo, covering `lengthSeconds`.
    @param onsets  dsp::onsetEnvelope of audio with clear onsets (drums) */
std::vector<double> trackBeats (const std::vector<float>& onsets, double lengthSeconds, int rate, float bpm);

/** Chords per beat, smoothed and biased towards the key.
    @param harmonic  instruments that play chords (no drums, no vocals)
    @param bass      the bass line (root and slash-chord bass notes)
    @param rhythm    audio to find the beats in (drums) */
ChordResult detectChords (const std::vector<float>& harmonic, const std::vector<float>& bass, const std::vector<float>& rhythm,
                          int rate, float bpm, const KeyResult& key);

/** The spectral peaks chords are found from: the chord instruments' and the bass line's. The slow part
    of chord detection, and independent of the beats and the key, so it can run alongside them. */
struct ChordPeaks
{
    dsp::PeakFrames harmonic, bass;
};
ChordPeaks chordPeaks (const std::vector<float>& harmonic, const std::vector<float>& bass, int rate);

/** detectChords with the peaks and the beats (from trackBeats) already known; a plain grid at `bpm`
    when `beats` is empty. */
ChordResult detectChordsOnBeats (const ChordPeaks&, double lengthSeconds, std::vector<double> beats, int rate, float bpm,
                                 const KeyResult& key);
} // namespace juststem
