// Полуполосный фильтр-дециматор 2x -> 1x для нелинейностей (warp, fold).
//
// История калибровки (всё измерено tests/test_wavetable.cpp):
//   23 отсчёта, Хэмминг  -> -33 дБ на 30 кГц: нелинейности давали -12 дБ алиасинга
//   63 отсчёта,  Кайзер  -> -85 дБ на 30 кГц, но переходная полоса ±4 кГц
//   127 отсчётов, Кайзер -> -85 дБ, переходная ±2 кГц
//   255 отсчётов, Кайзер -> переходная ±0.9 кГц: нужна ТОЛЬКО последней ступени.
// Последняя ступень режется не на 0.25, а на 0.22 от 2x-Найквиста: полоса
// 21.6-24 кГц приносится в жертву (там уже нет музыкального содержания),
// зато гармоники складки не успевают завернуться в слышимую область.
// Ширина перехода по Кайзеру: Δf ≈ (A − 8) / (2.285 · N).
//
// Стоимость. Симметрия КИХ используется явно (умножаются только kHalf пар),
// а для проектов с срезом ровно 0.25 (FcK = 250) половина коэффициентов
// тождественно нулевая: sin(πn/2) = 0 при чётном n. Такие пары пропускаются
// целиком (см. if constexpr ниже) — ещё вдвое меньше умножений при том же
// бит-в-бит фильтре. Буфер двойной записи (продублирован на kSize) убирает
// маску из сэмплового цикла и даёт непрерывное чтение окна —
// компилятор векторизует его сам.
#pragma once

#include "../core/dsp.h"
#include <array>

namespace orogen {

constexpr int pow2ceil (int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

template <int Taps, int FcK = 250>     // FcK: частота среза в тысячных долях от 2x-Nyquist
class HalfbandDecimator2x
{
public:
    static constexpr int kTaps = Taps;
    static constexpr int kHalf = kTaps / 2;
    // размер кольцевого буфера — степень двойки (для маски), >= Taps
    static constexpr int kSize = pow2ceil (Taps);

    HalfbandDecimator2x() { design(); }

    void reset() noexcept
    {
        for (auto& v : buf) v = 0.0f;
        pos = 0;
    }

    inline void push (float x) noexcept
    {
        // двойная запись: окно из kTaps отсчётов, начинающееся в pos, всегда
        // лежит непрерывно в buf[pos .. pos + kTaps - 1]
        buf[(size_t) pos] = x;
        buf[(size_t) (pos + kSize)] = x;
        pos = (pos + 1) & (kSize - 1);
    }

    // один выходной сэмпл на каждые два входных
    inline float decimate() const noexcept
    {
        const float* b = buf.data() + (size_t) pos;    // b[0] — самый старый
        float acc = hCenter * b[(size_t) kHalf];

        if constexpr (FcK == 250)
        {
            // срез ровно на половине полосы: все пары с нечётным k — нули
            for (int k = 0; k < kHalf; k += 2)
                acc += hSym[(size_t) k] * (b[(size_t) k] + b[(size_t) (kTaps - 1 - k)]);
        }
        else
        {
            for (int k = 0; k < kHalf; ++k)
                acc += hSym[(size_t) k] * (b[(size_t) k] + b[(size_t) (kTaps - 1 - k)]);
        }
        return acc;
    }

private:
    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 32; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum  += term;
            if (term < 1e-16 * sum) break;
        }
        return sum;
    }

    void design()
    {
        constexpr double fc    = FcK / 1000.0;   // 0.25 цикла/сэмпл на 2x-частоте
        constexpr double beta  = 8.0;
        constexpr int    M     = kTaps - 1;
        double c[kTaps];
        double sum = 0.0;

        for (int i = 0; i < kTaps; ++i)
        {
            const int    n    = i - M / 2;
            const double sinc = (n == 0) ? 2.0 * fc
                                         : std::sin (2.0 * kPi * fc * n) / (kPi * n);
            const double r    = 2.0 * i / M - 1.0;                     // -1..1
            const double w    = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
            c[i] = sinc * w;
            sum += c[i];
        }
        const double g = 1.0 / sum;
        for (int i = 0; i < kTaps; ++i) c[i] *= g;

        hCenter = (float) c[kHalf];
        for (int k = 0; k < kHalf; ++k) hSym[k] = (float) c[k];
    }

    float hCenter = 1.0f;
    std::array<float, kHalf> hSym {};
    std::array<float, 2 * kSize> buf {};   // двойная запись: непрерывное окно
    int pos = 0;
};

} // namespace orogen
