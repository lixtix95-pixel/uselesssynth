// Реверс отдельных зёрен: настоящий DSP, RNG, направление, автоматизация и границы.
#include "engine_suite.h"
#include "../src/engines/granular.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace orogen;

namespace orogen {
struct GranularTestProbe
{
    struct Cursor { bool active; double pos, rate, dur, amp; };
    static Cursor cursor (const GranularEngine& e, int i)
    {
        const auto& g = e.grains[i];
        return { g.active, g.pos, g.rate, g.dur, g.amp };
    }
    static int kernelOverruns (const GranularEngine& e,
                               const std::array<bool, GranularEngine::kMaxGrains>& previous)
    {
        int bad = 0;
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            const auto& g = e.grains[i];
            if (! g.active && ! previous[(size_t) i]) continue;
            const double x = g.pos - (double) g.rate; // координата последнего read()
            const auto outside = [x, &g] (int len, float invScale) {
                const int centre = (int) std::floor (x * (double) invScale);
                const int half = g.mip.grainHalf();
                return centre - (half - 1) < 0 || centre + half >= len;
            };
            if (outside (g.mip.lenA, g.mip.invScaleA)
                || (g.mip.wB > 0.001f && outside (g.mip.lenB, g.mip.invScaleB))) ++bad;
        }
        return bad;
    }
};
}

static OneShot stationary (int sr, double seconds)
{
    OneShot s; s.name = "Reverse stationary"; s.rootNote = 45;
    s.levels.resize (1);
    s.levels[0].resize ((size_t) (sr * seconds));
    for (size_t i = 0; i < s.levels[0].size(); ++i)
        s.levels[0][i] = (float) (0.30 * std::sin (kTwoPi * 1000.0 * i / sr));
    buildMipLevels (s, sr);
    return s;
}

static bool equalBits (float a, float b)
{
    return std::memcmp (&a, &b, sizeof (a)) == 0;
}

