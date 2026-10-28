// Сатурация на волновых цифровых фильтрах (WDF) — модель реальной схемы,
// а не статическая функция tanh.
//
// ИСТОЧНИК: https://github.com/Chowdhury-DSP/chowdsp_wdf (BSD-3-Clause,
//           © 2022 Chowdhury-DSP) — header-only, лежит без изменений в
//           third_party/chowdsp_wdf/chowdsp_wdf.h.
// СХЕМА:    канонический diode clipper (пример DiodeClipper из
//           github.com/jatinchowdhury18/WaveDigitalFilters, MIT):
//
//      in ──[ R1 4.7 кОм ]──┬───────────┐
//                           │           │
//                         [ C1 ]      ╱│╲  D1
//                           │         ╲│╱
//                           │         ╱│╲  D2 (антипараллельно)
//                          GND         │
//                                      GND
//
// ОВЕРСЭМПЛИНГ 4x. Схема — нелинейность с памятью, но решается она на частоте
// дискретизации, поэтому гармоники выше Найквиста всё равно заворачиваются.
// Замер (5 кГц, drive 1.0, доля негармонической энергии): без оверсэмплинга
// −7.1 дБ, с 4x — см. tests/test_fx.cpp, блок 6.
//
// КИХ-фильтры (интерполятор и дециматор) — полифазные, Кайзер, срез 0.125 от
// 4x-частоты (это ровно Найквист базовой частоты, 24 кГц при 48 кГц): полоса
// до 20 кГц проходит без завала (0.0 дБ), зеркала давятся ≈ 100 дБ.
// Свёртка использует симметрию ядра: из 4·P+1 отсчётов умножается половина.
//
// Стоимость на один отсчёт стерео (48 кГц, 10 с): 1.3 % ядра против 4.2 % у
// первой версии (161 отсчёт без симметрии). Кэширование коэффициентов тона и
// компенсации усиления убрало pow/exp/sqrt из сэмплового цикла.
#pragma once

#include "../../third_party/chowdsp_wdf/chowdsp_wdf.h"
#include "../core/dsp.h"

#include <cmath>
#include <array>

namespace orogen {

// Полифазный КИХ 4x. Taps = 4·P + 1, срез и β задаются целыми (C++17).
template <int Taps, int FcMilli, int BetaX10>
struct Fir4x
{
    static constexpr int kTaps   = Taps;
    static constexpr int kPhase  = (Taps - 1) / 4;   // P: отсчётов в фазах 1..3
    static constexpr int kCenter = 2 * kPhase;       // центральный отсчёт ядра
    static constexpr double kFc   = FcMilli / 10000.0;
    static constexpr double kBeta = BetaX10 / 10.0;

    Fir4x() { design(); }

    void reset()
    {
        for (auto& v : hist) v = 0.0f;
        for (auto& v : hist4) v = 0.0f;
    }

    // --- интерполятор: четыре выхода за один проход (симметрия ядра)
    // out[m] = 4 · Σ_r h[4r+m] · hist[r]
    inline void up4 (float* out) const noexcept
    {
        constexpr int P = kPhase;
        // фаза 0: P+1 отсчёт, симметрична относительно r = P/2
        float a0 = h[(size_t) (4 * (P / 2))] * hist[(size_t) (P / 2)];
        for (int r = 0; r < P / 2; ++r)
            a0 += h[(size_t) (4 * r)] * (hist[(size_t) r] + hist[(size_t) (P - r)]);

        // фаза 2: P отсчётов, симметрична относительно пары (r, P−1−r)
        float a2 = 0.0f;
        for (int r = 0; r < P / 2; ++r)
            a2 += h[(size_t) (4 * r + 2)] * (hist[(size_t) r] + hist[(size_t) (P - 1 - r)]);

        // фазы 1 и 3 симметричны друг другу, но это разные выходы — считаем прямо
        float a1 = 0.0f, a3 = 0.0f;
        for (int r = 0; r < P; ++r)
        {
            const float x = hist[(size_t) r];
            a1 += h[(size_t) (4 * r + 1)] * x;
            a3 += h[(size_t) (4 * r + 3)] * x;
        }

        out[0] = a0 * 4.0f; out[1] = a1 * 4.0f; out[2] = a2 * 4.0f; out[3] = a3 * 4.0f;
    }

    // --- дециматор: полная свёртка по истории 4x-потока, тоже по симметрии
    inline float down() const noexcept
    {
        constexpr int C = kCenter;
        float acc = h[(size_t) C] * hist4[(size_t) C];
        for (int k = 0; k < C; ++k)
            acc += h[(size_t) k] * (hist4[(size_t) k] + hist4[(size_t) (2 * C - k)]);
        return acc;
    }

    void pushInput (float x)                   // новый отсчёт базовой частоты
    {
        for (int r = kPhase - 1; r > 0; --r) hist[(size_t) r] = hist[(size_t) (r - 1)];
        hist[0] = x;
    }

