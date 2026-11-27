#include "StemPlayer.h"

#include <limits>

namespace juststem
{
void StemPlayer::prepare (double rate, int maxBlock)
{
    const juce::SpinLock::ScopedLockType sl (songLock);
    if (! std::isfinite (rate) || rate <= 0.0 || maxBlock <= 0)
    {
        preparedBlock = 0;
        return;
    }
    // Cap scratch allocations for hosts that announce huge offline block sizes (process() slices larger blocks).
    maxBlock = std::min (maxBlock, 65536);
    hostRate = rate;
    preparedBlock = maxBlock;
    const int need = (int) std::ceil (maxBlock * sampleRate / rate) + 16;
    mixL.assign ((size_t) need, 0.0f);
    mixR.assign ((size_t) need, 0.0f);
    const int maxFade = std::max (1, (int) std::ceil (rate * 0.004));
    fadeL.assign ((size_t) maxFade, 0.0f);
    fadeR.assign ((size_t) maxFade, 0.0f);
    fadePos = fadeLen = 0;
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
        fadePos = fadeLen = 0;
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
    // ~5 ms per unit gain change at 44.1 kHz, independent of the host's block or sub-block size.
    constexpr float maxGainStep = 1.0f / (0.005f * (float) sampleRate);
    if (avail > 0)
    {
        for (int i = 0; i < numStems; ++i)
        {
            const float g0 = currentGain[(size_t) i], g1 = targetGain (i);
            if (juce::exactlyEqual (g0, 0.0f) && juce::exactlyEqual (g1, 0.0f)) continue;
            const auto& st = song->stems.at (stemOrder[(size_t) i]);
            const float* l = st.left.data() + pos;
            const float* r = st.right.data() + pos;
            if (juce::exactlyEqual (g0, g1))
            {
                for (int k = 0; k < avail; ++k)
                {
                    mixL[(size_t) k] += l[k] * g0;
                    mixR[(size_t) k] += r[k] * g0;
                }
            }
            else
            {
                const float step = g1 > g0 ? maxGainStep : -maxGainStep;
                float g = g0;
                for (int k = 0; k < avail; ++k)
                {
                    g = step > 0.0f ? std::min (g + step, g1) : std::max (g + step, g1);
                    mixL[(size_t) k] += l[k] * g;
                    mixR[(size_t) k] += r[k] * g;
                }
                currentGain[(size_t) i] = g;
            }
        }
    }
    else
    {
        for (int i = 0; i < numStems; ++i) currentGain[(size_t) i] = targetGain (i);
    }
    const int used = interpL.process (ratio, mixL.data(), out.getWritePointer (0), numOut);
    interpR.process (ratio, mixR.data(), out.getWritePointer (1), numOut);
    position = pos + used;
}

void StemPlayer::renderBlock (int numOut)
{
    fillResampled (numOut);
    if (shiftedBy != 0)
    {
        const float* ins[] { out.getReadPointer (0), out.getReadPointer (1) };
        float* outs[] { shifted.getWritePointer (0), shifted.getWritePointer (1) };
        shifter.process (ins, outs, numOut);
        out.copyFrom (0, 0, shifted, 0, 0, numOut);
        out.copyFrom (1, 0, shifted, 1, 0, numOut);
    }
}

void StemPlayer::captureFade()
{
    const int total = (int) fadeL.size();
    if (total <= 0) return;
    // Preserve any in-progress fade tail before overwriting fadeL/fadeR.
    const int prevRem = std::max (0, fadeLen - fadePos);
    const int prevLen = std::max (1, fadeLen);
    if (prevRem > 0 && fadePos > 0)
    {
        std::move (fadeL.begin() + fadePos, fadeL.begin() + fadeLen, fadeL.begin());
        std::move (fadeR.begin() + fadePos, fadeR.begin() + fadeLen, fadeR.begin());
    }
    const juce::int64 endPos = length.load() + readAhead.load();
    int written = 0;
    while (written < total && position.load() < endPos)
    {
        const int step = std::min (total - written, preparedBlock);
        renderBlock (step);
        const float* l = out.getReadPointer (0);
        const float* r = out.getReadPointer (1);
        for (int i = 0; i < step; ++i)
        {
            const int idx = written + i;
            float sl = l[i], sr = r[i];
            if (idx < prevRem)
            {
                const float pw = (float) (fadePos + idx + 1) / (float) prevLen;
                sl = fadeL[(size_t) idx] + sl * pw;
                sr = fadeR[(size_t) idx] + sr * pw;
            }
            const float decay = 1.0f - (float) (idx + 1) / (float) total;
            fadeL[(size_t) idx] = sl * decay;
            fadeR[(size_t) idx] = sr * decay;
        }
        written += step;
    }
    for (int i = written; i < total; ++i)
    {
        const float decay = 1.0f - (float) (i + 1) / (float) total;
        fadeL[(size_t) i] = i < prevRem ? fadeL[(size_t) i] * decay : 0.0f;
        fadeR[(size_t) i] = i < prevRem ? fadeR[(size_t) i] * decay : 0.0f;
    }
    fadePos = 0;
    fadeLen = total;
}

void StemPlayer::primeShifter (juce::int64 targetPos)
{
    targetPos = juce::jlimit<juce::int64> (0, length.load(), targetPos);
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
    const bool active = followHost ? (hostSeconds >= 0) : playing.load();
    juce::int64 audible = std::clamp<juce::int64> (position.load() - readAhead.load(), 0, len);
    const int t = transpose;
    const double s = seekRequest.exchange (-1);
    const bool hostJump = followHost && active
                          && ((t != 0 && (shiftedBy == 0 || ! primed)) || std::abs ((juce::int64) (hostSeconds * sampleRate) - audible) > 2048);
    const bool stopping = ! active && wasActive;
    const bool jumping = active && wasActive && (s >= 0 || hostJump || (t == 0 && shiftedBy != 0));
    if ((stopping || jumping) && (shiftedBy == 0 || primed))
        captureFade();
    else if (active && ! wasActive)
    {
        for (int i = 0; i < numStems; ++i) currentGain[(size_t) i] = targetGain (i);
        if (fadePos < fadeLen)
        {
            std::move (fadeL.begin() + fadePos, fadeL.begin() + fadeLen, fadeL.begin());
            std::move (fadeR.begin() + fadePos, fadeR.begin() + fadeLen, fadeR.begin());
            fadeLen -= fadePos;
            fadePos = 0;
        }
        else
        {
            std::fill (fadeL.begin(), fadeL.end(), 0.0f);
            std::fill (fadeR.begin(), fadeR.end(), 0.0f);
            fadePos = 0;
            fadeLen = (int) fadeL.size();
        }
    }

    bool needPrime = false;

    // Transpose: the shifter delays its output by shifter.latency(), so starting or seeking feeds
    // that many samples from `audible` through the shifter first and renders silence-padded tails
    // at the end so the first and last ~120 ms are never dropped.
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
    if (s >= 0)
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
    if (! active && wasActive)
    {
        position = audible;
        readAhead = 0;
        primed = false;
        interpL.reset();
        interpR.reset();
    }
    wasActive = active;
    if (! active)
    {
        if (fadePos < fadeLen)
        {
            const int n = std::min (buffer.getNumSamples(), fadeLen - fadePos);
            if (buffer.getNumChannels() >= 2)
            {
                buffer.addFrom (0, 0, fadeL.data() + fadePos, n);
                buffer.addFrom (1, 0, fadeR.data() + fadePos, n);
            }
            else if (buffer.getNumChannels() == 1)
            {
                buffer.addFrom (0, 0, fadeL.data() + fadePos, n, 0.5f);
                buffer.addFrom (0, 0, fadeR.data() + fadePos, n, 0.5f);
            }
            fadePos += n;
        }
        return;
    }

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

    renderBlock (numOut);
    if (fadePos < fadeLen)
    {
        float* l = out.getWritePointer (0);
        float* r = out.getWritePointer (1);
        const int n = std::min (numOut, fadeLen - fadePos);
        for (int i = 0; i < n; ++i)
        {
            const float w = (float) (fadePos + i + 1) / (float) fadeLen;
            l[i] = fadeL[(size_t) (fadePos + i)] + l[i] * w;
            r[i] = fadeR[(size_t) (fadePos + i)] + r[i] * w;
        }
        fadePos += n;
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
