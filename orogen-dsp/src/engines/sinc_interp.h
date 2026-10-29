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

// Granular: фильтр для дробной скорости ВНУТРИ октавы MIP. Выбор одного
// лишь следующего MIP без алиасинга отрезал полполосы: например при rate=17.6
// исчезал тон 14.4 кГц, хотя Найквист — 24 кГц. Отношение q=rate/2^L лежит
// в (1,2]. На каждый диапазон q заранее рассчитан 64-tap low-pass sinc;
// частота среза выбирается по верхней границе диапазона с запасом перехода.
// Фильтр фиксируется на время зерна. Таблица создаётся при подготовке голоса,
// а не в первом render(), и далее только читается.
class GrainSincKernel
{
public:
    static constexpr int kTaps = 64, kHalf = kTaps / 2;
    static constexpr int kPhases = 256, kRateBins = 16;

    GrainSincKernel()
    {
        constexpr double beta = 8.0, guard = 0.035;
        const double i0b = besselI0 (beta);
        std::array<double, (size_t) (kPhases + 1) * kTaps> windows {};
        for (int ph = 0; ph <= kPhases; ++ph)
            for (int i = 0; i < kTaps; ++i)
            {
                const double x = (double) (i - kHalf + 1) - (double) ph / kPhases;
                const double r = x / kHalf;
                windows[(size_t) ph * kTaps + (size_t) i] = (std::fabs (r) >= 1.0)
                    ? 0.0 : besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            }
        for (int bin = 1; bin <= kRateBins; ++bin)
        {
            const double q = 1.0 + (double) bin / kRateBins;
            const double fc = 0.5 / q - guard; // cycles/отсчёт выбранного MIP
            for (int ph = 0; ph <= kPhases; ++ph)
            {
                const double f = (double) ph / kPhases;
                double h[kTaps], sum = 0.0;
                for (int i = 0; i < kTaps; ++i)
                {
                    const double x = (double) (i - kHalf + 1) - f;
                    const double z = 2.0 * fc * x;
                    const double sinc = std::fabs (z) < 1.0e-9 ? 1.0 : std::sin (kPi * z) / (kPi * z);
                    h[i] = 2.0 * fc * sinc * windows[(size_t) ph * kTaps + (size_t) i];
                    sum += h[i];
                }
                const double g = std::fabs (sum) > 1.0e-12 ? 1.0 / sum : 1.0;
                const size_t offset = ((size_t) (bin - 1) * (kPhases + 1) + (size_t) ph) * kTaps;
                for (int i = 0; i < kTaps; ++i) tab[offset + (size_t) i] = (float) (h[i] * g);
            }
        }
    }

    inline float apply (const float* d, int len, int i1, float f, int bin) const noexcept
    {
        const float pf = f * (float) kPhases;
        const int ph = std::min (kPhases - 1, (int) pf);
        const float ff = pf - (float) ph;
        const size_t offset = ((size_t) (bin - 1) * (kPhases + 1) + (size_t) ph) * kTaps;
        const float* A = &tab[offset];
        const float* B = A + kTaps;
        const int first = i1 - kHalf + 1;
        float acc = 0.0f;
        if (first >= 0 && first + kTaps <= len)
        {
            // Гарантируется безопасным диапазоном grain; без fmod и ветвления
            // на каждом отводе, цикл может векторизоваться компилятором.
            for (int i = 0; i < kTaps; ++i)
                acc += (A[i] + (B[i] - A[i]) * ff) * d[(size_t) (first + i)];
        }
        else
        {
            // Защитный путь на случай прямого использования извне движка.
            for (int i = 0; i < kTaps; ++i)
            {
                int index = (first + i) % len;
                if (index < 0) index += len;
                acc += (A[i] + (B[i] - A[i]) * ff) * d[(size_t) index];
            }
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
            sum += term;
            if (term < 1.0e-16 * sum) break;
        }
        return sum;
    }
    std::array<float, (size_t) kRateBins * (kPhases + 1) * kTaps> tab {};
};

inline const GrainSincKernel& grainSincKernel()
{
    static const GrainSincKernel k {};
    return k;
}

} // namespace orogen
