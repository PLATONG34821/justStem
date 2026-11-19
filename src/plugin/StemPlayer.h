#pragma once

#include "../core/PitchShift.h"
#include "SongResult.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace covertools
{
/** Real-time preview of the stems, mixed into the plug-in's output.
    Index 0..5 = stemOrder (vocals, drums, bass, guitar, piano, other). */
class StemPlayer
{
public:
    StemPlayer()
    {
        for (auto& v : volume) v = 1.0f;
        for (auto& m : mute) m = false;
        for (auto& s : solo) s = false;
    }

    /** Message thread, before playback (prepareToPlay). */
    void prepare (double hostSampleRate, int maxBlock);

    /** Message thread. Pass nullptr to clear. */
    void setSong (SongResultPtr song);

    /** Audio thread: adds the preview to `buffer`. hostSeconds < 0 when the host isn't playing. */
    void process (juce::AudioBuffer<float>& buffer, double hostSeconds);

    // ---- controls (any thread)
    void setPlaying (bool p) { playing = p; }
    bool isPlaying() const { return playing; }
    void setFollowHost (bool f) { followHost = f; }
    bool followsHost() const { return followHost; }
    void seek (double seconds) { seekRequest = seconds; }
    /** Where the listener is: playback reads ahead of this by the pitch shifter's delay. */
    double positionSeconds() const { return (double) std::max<juce::int64> (0, position.load() - readAhead.load()) / sampleRate; }
    /** Pitch-shift the preview (semitones, 0 = off). The tempo doesn't change. */
    void setTranspose (int semitones) { transpose = semitones; }
    double durationSeconds() const { return (double) length.load() / sampleRate; }

    void setVolume (int stem, float v) { volume[(size_t) stem] = v; }
    void setMute (int stem, bool m) { mute[(size_t) stem] = m; }
    void setSolo (int stem, bool s) { solo[(size_t) stem] = s; }
    float getVolume (int stem) const { return volume[(size_t) stem]; }
    bool getMute (int stem) const { return mute[(size_t) stem]; }
    bool getSolo (int stem) const { return solo[(size_t) stem]; }

private:
    float targetGain (int stem) const;

    juce::SpinLock songLock;
    SongResultPtr song; // guarded by songLock; only ever released on the message thread

    double hostRate = 44100;
    std::atomic<bool> playing { false }, followHost { false };
    std::atomic<double> seekRequest { -1 };
    std::atomic<juce::int64> position { 0 }, length { 0 };
    std::atomic<juce::int64> readAhead { 0 }; // 44.1 kHz samples; the shifter's delay while it's on
    std::atomic<int> transpose { 0 };
    PitchShifter shifter;
    int shiftedBy = 0;                        // audio thread: the transpose the shifter is set to
    bool wasActive = false;
    std::array<std::atomic<float>, numStems> volume;
    std::array<std::atomic<bool>, numStems> mute, solo;
    std::array<float, numStems> currentGain {};

    std::vector<float> mixL, mixR;   // 44.1 kHz scratch
    juce::LagrangeInterpolator interpL, interpR;
    juce::AudioBuffer<float> out, shifted;
};
} // namespace covertools
