#include "StemPlayer.h"

namespace juststem
{
void StemPlayer::prepare (double rate, int maxBlock)
{
    hostRate = rate;
    preparedBlock = maxBlock;
    const int need = (int) std::ceil (maxBlock * sampleRate / rate) + 16;
    mixL.assign ((size_t) need, 0.0f);
    mixR.assign ((size_t) need, 0.0f);
    out.setSize (2, maxBlock);
    shifted.setSize (2, maxBlock);
    interpL.reset();
    interpR.reset();
    shifter.prepare (2, rate, maxBlock);
    shiftedBy = 0;
    readAhead = 0;
}

void StemPlayer::setSong (SongResultPtr s)
{
    playing = false;
    position = 0;
    length = s ? (juce::int64) s->stems.at (Stem::Vocals).size() : 0;
    {
        const juce::SpinLock::ScopedLockType sl (songLock);
        std::swap (song, s);
    }
    // `s` now holds the previous song; it's freed here on the message thread, not the audio thread.
}

float StemPlayer::targetGain (int i) const
{
    bool anySolo = false;
    for (auto& s : solo) anySolo |= s.load();
    if (mute[(size_t) i] || (anySolo && ! solo[(size_t) i])) return 0.0f;
    return volume[(size_t) i];
}

void StemPlayer::process (juce::AudioBuffer<float>& buffer, double hostSeconds)
{
    const juce::SpinLock::ScopedTryLockType sl (songLock);
    if (! sl.isLocked() || song == nullptr || preparedBlock <= 0) return;

    const juce::int64 len = length;

    // Transpose: the shifter delays its output, so playback reads that far ahead and what you
    // hear stays at `position - readAhead` (in time with the DAW).
    const int t = transpose;
    if (t != shiftedBy)
    {
        const auto ahead = (juce::int64) std::llround (shifter.latency() * sampleRate / hostRate);
        if (shiftedBy == 0) { shifter.reset(); position = position + ahead; readAhead = ahead; }
        if (t == 0) { position = std::max<juce::int64> (0, position - readAhead); readAhead = 0; }
        else shifter.setSemitones ((float) t);
        shiftedBy = t;
    }

    // Follow the DAW: play while the host plays, at the host's position.
    bool active = playing;
    if (followHost)
    {
        active = hostSeconds >= 0;
        if (active)
        {
            const auto want = (juce::int64) (hostSeconds * sampleRate) + readAhead;
            if (std::abs (want - position.load()) > 2048) { position = want; interpL.reset(); interpR.reset(); }
        }
    }
    if (const double s = seekRequest.exchange (-1); s >= 0)
    {
        position = juce::jlimit ((juce::int64) 0, len, (juce::int64) (s * sampleRate)) + readAhead;
        interpL.reset();
        interpR.reset();
        if (shiftedBy != 0) shifter.reset();
    }
    if (active && ! wasActive && shiftedBy != 0) shifter.reset(); // no stale audio from before a pause
    wasActive = active;
    if (! active) return;

    // Hosts may send more samples than prepareToPlay announced: render in pieces of the prepared
    // size, so the scratch buffers are never overrun or reallocated here.
    const int numOut = buffer.getNumSamples();
    for (int done = 0; done < numOut; done += preparedBlock)
        if (! render (buffer, done, std::min (preparedBlock, numOut - done))) break;
}

bool StemPlayer::render (juce::AudioBuffer<float>& buffer, int start, int numOut)
{
    const juce::int64 len = length, pos = position;
    if (pos >= len)
    {
        if (! followHost) playing = false;
        return false;
    }

    // Mix the stems at 44.1 kHz with per-stem gains ramped across the block (no clicks).
    const double ratio = sampleRate / hostRate;
    const int need = std::min ((int) std::ceil (numOut * ratio) + 8, (int) mixL.size());
    std::fill (mixL.begin(), mixL.begin() + need, 0.0f);
    std::fill (mixR.begin(), mixR.begin() + need, 0.0f);
    const int avail = (int) std::min<juce::int64> (need, len - pos);
    for (int i = 0; i < numStems; ++i)
    {
        const float g0 = currentGain[(size_t) i], g1 = targetGain (i);
        currentGain[(size_t) i] = g1;
        if (juce::exactlyEqual (g0, 0.0f) && juce::exactlyEqual (g1, 0.0f)) continue;
        const auto& st = song->stems.at (stemOrder[(size_t) i]);
        const float* l = st.left.data() + pos;
        const float* r = st.right.data() + pos;
        const float step = (g1 - g0) / (float) std::max (1, avail);
        float g = g0;
        for (int k = 0; k < avail; ++k, g += step)
        {
            mixL[(size_t) k] += l[k] * g;
            mixR[(size_t) k] += r[k] * g;
        }
    }

    // Resample to the host rate (numOut <= the prepared block size, which `out` holds).
    const int used = interpL.process (ratio, mixL.data(), out.getWritePointer (0), numOut);
    interpR.process (ratio, mixR.data(), out.getWritePointer (1), numOut);
    position = pos + used;
    if (shiftedBy != 0)
    {
        const float* ins[] { out.getReadPointer (0), out.getReadPointer (1) };
        float* outs[] { shifted.getWritePointer (0), shifted.getWritePointer (1) };
        shifter.process (ins, outs, numOut);
        out.copyFrom (0, 0, shifted, 0, 0, numOut);
        out.copyFrom (1, 0, shifted, 1, 0, numOut);
    }

    if (buffer.getNumChannels() >= 2)
    {
        buffer.addFrom (0, start, out, 0, 0, numOut);
        buffer.addFrom (1, start, out, 1, 0, numOut);
    }
    else if (buffer.getNumChannels() == 1)
    {
        buffer.addFrom (0, start, out, 0, 0, numOut, 0.5f);
        buffer.addFrom (0, start, out, 1, 0, numOut, 0.5f);
    }
    return true;
}
} // namespace juststem
