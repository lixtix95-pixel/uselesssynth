// Высота вокальных источников НА ВЫХОДЕ Granular, не только в SampleBank.
// Одно отдельное зерно на корневой ноте; FX/Voice/лимитера нет. Измеряем
// периоды нормированной по окну формы в середине зерна, в обоих направлениях.
#include "../src/engines/granular.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace orogen;

struct Result { int periods = 0, bad = 0; double minHz = 1.0e9, maxHz = 0.0; };

static Result measure (const SampleBank& bank, int sr, float position, float reverse)
{
    GranularEngine engine; engine.setBank (&bank); engine.setSampleRate (sr); engine.start (45, 1.0f);
    EngineParams p; p.table = 0; p.p[0] = position; p.p[1] = 0.8f; p.p[2] = 0.0f; p.p[3] = 0.0f;
    p.detune = 0.0f; p.level = 1.0f; p.grainReverse = reverse; engine.prepare (p);
    constexpr double duration = 0.008 + 0.8 * 0.212;
    const int n = (int) (0.16 * sr);
    std::vector<double> x ((size_t) n, 0.0);
    Result r;
    for (int i = 0; i < n; ++i)
    {
        const float v = engine.render (p);
        if (! std::isfinite (v)) ++r.bad;
        const double phase = (double) i / (duration * sr);
        const double window = 0.5 - 0.5 * std::cos (kTwoPi * phase);
        if (i > sr / 40 && window > 0.1) x[(size_t) i] = v / window;
    }
    const int a = (int) (sr * 0.035), b = (int) (sr * 0.145);
    const double threshold = 0.65 * *std::max_element (x.begin() + a, x.begin() + b);
    std::vector<int> peaks;
    for (int i = a; i < b; ++i)
    {
        if (x[(size_t) i] <= threshold || x[(size_t) i] < x[(size_t) (i - 1)]
            || x[(size_t) i] <= x[(size_t) (i + 1)]) continue;
        if (! peaks.empty() && i - peaks.back() < sr / 180)
        {
            if (x[(size_t) i] > x[(size_t) peaks.back()]) peaks.back() = i;
        }
        else peaks.push_back (i);
    }
    std::vector<double> times;
    for (int i : peaks)
    {
        const double left = x[(size_t) (i - 1)], centre = x[(size_t) i], right = x[(size_t) (i + 1)];
        const double d = left - 2.0 * centre + right;
        const double frac = std::fabs (d) > 1.0e-20 ? 0.5 * (left - right) / d : 0.0;
        times.push_back ((i + frac) / sr);
    }
    for (size_t i = 1; i < times.size(); ++i)
    {
        const double hz = 1.0 / (times[i] - times[i - 1]);
        r.minHz = std::min (r.minHz, hz); r.maxHz = std::max (r.maxHz, hz); ++r.periods;
    }
    return r;
}

int main()
{
    int passed = 0, failed = 0;
    for (int sr : { 44100, 48000, 96000 })
        for (auto kind : { OneShotKind::Choir, OneShotKind::VoxAh })
        {
            SampleBank bank; bank.shots.push_back (buildOneShot (kind, sr, 2.0));
            double lo = 1.0e9, hi = 0.0; int periods = 0, bad = 0, missing = 0;
            for (float position : { 0.60f, 0.70f, 0.80f, 0.90f })
                for (float reverse : { 0.0f, 1.0f })
                {
                    const auto r = measure (bank, sr, position, reverse);
                    lo = std::min (lo, r.minHz); hi = std::max (hi, r.maxHz);
                    periods += r.periods; bad += r.bad; if (r.periods < 5) ++missing;
                }
            // Запас больше глубины вокального вибрато, но намного меньше
            // ошибки f(t)*t. Несколько отсчётов допускаем для окна/DC-фильтра.
            const bool ok = missing == 0 && bad == 0 && lo > 109.2 && hi < 110.8;
            std::printf ("  [%s] Granular/%s %d Hz: %d периодов, %.3f..%.3f Hz, "
                         "8 позиций/направлений, missing %d, NaN %d\n",
                         ok ? " OK " : "FAIL", kind == OneShotKind::Choir ? "Choir" : "Vox",
                         sr, periods, lo, hi, missing, bad);
            if (ok) ++passed; else ++failed;
        }
    std::printf ("Granular vocal: %d OK, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}
