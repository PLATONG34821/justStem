#include "Analysis.h"
#include "Dsp.h"
#include "Pitch.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <numbers>

namespace juststem
{
namespace
{
const char* noteName (int pc, bool minor)
{
    // Keys conventionally written with flats: F Bb Eb Ab Db major; D G C F Bb minor.
    static constexpr bool flatMajor[12] { 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0 };
    static constexpr bool flatMinor[12] { 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0 };
    return (minor ? flatMinor[pc] : flatMajor[pc]) ? flatNames[pc] : sharpNames[pc];
}

// Key profiles, index 0 = tonic. Krumhansl-Kessler (1982), Temperley (2001), Aarden (2003).
constexpr float profiles[3][2][12] {
    { { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f },
      { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f } },
    { { 0.748f, 0.060f, 0.488f, 0.082f, 0.670f, 0.460f, 0.096f, 0.715f, 0.104f, 0.366f, 0.057f, 0.400f },
      { 0.712f, 0.084f, 0.474f, 0.618f, 0.049f, 0.460f, 0.105f, 0.747f, 0.404f, 0.067f, 0.133f, 0.330f } },
    { { 17.7661f, 0.145624f, 14.9265f, 0.160186f, 19.8049f, 11.3587f, 0.291248f, 22.062f, 0.145624f, 8.15494f, 0.232998f, 4.95122f },
      { 18.2648f, 0.737619f, 14.0499f, 16.8599f, 0.702494f, 14.4362f, 0.702494f, 18.6161f, 4.56621f, 1.93186f, 7.37619f, 1.75623f } },
};

float pearson (const float* a, const float* b, int n)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < n; ++i)
    {
        num += (a[i] - ma) * (b[i] - mb);
        da += (a[i] - ma) * (a[i] - ma);
        db += (b[i] - mb) * (b[i] - mb);
    }
    return (float) (num / std::sqrt (std::max (da * db, 1e-20)));
}
} // namespace

// ------------------------------------------------------------------------------------ KeyResult
std::string KeyResult::name() const { return std::string (noteName (tonic, minor)) + (minor ? " minor" : " major"); }
std::string KeyResult::shortName() const { return std::string (noteName (tonic, minor)) + (minor ? "m" : ""); }

