#pragma once

#include "AppPaths.h"

#include <atomic>

namespace juststem
{
/** The command-line tools YouTube import needs, downloaded on first use into paths::tools().
    Each download is checked against the SHA-256 published with the official release.
    Both are blocking (call from a background thread) and throw std::runtime_error. */

/** yt-dlp (the official build for this OS). Lets yt-dlp update itself at most once a day. */
juce::File ensureYtDlp (const ProgressCallback&, const std::atomic<bool>& cancel);

/** Deno, the JavaScript runtime yt-dlp needs for YouTube. Refreshed monthly. */
juce::File ensureDeno (const ProgressCallback&, const std::atomic<bool>& cancel);

/** Download an ONNX separation model into `dest` from justStem's GitHub release if missing,
    verifying its SHA-256 against SHA256SUMS. */
juce::File ensureModel (const juce::File& dest, const ProgressCallback&, const std::atomic<bool>& cancel);

/** Extract a 64-hex SHA-256 digest for `fileName` from a checksum file (single-line or multi-line),
    or empty when missing or malformed (e.g. an HTTP error page). */
juce::String findSha256InSums (const juce::String& sumsText, const juce::String& fileName);
} // namespace juststem
