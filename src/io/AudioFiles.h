#pragma once

#include "../core/Audio.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace juststem
{
/** Decode an audio file (WAV, AIFF, FLAC, Ogg Vorbis, MP3, M4A/AAC) to 44.1 kHz stereo.
    The same decoders on every OS. Throws std::runtime_error if the file can't be read or is too long. */
StereoAudio loadAudioFile (const juce::File& file, double maxSeconds = 15.0 * 60.0);

/** Write 44.1 kHz stereo as a WAV file (replacing it). Throws std::runtime_error. */
void writeWav (const juce::File& file, const StereoAudio& audio, int bitsPerSample = 24);

/** Wildcard of the formats loadAudioFile can read, e.g. "*.wav;*.mp3;...". */
juce::String supportedAudioWildcards();
} // namespace juststem
