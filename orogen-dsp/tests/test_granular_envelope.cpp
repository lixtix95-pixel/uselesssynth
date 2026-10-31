// Независимая регрессия нормировки Granular на реальном нестационарном PCM.
// Зерно и сэмплы не подменяются: заводим атаку на одной из граничных 10-мс
// ячеек и читаем ОДИН И ТОТ ЖЕ участок вперёд/назад при одинаковом seed.
#include "engine_suite.h"
#include "../src/engines/granular.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace orogen;

namespace orogen {
struct GranularTestProbe
{
    struct Cursor { bool active; double start, rate, duration, amp; };
    static Cursor first (const GranularEngine& e)
    {
        const auto& g = e.grains[0];
        // Вызывается после первого render: первый отсчёт уже прочитан.
        return { g.active, g.pos - g.rate, g.rate, g.dur, g.amp };
    }
};
}

static OneShot sineShot (int sr, double seconds, float amplitude)
{
    OneShot shot; shot.name = "Envelope regression"; shot.rootNote = 45;
    shot.levels.resize (1);
    shot.levels[0].resize ((size_t) std::lround (sr * seconds));
    for (size_t i = 0; i < shot.levels[0].size(); ++i)
        shot.levels[0][i] = (float) (amplitude * std::sin (kTwoPi * 1000.0 * (double) i / sr));
    buildMipLevels (shot, sr);
    return shot;
}

struct Comparison
{
    double ratio = 0.0, reference = 0.0, forward = 0.0, backward = 0.0;
    double rmsRatio = 0.0, edgeLow = 0.0, edgeHigh = 0.0;
    bool valid = false;
};

static Comparison compareDirections (int sr, float size, bool loudHigh)
{
    SampleBank bank; bank.shots.push_back (sineShot (sr, 2.0, 0.08f));
    EngineParams p; p.p[0] = 0.4f; p.p[1] = size; p.p[2] = 0.0f;
    p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;

    // Узнаём реальные безопасные границы первого зерна, затем меняем PCM,
    // полностью пересчитываем MIP/огибающую и запускаем тот же детерминированный
    // планировщик. Расстановка атаки не задаётся искусственной подменой env.
    GranularEngine location; location.setBank (&bank); location.setSampleRate (sr);
    location.start (45, 1.0f); location.prepare (p); (void) location.render (p);
    const auto initial = GranularTestProbe::first (location);
    OneShot& source = bank.shots[0];
    const int cell = std::clamp ((int) ((initial.start + (loudHigh ? initial.duration * initial.rate : 0.0))
                                       / source.envStep), 0, (int) source.env.size() - 1);
    const size_t a = (size_t) cell * (size_t) source.envStep;
    const size_t z = std::min (source.levels[0].size(), a + (size_t) source.envStep);
    for (size_t i = a; i < z; ++i)
        source.levels[0][i] = (float) (0.8 * std::sin (kTwoPi * 1000.0 * (double) i / sr));
    buildMipLevels (source, sr);

    GranularEngine fwd, rev;
    fwd.setBank (&bank); rev.setBank (&bank);
    fwd.setSampleRate (sr); rev.setSampleRate (sr);
    fwd.start (45, 1.0f); rev.start (45, 1.0f);
    EngineParams rp = p; rp.grainReverse = 1.0f;
    fwd.prepare (p); rev.prepare (rp);
    (void) fwd.render (p); (void) rev.render (rp);
    const auto f = GranularTestProbe::first (fwd);
    const auto r = GranularTestProbe::first (rev);
    const double lo = f.start, hi = f.start + f.duration * f.rate;
    const double lowEnv = source.envAt ((float) lo), highEnv = source.envAt ((float) hi);

    // Размер >=50.4 мс, density=4/с => overlap=1, множитель размера=1.
    // Громкая крайняя ячейка гарантированно превышает RMS Hann²; этим эталон
    // не зависит от конкретного числа проб реализации (8..256).
    const double body = source.bodyRms;
    const double reference = clamp ((float) (body / std::max (lowEnv, highEnv)), 0.25f, 5.0f)
                           * clamp ((float) (GranularEngine::kBodyTarget / body), 0.05f, 6.0f);
    const int samples = (int) std::ceil (f.duration) + 1;
    double fPower = 0.0, rPower = 0.0; bool finite = true;
    for (int i = 0; i < samples; ++i)
    {
        const float a0 = fwd.render (p), b0 = rev.render (rp);
        finite = finite && std::isfinite (a0) && std::isfinite (b0);
        fPower += (double) a0 * a0; rPower += (double) b0 * b0;
    }
    Comparison c;
    c.forward = f.amp; c.backward = r.amp; c.reference = reference;
    c.ratio = f.amp / std::max (1.0e-20, r.amp);
    c.rmsRatio = std::sqrt (fPower / std::max (1.0e-20, rPower));
    c.edgeLow = lowEnv; c.edgeHigh = highEnv;
    c.valid = finite && initial.active && f.active && r.active && f.rate > 0 && r.rate < 0
           && std::fabs (f.rate + r.rate) < 1.0e-6
           && std::fabs ((r.start + r.duration * r.rate) - f.start) < 0.02
           && ((loudHigh && highEnv > 5.0 * lowEnv) || (! loudHigh && lowEnv > 5.0 * highEnv));
    return c;
}

