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
#include "../content/wavetable_import.h"

namespace orogen {

class WavetableEngine final : public EngineBase
{
    // Только невладеющие указатели на неизменяемый банк; переключение таблиц
    // не выделяет память и не копирует данные в аудиопотоке.
    struct TableView
    {
        const float* base = nullptr;
        int len = 0, mask = 0, frames = 0;
        bool operator== (const TableView& b) const noexcept
        { return base == b.base && len == b.len && frames == b.frames; }
        bool operator!= (const TableView& b) const noexcept { return ! (*this == b); }
    };
    enum class RouteFade { Stable, Down, Up };

public:
    // Калибровка уровня: подобрана измерением RMS (см. tests/test_wavetable.cpp),
    // чтобы слот с Wavetable не был на 15 дБ громче слота с Modal.
    static constexpr float kGainTrim = 0.48f;

    void setBank (const WavetableBank* b) noexcept { bank = b; }
    // Only the staging/control thread installs ownership; the audio thread
    // reads immutable data and never copies or destroys this shared_ptr.
    void setUserTable (std::shared_ptr<const UserWavetable> table) noexcept
    { userTable = std::move (table); }

    // Качество оверсэмплинга нелинейностей:
    //   0 = Eco    (2×, финальный фильтр 127)  — ~4× дешевле, алиасинг ~-50 дБ
    //   1 = Normal (4×/8×, финальный 255)      — текущие измерения
    //   2 = High   (8× всегда, финальный 255)
    void setQuality (int q) noexcept { quality = clamp (q, 0, 2); }

    const char* name() const override { return "Wavetable"; }
    float costFactor() const override { return 1.0f; }

    void setSampleRate (double sr) override
    {
        sampleRate = (std::isfinite (sr) && sr >= 1000.0) ? sr : 48000.0;
        posSm.setTime      (sampleRate, 18.0);
        warpSm.setTime     (sampleRate, 25.0);
        foldSm.setTime     (sampleRate, 25.0);
        foldTrimSm.setTime (sampleRate, 25.0);
        tiltSm.setTime     (sampleRate, 20.0);
        pitchGainSm.setTime (sampleRate, 5.0);
        tableStep = 1.0f / (float) std::max (1.0, sampleRate * 0.005); // 5 ms table crossfade
        routeStep = 1.0f / (float) std::max (1.0, sampleRate * 0.003); // 3 ms down + 3 ms up
        decimA.reset(); decimB.reset(); decimC.reset();
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        phase = rng.nextFloat();                              // случайный старт фазы
        driftCents = rng.nextBipolar() * 7.0f;                // ±7 центов «аналога»
        dcX = dcY = tiltState = 0.0f;
        // Повторный note-on не должен проигрывать историю фильтров прошлой ноты.
        decimA.reset(); decimB.reset(); decimC.reset();
        currentTable = nextTable = requestedTable = {};
        tableTransition = false;
        routeFade = RouteFade::Stable;
        routeGain = 1.0f;
        needSnap = true;
        active = true;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        const bool useUser = p.wtUser && userTable != nullptr;
        if (! useUser && (bank == nullptr || bank->tables.empty()))
        {
            requestedTable = currentTable = nextTable = {};
            return;
        }

        const Wavetable& wt = useUser ? userTable->table
            : bank->tables[(size_t) clamp (p.table, 0, (int) bank->tables.size() - 1)];
        const size_t count = std::min ({ wt.levels.size(), wt.lens.size(), wt.harms.size() });
        if (count == 0 || wt.frames < 1)
        {
            requestedTable = currentTable = nextTable = {};
            return;
        }

        // Частота выше Найквиста не может быть представлена. Плавно гасим её
        // в полосе 0.45..0.5 SR и ограничиваем phase increment: это убирает и
        // alias, и выход чтения за массив при MIDI 127 + Tune +24 на низком SR.
        const double cents = (double) driftCents * (double) p.detune * 2.0;
        double f0 = midiToHz ((double) note + (double) p.tuneSemis) * std::pow (2.0, cents / 1200.0);
        if (! std::isfinite (f0) || f0 < 0.0) f0 = sampleRate;
        const double allowed = 0.5 * sampleRate / std::max (1.0, f0);
        int level = clamp (levelForAllowed (allowed), 0, (int) count - 1);
        while (level + 1 < (int) count && (double) wt.harms[(size_t) level] > allowed) ++level;
        int len = wt.lens[(size_t) level];
        const std::vector<float>* samples = &wt.levels[(size_t) level];
        if ((size_t) level < wt.highDensityLevels.size() && (size_t) level < wt.highDensityLens.size()
            && ! wt.highDensityLevels[(size_t) level].empty())
        {
            samples = &wt.highDensityLevels[(size_t) level];
            len = wt.highDensityLens[(size_t) level];
        }
        const bool valid = len > 1 && (len & (len - 1)) == 0
                        && samples->size() >= (size_t) wt.frames * (size_t) len;
        if (! valid)
        {
            requestedTable = currentTable = nextTable = {};
            return;
        }
        requestedTable = { samples->data(), len, len - 1, wt.frames };
        const bool safeMip = (double) wt.harms[(size_t) level] <= allowed;
        const float pitchGain = safeMip
            ? (float) clamp ((0.5 * sampleRate - f0) / (0.05 * sampleRate), 0.0, 1.0) : 0.0f;
        incBase = std::min (f0, 0.499999 * sampleRate) / sampleRate;
        inc = (float) incBase;

        // Eco 2×; Normal 4×/8×; High 8×. Фильтры между разными частотами
        // нельзя переключать мгновенно: render() делает короткий fade-out/in.
        const bool hardFold = clamp (p.p[2], 0.0f, 1.0f) > 0.45f;
        requestedFactor = (quality == 0) ? 2 : (quality == 2 || hardFold) ? 8 : 4;

        const float posT  = clamp (p.p[0], 0.0f, 1.0f);
        const float warpT = clamp (p.p[1], 0.0f, 1.0f);
        const float foldT = clamp (p.p[2], 0.0f, 1.0f);
        const float tiltT = clamp (p.p[3], 0.0f, 1.0f);
        const float trimT = 1.0f / std::sqrt (1.0f + foldT * 2.5f);

        const float tiltTarget = (tiltT < kTiltNeutral) ? (tiltT - kTiltNeutral) * 2.3f
                                                        : (tiltT - kTiltNeutral) * 1.6f;
        tiltCoef = 1.0f - std::exp (-6.2831853f * 3000.0f / (float) sampleRate);

        if (needSnap)
        {
            posSm.snap (posT); warpSm.snap (warpT); foldSm.snap (foldT);
            foldTrimSm.snap (trimT); tiltSm.snap (tiltTarget); pitchGainSm.snap (pitchGain);
            currentTable = requestedTable;
            tableTransition = false;
            activeRoute = (warpT > 1.0e-4f || foldT > 1.0e-4f) ? requestedFactor : 0;
            incOs = activeRoute > 0 ? (float) (incBase / (double) activeRoute) : 0.0f;
            routeFade = RouteFade::Stable; routeGain = 1.0f;
            needSnap = false;
        }
        else
        {
            posSm.setTarget (posT); warpSm.setTarget (warpT); foldSm.setTarget (foldT);
            foldTrimSm.setTarget (trimT); tiltSm.setTarget (tiltTarget); pitchGainSm.setTarget (pitchGain);
            if (activeRoute > 0) incOs = (float) (incBase / (double) activeRoute);
        }
        gain = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
    }

