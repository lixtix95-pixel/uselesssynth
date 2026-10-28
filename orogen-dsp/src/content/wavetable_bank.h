// Контент вейвтейблов: спектры -> MIP-пирамиды (band-limited по построению).
//
// Ключевая идея: таблица НЕ рисуется во времени и потом фильтруется, а сразу
// синтезируется из спектра обратным FFT, где число партиалов ограничено
// половиной длины таблицы этого уровня. Поэтому каждый уровень MIP-пирамиды
// алиасинг-free по построению, а не «на глаз».
#pragma once

#include "../core/dsp.h"
#include "../core/fft.h"

#include <string>
#include <vector>
#include <array>
#include <cstdio>
#include <cstring>

namespace orogen {

// ---------------------------------------------------------------------------
// MIP-иерархия с шагом в ТРЕТЬ октавы (а не в октаву).
//
// Почему так. Уровень безопасен, если число его партиалов не превышает
// 0.5·SR/f0. При шаге в октаву между соседними безопасными уровнями
// теряется вдвое больше партиалов, чем нужно — слышимый «ступенчатый» тембр
// при глиссандо. При шаге в треть октавы максимальная потеря — 2^(1/3) ≈ 1.26×,
// то есть незаметна, и при этом НЕ НУЖЕН кроссфейд с небезопасным уровнем
// (именно он и давал алиасинг -33 дБ в первой версии).
// ---------------------------------------------------------------------------
// Плотность таблицы: длина = LEN_MULT · (2 · число партиалов).
// Запас нужен из-за ИНТЕРПОЛЯЦИИ: если верхний партиал уровня стоит ровно на
// «Найквисте таблицы», любая интерполяция (кроме идеальной sinc) даёт образы,
// которые в выходном сигнале складываются в алиасинг. При LEN_MULT = 8 и
// кубической интерполяции образы уходят ниже -80 дБ (см. tests/).
#ifndef ORG_WT_LEN_MULT
#define ORG_WT_LEN_MULT 8
#endif
inline constexpr int kWtLenMult  = ORG_WT_LEN_MULT;
inline constexpr int kWtBaseLen  = 4096;
inline constexpr int kWtMaxHarm  = 512;
inline constexpr int kWtLevels  = 25;      // 1024, 815, 648 … 4 партиала

inline int levelMaxHarm (int level)
{
    const double h = kWtMaxHarm * std::pow (2.0, -(double) level / 3.0);
    return std::max (4, (int) std::llround (h));
}

// индекс уровня, у которого maxHarm <= allowed
inline int levelForAllowed (double allowed)
{
    if (allowed >= kWtMaxHarm) return 0;
    const double xl = std::log2 ((double) kWtMaxHarm / allowed) * 3.0;   // в треть-октавах
    int l = (int) std::ceil (xl);
    return clamp (l, 0, kWtLevels - 1);
}

inline int nextPow2 (int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

enum class TableKind
{
    Bloom,      // спектр, разворачивающийся от синуса к пиле (Superformula Bloom)
    ReverseSaw, // «обратная пила» — сдвиг фаз между кадрами
    Glass,      // негармонические отношения (стекло/колокол)
    Formant,    // форманты, ползущие вверх (вокальная «а-э-и»)
    Fold,       // спектр, получающийся из волновой складки
    Vocal,      // две форманты, переезжающие как гласные
    Organ,      // регистры органа (1,2,3,4,6,8,10,12)
    Pwm         // ширина импульса
};

inline const char* tableKindName (TableKind k)
{
    switch (k)
    {
        case TableKind::Bloom:      return "Superformula Bloom";
        case TableKind::ReverseSaw: return "Reverse Saw";
        case TableKind::Glass:      return "Glass Harmonics";
        case TableKind::Formant:    return "Formant Sweep";
        case TableKind::Fold:       return "Digital Fold";
        case TableKind::Vocal:      return "Vocal Ah-Ee";
        case TableKind::Organ:      return "Drawbar Organ";
        case TableKind::Pwm:        return "PWM Wide";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Спектр кадра: амплитуды партиалов 1..maxHarm (индексы с 0 = партиал 1).
// Фазы задаются отдельно и детерминированно (см. buildTable), чтобы кадры
// морфились согласованно, а crest factor не был «игольчатым».
// ---------------------------------------------------------------------------
inline void makeSpectrum (TableKind kind, float t, float* amps, int maxHarm)
{
    std::fill (amps, amps + maxHarm, 0.0f);

    switch (kind)
    {
        case TableKind::Bloom:
        {
            const int n = (int) (3.0f + t * 90.0f);
            for (int k = 1; k <= std::min (n, maxHarm); ++k)
                amps[k - 1] = 1.0f / std::pow ((float) k, 1.15f - t * 0.35f);
            break;
        }
        case TableKind::ReverseSaw:
        {
            const int n = (int) (4.0f + t * 80.0f);
            for (int k = 1; k <= std::min (n, maxHarm); ++k)
                amps[k - 1] = 1.0f / (float) k;
            break;
        }
        case TableKind::Glass:
        {
            const float ratios[6] = { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.60f };
            for (int m = 0; m < 6; ++m)
            {
                const float w = 0.55f + 0.45f * std::sin ((t * 4.0f + m) * 0.9f);
                const int   k = (int) std::lround (ratios[m]);
                if (k >= 1 && k <= maxHarm) amps[k - 1] += w / (1.0f + m * 0.7f);
                // микрорасстройка негармонических мод -> соседние партиалы
                const int k2 = (int) std::lround (ratios[m] * 1.02f);
                if (k2 >= 1 && k2 <= maxHarm) amps[k2 - 1] += w * 0.35f / (1.0f + m * 0.7f);
            }
            break;
        }
        case TableKind::Formant:
        {
            const float f0 = 110.0f;
            const float F[3] = { 300.0f + t * 500.0f, 900.0f + t * 900.0f, 2400.0f + t * 700.0f };
            const float G[3] = { 1.0f, 0.6f, 0.3f };
            for (int k = 1; k <= maxHarm; ++k)
            {
                float w = 0.0f;
                for (int fi = 0; fi < 3; ++fi)
                {
                    const float d = ((float) k - F[fi] / f0) / (F[fi] / f0 * 0.28f);
                    w += std::exp (-d * d) * G[fi];
                }
                amps[k - 1] = w / std::pow ((float) k, 0.35f);
            }
            break;
        }
        case TableKind::Fold:
        {
            // спектр sin(x + d·sin x): считаем аналитически через функции Бесселя
            // первого рода (ряд), d = 0..6. Это ровно то, что даёт wavefolding.
            const float d = 0.2f + t * 6.0f;
            for (int n = 0; n <= 24; ++n)
            {
                const int k = 2 * n + 1;                 // только нечётные партиалы
                if (k > maxHarm) break;
                const double a = std::cyl_bessel_j (n, (double) d);
                amps[k - 1] = (float) std::fabs (a) * (n == 0 ? 1.0f : 2.0f);
            }
            break;
        }
        case TableKind::Vocal:
        {
            const float f0 = 110.0f;
            const float F1 = lerp (730.0f, 400.0f, t), F2 = lerp (1090.0f, 1900.0f, t),
                        F3 = lerp (2440.0f, 2600.0f, t);
            for (int k = 1; k <= maxHarm; ++k)
            {
                float w = 0.0f;
                const float hz = (float) k * f0;
                for (auto [fc, g] : { std::pair<float,float>{F1,1.0f}, {F2,0.6f}, {F3,0.3f} })
                {
                    const float d = (hz - fc) / (fc * 0.28f);
                    w += std::exp (-d * d) * g;
                }
                amps[k - 1] = w / std::pow ((float) k, 0.25f);
            }
            break;
        }
        case TableKind::Organ:
        {
            const int   mult[8] = { 1, 2, 3, 4, 6, 8, 10, 12 };
            const float bars[8] = { 1.0f, 0.7f - t * 0.4f, 0.5f, 0.35f + t * 0.4f,
                                    0.25f, 0.2f, 0.3f * t, 0.15f };
            for (int m = 0; m < 8; ++m)
                if (mult[m] <= maxHarm) amps[mult[m] - 1] += std::max (0.0f, bars[m]);
            break;
        }
        case TableKind::Pwm:
        {
            const float w = 0.5f - t * 0.44f;            // ширина импульса
            for (int k = 1; k <= maxHarm; ++k)
                amps[k - 1] = std::fabs (std::sin (kPi * (float) k * w)) / (float) k;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Хранение: level-major, внутри — frame-major (frame*len + i).
// При чтении нужен один уровень и два соседних кадра -> лучшая локальность.
// ---------------------------------------------------------------------------
struct Wavetable
{
    std::string name;
    int frames = 6;
    std::vector<std::vector<float>> levels;   // levels[l].size() == frames * lens[l]
    std::vector<int> lens;
    std::vector<int> harms;                   // сколько партиалов в уровне

    int levelLen (int l) const { return lens[(size_t) l]; }
};

// Строим все уровни одной таблицы.
// taperTop: доля верхних партиалов, гасимая косинусной «шторкой» (0.2 = 20 %).
// Это нужно для бесшовного кроссфейда между уровнями MIP: соседние уровни
// перекрываются, а не обрываются кирпичной стеной.
inline Wavetable buildTable (TableKind kind, int frames = 6, float taperTop = 0.22f)
{
    Wavetable wt;
    wt.name   = tableKindName (kind);
    wt.frames = frames;
    wt.levels.resize ((size_t) kWtLevels);
    wt.lens.resize   ((size_t) kWtLevels);
    wt.harms.resize  ((size_t) kWtLevels);

    // фазы партиалов: детерминированные, одинаковые для всех кадров и уровней.
    // Нужны, чтобы кадры морфились согласованно (иначе позиция «плывёт» по фазе),
    // и чтобы crest factor не был «игольчатым» (как у нулевых фаз пилы).
    std::vector<float> phase ((size_t) kWtMaxHarm, 0.0f);
    {
        Rng rng (0xC0FFEEu ^ (uint32_t) kind);
        for (auto& p : phase) p = rng.nextFloat() * 6.2831853f;
    }

    std::vector<float> amps ((size_t) kWtMaxHarm);

    for (int l = 0; l < kWtLevels; ++l)
    {
        const int maxHarm = levelMaxHarm (l);              // партиалы этого уровня
        const int len     = nextPow2 (maxHarm * kWtLenMult);   // степень двойки -> маска
        wt.lens[(size_t) l]  = len;
        wt.harms[(size_t) l] = maxHarm;
        wt.levels[(size_t) l].assign ((size_t) frames * len, 0.0f);

        for (int f = 0; f < frames; ++f)
        {
            const float t = frames > 1 ? (float) f / (float) (frames - 1) : 0.0f;
            makeSpectrum (kind, t, amps.data(), maxHarm);

            std::vector<std::complex<float>> spec ((size_t) len, { 0.0f, 0.0f });
            for (int k = 1; k <= maxHarm; ++k)
            {
                float a = amps[(size_t) (k - 1)];

                // косинусная «шторка» на верхних 22 % партиалов — смягчает
                // «кирпичную стену» на краю полосы уровня
                const float edge = (float) k / (float) maxHarm;
                if (edge > 1.0f - taperTop)
                {
                    const float x = (edge - (1.0f - taperTop)) / taperTop;
                    a *= 0.5f + 0.5f * std::cos (kPi * clamp (x, 0.0f, 1.0f));
                }

                // нечётные партиалы для Fold — по построению (см. makeSpectrum)
                const float ph = phase[(size_t) (k - 1)];
                spec[(size_t) k] = { a * std::cos (ph), a * std::sin (ph) };
            }

            Fft::inverse (spec);

            float peak = 1.0e-9f;
            for (auto& v : spec) peak = std::max (peak, std::fabs (v.real()));
            const float g = 0.98f / peak;

            float* dst = wt.levels[(size_t) l].data() + (size_t) f * len;
            for (int i = 0; i < len; ++i) dst[i] = spec[(size_t) i].real() * g;
        }
    }
    return wt;
}

// ---------------------------------------------------------------------------
// Банк таблиц + бинарная сериализация (свой формат .orowt).
// Формат: "OROWT1" | u32 tables | [ u32 frames | u32 levels | (u32 len | floats)… | u32 nameLen | имя ]
// ---------------------------------------------------------------------------
class WavetableBank
{
public:
    std::vector<Wavetable> tables;

    void buildFactory (int frames = 6)
    {
        tables.clear();
        for (auto k : { TableKind::Bloom, TableKind::ReverseSaw, TableKind::Glass,
                        TableKind::Formant, TableKind::Fold, TableKind::Vocal,
                        TableKind::Organ, TableKind::Pwm })
            tables.push_back (buildTable (k, frames));
    }

    bool save (const std::string& path) const
    {
        FILE* f = std::fopen (path.c_str(), "wb");
        if (! f) return false;
        std::fwrite ("OROWT1", 1, 6, f);
        const uint32_t nt = (uint32_t) tables.size();
        std::fwrite (&nt, 4, 1, f);
        for (const auto& t : tables)
        {
            const uint32_t nf = (uint32_t) t.frames, nl = (uint32_t) t.lens.size();
            std::fwrite (&nf, 4, 1, f);
            std::fwrite (&nl, 4, 1, f);
            for (size_t l = 0; l < t.lens.size(); ++l)
            {
                const uint32_t len = (uint32_t) t.lens[l];
                const uint32_t hm  = (uint32_t) t.harms[l];
                std::fwrite (&hm, 4, 1, f);
                std::fwrite (&len, 4, 1, f);
                std::fwrite (t.levels[l].data(), sizeof (float), (size_t) t.frames * len, f);
            }
            const uint32_t nameLen = (uint32_t) t.name.size();
            std::fwrite (&nameLen, 4, 1, f);
            std::fwrite (t.name.data(), 1, nameLen, f);
        }
        std::fclose (f);
        return true;
    }

    bool load (const std::string& path)
    {
        FILE* f = std::fopen (path.c_str(), "rb");
        if (! f) return false;
        char magic[6] = {};
        if (std::fread (magic, 1, 6, f) != 6 || std::memcmp (magic, "OROWT1", 6) != 0)
        { std::fclose (f); return false; }

        uint32_t nt = 0;
        if (std::fread (&nt, 4, 1, f) != 1) { std::fclose (f); return false; }
        tables.assign (nt, {});
        for (auto& t : tables)
        {
            uint32_t nf = 0, nl = 0;
            std::fread (&nf, 4, 1, f);
            std::fread (&nl, 4, 1, f);
            t.frames = (int) nf;
            t.lens.resize (nl); t.harms.resize (nl); t.levels.resize (nl);
            for (uint32_t l = 0; l < nl; ++l)
            {
                uint32_t len = 0, hm = 0;
                std::fread (&hm, 4, 1, f);
                std::fread (&len, 4, 1, f);
                t.harms[l] = (int) hm;
                t.lens[l]  = (int) len;
                t.levels[l].resize ((size_t) nf * len);
                std::fread (t.levels[l].data(), sizeof (float), (size_t) nf * len, f);
            }
            uint32_t nameLen = 0;
            std::fread (&nameLen, 4, 1, f);
            t.name.resize (nameLen);
            std::fread (t.name.data(), 1, nameLen, f);
        }
        std::fclose (f);
        return true;
    }
};

} // namespace orogen