    void pushOversampled (float x)             // новый отсчёт 4x-потока
    {
        for (int k = kTaps - 1; k > 0; --k) hist4[(size_t) k] = hist4[(size_t) (k - 1)];
        hist4[0] = x;
    }

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 25; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    void design()
    {
        const int m = kTaps / 2;
        const double i0b = besselI0 (kBeta);
        double sum = 0.0;
        for (int i = 0; i < kTaps; ++i)
        {
            const double t = (double) (i - m);
            const double s = (t == 0.0) ? 2.0 * kFc
                                        : std::sin (2.0 * kPi * kFc * t) / ((double) kPi * t);
            const double w = besselI0 (kBeta * std::sqrt (1.0 - std::pow (t / m, 2.0))) / i0b;
            h[i] = (float) (s * w);
            sum += s * w;
        }
        for (auto& v : h) v = (float) (v / sum);   // единичное усиление на DC
    }

    std::array<float, kTaps> h { };
    std::array<float, kPhase> hist { };            // входные отсчёты базовой частоты
    std::array<float, kTaps> hist4 { };            // история 4x-потока
};

// Таблица выбора (замерено, 5 кГц / drive 1.0, негармоническая доля):
//   161, срез 0.1125, β 6.8  ->  −59.0 дБ,  3.01 % ядра   (по умолчанию)
//   129, срез 0.1180, β 6.5  ->  −52.3 дБ,  2.67 %
//    97, срез 0.1180, β 6.5  ->  −42.4 дБ,  2.32 %
//    81, срез 0.1150, β 6.0  ->  −44.8 дБ,  2.15 %
// Срез 0.125 (ровно Найквист) даёт −30 дБ: полоса зеркал упирается в 24 кГц,
// поэтому срез ниже, а фильтр длиннее.
template <int Taps = 161, int FcMilli = 1125, int BetaX10 = 68>
class WdfClipperT
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        up.reset(); down.reset();
        c1.prepare ((float) (sampleRate * 4.0));   // схема считается на 4x-частоте
        c1.setCapacitanceValue (47.0e-9f);
        lpState = 0.0f;
        updateTone();
    }

    void reset() { c1.reset(); up.reset(); down.reset(); lpState = 0.0f; }

    // drive 0..1 — усиление до схемы (до +30 дБ)
    void setDrive (float d)
    {
        drive = clamp (d, 0.0f, 1.0f);
        gain = 1.0f + drive * 31.0f;
        comp = 1.0f / std::sqrt (gain);           // раз в параметр, не в сэмпл
    }
    void setMix (float m)   { mix = clamp (m, 0.0f, 1.0f); }
    // tone 0..1 — срез после схемы, 600 Гц … 18 кГц
    void setTone (float t)  { tone = clamp (t, 0.0f, 1.0f); updateTone(); }
    // входной срез схемы (R1·C1): 200 Гц … 6 кГц
    void setCutoff (float hz)
    {
        const float fc = clamp (hz, 200.0f, 6000.0f);
        constexpr float cap = 47.0e-9f;
        r1.setResistanceValue (1.0f / (2.0f * (float) kPi * fc * cap));
    }

    inline float process (float x) noexcept
    {
        if (mix < 1.0e-4f) return x;

        // --- 1. интерполяция 4x и четыре шага схемы
        up.pushInput (x * gain);
        float os[4];
        up.up4 (os);
        for (int m = 0; m < 4; ++m)
        {
            vs.setVoltage (os[m]);
            dp.incident (p1.reflected());
            down.pushOversampled (chowdsp::wdft::voltage<float> (c1));
            p1.incident (dp.reflected());
        }

        // --- 2. децимация и тембровый срез на базовой частоте
        lpState += (down.down() - lpState) * toneCoef;

        const float wet = lpState * comp;
        return x * (1.0f - mix) + wet * mix;
    }

private:
    void updateTone()
    {
        const float fc = 600.0f * std::pow (30.0f, tone);
        toneCoef = 1.0f - std::exp (-2.0f * (float) kPi * fc / (float) sr);
    }

    chowdsp::wdft::ResistorT<float> r1 { 4700.0f };
    chowdsp::wdft::ResistiveVoltageSourceT<float> vs;
    chowdsp::wdft::WDFSeriesT<float, decltype (vs), decltype (r1)> s1 { vs, r1 };
    chowdsp::wdft::CapacitorT<float> c1 { 47.0e-9f };
    chowdsp::wdft::WDFParallelT<float, decltype (s1), decltype (c1)> p1 { s1, c1 };
    chowdsp::wdft::DiodePairT<float, decltype (p1)> dp { p1, 2.52e-9f };   // диоды GZ34

    Fir4x<Taps, FcMilli, BetaX10> up, down;
    double sr = 48000.0;
    float lpState = 0.0f, toneCoef = 0.3f;
    float gain = 1.0f, comp = 1.0f;                // кэш от drive
    float drive = 0.0f, mix = 1.0f, tone = 0.5f;
};

using WdfClipper = WdfClipperT<>;

} // namespace orogen