std::string KeyResult::camelot() const
{
    static constexpr int majorWheel[12] { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
    static constexpr int minorWheel[12] { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };
    return std::to_string (minor ? minorWheel[tonic] : majorWheel[tonic]) + (minor ? "A" : "B");
}

KeyResult KeyResult::relative() const
{
    auto r = *this;
    r.minor = ! minor;
    r.tonic = minor ? (tonic + 3) % 12 : (tonic + 9) % 12;
    return r;
}

KeyResult KeyResult::transposed (int semitones) const
{
    auto r = *this;
    r.tonic = wrapPitchClass (tonic + semitones);
    return r;
}

// ------------------------------------------------------------------------------------ key
KeyResult detectKey (const std::vector<float>& x, int sr)
{
    const float minF = 55.0f;
    const auto peaks = dsp::spectralPeaks (x, sr, 8192, 4096, minF, 5000.0f);
    const float tuning = peaks.tuning;
    const double ref = 440.0 * std::pow (2.0, tuning / 12.0);

    // Harmonic pitch-class profile: each peak votes for itself and for the fundamentals it
    // could be a harmonic of (weight 0.6^(h-1)), spread over +-2/3 semitone with a cos^2 window.
    std::array<double, 12> chroma {};
    for (auto& fp : peaks.frames)
    {
        std::array<double, 12> c {};
        for (auto& p : fp)
            for (int h = 1; h <= 4; ++h)
            {
                const double f = p.freq / h;
                if (f < minF) break;
                const double pitch = 12.0 * std::log2 (f / ref) + 69.0;
                const double w = p.mag * std::pow (0.6, h - 1);
                const int centre = (int) std::round (pitch);
                for (int q = centre - 1; q <= centre + 1; ++q)
                {
                    const double dist = std::fabs (pitch - q);
                    if (dist >= 2.0 / 3.0) continue;
                    const double cw = std::cos (std::numbers::pi / 2 * dist / (2.0 / 3.0));
                    c[(size_t) wrapPitchClass (q)] += w * cw * cw;
                }
            }
        const double mx = *std::max_element (c.begin(), c.end());
        if (mx <= 0) continue;
        for (int i = 0; i < 12; ++i) chroma[(size_t) i] += c[(size_t) i] / mx; // per-frame normalisation
    }

    std::array<float, 12> cf {};
    for (int i = 0; i < 12; ++i) cf[(size_t) i] = (float) chroma[(size_t) i];

    // Correlate with all 24 keys for each profile, then vote.
    struct Vote { int tonic; bool minor; float corr; };
    std::vector<Vote> best;
    for (auto& prof : profiles)
    {
        Vote v { 0, false, -2.0f };
        for (int mode = 0; mode < 2; ++mode)
            for (int t = 0; t < 12; ++t)
            {
                float rotated[12];
                for (int i = 0; i < 12; ++i) rotated[(i + t) % 12] = prof[mode][i];
                const float r = pearson (cf.data(), rotated, 12);
                if (r > v.corr) v = { t, mode == 1, r };
            }
        best.push_back (v);
    }

    KeyResult result;
    int bestVotes = 0;
    float bestCorr = -2;
    for (auto& v : best)
    {
        int votes = 0;
        float corr = 0;
        for (auto& o : best)
            if (o.tonic == v.tonic && o.minor == v.minor) { ++votes; corr = std::max (corr, o.corr); }
        if (votes > bestVotes || (votes == bestVotes && corr > bestCorr))
        {
            bestVotes = votes;
            bestCorr = corr;
            result.tonic = v.tonic;
            result.minor = v.minor;
        }
    }
    result.votes = bestVotes;
    result.confidence = std::clamp (bestCorr, 0.0f, 1.0f) * (float) bestVotes / 3.0f;
    result.tuningCents = tuning * 100.0f;
    return result;
}

// ------------------------------------------------------------------------------------ tempo
namespace
{
/** detectTempo from the audio's onset envelope (dsp::onsetEnvelope). */
TempoResult tempoFromOnsets (const std::vector<float>& env, int sr)
{
    const double frameRate = (double) sr / dsp::onsetHop;
    if (env.size() < 64) return {};  // too short to measure

    // Autocorrelation over ~8 s of lags, so the comb below can compare up to 16 beats.
    const int maxLag = std::min ((int) env.size() - 1, (int) (frameRate * 8.0) + 2);
    std::vector<double> ac ((size_t) maxLag + 1, 0.0);
    for (int lag = 0; lag <= maxLag; ++lag)
        for (std::size_t i = (size_t) lag; i < env.size(); ++i) ac[(size_t) lag] += (double) env[i] * env[i - (size_t) lag];
    if (ac[0] <= 0) return {};

    auto acAt = [&] (double lag) {
        const int i = (int) lag;
        if (i < 0 || i + 1 > maxLag) return 0.0;
        const double f = lag - i;
        return ac[(size_t) i] * (1 - f) + ac[(size_t) i + 1] * f;
    };
    // Score a tempo by the autocorrelation at 1..beats beat periods (normalised per beat).
    // The coarse search uses 4 beats and a log-Gaussian prior centred on 120 BPM (one octave
    // wide) to pick the tempo octave; refinement uses up to 16 beats for a precise period.
    auto score = [&] (double bpm, bool prior, int beats) {
        const double lag = frameRate * 60.0 / bpm;
        double s = 0;
        int used = 0;
        for (int k = 1; k <= beats && lag * k + 1 < maxLag; ++k, ++used) s += acAt (lag * k);
        s /= std::max (used, 1);
        if (prior) s *= std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0), 2));
        return s;
    };

    double bestBpm = 120, bestScore = -1;
    for (double bpm = 60; bpm <= 200; bpm += 0.5)
        if (const double s = score (bpm, true, 4); s > bestScore) { bestScore = s; bestBpm = bpm; }
    // Refine to 0.01 BPM around the coarse peak (no prior: only the period matters now).
    double fine = bestBpm, fineScore = -1;
    for (double bpm = bestBpm - 1; bpm <= bestBpm + 1; bpm += 0.01)
    {
        const double s = score (bpm, false, 16);
        if (s > fineScore) { fineScore = s; fine = bpm; }
    }

    return { (float) fine };
}
} // namespace

