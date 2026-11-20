#pragma once

namespace juststem
{
/** The pitch class (0..11, 0 = C) of a semitone number, negative ones included. */
constexpr int wrapPitchClass (int semitones) { return ((semitones % 12) + 12) % 12; }

/** Note names by pitch class, spelled with sharps or with flats. */
inline constexpr const char* sharpNames[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
inline constexpr const char* flatNames[12] { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
} // namespace juststem
