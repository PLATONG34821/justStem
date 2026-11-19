#include "Analysis.h"
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace covertools
{
namespace
{
constexpr const char* sharpNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr const char* flatNames[] { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

const char* noteName (int pc, bool minor)
{
    // Keys conventionally written with flats: F Bb Eb Ab Db major; D G C F Bb minor.
    static constexpr bool flatMajor[12] { 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0 };
    static constexpr bool flatMinor[12] { 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0 };
    return (minor ? flatMinor[pc] : flatMajor[pc]) ? flatNames[pc] : sharpNames[pc];
}

std::vector<float> hann (int n)
{
    std::vector<float> w ((size_t) n);
    for (int i = 0; i < n; ++i) w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2 * std::numbers::pi * i / n));
    return w;
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
    r.tonic = ((tonic + semitones) % 12 + 12) % 12;
    return r;
}

// ------------------------------------------------------------------------------------ key
KeyResult detectKey (const std::vector<float>& x, int sr)
{
    constexpr int n = 8192, hop = 4096;
    const float minF = 55.0f, maxF = 5000.0f;
    RealFft fft (n);
    const auto win = hann (n);
    std::vector<float> frame ((size_t) n), mag ((size_t) n / 2 + 1);

    struct Peak { float freq, mag; };
    std::vector<std::vector<Peak>> peaks;
    double tuneRe = 0, tuneIm = 0;

    for (std::size_t start = 0; start + (size_t) n <= x.size(); start += hop)
    {
        double energy = 0;
        for (int i = 0; i < n; ++i)
        {
            frame[(size_t) i] = x[start + (size_t) i] * win[(size_t) i];
            energy += (double) frame[(size_t) i] * frame[(size_t) i];
        }
        if (energy / n < 1e-8) { peaks.emplace_back(); continue; } // silence
        fft.magnitudes (frame.data(), mag.data());

        const int lo = (int) (minF * n / sr), hi = std::min ((int) (maxF * n / sr), n / 2 - 1);
        const float frameMax = *std::max_element (mag.begin() + lo, mag.begin() + hi);
        auto& fp = peaks.emplace_back();
        for (int k = lo + 1; k < hi; ++k)
        {
            const float m = mag[(size_t) k];
            if (m < frameMax * 0.01f || m <= mag[(size_t) k - 1] || m < mag[(size_t) k + 1]) continue;
            // Parabolic interpolation on log magnitude.
            const float a = std::log (mag[(size_t) k - 1] + 1e-9f), b = std::log (m + 1e-9f), c = std::log (mag[(size_t) k + 1] + 1e-9f);
            const float d = 0.5f * (a - c) / (a - 2 * b + c);
            const float f = (k + d) * sr / (float) n;
            const float pm = std::exp (b - 0.25f * (a - c) * d);
            fp.push_back ({ f, pm });
            // Tuning: circular mean of each peak's deviation from the 12-TET grid.
            const double semis = 12.0 * std::log2 (f / 440.0);
            const double angle = 2 * std::numbers::pi * (semis - std::round (semis));
            tuneRe += pm * std::cos (angle);
            tuneIm += pm * std::sin (angle);
        }
    }

    const float tuning = (float) (std::atan2 (tuneIm, tuneRe) / (2 * std::numbers::pi)); // semitones, -0.5..0.5
    const double ref = 440.0 * std::pow (2.0, tuning / 12.0);

    // Harmonic pitch-class profile: each peak votes for itself and for the fundamentals it
    // could be a harmonic of (weight 0.6^(h-1)), spread over +-2/3 semitone with a cos^2 window.
    std::array<double, 12> chroma {};
    for (auto& fp : peaks)
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
                    c[(size_t) (((q % 12) + 12) % 12)] += w * cw * cw;
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
TempoResult detectTempo (const std::vector<float>& x, int sr)
{
    constexpr int n = 2048, hop = 512, bands = 40;
    const double frameRate = (double) sr / hop;
    RealFft fft (n);
    const auto win = hann (n);
    std::vector<float> frame ((size_t) n), mag ((size_t) n / 2 + 1);

    // Log-spaced bands from 30 Hz to 16 kHz.
    std::vector<int> edges (bands + 1);
    for (int b = 0; b <= bands; ++b)
        edges[(size_t) b] = std::clamp ((int) std::round (30.0 * std::pow (16000.0 / 30.0, (double) b / bands) * n / sr), 1, n / 2);

    // Onset strength: half-wave rectified flux of log band energies.
    std::vector<float> onset, prev ((size_t) bands, 0.0f);
    for (std::size_t start = 0; start + (size_t) n <= x.size(); start += hop)
    {
        for (int i = 0; i < n; ++i) frame[(size_t) i] = x[start + (size_t) i] * win[(size_t) i];
        fft.magnitudes (frame.data(), mag.data());
        float flux = 0;
        for (int b = 0; b < bands; ++b)
        {
            double e = 0;
            for (int k = edges[(size_t) b]; k < std::max (edges[(size_t) b] + 1, edges[(size_t) b + 1]); ++k) e += (double) mag[(size_t) k] * mag[(size_t) k];
            const float l = (float) std::log1p (1000.0 * e);
            flux += std::max (0.0f, l - prev[(size_t) b]);
            prev[(size_t) b] = l;
        }
        onset.push_back (flux);
    }
    if (onset.size() < 64) return {};  // too short to measure

    // Remove the slowly varying part (local mean over ~1 s), keep positive deviations.
    const int m = (int) frameRate;
    std::vector<double> cum (onset.size() + 1, 0.0);
    for (std::size_t i = 0; i < onset.size(); ++i) cum[i + 1] = cum[i] + onset[i];
    std::vector<float> env (onset.size());
    for (std::size_t i = 0; i < onset.size(); ++i)
    {
        const std::size_t a = i >= (size_t) m ? i - (size_t) m : 0, b = std::min (onset.size(), i + (size_t) m + 1);
        env[i] = std::max (0.0f, onset[i] - (float) ((cum[b] - cum[a]) / (double) (b - a)));
    }

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

    float peak = 0;
    std::vector<double> sq[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto& x = ch == 0 ? left : right;
        sq[ch].resize (n);
        double s1[2] {}, s2[2] {};
        for (std::size_t i = 0; i < n; ++i)
        {
            peak = std::max (peak, std::fabs (x[i]));
            double v = x[i];
            for (int st = 0; st < 2; ++st) // transposed direct form II
            {
                const auto& k = coef[(size_t) st];
                const double y = k[0] * v + s1[st];
                s1[st] = k[1] * v - k[3] * y + s2[st];
                s2[st] = k[2] * v - k[4] * y;
                v = y;
            }
            sq[ch][i] = v * v;
        }
    }
    r.samplePeakDb = (float) (20.0 * std::log10 (std::max (peak, 1e-10f)));

    // 400 ms blocks with 75% overlap.
    const std::size_t block = (std::size_t) (0.4 * sr), step = block / 4;
    if (n < block) return r;
    std::vector<double> cum[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        cum[ch].assign (n + 1, 0.0);
        for (std::size_t i = 0; i < n; ++i) cum[ch][i + 1] = cum[ch][i] + sq[ch][i];
    }
    std::vector<double> z;
    for (std::size_t s = 0; s + block <= n; s += step)
        z.push_back ((cum[0][s + block] - cum[0][s] + cum[1][s + block] - cum[1][s]) / (double) block);

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
SongAnalysis analyzeSong (const StereoAudio& mix, const StemMap& stems)
{
    // Key from everything except drums; tempo from the drums; loudness from the original mix.
    // Without stems, all three use the mix.
    std::vector<float> harmonic (mix.size(), 0.0f), drums (mix.size(), 0.0f);
    auto addMono = [] (std::vector<float>& dst, const StereoAudio& src) {
        for (std::size_t i = 0; i < std::min (dst.size(), src.size()); ++i) dst[i] += 0.5f * (src.left[i] + src.right[i]);
    };
    if (stems.empty())
    {
        addMono (harmonic, mix);
        drums = harmonic;
    }
    else
        for (const auto& [stem, audio] : stems)
            addMono (stem == Stem::Drums ? drums : harmonic, audio);

    return { detectKey (harmonic, sampleRate), detectTempo (drums, sampleRate),
             measureLoudness (mix.left, mix.right, sampleRate) };
}
} // namespace covertools
