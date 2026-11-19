#pragma once

#include "AppPaths.h"

#include <atomic>

namespace covertools
{
/** The command-line tools YouTube import needs, downloaded on first use into paths::tools().
    Each download is checked against the SHA-256 published with the official release.
    Both are blocking (call from a background thread) and throw std::runtime_error. */

/** yt-dlp (the official build for this OS). Lets yt-dlp update itself at most once a day. */
juce::File ensureYtDlp (const ProgressCallback&, const std::atomic<bool>& cancel);

/** Deno, the JavaScript runtime yt-dlp needs for YouTube. Refreshed monthly. */
juce::File ensureDeno (const ProgressCallback&, const std::atomic<bool>& cancel);
} // namespace covertools
