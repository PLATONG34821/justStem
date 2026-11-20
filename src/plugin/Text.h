#pragma once

#include "../core/Audio.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <string_view>

/** Small text helpers shared by the plug-in and the developer tools. */
namespace juststem::text
{
/** A UTF-8 string_view (stem and model names) as a juce::String. */
inline juce::String from (std::string_view s) { return juce::String::fromUTF8 (s.data(), (int) s.size()); }

/** A stem's name for people: "Vocals". */
inline juce::String stemLabel (Stem s)
{
    const auto n = from (stemName (s));
    return n.substring (0, 1).toUpperCase() + n.substring (1);
}

/** A tempo: "120" or "92.5". */
inline juce::String bpm (double value)
{
    const double r = std::round (value * 10.0) / 10.0;
    return std::abs (r - std::round (r)) < 0.05 ? juce::String ((int) std::round (r)) : juce::String (r, 1);
}

/** "m:ss" (negative times show as 0:00). */
inline juce::String minutesSeconds (double seconds)
{
    const int t = juce::jmax (0, (int) seconds);
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}
} // namespace juststem::text
