#pragma once

#include "../core/PitchShift.h"
#include "SongResult.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <functional>

namespace juststem
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

    /** Called when a user-facing setting (mute, solo, volume, Follow DAW) changes. */
    std::function<void()> onSettingChanged;

    // ---- controls (any thread)
    void setPlaying (bool p) { playing = p; }
    bool isPlaying() const { return playing; }
    void setFollowHost (bool f) { if (followHost.exchange (f) != f && onSettingChanged) onSettingChanged(); }
    bool followsHost() const { return followHost; }
    void seek (double seconds) { seekRequest = seconds; }
    /** Where the listener is: playback reads ahead of this by the pitch shifter's delay. */
    double positionSeconds() const
    {
        return (double) std::clamp<juce::int64> (position.load() - readAhead.load(), 0, length.load()) / sampleRate;
    }
    /** Pitch-shift the preview (semitones, 0 = off). The tempo doesn't change. */
    void setTranspose (int semitones) { transpose = semitones; }
    double durationSeconds() const { return (double) length.load() / sampleRate; }

    void setVolume (int stem, float v)
    {
        if (! juce::exactlyEqual (volume[(size_t) stem].exchange (v), v) && onSettingChanged) onSettingChanged();
    }
    void setMute (int stem, bool m) { if (mute[(size_t) stem].exchange (m) != m && onSettingChanged) onSettingChanged(); }
    void setSolo (int stem, bool s) { if (solo[(size_t) stem].exchange (s) != s && onSettingChanged) onSettingChanged(); }
    float getVolume (int stem) const { return volume[(size_t) stem]; }
    bool getMute (int stem) const { return mute[(size_t) stem]; }
    bool getSolo (int stem) const { return solo[(size_t) stem]; }

private:
    float targetGain (int stem) const;
    /** Audio thread: mixes and resamples `numSamples` (<= preparedBlock) into `out`. */
    void fillResampled (int numSamples);
    /** Audio thread: fills the pitch shifter's pipeline from `targetPos` so playback starts there with no gap. */
    void primeShifter (juce::int64 targetPos);
    /** Audio thread: mixes, resamples and shifts `numSamples` (<= preparedBlock) into `buffer` at
        `start`. False once the song has ended. */
    bool render (juce::AudioBuffer<float>& buffer, int start, int numSamples);

    juce::SpinLock songLock;
    SongResultPtr song; // guarded by songLock; only ever released on the message thread

    double hostRate = 44100;
    int preparedBlock = 0;                    // maxBlock from prepare(); 0 before it
    std::atomic<bool> playing { false }, followHost { false };
    std::atomic<double> seekRequest { -1 };
    std::atomic<juce::int64> position { 0 }, length { 0 };
    std::atomic<juce::int64> readAhead { 0 }; // 44.1 kHz samples; the shifter's delay while it's on
    std::atomic<int> transpose { 0 };
    PitchShifter shifter;
    int shiftedBy = 0;                        // audio thread: the transpose the shifter is set to
    bool wasActive = false, primed = false;
    std::array<std::atomic<float>, numStems> volume;
    std::array<std::atomic<bool>, numStems> mute, solo;
    std::array<float, numStems> currentGain {};

    std::vector<float> mixL, mixR;   // 44.1 kHz scratch
    juce::LagrangeInterpolator interpL, interpR;
    juce::AudioBuffer<float> out, shifted;
};
} // namespace juststem