int main()
{
    constexpr int sr = 48000;
    std::printf ("OROGEN — Granular: reverse probability\n=================================================\n");
    SampleBank bank; bank.shots.push_back (stationary (sr, 2.0));
    EngineParams p;
    p.p[0] = 0.55f; p.p[1] = 0.20f; p.p[2] = 1.0f; p.p[3] = 0.55f;
    p.detune = 0.0f; p.level = 1.0f;

    // Один и тот же источник и планировщик, но 0%, 50%, 100% и повтор 50%.
    // Сопоставляем каждое рождённое зерно: направление не должно сдвигать
    // последовательность случайных позиций, длительностей или питч-рейтов.
    GranularEngine engines[4];
    EngineParams params[4] = { p, p, p, p };
    params[0].grainReverse = 0.0f;
    params[1].grainReverse = params[3].grainReverse = 0.5f;
    params[2].grainReverse = 1.0f;
    for (int k = 0; k < 4; ++k)
    {
        engines[k].setBank (&bank); engines[k].setSampleRate (sr);
        engines[k].start (60, 1.0f); engines[k].prepare (params[k]);
    }
    std::array<std::array<bool, GranularEngine::kMaxGrains>, 4> prev {};
    int counts[4] {}, backwards[4] {};
    int unequalSamples = 0, mismatchedGrains = 0, badBounds = 0, badValues = 0;
    double maxPositionDifference = 0.0, maxRateDifference = 0.0;
    for (int t = 0; t < 4 * sr; ++t)
    {
        float out[4];
        for (int k = 0; k < 4; ++k)
        {
            out[k] = engines[k].render (params[k]);
            if (! std::isfinite (out[k])) ++badValues;
            badBounds += GranularTestProbe::kernelOverruns (engines[k], prev[(size_t) k]);
        }
        if (! equalBits (out[1], out[3])) ++unequalSamples;
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            GranularTestProbe::Cursor g[4];
            bool born[4];
            for (int k = 0; k < 4; ++k)
            {
                g[k] = GranularTestProbe::cursor (engines[k], i);
                born[k] = g[k].active && ! prev[(size_t) k][(size_t) i];
                if (born[k]) { ++counts[k]; if (g[k].rate < 0.0) ++backwards[k]; }
            }
            if (born[0] || born[1] || born[2] || born[3])
            {
                if (! (born[0] && born[1] && born[2] && born[3])) ++mismatchedGrains;
                else for (int k = 1; k < 4; ++k)
                {
                    const double start0 = g[0].pos - g[0].rate;
                    const double start = g[k].pos - g[k].rate;
                    // Для reverse старт — верхняя граница того же участка.
                    const double lower = g[k].rate < 0.0 ? start + g[k].dur * g[k].rate : start;
                    maxPositionDifference = std::max (maxPositionDifference, std::fabs (lower - start0));
                    maxRateDifference = std::max (maxRateDifference,
                                                  std::fabs (std::fabs (g[k].rate) - std::fabs (g[0].rate)));
                    if (g[k].dur != g[0].dur) ++mismatchedGrains;
                }
            }
            for (int k = 0; k < 4; ++k) prev[(size_t) k][(size_t) i] = g[k].active;
        }
    }
    std::printf ("   0%%: %d/%d, 50%%: %d/%d, 100%%: %d/%d реверсных зёрен\n",
                 backwards[0], counts[0], backwards[1], counts[1], backwards[2], counts[2]);
    const double fraction = counts[1] ? (double) backwards[1] / counts[1] : 0.0;
    suite::check (counts[0] > 300 && backwards[0] == 0 && backwards[2] == counts[2]
                  && counts[2] == counts[0], "0% и 100% задают направление всех новых зёрен");
    suite::check (fraction > 0.38 && fraction < 0.62,
                  "50% даёт смесь направлений", suite::num (fraction * 100.0, 2) + "%");
    suite::check (unequalSamples == 0 && backwards[1] == backwards[3],
                  "одинаковый seed и 50% воспроизводят звук побайтно",
                  suite::num (unequalSamples, 0) + " отличающихся отсчётов");
    std::printf ("   различия границ %.9f отсч., rate %.9f, рождения %d, выходы ядра %d, NaN/Inf %d\n",
                 maxPositionDifference, maxRateDifference, mismatchedGrains, badBounds, badValues);
    suite::check (mismatchedGrains == 0 && maxPositionDifference < 0.02 && maxRateDifference == 0.0,
                  "RNG направления не сдвигает ни Position, ни Spread");
    suite::check (badBounds == 0 && badValues == 0, "32/64-tap sinc не пересекает границы");

    // Автоматизация не может перевернуть уже играющее зерно. Первый отсчёт
    // сравнивается с контрольным экземпляром до рождения нового зерна.
    GranularEngine control, live;
    control.setBank (&bank); live.setBank (&bank);
    control.setSampleRate (sr); live.setSampleRate (sr);
    control.start (60, 1.0f); live.start (60, 1.0f);
    EngineParams c = p, l = p;
    c.p[1] = l.p[1] = 1.0f; c.p[3] = l.p[3] = 0.0f;
    control.prepare (c); live.prepare (l);
    for (int i = 0; i < 100; ++i) { (void) control.render (c); (void) live.render (l); }
    const auto before = GranularTestProbe::cursor (live, 0);
    l.grainReverse = 1.0f; live.prepare (l);
    const float firstControl = control.render (c), firstLive = live.render (l);
    int response = -1; bool heldUnchanged = true;
    for (int i = 1; i <= 1000 && response < 0; ++i)
    {
        (void) live.render (l);
        auto held = GranularTestProbe::cursor (live, 0);
        heldUnchanged = heldUnchanged && held.active && held.rate == before.rate;
        for (int j = 1; j < GranularEngine::kMaxGrains; ++j)
            if (GranularTestProbe::cursor (live, j).active
                && GranularTestProbe::cursor (live, j).rate < 0.0) { response = i; break; }
    }
    std::printf ("   автоматизация 0→100%%: первое reverse-зерно через %d отсч.; первый сэмпл %s\n",
                 response, equalBits (firstControl, firstLive) ? "совпадает" : "различается");
    suite::check (before.active && before.rate > 0.0 && heldUnchanged
                  && response > 0 && response <= 600 && equalBits (firstControl, firstLive),
                  "автоматизация меняет только новые зёрна, без скачка текущего");

    // Отдельно проверяем начало обратного прохода для компенсации огибающей:
    // заведомо громкая последняя 10-мс ячейка не должна считаться тихим стартом.
    SampleBank envBank; envBank.shots.push_back (stationary (sr, 2.0));
    EngineParams ep; ep.p[0] = 0.4f; ep.p[1] = 1.0f; ep.p[2] = 0.0f;
    ep.p[3] = 0.0f; ep.detune = 0.0f;
    GranularEngine probe; probe.setBank (&envBank); probe.setSampleRate (sr);
    probe.start (45, 1.0f); probe.prepare (ep); (void) probe.render (ep);
    const auto location = GranularTestProbe::cursor (probe, 0);
    OneShot& source = envBank.shots[0];
    const double low = location.pos - location.rate;
    const double high = low + location.dur * location.rate;
    const int cell = std::clamp ((int) (high / source.envStep), 0, (int) source.env.size() - 1);
    std::fill (source.env.begin(), source.env.end(), 0.1f);
    source.env[(size_t) cell] = 0.8f; source.bodyRms = 0.1f;
    source.envEnergyPrefix.assign (source.env.size() + 1, 0.0);
    for (size_t i = 0; i < source.env.size(); ++i)
        source.envEnergyPrefix[i + 1] = source.envEnergyPrefix[i] + (double) source.env[i] * source.env[i];
    GranularEngine fwd, rev;
    fwd.setBank (&envBank); rev.setBank (&envBank);
    fwd.setSampleRate (sr); rev.setSampleRate (sr);
    fwd.start (45, 1.0f); rev.start (45, 1.0f);
    EngineParams er = ep; er.grainReverse = 1.0f;
    fwd.prepare (ep); rev.prepare (er); (void) fwd.render (ep); (void) rev.render (er);
    const auto a = GranularTestProbe::cursor (fwd, 0), b = GranularTestProbe::cursor (rev, 0);
    const double ampRatio = b.amp / std::max (1.0e-20, a.amp);
    std::printf ("   env на верхнем краю: amp reverse/forward %.6f\n", ampRatio);
    suite::check (location.active && a.active && b.active && b.rate < 0.0 && ampRatio < 0.5,
                  "компенсация учитывает фактическое начало обратного прохода");

    // Обратное зерно может закончиться на громкой атаке при тихом старте.
    // Окно Ханна ослабляет край, но компенсация не должна поднять всю
    // атаку в несколько раз только потому, что читаем тот же участок назад.
    const int lowCell = std::clamp ((int) (low / source.envStep), 0, (int) source.env.size() - 1);
    std::fill (source.env.begin(), source.env.end(), 0.1f); source.env[(size_t) lowCell] = 0.8f;
    for (size_t i = 0; i < source.env.size(); ++i)
        source.envEnergyPrefix[i + 1] = source.envEnergyPrefix[i] + (double) source.env[i] * source.env[i];
    fwd.start (45, 1.0f); rev.start (45, 1.0f);
    fwd.prepare (ep); rev.prepare (er); (void) fwd.render (ep); (void) rev.render (er);
    const auto loudLowFwd = GranularTestProbe::cursor (fwd, 0);
    const auto loudLowRev = GranularTestProbe::cursor (rev, 0);
    const double loudLowRatio = loudLowRev.amp / std::max (1.0e-20, loudLowFwd.amp);
    std::printf ("   env на нижнем краю: amp reverse/forward %.6f\n", loudLowRatio);
    suite::check (loudLowFwd.active && loudLowRev.active && loudLowRev.rate < 0.0 && loudLowRatio <= 1.01,
                  "реверс не усиливает громкий противоположный край зерна");

    std::printf ("\n=================================================\n%s (провалов: %d)\n",
                 suite::gFailures ? "ЕСТЬ ПРОВАЛЫ" : "ВСЕ ТЕСТЫ ПРОЙДЕНЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
