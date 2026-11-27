#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace juststem
{
/** Reports a step name and its progress (0..1, or -1 when unknown). */
using ProgressCallback = std::function<void (const juce::String& step, double fraction)>;

/** Keeps the plug-in's shared library mapped in the host process when a detached/abandoned background
    task outlives the last processor instance, so dlclose / FreeLibrary can't unmap code still running. */
void pinPluginBinary() noexcept;

/** Where justStem keeps its files. */
namespace paths
{
    /** JUSTSTEM_DATA_DIR (for tests and screenshots): keep everything in that folder instead, so a run
        doesn't touch the user's songs or caches. Empty in normal use. */
    inline juce::File dataOverride()
    {
        const auto v = juce::SystemStats::getEnvironmentVariable ("JUSTSTEM_DATA_DIR", {});
        return v.isNotEmpty() && juce::File::isAbsolutePath (v) ? juce::File (v) : juce::File();
    }

    /** The app's data folder, created on demand:
          macOS    ~/Library/Application Support/justStem
          Windows  %APPDATA%\justStem
          Linux    ~/.config/justStem
        or <JUSTSTEM_DATA_DIR>/support. */
    inline juce::File supportRoot()
    {
        if (const auto o = dataOverride(); o != juce::File())
        {
            const auto dir = o.getChildFile ("support");
            dir.createDirectory();
            return dir;
        }
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        dir = dir.getChildFile ("Application Support");
       #endif
        dir = dir.getChildFile ("justStem");
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
    inline juce::File models() { return support ("models"); }            // downloaded ONNX models
    inline juce::File coremlCache() { return support ("coreml-cache"); } // compiled model
    inline juce::File downloads() { return support ("downloads"); }      // YouTube audio, deleted after use

    /** A song title as a folder name every OS accepts, or "Song". Windows silently drops trailing dots
        and spaces when it creates a folder ("Wait..." becomes "Wait"), so every later path with the
        title in it would miss; it also refuses control characters and device names like CON or NUL. */
    inline juce::String folderNameFor (const juce::String& title)
    {
        juce::String controls;
        for (juce::juce_wchar c = 1; c < 32; ++c) controls += juce::String::charToString (c);
        auto name = juce::File::createLegalFileName (title.removeCharacters (controls)).substring (0, 80)
                        .trim().trimCharactersAtEnd (". ").trim();
        static const juce::StringArray devices { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                                 "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                                 "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        if (devices.contains (name.upToFirstOccurrenceOf (".", false, false).trim(), true)) name = "Song " + name;
        return name.isEmpty() ? juce::String ("Song") : name;
    }

    /** <user's Music folder>/justStem: one folder of stems per song. */
    inline juce::File stems()
    {
        const auto o = dataOverride();
        auto dir = (o != juce::File() ? o.getChildFile ("Music") : juce::File::getSpecialLocation (juce::File::userMusicDirectory))
                       .getChildFile ("justStem");
        dir.createDirectory();
        return dir;
    }
} // namespace paths
} // namespace juststem
