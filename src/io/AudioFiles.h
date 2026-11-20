#pragma once

#include "../core/Audio.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <stdexcept>

namespace juststem
{
/** The longest song justStem takes. Longer files are refused before they're decoded (or downloaded). */
constexpr double maxSongSeconds = 15.0 * 60.0;

/** The error for a song longer than `maxSeconds`: "Songs longer than 15 minutes aren't supported". */
std::runtime_error songTooLong (double maxSeconds = maxSongSeconds);

/** Decode an audio file (WAV, AIFF, FLAC, Ogg Vorbis, MP3, M4A/AAC) to 44.1 kHz stereo.
    The same decoders on every OS. Throws std::runtime_error if the file can't be read or is too long. */
StereoAudio loadAudioFile (const juce::File& file, double maxSeconds = maxSongSeconds);

/** Write 44.1 kHz stereo as a WAV file (replacing it). Throws std::runtime_error. */
void writeWav (const juce::File& file, const StereoAudio& audio, int bitsPerSample = 24);

/** A short description of a file's format for logs, e.g. "MP3, 48 kHz, stereo", from its header only.
    Just the format name when the header can't be read here (M4A). */
juce::String describeAudioFile (const juce::File& file);

/** Wildcard of the formats loadAudioFile can read, e.g. "*.wav;*.mp3;...". */
juce::String supportedAudioWildcards();
} // namespace juststem
