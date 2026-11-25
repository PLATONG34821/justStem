#include "StemPlayer.h"

#include <limits>

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
    // The shifter starts over, so playback stops reading ahead and re-primes at the current position.
    position = std::max<juce::int64> (0, position - readAhead);
    shiftedBy = 0;
    readAhead = 0;
    primed = false;
}

void StemPlayer::setSong (SongResultPtr s)
{
    // Playback stops at the end of the shortest stem, so no stem is ever read past its end (a song
    // missing a stem doesn't play at all).
    juce::int64 newLength = s != nullptr ? std::numeric_limits<juce::int64>::max() : 0;
    if (s != nullptr)
        for (auto stem : stemOrder)
        {
            const auto it = s->stems.find (stem);
            newLength = std::min<juce::int64> (newLength, it != s->stems.end() ? (juce::int64) it->second.size() : 0);
        }
    {
        // The song, its length and the position change together: the audio thread reads them under this lock.
        const juce::SpinLock::ScopedLockType sl (songLock);
        std::swap (song, s);
        length = newLength;
        position = 0;
        readAhead = 0;
        primed = false;
        playing = false;
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

void StemPlayer::fillResampled (int numOut)
{
    const juce::int64 len = length, pos = position;
    const double ratio = sampleRate / hostRate;
    const int need = std::min ((int) std::ceil (numOut * ratio) + 8, (int) mixL.size());
    std::fill (mixL.begin(), mixL.begin() + need, 0.0f);
    std::fill (mixR.begin(), mixR.begin() + need, 0.0f);
    const int avail = (int) std::clamp<juce::int64> (len - pos, 0, need);
    if (avail > 0)
    {
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
    }
    const int used = interpL.process (ratio, mixL.data(), out.getWritePointer (0), numOut);
    interpR.process (ratio, mixR.data(), out.getWritePointer (1), numOut);
    position = pos + used;
}

void StemPlayer::primeShifter (juce::int64 targetPos)
{
    targetPos = juce::jlimit<juce::int64> (0, length.load(), targetPos);
    if (! wasActive)
        for (int i = 0; i < numStems; ++i) currentGain[(size_t) i] = targetGain (i);
    interpL.reset();
    interpR.reset();
    shifter.reset();
    position = targetPos;
    const float* ins[] { out.getReadPointer (0), out.getReadPointer (1) };
    float* outs[] { shifted.getWritePointer (0), shifted.getWritePointer (1) };
    for (int left = shifter.latency(); left > 0;)
    {
        const int step = std::min (left, preparedBlock);
        fillResampled (step);
        shifter.process (ins, outs, step);
        left -= step;
    }
    readAhead = position.load() - targetPos;
    primed = true;
}

void StemPlayer::process (juce::AudioBuffer<float>& buffer, double hostSeconds)
{
    const juce::SpinLock::ScopedTryLockType sl (songLock);
    if (! sl.isLocked() || song == nullptr || preparedBlock <= 0) return;

    const juce::int64 len = length;
    bool active = followHost ? (hostSeconds >= 0) : playing.load();
    juce::int64 audible = std::clamp<juce::int64> (position.load() - readAhead.load(), 0, len);
    bool needPrime = false;

    // Transpose: the shifter delays its output by shifter.latency(), so starting or seeking feeds
    // that many samples from `audible` through the shifter first and renders silence-padded tails
    // at the end so the first and last ~120 ms are never dropped.
    const int t = transpose;
    if (t != shiftedBy)
    {
        if (t == 0)
        {
            position = audible;
            readAhead = 0;
            primed = false;
            interpL.reset();
            interpR.reset();
        }
        else
        {
            shifter.setSemitones ((float) t);
            if (shiftedBy == 0) needPrime = true;
        }
        shiftedBy = t;
    }

    // Follow the DAW: play while the host plays, at the host's position.
    if (followHost && active)
    {
        const auto want = (juce::int64) (hostSeconds * sampleRate);
        if ((shiftedBy != 0 && ! primed) || std::abs (want - audible) > 2048)
        {
            audible = std::max<juce::int64> (0, want);
            if (shiftedBy != 0)
            {
                if (audible < len) needPrime = true;
                else { position = audible; readAhead = 0; primed = true; }
            }
            else
            {
                position = audible;
                interpL.reset();
                interpR.reset();
            }
        }
    }
    if (const double s = seekRequest.exchange (-1); s >= 0)
    {
        audible = juce::jlimit<juce::int64> (0, len, (juce::int64) (s * sampleRate));
        if (shiftedBy != 0 && active)
            needPrime = true;
        else
        {
            position = audible;
            readAhead = 0;
            primed = false;
            interpL.reset();
            interpR.reset();
        }
    }
    if (active && ! wasActive && shiftedBy != 0 && ! primed) needPrime = true;
    if (! active && wasActive && shiftedBy != 0)
    {
        position = audible;
        readAhead = 0;
        primed = false;
    }
    wasActive = active;
    if (! active) return;

    if (needPrime && shiftedBy != 0) primeShifter (audible);

    // Hosts may send more samples than prepareToPlay announced: render in pieces of the prepared
    // size, so the scratch buffers are never overrun or reallocated here.
    const int numOut = buffer.getNumSamples();
    for (int done = 0; done < numOut; done += preparedBlock)
        if (! render (buffer, done, std::min (preparedBlock, numOut - done))) break;
}

bool StemPlayer::render (juce::AudioBuffer<float>& buffer, int start, int numOut)
{
    const juce::int64 len = length, pos = position, endPos = len + readAhead.load();
    if (pos >= endPos)
    {
        if (! followHost)
        {
            playing = false;
            position = len;
            readAhead = 0;
            primed = false;
        }
        return false;
    }

    fillResampled (numOut);
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
    if (! followHost && position.load() >= endPos)
    {
        playing = false;
        position = len;
        readAhead = 0;
        primed = false;
        return false;
    }
    return true;
}
} // namespace juststem
