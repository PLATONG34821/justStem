#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace juststem
{
/** Decoded audio at its original sample rate. */
struct DecodedAudio
{
    std::vector<float> left, right;
    double sampleRate = 0;
};

/** True if the file is an MP4 / M4A container (checks for the 'ftyp' box, not the extension). */
bool isMp4File (const juce::File&);

/** Decode the AAC track of an MP4 / M4A file: classic files (iTunes, most encoders) and
    fragmented ones (YouTube). Uses FAAD2, so it works the same on every OS.
    Mono is copied to both channels; more than two channels are downmixed.
    Throws std::runtime_error, also (songTooLong) for audio longer than `maxSeconds`, which is found
    from the frame count before decoding. */
DecodedAudio decodeM4a (const juce::File&, double maxSeconds);
} // namespace juststem
