// ДВИЖОК 1: Wavetable — доведён до продакшн-качества.
//
// Что делает его «готовым», в отличие от демки на JS:
//   1. MIP-пирамида из 25 уровней с шагом в ТРЕТЬ октавы, каждый band-limited
//      ПО ПОСТРОЕНИЮ (синтез из спектра, а не фильтрация готовой таблицы).
//   2. Выбор уровня — «самый яркий безопасный» (ceil), без кроссфейда с
//      небезопасным уровнем: именно он давал алиасинг -33 дБ в первой версии.
//      Шаг в треть октавы делает шаг тембра незаметным (≤1.26× партиалов).
//   3. Косинусная «шторка» верхних 22 % партиалов уровня.
//   4. Warp (фазовая дисторсия) и Fold (волновая складка) — нелинейности,
//      поэтому обе идут через 4× оверсэмплинг (два каскада полуполосных фильтров).
//   5. Сглаживание всех параметров (15–25 мс): нет zipper noise.
//   6. Случайная начальная фаза и «аналоговый» дрейф в центах на голос.
//   7. Денормал-защита, отсутствие аллокаций и делений в горячем цикле.
#pragma once

#include "engine.h"
#include "halfband.h"
#include "../content/wavetable_bank.h"

namespace orogen {

class WavetableEngine final : public EngineBase
{
public:
    // Калибровка уровня: подобрана измерением RMS (см. tests/test_wavetable.cpp),
    // чтобы слот с Wavetable не был на 15 дБ громче слота с Modal.
    static constexpr float kGainTrim = 0.48f;

    void setBank (const WavetableBank* b) noexcept { bank = b; }

    // Качество оверсэмплинга нелинейностей:
    //   0 = Eco    (2×, финальный фильтр 127)  — ~4× дешевле, алиасинг ~-50 дБ
    //   1 = Normal (4×/8×, финальный 255)      — текущие измерения
    //   2 = High   (8× всегда, финальный 255)
    void setQuality (int q) noexcept { quality = clamp (q, 0, 2); }

    const char* name() const override { return "Wavetable"; }
    float costFactor() const override { return 1.0f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        posSm.setTime  (sr, 18.0);
        warpSm.setTime (sr, 25.0);
        foldSm.setTime (sr, 25.0);
        decimA.reset(); decimB.reset(); decimC.reset();
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        phase = rng.nextFloat();                              // случайный старт фазы
        driftCents = rng.nextBipolar() * 7.0f;                // ±7 центов «аналога»
        dcX = dcY = 0.0f;
        needSnap = true;
        active = true;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        if (bank == nullptr || bank->tables.empty()) return;

        const Wavetable& wt = bank->tables[(size_t) clamp (p.table, 0, (int) bank->tables.size() - 1)];
        frames = wt.frames;
        nFramesMinus1 = std::max (1, frames - 1);

        // ---- частота: нота + тюн слота + дрейф голоса
        const double cents = (double) driftCents * (double) p.detune * 2.0;   // до ±14 центов
        const double f0 = midiToHz ((double) note + (double) p.tuneSemis) * std::pow (2.0, cents / 1200.0);
        const double inc1x = f0 / sampleRate;

        // ---- выбор MIP-уровня: самый яркий БЕЗОПАСНЫЙ уровень
        // allowed = сколько партиалов влезает ниже Найквиста
        const double allowed = 0.5 * sampleRate / std::max (1.0, f0);
        level   = levelForAllowed (allowed);
        len     = wt.lens[(size_t) level];
        mask    = len - 1;
        base    = wt.levels[(size_t) level].data();
        maxHarm = wt.harms[(size_t) level];

        inc    = (float) inc1x;
        // Eco: 2×; Normal: 4× для warp и 8× для сильной складки; High: всегда 8×
        const bool hardFold = clamp (p.p[2], 0.0f, 1.0f) > 0.45f;
        osFactor = (quality == 0) ? 2
                 : (quality == 2 || hardFold) ? 8 : 4;
        incOs    = (float) (inc1x / (double) osFactor);

        // ---- параметры со сглаживанием
        const float posT  = clamp (p.p[0], 0.0f, 1.0f);
        const float warpT = clamp (p.p[1], 0.0f, 1.0f);
        const float foldT = clamp (p.p[2], 0.0f, 1.0f);

        if (needSnap)
        {
            posSm.snap (posT);
            warpSm.snap (warpT);
            foldSm.snap (foldT);
            needSnap = false;
        }
        else
        {
            posSm.setTarget (posT);
            warpSm.setTarget (warpT);
            foldSm.setTarget (foldT);
        }

        // warp: глубина фазовой дисторсии (0..0.15 — сохраняет монотонность фазы)
        warpDepth = warpT * 0.15f;
        // fold: коэффициент складки; при 0 не применяется вовсе.
        // 1..4 — выше начинается «звон» из-за гармоник выше Найквиста даже при 4×.
        foldDrive = 1.0f + foldT * 2.5f;
        foldTrim  = 1.0f / std::sqrt (foldDrive);
        gain      = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
    }

    void prepareBlock (const EngineParams& p) { prepare (p); }

