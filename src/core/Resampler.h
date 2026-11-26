#pragma once

#include <vector>

namespace juststem
{
/** High-quality sample-rate conversion (polyphase, Kaiser-windowed sinc, about 90 dB stopband).
    Output sample k sits exactly at input position k * inRate / outRate, so timing is preserved.
    The low-pass follows the lower of the two rates, so downsampling doesn't alias. */
std::vector<float> resample (const std::vector<float>& in, int inRate, int outRate);
void resampleStereo (std::vector<float>& left, std::vector<float>& right, int inRate, int outRate);
} // namespace juststem
