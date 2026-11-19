#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace covertools
{
/** Reports a step name and its progress (0..1, or -1 when unknown). */
using ProgressCallback = std::function<void (const juce::String& step, double fraction)>;

/** Where CoverTools keeps its files. */
namespace paths
{
    /** The app's data folder, created on demand:
          macOS    ~/Library/Application Support/CoverTools
          Windows  %APPDATA%\CoverTools
          Linux    ~/.config/CoverTools */
    inline juce::File supportRoot()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        dir = dir.getChildFile ("Application Support");
       #endif
        dir = dir.getChildFile ("CoverTools");
        dir.createDirectory();
        return dir;
    }

    /** A sub-folder of supportRoot(), created on demand. */
    inline juce::File support (const char* name)
    {
        auto dir = supportRoot().getChildFile (name);
        dir.createDirectory();
        return dir;
    }

    inline juce::File tools() { return support ("bin"); }                // yt-dlp, Deno
    inline juce::File coremlCache() { return support ("coreml-cache"); } // compiled model
    inline juce::File downloads() { return support ("downloads"); }      // YouTube audio, deleted after use

    /** <user's Music folder>/CoverTools: one folder of stems per song. */
    inline juce::File stems()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("CoverTools");
        dir.createDirectory();
        return dir;
    }
} // namespace paths
} // namespace covertools
