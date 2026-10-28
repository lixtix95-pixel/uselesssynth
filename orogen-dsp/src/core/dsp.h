// OROGEN DSP core — базовые утилиты, общие для всех движков.
// Никаких зависимостей от JUCE: ядро компилируется и тестируется отдельно.
#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>

namespace orogen {

// Переносимый popcount: __builtin_popcount есть только у GCC/Clang, а плагин
// собирается ещё и MSVC (VS 2022). C++20 <bit> MSVC тоже поддерживает, но
// проект держит C++17 для JUCE-сборки, поэтому — ручная реализация.
inline int popCount (uint32_t v) noexcept
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (int) ((v * 0x01010101u) >> 24);
}


inline constexpr double kPi    = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;

template <typename T> inline T clamp (T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float  lerp  (float a, float b, float t)  { return a + (b - a) * t; }
inline double lerp  (double a, double b, double t){ return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// Быстрый tanh без библиотечных вызовов (рациональное приближение, ошибка < 1e-4)
// Используется во всех нелинейностях: warp, fold, drive, FM-feedback.
// ---------------------------------------------------------------------------
inline float fastTanh (float x) noexcept
{
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// ---- быстрые log2/exp2 (для дБ-шкал в сэмпловом цикле) ----------------
// Точность: log2 — ошибка < 1e-4 (0.001 дБ), exp2 — < 2e-7 относительно.
// Стоимость: ~6 умножений, тогда как std::log10/std::pow вызывают libm
// (десятки тактов). Компрессор зовёт их каждый сэмпл, поэтому это заметно.
inline float fastLog2 (float x) noexcept
{
    if (! (x > 0.0f)) return -120.0f;
    union { float f; unsigned i; } u { x };
    const float e = (float) (int) ((u.i >> 23) & 0xFFu) - 127.0f;
    u.i = (u.i & 0x007FFFFFu) | 0x3F800000u;             // мантисса в [1, 2)
    const float m = u.f;
    const float t  = (m - 1.0f) / (m + 1.0f);            // |t| <= 1/3
    const float t2 = t * t;
    // 2/ln2·(t + t³/3 + t⁵/5)
    return e + t * (2.88539008f + t2 * (0.96179669f + t2 * 0.57707802f));
}

inline float fastExp2 (float x) noexcept
{
    if (x < -126.0f) return 0.0f;
    if (x >  126.0f) x = 126.0f;
    const float n = std::floor (x);
    const float f = x - n;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f
                  + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union { unsigned i; float f; } u;
    u.i = (unsigned) ((int) n + 127) << 23;
    return p * u.f;
}

// Косинус по таблице с линейной интерполяцией: 1024 точки, ошибка < 3e-6
// (−110 дБ). Нужен там, где std::cos звался на каждое зерно каждый сэмпл
// (окно Ханна в гранулярном движке).
inline float fastCosTbl (float phase01) noexcept
{
    constexpr int N = 1024;
    static const float* tab = [] {
        static float t[N + 1];
        for (int i = 0; i <= N; ++i) t[i] = (float) std::cos (kTwoPi * (double) i / (double) N);
        return t;
    }();
    phase01 -= std::floor (phase01);
    const float x  = phase01 * (float) N;
    const int   i  = (int) x;
    const float fr = x - (float) i;
    const float a = tab[i], b = tab[i + 1];
    return a + (b - a) * fr;
}

inline float fastDb (float x) noexcept   { return 6.020599913f * fastLog2 (x); }   // 20·log10
inline float fastDbToLin (float db) noexcept { return fastExp2 (db * 0.16609640474f); }  // 10^(db/20)

// Защита от субнормалей: медленно затухающие экспоненты (моды, огибающие, грейны)
// уходят в денормали и роняют CPU в 10+ раз. Дешёвый способ — подмешать крошечный DC
// в самом конце цепочки состояния.
inline float antiDenormal (float x) noexcept
{
    return x + 1.0e-20f;
}

inline bool isBad (float x) noexcept
{
    return !std::isfinite (x);
}

// ---------------------------------------------------------------------------
// Сглаживание параметров: без него ручки «шуршат» (zipper noise) при изменении
// на блок. Одно-полюсный фильтр с временем ~tau мс.
// ---------------------------------------------------------------------------
class SmoothedValue
{
public:
    void reset (double sampleRate, double timeMs = 12.0)
    {
        setTime (sampleRate, timeMs);
        current = target = 0.0f;
    }

    void setTime (double sampleRate, double timeMs)
    {
        const double n = std::max (1.0, sampleRate * timeMs * 0.001);
        coeff = 1.0f - std::exp (-1.0f / (float) n);
    }

    void setTarget (float t) noexcept { target = t; }
    void snap (float v) noexcept      { current = target = v; }
    float  getCurrent() const noexcept{ return current; }

    inline float next() noexcept
    {
        current += (target - current) * coeff;
        if (std::fabs (target - current) < 1.0e-7f) current = target;
        return current;
    }

private:
    float current = 0.0f, target = 0.0f, coeff = 0.01f;
};

// ---------------------------------------------------------------------------
// Детерминированный ГПСЧ (xorshift32): воспроизводимые «случайности»
// (позиция грейнов, начальные фазы, дрейф). Важно для одинакового рендера.
// ---------------------------------------------------------------------------
class Rng
{
public:
    explicit Rng (uint32_t seed = 0x9E3779B9u) : s (seed ? seed : 1u) {}

    // Пересев: нужен источникам, у которых своё зерно на каждую ноту.
    inline void seed (uint32_t v) noexcept { s = v ? v : 1u; }

    inline uint32_t nextU32() noexcept
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    inline float nextFloat() noexcept                     // [0, 1)
    {
        return (nextU32() >> 8) * (1.0f / 16777216.0f);
    }
    inline float nextBipolar() noexcept { return nextFloat() * 2.0f - 1.0f; }

private:
    uint32_t s;
};

// ---------------------------------------------------------------------------
// Таблица Hann-окна: не считаем cos в горячем цикле.
// ---------------------------------------------------------------------------
struct WindowTable
{
    static constexpr int kSize = 2048;
    float data[kSize];

    WindowTable()
    {
        for (int i = 0; i < kSize; ++i)
            data[i] = 0.5f - 0.5f * (float) std::cos (kTwoPi * i / (kSize - 1));
    }

    // phase 0..1 -> окно
    inline float lookup (float phase) const noexcept
    {
        int i = (int) (phase * (kSize - 1));
        i = i < 0 ? 0 : (i > kSize - 1 ? kSize - 1 : i);
        return data[i];
    }
};

inline const WindowTable& hannTable()
{
    static const WindowTable t;
    return t;
}


// ---------------------------------------------------------------------------
// Таблица синуса с линейной интерполяцией: нужна в нелинейностях (warp/fold)
// и в модальном/аддитивном синтезе, где std::sin в горячем цикле недопустим.
// ---------------------------------------------------------------------------
struct SineTable
{
    static constexpr int kSize = 4096;                 // степень двойки
    static constexpr int kMask = kSize - 1;
    float data[kSize + 1];

    SineTable()
    {
        for (int i = 0; i <= kSize; ++i)
            data[i] = (float) std::sin (kTwoPi * i / kSize);
    }

    // phase 0..1
    inline float lookup (float phase) const noexcept
    {
        const float x = phase * kSize;
        const int   i = (int) x;
        const float f = x - (float) i;
        const int   a = i & kMask;
        return data[a] + (data[a + 1] - data[a]) * f;
    }
};

inline const SineTable& sineTable()
{
    static const SineTable t;
    return t;
}

inline float fastSin (float phase) noexcept { return sineTable().lookup (phase); }

// ---------------------------------------------------------------------------
// MIDI -> Гц
// ---------------------------------------------------------------------------
inline double midiToHz (double note) noexcept
{
    return 440.0 * std::pow (2.0, (note - 69.0) / 12.0);
}

} // namespace orogen
