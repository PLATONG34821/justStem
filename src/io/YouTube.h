#pragma once

#include "AppPaths.h"

#include <atomic>
#include <string>

namespace juststem
{
/** A downloaded audio track. It sits in a folder of its own in paths::downloads(), which is deleted
    with this object: only the stems are kept, and nothing is left behind if anything fails. */
struct DownloadedAudio
{
    juce::File file;
    juce::String title;

    DownloadedAudio() = default;
    explicit DownloadedAudio (juce::File ownFolder) : folder (std::move (ownFolder)) {}
    DownloadedAudio (DownloadedAudio&& other) noexcept { *this = std::move (other); }
    DownloadedAudio& operator= (DownloadedAudio&& other) noexcept;
    ~DownloadedAudio() { deleteFolder(); }

private:
    void deleteFolder();
    juce::File folder;
};

/** True for youtube.com, youtu.be and music.youtube.com links. */
bool isYouTubeLink (const juce::String& text);

/** Appends raw UTF-8 pipe bytes to `pending` and passes each complete line (plus any trailing line at
    `eof`) to `onLine`, keeping multi-byte characters split across reads intact. */
void readUtf8Lines (std::string& pending, const char* data, int bytes, bool eof,
                    const std::function<void (const juce::String&)>& onLine);

/** Download a video's audio track with yt-dlp, as AAC (M4A), which loadAudioFile decodes itself
    (FAAD2), so ffmpeg isn't needed. Blocking (call from a background thread). Throws std::runtime_error. */
DownloadedAudio downloadYouTubeAudio (const juce::String& link, const ProgressCallback&, const std::atomic<bool>& cancel);
} // namespace juststem
