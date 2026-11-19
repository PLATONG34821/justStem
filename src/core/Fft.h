#pragma once

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace juststem
{
/** Small radix-2 real FFT (power-of-two sizes) for the analysis code. */
class RealFft
{
public:
    explicit RealFft (int n) : n (n), tw ((size_t) n / 2), rev ((size_t) n)
    {
        for (int k = 0; k < n / 2; ++k)
            tw[(size_t) k] = std::polar (1.0f, (float) (-2.0 * std::numbers::pi * k / n));
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        buf.resize ((size_t) n);
    }

    /** Magnitudes of bins 0..n/2 of the real input x[0..n-1]. */
    void magnitudes (const float* x, float* mag)
    {
        for (int i = 0; i < n; ++i) buf[(size_t) rev[(size_t) i]] = { x[i], 0.0f };
        for (int size = 2; size <= n; size <<= 1)
        {
            const int half = size / 2, step = n / size;
            for (int i = 0; i < n; i += size)
                for (int k = 0; k < half; ++k)
                {
                    const auto t = tw[(size_t) (k * step)] * buf[(size_t) (i + k + half)];
                    buf[(size_t) (i + k + half)] = buf[(size_t) (i + k)] - t;
                    buf[(size_t) (i + k)] += t;
                }
        }
        for (int k = 0; k <= n / 2; ++k) mag[k] = std::abs (buf[(size_t) k]);
    }

    int size() const { return n; }

private:
    int n;
    std::vector<std::complex<float>> tw, buf;
    std::vector<int> rev;
};
} // namespace juststem
