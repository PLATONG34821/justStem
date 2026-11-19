#pragma once

#include "AppPaths.h"

#include <atomic>

namespace juststem
{
struct DownloadedAudio
{
    juce::File file;
    juce::String title;
};

/** True for youtube.com, youtu.be and music.youtube.com links. */
bool isYouTubeLink (const juce::String& text);

/** Download a video's audio track with yt-dlp, as AAC (M4A), which macOS decodes natively, so
    ffmpeg isn't needed. Blocking (call from a background thread). Throws std::runtime_error. */
DownloadedAudio downloadYouTubeAudio (const juce::String& link, const ProgressCallback&, const std::atomic<bool>& cancel);
} // namespace juststem