    void prepareBlock (const EngineParams& p) { prepare (p); }

    inline float render (const EngineParams&) override
    {
        if (! active || currentTable.base == nullptr) return 0.0f;

        const float pos  = posSm.next();
        const float warp = warpSm.next();
        const float fold = foldSm.next();
        const float trim = foldTrimSm.next();
        const float tiltAmount = tiltSm.next();
        const float pitchGain = pitchGainSm.next();

        // Замену источника/MIP смешиваем в фазе на протяжении 5 ms. Если
        // выбор меняется чаще, текущий переход завершается, затем применяется
        // самое последнее значение (без разрыва и без очереди аллокаций).
        if (! tableTransition && requestedTable != currentTable && requestedTable.base != nullptr)
        {
            nextTable = requestedTable;
            tableMix = 0.0f;
            tableTransition = true;
        }
        const auto frameInfo = [pos] (const TableView& t, int& fa, int& fb, float& ff)
        {
            const int last = std::max (0, t.frames - 1);
            const float fp = pos * (float) last;
            fa = std::min ((int) fp, last);
            fb = fa < last ? fa + 1 : fa;
            ff = fp - (float) fa;
        };
        int fa = 0, fb = 0, ta = 0, tb = 0;
        float ff = 0.0f, tf = 0.0f;
        frameInfo (currentTable, fa, fb, ff);
        if (tableTransition) frameInfo (nextTable, ta, tb, tf);
        const auto tableAt = [&] (float ph)
        {
            const float a = readLevel (currentTable.base, currentTable.len, currentTable.mask, fa, fb, ff, ph);
            if (! tableTransition) return a;
            const float b = readLevel (nextTable.base, nextTable.len, nextTable.mask, ta, tb, tf, ph);
            return a + (b - a) * tableMix;
        };

        const bool distort = (fold > 1.0e-4f) || (warp > 1.0e-4f);
        const int wantedRoute = distort ? requestedFactor : 0;
        if (routeFade == RouteFade::Stable && wantedRoute != activeRoute) routeFade = RouteFade::Down;
        if (routeFade == RouteFade::Down)
        {
            if (wantedRoute == activeRoute) routeFade = RouteFade::Up;
            else
            {
                routeGain = std::max (0.0f, routeGain - routeStep);
                if (routeGain <= 0.0f)
                {
                    activeRoute = wantedRoute;
                    decimA.reset(); decimB.reset(); decimC.reset();
                    incOs = activeRoute > 0 ? (float) (incBase / (double) activeRoute) : 0.0f;
                    routeFade = RouteFade::Up;
                }
            }
        }
        else if (routeFade == RouteFade::Up)
        {
            routeGain = std::min (1.0f, routeGain + routeStep);
            if (routeGain >= 1.0f) routeFade = RouteFade::Stable;
        }

        const float wet = std::min (1.0f, fold * 20.0f); // плавный ввод нелинейности в первые 5% ручки
        const float foldDrive = 1.0f + fold * 2.5f;
        float out;
        if (activeRoute == 0)
        {
            out = tableAt (phase) * gain;
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        else
        {
            const float wd = warp * 0.15f;
            for (int k = 0; k < activeRoute; ++k)
            {
                float ph = phase;
                if (wd > 0.0f)
                {
                    ph += wd * fastSin (ph);
                    if (ph >= 1.0f) ph -= 1.0f;
                    else if (ph < 0.0f) ph += 1.0f;
                }
                float sample = tableAt (ph);
                if (fold > 1.0e-4f)
                {
                    const float folded = fastSin (sample * foldDrive * 0.5f);
                    sample = wet >= 1.0f ? folded : sample + (folded - sample) * wet;
                }
                if (activeRoute == 2) decimC.push (sample);
                else
                {
                    decimA.push (sample);
                    if ((k & 1) == 1)
                    {
                        if (activeRoute == 8)
                        {
                            decimB.push (decimA.decimate());
                            if ((k & 3) == 3) decimC.push (decimB.decimate());
                        }
                        else decimC.push (decimA.decimate());
                    }
                }
                phase += incOs;
                if (phase >= 1.0f) phase -= 1.0f;
            }
            const float foldGain = wet >= 1.0f ? trim : 1.0f + (trim - 1.0f) * wet;
            out = decimC.decimate() * gain * (fold > 1.0e-4f ? foldGain : 1.0f);
        }
        if (tableTransition)
        {
            tableMix += tableStep;
            if (tableMix >= 1.0f) { currentTable = nextTable; tableTransition = false; }
        }
        tiltState += tiltCoef * (out - tiltState);
        if (tiltAmount < -1.0e-4f)      out += (-tiltAmount) * (tiltState - out);
        else if (tiltAmount > 1.0e-4f)  out += tiltAmount * (out - tiltState);

        const float y = out - dcX + 0.999f * dcY;
        dcX = out; dcY = y;
        return y * pitchGain * routeGain;
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
        const int   ix = (int) x;
        const float fx = x - (float) ix;
        const int   i1 = ix & mask; // защита и от округления phase ровно до 1
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
    std::shared_ptr<const UserWavetable> userTable;
    double sampleRate = 48000.0;

    int    note = 60;
    float  vel = 1.0f;
    bool   active = false;
    bool   needSnap = true;

    float  phase = 0.0f;
    double incBase = 0.0;
    float  inc = 0.0f, incOs = 0.0f;
    int    requestedFactor = 4, activeRoute = 0, quality = 1;
    float  driftCents = 0.0f;

    TableView currentTable, nextTable, requestedTable;
    bool tableTransition = false;
    float tableMix = 0.0f, tableStep = 1.0f / 240.0f;
    RouteFade routeFade = RouteFade::Stable;
    float routeGain = 1.0f, routeStep = 1.0f / 144.0f;
    float gain = 0.4f;

    SmoothedValue posSm, warpSm, foldSm, foldTrimSm, tiltSm, pitchGainSm;
    static constexpr float kTiltNeutral = 0.3f;   // значение p[3] по умолчанию
    float tiltCoef = 0.3f, tiltState = 0.0f;
    float dcX = 0.0f, dcY = 0.0f;          // состояние DC-блокера
    // промежуточные ступени — 127 отсчётов, финальная — 255 (см. halfband.h)
    HalfbandDecimator2x<127> decimA, decimB;
    HalfbandDecimator2x<127, 220> decimC;
    Rng rng { 0x1234567u };
};

} // namespace orogen
