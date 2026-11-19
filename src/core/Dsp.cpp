#include "Dsp.h"
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace juststem::dsp
{
std::vector<float> hann (int n)
{
    std::vector<float> w ((size_t) n);
    for (int i = 0; i < n; ++i) w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2 * std::numbers::pi * i / n));
    return w;
}

PeakFrames spectralPeaks (const std::vector<float>& x, int sr, int n, int hop, float minF, float maxF)
{
    PeakFrames out;
    out.size = n;
    out.hop = hop;
    RealFft fft (n);
    const auto win = hann (n);
    std::vector<float> frame ((size_t) n), mag ((size_t) n / 2 + 1);
    double tuneRe = 0, tuneIm = 0;

    for (std::size_t start = 0; start + (size_t) n <= x.size(); start += (size_t) hop)
    {
        auto& fp = out.frames.emplace_back();
        double energy = 0;
        for (int i = 0; i < n; ++i)
        {
            frame[(size_t) i] = x[start + (size_t) i] * win[(size_t) i];
            energy += (double) frame[(size_t) i] * frame[(size_t) i];
        }
        if (energy / n < 1e-8) continue; // silence
        fft.magnitudes (frame.data(), mag.data());

        const int lo = (int) (minF * n / sr), hi = std::min ((int) (maxF * n / sr), n / 2 - 1);
        const float frameMax = *std::max_element (mag.begin() + lo, mag.begin() + hi);
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
    out.tuning = (float) (std::atan2 (tuneIm, tuneRe) / (2 * std::numbers::pi));
    return out;
}

std::vector<float> onsetEnvelope (const std::vector<float>& x, int sr)
{
    constexpr int n = onsetSize, hop = onsetHop, bands = 40;
    RealFft fft (n);
    const auto win = hann (n);
    std::vector<float> frame ((size_t) n), mag ((size_t) n / 2 + 1);

    // Log-spaced bands from 30 Hz to 16 kHz.
    std::vector<int> edges (bands + 1);
    for (int b = 0; b <= bands; ++b)
        edges[(size_t) b] = std::clamp ((int) std::round (30.0 * std::pow (16000.0 / 30.0, (double) b / bands) * n / sr), 1, n / 2);

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

    // Remove the slowly varying part (local mean over ~1 s), keep positive deviations.
    const int m = sr / hop;
    std::vector<double> cum (onset.size() + 1, 0.0);
    for (std::size_t i = 0; i < onset.size(); ++i) cum[i + 1] = cum[i] + onset[i];
    std::vector<float> env (onset.size());
    for (std::size_t i = 0; i < onset.size(); ++i)
    {
        const std::size_t a = i >= (size_t) m ? i - (size_t) m : 0, b = std::min (onset.size(), i + (size_t) m + 1);
        env[i] = std::max (0.0f, onset[i] - (float) ((cum[b] - cum[a]) / (double) (b - a)));
    }
    return env;
}
} // namespace juststem::dsp
