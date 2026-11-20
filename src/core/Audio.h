#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace juststem
{
/** Everything inside justStem runs at this rate (the model's rate). */
constexpr int sampleRate = 44100;

struct StereoAudio
{
    std::vector<float> left, right;
    std::size_t size() const { return left.size(); }
};

enum class Stem { Vocals, Drums, Bass, Guitar, Piano, Other };
constexpr int numStems = 6;

/** Display order in the UI. */
constexpr std::array<Stem, numStems> stemOrder { Stem::Vocals, Stem::Drums, Stem::Bass,
                                                 Stem::Guitar, Stem::Piano, Stem::Other };

/** Lower-case name, also used for the stem's file name. */
constexpr std::string_view stemName (Stem s)
{
    switch (s)
    {
        case Stem::Vocals: return "vocals";
        case Stem::Drums: return "drums";
        case Stem::Bass: return "bass";
        case Stem::Guitar: return "guitar";
        case Stem::Piano: return "piano";
        case Stem::Other: return "other";
    }
    return "";
}

/** The stem's file in a song folder: "vocals.flac" (songs saved before 0.0.3 have "vocals.wav"). */
inline std::string stemFileName (Stem s) { return std::string (stemName (s)) + ".flac"; }

/** The stem's name when exported for a DAW: "vocals.wav". */
inline std::string stemWavName (Stem s) { return std::string (stemName (s)) + ".wav"; }

using StemMap = std::map<Stem, StereoAudio>;
} // namespace juststem
