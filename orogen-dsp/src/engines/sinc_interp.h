// Полифазный интерполятор (windowed-sinc) — общий для движков Sample и Granular.
#pragma once

#include "../core/dsp.h"
#include <array>

namespace orogen {

// ---------------------------------------------------------------------------
// Полифазный интерполятор (windowed sinc), а не Catmull-Rom.
//
// Почему: при питче +3 октавы шаг чтения равен 8 отсчётам, то есть выход
// формально «выбирает» из уровня с частотой в 8 раз ниже. Кубика даёт при
// этом образы около -27 дБ, которые заворачиваются в слышимую полосу:
// измерено -27.3 дБ алиасинга на ноте 81 и -9.5 дБ на ноте 93, хотя
// MIP-уровни чистые (-91 дБ на 1x/2x/4x).
// 32-отводный sinc с окном Кайзера (beta=8) и 256 фазами убирает образы
// ниже -80 дБ; линейная интерполяция между соседними фазами добавляет
// ещё ~-90 дБ. Это стандартный подход в сэмплерах (Kontakt, Falcon).
// ---------------------------------------------------------------------------
class SincKernel
{
public:
    static constexpr int kTaps   = 32;    // 16 слева + 16 справа от точки чтения
    static constexpr int kHalf   = kTaps / 2;
    static constexpr int kPhases = 256;

    SincKernel()
    {
        constexpr double beta = 8.0;
        const double i0b = besselI0 (beta);
        for (int ph = 0; ph <= kPhases; ++ph)
        {
            const double f = (double) ph / (double) kPhases;
            double h[kTaps];
            double sum = 0.0;
            for (int i = 0; i < kTaps; ++i)
            {
                const int    n = i - kHalf + 1;               // -15 .. +16
                const double x = (double) n - f;              // расстояние до точки
                const double sc = (std::fabs (x) < 1.0e-9) ? 1.0
                                                            : std::sin (kPi * x) / (kPi * x);
                const double r = x / (double) kHalf;
                const double w = (std::fabs (r) >= 1.0)
                                   ? 0.0
                                   : besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
                h[i] = sc * w;
                sum += h[i];
            }
            const double g = (std::fabs (sum) > 1.0e-12) ? 1.0 / sum : 1.0;
            for (int i = 0; i < kTaps; ++i)
                tab[(size_t) ph * kTaps + (size_t) i] = (float) (h[i] * g);
        }
    }

    // d — буфер длины len, читаем в точку p (целая часть + дробная f)
    inline float apply (const float* d, int len, int i1, float f) const noexcept
    {
        const float pf  = f * (float) kPhases;
        const int   ph  = std::min (kPhases - 1, (int) pf);
        const float ff  = pf - (float) ph;
        const float* A  = &tab[(size_t) ph * kTaps];
        const float* B  = &tab[(size_t) (ph + 1) * kTaps];

        int idx = i1 - kHalf + 1;
        if (idx < 0) idx += len;
        float acc = 0.0f;
        for (int i = 0; i < kTaps; ++i)
        {
            acc += (A[i] + (B[i] - A[i]) * ff) * d[(size_t) idx];
            if (++idx >= len) idx = 0;
        }
        return acc;
    }

private:
    static double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum  += term;
            if (term < 1.0e-16 * sum) break;
        }
        return sum;
    }

    std::array<float, (size_t) (kPhases + 1) * kTaps> tab {};
};

inline const SincKernel& sincKernel()
{
    static const SincKernel k {};
    return k;
}

} // namespace orogen