TempoResult detectTempo (const std::vector<float>& x, int sr) { return tempoFromOnsets (dsp::onsetEnvelope (x, sr), sr); }

// ------------------------------------------------------------------------------------ loudness
std::array<std::array<double, 5>, 2> kWeightingCoefficients (double fs)
{
    // ITU-R BS.1770 K-weighting, derived for any sample rate (same design as pyloudnorm / libebur128).
    std::array<std::array<double, 5>, 2> c {};
    {
        const double G = 3.999843853973347, f0 = 1681.974450955533, Q = 0.7071752369554196;
        const double K = std::tan (std::numbers::pi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        c[0] = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (std::numbers::pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        c[1] = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
    }
    return c;
}

LoudnessResult measureLoudness (const std::vector<float>& left, const std::vector<float>& right, int sr)
{
    LoudnessResult r;
    const auto coef = kWeightingCoefficients (sr);
    const std::size_t n = left.size();

    // Gating blocks are 400 ms with 75% overlap, i.e. four 100 ms steps, so only the K-weighted
    // power of each step is kept (not a copy of the whole song).
    const std::size_t step = (std::size_t) std::max (1, sr / 10), block = 4 * step;
    std::vector<double> stepPower (n / step, 0.0); // whole steps only
    float peak = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto& x = ch == 0 ? left : right;
        double s1[2] {}, s2[2] {};
        std::size_t i = 0;
        for (std::size_t k = 0; i < n; ++k)
        {
            double power = 0;
            for (const std::size_t end = std::min (n, i + step); i < end; ++i)
            {
                peak = std::max (peak, std::fabs (x[i]));
                double v = x[i];
                for (int st = 0; st < 2; ++st) // transposed direct form II
                {
                    const auto& c = coef[(size_t) st];
                    const double y = c[0] * v + s1[st];
                    s1[st] = c[1] * v - c[3] * y + s2[st];
                    s2[st] = c[2] * v - c[4] * y;
                    v = y;
                }
                power += v * v;
            }
            if (k < stepPower.size()) stepPower[k] += power;
        }
    }
    r.samplePeakDb = (float) (20.0 * std::log10 (std::max (peak, 1e-10f)));

    if (n < block) return r;
    std::vector<double> z;
    for (std::size_t j = 0; j + 4 <= stepPower.size(); ++j)
        z.push_back ((stepPower[j] + stepPower[j + 1] + stepPower[j + 2] + stepPower[j + 3]) / (double) block);

    auto lufs = [] (double power) { return -0.691 + 10.0 * std::log10 (std::max (power, 1e-20)); };
    double sumAbs = 0;
    int nAbs = 0;
    for (double v : z) if (lufs (v) > -70.0) { sumAbs += v; ++nAbs; }
    if (nAbs == 0) return r;
    const double relGate = lufs (sumAbs / nAbs) - 10.0;
    double sum = 0;
    int cnt = 0;
    for (double v : z) if (lufs (v) > -70.0 && lufs (v) > relGate) { sum += v; ++cnt; }
    r.integratedLufs = (float) lufs (sum / std::max (cnt, 1));
    return r;
}
// ------------------------------------------------------------------------------------ whole song
namespace
{
/** Mono mixes of a song's parts, made once for all the detectors: from the separated stems, or
    the mix for every part when there are no stems. */
struct SongParts
{
    std::vector<float> nonDrums; // everything but the drums (key)
    std::vector<float> harmonic; // guitar + piano + other: the chord instruments (no vocals, no bass)
    std::vector<float> bass, drums;

    SongParts (const StereoAudio& mix, const StemMap& stems)
    {
        const std::size_t n = stems.empty() ? mix.size() : stems.begin()->second.size();
        auto mono = [n] (std::vector<float>& dst, const StereoAudio& src) {
            for (std::size_t i = 0; i < std::min (n, src.size()); ++i) dst[i] += 0.5f * (src.left[i] + src.right[i]);
        };
        nonDrums.assign (n, 0.0f);
        if (stems.empty())
        {
            mono (nonDrums, mix);
            harmonic = bass = drums = nonDrums;
            return;
        }
        for (auto* v : { &harmonic, &bass, &drums }) v->assign (n, 0.0f);
        for (const auto& [stem, audio] : stems)
        {
            if (stem != Stem::Drums) mono (nonDrums, audio);
            switch (stem)
            {
                case Stem::Vocals: break; // melody, not harmony
                case Stem::Drums: mono (drums, audio); break;
                case Stem::Bass: mono (bass, audio); break;
                default: mono (harmonic, audio); break;
            }
        }
    }
};

/** The onset envelope that tempo and beats are found in: the drums', or for a song without drums
    (a near-silent drums stem) the chord instruments' and the bass's. */
std::vector<float> rhythmOnsets (const SongParts& parts)
{
    const std::size_t n = parts.drums.size();
    double drumEnergy = 0;
    for (float v : parts.drums) drumEnergy += (double) v * v;
    if (n == 0 || std::sqrt (drumEnergy / (double) n) >= 1e-3) return dsp::onsetEnvelope (parts.drums, sampleRate);
    std::vector<float> rhythm (n);
    for (std::size_t i = 0; i < n; ++i) rhythm[i] = parts.harmonic[i] + parts.bass[i];
    return dsp::onsetEnvelope (rhythm, sampleRate);
}

/** The chords, from peaks and rhythm onsets worked out alongside each other. */
ChordResult songChords (const SongParts& parts, const ChordPeaks& peaks, const std::vector<float>& onsets, float bpm, const KeyResult& key)
{
    const double length = (double) parts.drums.size() / sampleRate;
    return detectChordsOnBeats (peaks, length, trackBeats (onsets, length, sampleRate, bpm), sampleRate, bpm, key);
}
} // namespace

SongAnalysis analyzeSong (const StemMap& stems, LoudnessResult loudness)
{
    const SongParts parts ({}, stems);
    auto onsets = std::async (std::launch::async, [&] { return rhythmOnsets (parts); });
    auto key = std::async (std::launch::async, [&] { return detectKey (parts.nonDrums, sampleRate); });
    const auto peaks = chordPeaks (parts.harmonic, parts.bass, sampleRate);
    const auto rhythm = onsets.get();
    SongAnalysis a { key.get(), tempoFromOnsets (rhythm, sampleRate), loudness, {} };
    a.chords = songChords (parts, peaks, rhythm, a.tempo.bpm, a.key);
    return a;
}

SongAnalysis analyzeSong (const StereoAudio& mix, const StemMap& stems)
{
    // Key from everything except drums; tempo and beats from the drums (from the other instruments
    // when there are none); loudness from the original mix; chords from the chord instruments and
    // the bass. Without stems, all of them use the mix.
    // The slow steps don't depend on each other, so they run at the same time (the results are the
    // same as one after another).
    const SongParts parts (mix, stems);
    auto onsets = std::async (std::launch::async, [&] { return rhythmOnsets (parts); });
    auto key = std::async (std::launch::async, [&] { return detectKey (parts.nonDrums, sampleRate); });
    auto loudness = std::async (std::launch::async, [&] { return measureLoudness (mix.left, mix.right, sampleRate); });
    const auto peaks = chordPeaks (parts.harmonic, parts.bass, sampleRate);
    const auto rhythm = onsets.get();
    SongAnalysis a { key.get(), tempoFromOnsets (rhythm, sampleRate), loudness.get(), {} };
    a.chords = songChords (parts, peaks, rhythm, a.tempo.bpm, a.key);
    return a;
}

ChordResult detectSongChords (const StereoAudio& mix, const StemMap& stems, float bpm, const KeyResult& key)
{
    const SongParts parts (mix, stems);
    auto onsets = std::async (std::launch::async, [&] { return rhythmOnsets (parts); });
    const auto peaks = chordPeaks (parts.harmonic, parts.bass, sampleRate);
    return songChords (parts, peaks, onsets.get(), bpm, key);
}
} // namespace juststem