static void testPartialTail (int sr)
{
    const int step = (int) (sr * 0.01);
    const int boundary = 100 * step;
    const int length = boundary + step - 1; // последняя ячейка неполная
    OneShot source = sineShot (sr, (double) length / sr, 0.08f);
    double tailEnergy = 0.0;
    for (int i = boundary; i < length; ++i)
    {
        source.levels[0][(size_t) i] = (float) (0.8 * std::sin (kTwoPi * 1000.0 * i / sr));
        tailEnergy += (double) source.levels[0][(size_t) i] * source.levels[0][(size_t) i];
    }
    const double tailRms = std::sqrt (tailEnergy / (length - boundary));
    buildMipLevels (source, sr);
    const double lastRms = source.env.back(), body = source.bodyRms;
    const double from = boundary - step / 2.0, to = boundary + step / 2.0;
    const double expectedEnergy = (lastRms * lastRms + tailRms * tailRms) * 0.5;
    const double actualEnergy = source.meanEnvEnergy (from, to);

    SampleBank bank; bank.shots.push_back (std::move (source));
    EngineParams p; p.p[0] = 1.0f; p.p[1] = 0.0f; p.p[2] = 0.0f;
    p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;
    EngineParams rp = p; rp.grainReverse = 1.0f;
    GranularEngine fwd, rev;
    fwd.setBank (&bank); rev.setBank (&bank);
    fwd.setSampleRate (sr); rev.setSampleRate (sr);
    fwd.start (45, 1.0f); rev.start (45, 1.0f);
    fwd.prepare (p); rev.prepare (rp); (void) fwd.render (p); (void) rev.render (rp);
    const auto f = GranularTestProbe::first (fwd), r = GranularTestProbe::first (rev);
    const double sizeFactor = 0.6 + 0.4 * (0.008 / 0.05);
    const double referenceGain = sizeFactor
        * clamp ((float) (body / std::max (lastRms, tailRms)), 0.25f, 5.0f)
        * clamp ((float) (GranularEngine::kBodyTarget / body), 0.05f, 6.0f);
    const bool crosses = f.active && r.active && f.start < boundary
                      && f.start + f.duration * f.rate > boundary;
    double peak = 0.0; bool finite = true;
    for (int i = 0; i <= (int) std::ceil (f.duration); ++i)
    {
        const float x = fwd.render (p), y = rev.render (rp);
        finite = finite && std::isfinite (x) && std::isfinite (y);
        peak = std::max ({ peak, std::fabs ((double) x), std::fabs ((double) y) });
    }
    std::printf ("   %d Гц: неполный хвост %d отсч., RMS %.6f / прежняя ячейка %.6f; "
                 "энергия %.6f / эталон %.6f, gain %.6f / %.6f / эталон %.6f, peak %.6f\n",
                 sr, length - boundary, tailRms, lastRms, actualEnergy, expectedEnergy,
                 f.amp, r.amp, referenceGain, peak);
    suite::check (crosses && finite && tailRms > lastRms * 5.0
                  && std::fabs (f.amp / referenceGain - 1.0) < 1.0e-4
                  && std::fabs (r.amp / referenceGain - 1.0) < 1.0e-4,
                  "неполный громкий хвост учитывается реальным зерном в обоих направлениях");
    suite::check (std::fabs (actualEnergy - expectedEnergy) < 1.0e-7,
                  "интеграл огибающей не продолжает прежнюю ячейку вместо хвоста");
    auto& rebuilt = bank.shots[0];
    const bool fullAndTailDistinct = rebuilt.envTailSamples == (size_t) (length - boundary)
        && std::fabs (rebuilt.envAt ((float) boundary + 1.0f) - lastRms) < 1.0e-6
        && std::fabs (rebuilt.granularEnvAt (boundary + 1.0) - tailRms) < 1.0e-6;
    rebuilt.levels[0].resize ((size_t) boundary);
    computeEnvelope (rebuilt, sr);
    suite::check (fullAndTailDistinct && rebuilt.envTailSamples == 0 && rebuilt.envTailRms == 0.0f
                  && std::fabs (rebuilt.granularEnvAt (boundary + 1.0) - lastRms) < 1.0e-6,
                  "общая огибающая сохранена; метаданные хвоста сбрасываются при перестроении");
}

