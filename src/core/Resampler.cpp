#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

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
} // namespace

std::vector<float> resample (const std::vector<float>& in, int inRate, int outRate)
{
    if (inRate == outRate || in.empty()) return in;

    // Output k reads input position k * M / L, with L / M = outRate / inRate in lowest terms.
    const int g = std::gcd (inRate, outRate);
    const long long L = outRate / g, M = inRate / g;

    // Low-pass at 95% of the lower Nyquist frequency, in cycles per input sample.
    const double scale = std::min (1.0, (double) outRate / inRate);
    const double cutoff = 0.5 * scale * 0.95;
    const int halfWidth = (int) std::ceil (32.0 / scale); // taps on each side, in input samples
    const int taps = 2 * halfWidth;
    const double beta = 8.6; // Kaiser window: ~90 dB stopband
    const double i0Beta = besselI0 (beta);

    // One filter per fractional position p / L (L phases), normalised to unity gain.
    // Rates with a huge L (unusual rates like 44056 Hz) share a quantised set of 4096 phases.
    const long long phases = std::min<long long> (L, 4096);
    std::vector<float> table ((std::size_t) (phases * taps));
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
            const float* x = in.data() + first;
            for (int j = 0; j < taps; ++j) acc += x[j] * h[j];
        }
        else // near the edges: samples outside the signal count as silence
            for (int j = 0; j < taps; ++j)
                if (const long long i = first + j; i >= 0 && i < n) acc += in[(std::size_t) i] * h[j];
        out[(std::size_t) k] = acc;
    }
    return out;
}
} // namespace juststem