    inline float render (const EngineParams&) override
    {
        if (! active) return 0.0f;

        const float pos  = posSm.next();
        const float warp = warpSm.next();
        const float fold = foldSm.next();

        // ---- выбор кадров (морфинг позиции)
        const float fp = pos * (float) nFramesMinus1;
        int fa = (int) fp;
        if (fa > nFramesMinus1) fa = nFramesMinus1;
        const int   fb = (fa + 1 < frames) ? fa + 1 : fa;
        const float ff = fp - (float) fa;

        const bool distort = (fold > 1.0e-4f) || (warp > 1.0e-4f);
        float out;

        if (! distort)
        {
            // ---- прямой путь (одна выборка на сэмпл)
            out = readLevel (base, len, mask, fa, fb, ff, phase) * gain;
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        else
        {
            // ---- путь с оверсэмплингом: 4× для warp, 8× при сильной складке
            // (волновая складка генерирует гармоники до ~2·drive-го порядка;
            //  на 4× они ещё заворачиваются в полосу, на 8× — заметно чище)
            const float wd = warp * 0.15f;
            for (int k = 0; k < osFactor; ++k)
            {
                float ph = phase;
                if (wd > 0.0f)
                {
                    // фазовая дисторсия: φ' = φ + d·sin(2πφ), монотонна при d < 1/(2π)
                    ph += wd * fastSin (ph);
                    if (ph >= 1.0f) ph -= 1.0f;
                    else if (ph < 0.0f) ph += 1.0f;
                }
                float s = readLevel (base, len, mask, fa, fb, ff, ph);
                if (fold > 1.0e-4f)
                    s = fastSin (s * foldDrive * 0.5f);          // sin(π·x·d/2) — складка
                if (osFactor == 2)
                {
                    decimC.push (s);                                   // 2× -> 1×
                }
                else
                {
                    decimA.push (s);
                    if ((k & 1) == 1)
                    {
                        if (osFactor == 8)
                        {
                            decimB.push (decimA.decimate());           // 8× -> 4×
                            if ((k & 3) == 3)
                                decimC.push (decimB.decimate());       // 4× -> 2×
                        }
                        else
                        {
                            decimC.push (decimA.decimate());           // 4× -> 2×
                        }
                    }
                }
                phase += incOs;
                if (phase >= 1.0f) phase -= 1.0f;
            }
            out = decimC.decimate()                                // 2× -> 1× (острый фильтр)
                * gain * (fold > 1.0e-4f ? foldTrim : 1.0f);
        }
        // DC-блокер: нелинейности (fold, drive) дают постоянную составляющую,
        // которая «съедает» headroom и уводит фильтр голоса в смещение.
        const float y = out - dcX + 0.999f * dcY;
        dcX = out; dcY = y;
        return y;
    }

private:
    // Кубическая интерполяция Catmull-Rom: линейная даёт образы -21 дБ на
    // верхних партиалах уровня, кубическая — на 20-30 дБ ниже при той же цене
    // (4 чтения вместо 2, зато без «цифрового песка» на верхах).
    static inline float interpCubic (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b = w + a;
        return ((a * t - b) * t + c) * t + x0;
    }

    static inline float readLevel (const float* base, int len, int mask,
                                   int fa, int fb, float ff, float ph) noexcept
    {
        const float x  = ph * (float) len;
        const int   i1 = (int) x;
        const float fx = x - (float) i1;
        const int   i0 = (i1 - 1) & mask;
        const int   i2 = (i1 + 1) & mask;
        const int   i3 = (i1 + 2) & mask;

        const float* A = base + (size_t) fa * (size_t) len;
        const float* B = base + (size_t) fb * (size_t) len;

        const float a = interpCubic (A[i0], A[i1], A[i2], A[i3], fx);
        if (fb == fa) return a;
        const float b = interpCubic (B[i0], B[i1], B[i2], B[i3], fx);
        return a + (b - a) * ff;
    }

    const WavetableBank* bank = nullptr;
    double sampleRate = 48000.0;

    int    note = 60;
    float  vel = 1.0f;
    bool   active = false;
    bool   needSnap = true;

    float  phase = 0.0f;
    float  inc = 0.0f, incOs = 0.0f;
    int    osFactor = 4;
    int    quality  = 1;
    float  driftCents = 0.0f;

    int    level = 0;
    int    len   = kWtBaseLen;
    int    mask  = kWtBaseLen - 1;
    int    maxHarm = kWtMaxHarm;
    const float* base = nullptr;

    int    frames = 6;
    int    nFramesMinus1 = 5;
    float  warpDepth = 0.0f, foldDrive = 1.0f, foldTrim = 1.0f, gain = 0.4f;

    SmoothedValue posSm, warpSm, foldSm;
    float dcX = 0.0f, dcY = 0.0f;          // состояние DC-блокера
    // промежуточные ступени — 127 отсчётов, финальная — 255 (см. halfband.h)
    HalfbandDecimator2x<127> decimA, decimB;
    HalfbandDecimator2x<127, 220> decimC;
    Rng rng { 0x1234567u };
};

} // namespace orogen