static void testFactoryAttack (int sr, int table)
{
    SampleBank bank; bank.buildFactory (sr);
    GranularEngine e; e.setBank (&bank); e.setSampleRate (sr); e.start (45, 1.0f);
    EngineParams p; p.table = table; p.p[0] = 0.0f; p.p[1] = 0.2f;
    p.p[2] = 0.0f; p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;
    e.prepare (p);
    const int count = sr * 3 / 10;
    double power = 0.0; float peak = 0.0f; int over = 0, bad = 0;
    for (int i = 0; i < count; ++i)
    {
        const float x = e.render (p);
        if (! std::isfinite (x)) ++bad;
        peak = std::max (peak, std::fabs (x)); power += (double) x * x;
        if (std::fabs (x) > 1.0f) ++over;
    }
    const double rms = std::sqrt (power / count);
    std::printf ("   фабричный %s, %d Гц: peak %.6f, RMS %.6f, |x|>1 %d/%d, NaN/Inf %d\n",
                 bank.shots[(size_t) table].name.c_str(), sr, peak, rms, over, count, bad);
    // Это только данный конкретный сценарий, не гарантия headroom для всех
    // остальных параметров, голосов и фабричных источников.
    suite::check (bad == 0 && over == 0 && peak > 0.2f && peak < 1.0f && rms > 0.06,
                  "реальная фабричная атака не усиливается до пика выше 1 в данном сценарии");
}

int main()
{
    std::printf ("OROGEN — Granular: реальная атака на краю зерна\n=================================================\n");
    for (int sr : { 44100, 48000, 96000 })
        for (float size : { 0.2f, 1.0f })
            for (bool high : { false, true })
            {
                const auto c = compareDirections (sr, size, high);
                std::printf ("   %d Гц size %.1f край %s: env %.6f / %.6f, gain %.6f / %.6f, эталон %.6f, RMS fwd/rev %.4f\n",
                             sr, size, high ? "верхний" : "нижний", c.edgeLow, c.edgeHigh,
                             c.forward, c.backward, c.reference, c.rmsRatio);
                const bool bothMatchReference = std::fabs (c.forward / c.reference - 1.0) < 1.0e-4
                                             && std::fabs (c.backward / c.reference - 1.0) < 1.0e-4;
                suite::check (c.valid && bothMatchReference && std::fabs (c.rmsRatio - 1.0) < 0.04,
                              std::string (high ? "верхняя" : "нижняя") + " атака: gain и RMS не зависят от направления");
            }
    std::printf ("\n--- Последняя неполная 10-мс ячейка ---\n");
    for (int sr : { 44100, 48000, 47999, 96000 }) testPartialTail (sr);
    std::printf ("\n--- Реальные фабричные атаки при Position 0, MIDI 45 ---\n");
    for (int sr : { 48000, 47999 })
        for (int table : { 2, 6 }) testFactoryAttack (sr, table);
    std::printf ("\n%s (провалов: %d)\n", suite::gFailures ? "ЕСТЬ ПРОВАЛЫ" : "ВСЕ ТЕСТЫ ПРОЙДЕНЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
