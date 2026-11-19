#pragma once

#include "Audio.h"

#include <functional>
#include <memory>

namespace covertools
{
/** Real-time pitch shifting without changing the tempo (Signalsmith Stretch). The output is
    delayed by latency() samples. */
class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    /** Allocates; call before processing (not on the audio thread). */
    void prepare (int channels, double rate, int maxBlock);
    // Real-time safe from here on.
    void setSemitones (float semitones);
    int latency() const { return delay; }
    void reset();
    void process (const float* const* in, float* const* out, int numSamples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    int delay = 0;
};

/** Offline: the whole song shifted, aligned with the original (no delay).
    `progress` gets 0..1 and returns false to cancel (the result is then empty). */
StereoAudio pitchShift (const StereoAudio&, float semitones, int rate, const std::function<bool (double)>& progress = {});
} // namespace covertools
