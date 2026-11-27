#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace juststem
{
namespace
{
double besselI0 (double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k)
    {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

struct PolyphaseFilter
{
    long long L, M, phases;
    int halfWidth, taps;
    std::vector<float> table;

    PolyphaseFilter (int inRate, int outRate)
    {
        // Output k reads input position k * M / L, with L / M = outRate / inRate in lowest terms.
        const int g = std::gcd (inRate, outRate);
        L = outRate / g;
        M = inRate / g;

        // Low-pass at 95% of the lower Nyquist frequency, in cycles per input sample.
        const double scale = std::min (1.0, (double) outRate / inRate);
        const double cutoff = 0.5 * scale * 0.95;
        halfWidth = (int) std::ceil (32.0 / scale); // taps on each side, in input samples
        taps = 2 * halfWidth;
        const double beta = 8.6; // Kaiser window: ~90 dB stopband
        const double i0Beta = besselI0 (beta);

        // One filter per fractional position p / L (L phases), normalised to unity gain.
        // Rates with a huge L (unusual rates like 44056 Hz) share a quantised set of 4096 phases.
        phases = std::min<long long> (L, 4096);
        table.resize ((std::size_t) (phases * taps));
        for (long long p = 0; p < phases; ++p)
        {
            const double frac = (double) p / (double) phases;
            double sum = 0;
            for (int j = 0; j < taps; ++j)
            {
                const double t = (j - halfWidth + 1) - frac; // distance from the output position
                const double x = 2 * std::numbers::pi * cutoff * t;
                const double sinc = t == 0 ? 1.0 : std::sin (x) / x;
                const double w = t / halfWidth;
                const double kaiser = std::fabs (w) >= 1 ? 0.0 : besselI0 (beta * std::sqrt (1 - w * w)) / i0Beta;
                table[(std::size_t) (p * taps + j)] = (float) (2 * cutoff * sinc * kaiser);
                sum += 2 * cutoff * sinc * kaiser;
            }
            for (int j = 0; j < taps; ++j) table[(std::size_t) (p * taps + j)] /= (float) sum;
        }
    }

    std::vector<float> apply (const std::vector<float>& in) const
    {
        const long long n = (long long) in.size();
        const long long outLen = n * L / M;
        std::vector<float> out ((std::size_t) outLen);
        for (long long k = 0; k < outLen; ++k)
        {
            const long long num = k * M;
            const long long i0 = num / L;
            const long long phase = (num % L) * phases / L;
            const float* h = table.data() + phase * taps;
            const long long first = i0 - halfWidth + 1;
            float acc = 0;
            if (first >= 0 && first + taps <= n)
            {
                // Four independent accumulators break the FP dependency chain so NEON/SSE vectorizes without -ffast-math.
                const float* x = in.data() + first;
                float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                int j = 0;
                for (; j + 4 <= taps; j += 4)
                {
                    a0 += x[j] * h[j];
                    a1 += x[j + 1] * h[j + 1];
                    a2 += x[j + 2] * h[j + 2];
                    a3 += x[j + 3] * h[j + 3];
                }
                acc = (a0 + a1) + (a2 + a3);
                for (; j < taps; ++j) acc += x[j] * h[j];
            }
            else // near the edges: samples outside the signal count as silence
                for (int j = 0; j < taps; ++j)
                    if (const long long i = first + j; i >= 0 && i < n) acc += in[(std::size_t) i] * h[j];
            out[(std::size_t) k] = acc;
        }
        return out;
    }
};
} // namespace

std::vector<float> resample (const std::vector<float>& in, int inRate, int outRate)
{
    if (inRate <= 0 || outRate <= 0) throw std::invalid_argument ("Invalid sample rate for resampling");
    if (inRate == outRate || in.empty()) return in;
    return PolyphaseFilter (inRate, outRate).apply (in);
}

void resampleStereo (std::vector<float>& left, std::vector<float>& right, int inRate, int outRate)
{
    if (inRate <= 0 || outRate <= 0) throw std::invalid_argument ("Invalid sample rate for resampling");
    if (inRate == outRate || left.empty()) return;
    const PolyphaseFilter filter (inRate, outRate);
    if (left == right)
    {
        left = filter.apply (left);
        right = left;
        return;
    }
    auto r = std::async (std::launch::async, [&] { return filter.apply (right); });
    left = filter.apply (left);
    right = r.get();
}
} // namespace juststem
