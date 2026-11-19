#include "PitchShift.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>

namespace covertools
{
struct PitchShifter::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> stretch { 42 }; // fixed seed: the same input gives the same output
};

PitchShifter::PitchShifter() : impl (std::make_unique<Impl>()) {}
PitchShifter::~PitchShifter() = default;

void PitchShifter::prepare (int channels, double rate, int maxBlock)
{
    auto& s = impl->stretch;
    s.presetDefault (channels, (float) rate);
    delay = s.inputLatency() + s.outputLatency();
    // Run one block of the largest size so internal buffers are allocated now, not on the audio thread.
    std::vector<std::vector<float>> in ((size_t) channels, std::vector<float> ((size_t) maxBlock, 0.0f)), out = in;
    s.process (in, maxBlock, out, maxBlock);
    s.reset();
}

void PitchShifter::setSemitones (float semitones) { impl->stretch.setTransposeSemitones (semitones); }
void PitchShifter::reset() { impl->stretch.reset(); }

void PitchShifter::process (const float* const* in, float* const* out, int n)
{
    impl->stretch.process (in, n, out, n);
}

StereoAudio pitchShift (const StereoAudio& audio, float semitones, int rate, const std::function<bool (double)>& progress)
{
    constexpr int block = 4096;
    PitchShifter shifter;
    shifter.prepare (2, rate, block);
    shifter.setSemitones (semitones);
    const std::size_t n = audio.size(), delay = (size_t) shifter.latency();

    // Feed the song then `delay` samples of silence; drop the first `delay` output samples.
    StereoAudio result;
    result.left.resize (n);
    result.right.resize (n);
    std::vector<float> inL (block), inR (block), outL (block), outR (block);
    for (std::size_t done = 0; done < n + delay; done += block)
    {
        const int count = (int) std::min<std::size_t> (block, n + delay - done);
        for (int i = 0; i < count; ++i)
        {
            const std::size_t k = done + (size_t) i;
            inL[(size_t) i] = k < n ? audio.left[k] : 0.0f;
            inR[(size_t) i] = k < n ? audio.right[k] : 0.0f;
        }
        const float* ins[] { inL.data(), inR.data() };
        float* outs[] { outL.data(), outR.data() };
        shifter.process (ins, outs, count);
        for (int i = 0; i < count; ++i)
        {
            const std::size_t k = done + (size_t) i;
            if (k < delay || k - delay >= n) continue;
            result.left[k - delay] = outL[(size_t) i];
            result.right[k - delay] = outR[(size_t) i];
        }
        if (progress && ! progress ((double) (done + (size_t) count) / (double) (n + delay))) return {};
    }
    return result;
}
} // namespace covertools
