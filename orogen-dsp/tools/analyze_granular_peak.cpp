// Локализация пика именно на выходе Granular, без Voice/фильтра/FX/лимитера.
// Сценарий соответствует render_granular_raw: 48 кГц, шесть секунд,
// Density 0→1 на 2-й секунде, Position 0.35→0.90 на 4-й.
// Пробник считывает состояние, но не меняет параметры или PCM движка.
#include "../src/engines/granular.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace orogen;
namespace orogen {
struct GranularTestProbe
{
    struct Detail
    {
        int active = 0;
        double position = 0.0, lower = 0.0, upper = 0.0;
        double read = 0.0, window = 0.0, amplitude = 0.0, contribution = 0.0;
        double edgeLow = 0.0, edgeHigh = 0.0, spanRms = 0.0;
        double preDc = 0.0, output = 0.0, gain = 0.0;
    };
    static void snapshot (const GranularEngine& e,
                          const std::array<double, GranularEngine::kMaxGrains>& starts,
                          Detail& d)
    {
        d = {};
        d.preDc = e.dcX; d.output = e.dcY; d.gain = e.gainSm.getCurrent();
        double largest = -1.0;
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            const auto& g = e.grains[i];
            if (! g.active) continue;
            ++d.active;
            const double pos = g.pos - g.rate;
            const float phase = g.phase - 1.0f / g.dur;
            const double sample = g.mip.read (pos);
            const double window = 0.5f - 0.5f * fastCosTbl (phase);
            const double contribution = sample * window * g.amp * d.gain;
            if (std::fabs (contribution) <= largest) continue;
            largest = std::fabs (contribution);
            d.position = pos; d.read = sample; d.window = window; d.amplitude = g.amp;
            d.contribution = contribution;
            const double start = starts[(size_t) i];
            const double end = start + (double) g.dur * g.rate;
            d.lower = std::min (start, end); d.upper = std::max (start, end);
            d.edgeLow = e.shot->granularEnvAt (d.lower);
            d.edgeHigh = e.shot->granularEnvAt (d.upper);
            const double span = d.upper - d.lower;
            const int probes = clamp ((int) std::ceil (2.0 * span / e.shot->envStep), 8, 256);
            double sum = 0.0, weights = 0.0;
            for (int j = 0; j < probes; ++j)
            {
                const float ph = ((float) j + 0.5f) / (float) probes;
                const float w = 0.5f - 0.5f * fastCosTbl (ph);
                const float env = e.shot->granularEnvAt (d.lower + ph * span);
                sum += (double) w * w * env * env; weights += (double) w * w;
            }
            d.spanRms = std::sqrt (sum / weights);
        }
    }
    static void birthPositions (const GranularEngine& e,
                                std::array<bool, GranularEngine::kMaxGrains>& active,
                                std::array<double, GranularEngine::kMaxGrains>& starts)
    {
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            const auto& g = e.grains[i];
            if (g.active && ! active[(size_t) i]) starts[(size_t) i] = g.pos - g.rate;
            active[(size_t) i] = g.active;
        }
    }
};
}

int main()
{
    constexpr int sr = 48000;
    SampleBank bank; bank.buildFactory (sr);
    for (int source : { 0, 2, 6 })
    {
        GranularEngine e; e.setBank (&bank); e.setSampleRate (sr); e.start (60, 1.0f);
        EngineParams p; p.table = source; p.p[0] = 0.35f; p.p[1] = 0.45f;
        p.p[2] = 0.0f; p.p[3] = 0.20f; p.level = 1.0f; p.detune = 0.0f;
        e.prepare (p);
        float peak = 0.0f; int at = 0, over = 0, firstOver = 0, bad = 0;
        double sumSq = 0.0;
        std::array<bool, GranularEngine::kMaxGrains> active {};
        std::array<double, GranularEngine::kMaxGrains> starts {};
        GranularTestProbe::Detail detail;
        for (int i = 0; i < 6 * sr; ++i)
        {
            if (i == 2 * sr) p.p[2] = 1.0f;
            if (i == 4 * sr) p.p[0] = 0.90f;
            if (i % 128 == 0) e.prepare (p);
            const float x = e.render (p);
            GranularTestProbe::birthPositions (e, active, starts);
            if (! std::isfinite (x)) ++bad;
            sumSq += (double) x * x;
            if (std::fabs (x) > 1.0f) { ++over; if (i < 2 * sr) ++firstOver; }
            if (std::fabs (x) > peak)
            {
                peak = std::fabs (x); at = i;
                GranularTestProbe::snapshot (e, starts, detail);
            }
        }
        const auto& s = bank.shots[(size_t) source];
        std::printf ("%s (%d): peak %.6f t=%.6f с, RMS %.6f, |x|>1: %d/288000; первые 2 с: %d/96000; NaN %d\n",
                     s.name.c_str(), source, peak, (double) at / sr, std::sqrt (sumSq / (6 * sr)), over, firstOver, bad);
        std::printf ("   активных зёрен %d; до DC %.6f, после DC %.6f, kGain %.6f; вклад главного %.6f\n",
                     detail.active, detail.preDc, detail.output, detail.gain, detail.contribution);
        std::printf ("   позиция %.3f, источник %.6f, окно %.6f, gain зерна %.6f; RMS краёв %.6f/%.6f, Hann² RMS %.6f, body %.6f\n",
                     detail.position, detail.read, detail.window, detail.amplitude,
                     detail.edgeLow, detail.edgeHigh, detail.spanRms, s.bodyRms);
        if (bad) return 1;
    }
    return 0;
}
