// Минимальный radix-2 FFT: нужен офлайн (построение таблиц из спектра) и в тестах
// (измерение алиасинга). В реальном времени не используется.
#pragma once

#include "dsp.h"

#include <complex>
#include <vector>
#include <cmath>

namespace orogen {

class Fft
{
public:
    // n должно быть степенью двойки
    static void forward (std::vector<std::complex<float>>& a)
    {
        const int n = (int) a.size();
        bitReverse (a);
        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * kPi / len;
            const std::complex<float> wlen ((float) std::cos (ang), (float) std::sin (ang));
            for (int i = 0; i < n; i += len)
            {
                std::complex<float> w (1.0f, 0.0f);
                for (int j = 0; j < len / 2; ++j)
                {
                    const auto u = a[i + j];
                    const auto v = a[i + j + len / 2] * w;
                    a[i + j]             = u + v;
                    a[i + j + len / 2]   = u - v;
                    w *= wlen;
                }
            }
        }
    }

    static void inverse (std::vector<std::complex<float>>& a)
    {
        for (auto& x : a) x = std::conj (x);
        forward (a);
        const float s = 1.0f / (float) a.size();
        for (auto& x : a) x = std::conj (x) * s;
    }

private:
    static void bitReverse (std::vector<std::complex<float>>& a)
    {
        const int n = (int) a.size();
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (a[i], a[j]);
        }
    }
};

} // namespace orogen
