// Демо COMB и CHORUS из MonoJuice 0.9.5: 24 секунды, 5 секций.
//   audio/comb_chorus_demo.wav
//   1. COMB ODIN2: resonance 40 → 95 на пиле 110 Гц
//   2. COMB ODIN2: три схемы включения (A, A→B, A+B)
//   3. COMB ParallelComb: обратная связь 20 → 85 %
//   4. CHORUS из прошивки: микс 0 → 1, глубина и скорость растут
//   5. Всё вместе: drive → comb → chorus → plate
#include "../src/instrument/fx.h"
#include "../src/instrument/fx_comb.h"
#include "../src/instrument/fx_chorus_fw.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

using namespace orogen;

static void writeWav16 (const std::string& path, const std::vector<float>& l,
                        const std::vector<float>& r, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return;
    const uint32_t n = (uint32_t) l.size(), db = n * 4;
    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (36 + db); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (1); u16 (2); u32 ((uint32_t) sr);
    u32 ((uint32_t) sr * 4); u16 (4); u16 (16);
    std::fwrite ("data", 1, 4, f); u32 (db);
    for (uint32_t i = 0; i < n; ++i)
    {
        auto q = [] (float x) { return (int16_t) std::lrint (clamp (x, -1.0f, 1.0f) * 32767.0f); };
        const int16_t a = q (l[i]), b = q (r[i]);
        std::fwrite (&a, 2, 1, f); std::fwrite (&b, 2, 1, f);
    }
    std::fclose (f);
}

static float saw (double t, double hz)
{
    const double ph = hz * t;
    return (float) (2.0 * (ph - std::floor (ph)) - 1.0);
}

int main()
{
    const double sr = 48000.0;
    const double secs = 24.0;
    const int n = (int) (sr * secs);
    std::vector<float> outL ((size_t) n, 0.0f), outR ((size_t) n, 0.0f);

    // --- 1. ODIN2: resonance 40 → 95 (0–6 с)
    {
        CombFx c; c.prepare (sr);
        c.setModel (0); c.setMix (1.0f);
        c.setOdinFreq (110.0f, 165.0f);
        for (int i = 0; i < (int) (sr * 6); ++i)
        {
            const float res = 40.0f + 55.0f * (float) i / (float) (sr * 6);
            if ((i % 64) == 0) c.setOdinRes (res, res);
            float l = 0.35f * saw (i / sr, 110.0), r = l;
            c.process (l, r);
            outL[(size_t) i] = l * 0.45f;
            outR[(size_t) i] = r * 0.45f;
        }
        std::printf ("  1. COMB ODIN2: resonance 40 → 95\n");
    }

    // --- 2. три схемы включения (6–10 с)
    {
        for (int rt = 0; rt < 3; ++rt)
        {
            CombFx c; c.prepare (sr);
            c.setModel (0); c.setMix (1.0f);
            c.setOdinFreq (147.0f, 220.0f); c.setOdinRes (90.0f, 85.0f);
            c.setOdinRouting (rt);
            const int seg = (int) (sr * 1.33);
            for (int i = 0; i < seg; ++i)
            {
                const int t = (int) (sr * 6) + rt * seg + i;
                if (t >= n) break;
                float l = 0.4f * saw (i / sr, 147.0), r = 0.4f * saw (i / sr, 147.5);
                c.process (l, r);
                outL[(size_t) t] += l * 0.4f;
                outR[(size_t) t] += r * 0.4f;
            }
        }
        std::printf ("  2. COMB ODIN2: routing A / A→B / A+B\n");
    }

    // --- 3. ParallelComb: feedback 20 → 85 (10–16 с)
    {
        CombFx c; c.prepare (sr);
        c.setModel (1); c.setMix (1.0f);
        c.setParDelayMs (0, 40.0f); c.setParDelayMs (1, 63.0f);
        c.setParDelayMs (2, 89.0f); c.setParDelayMs (3, 120.0f);
        const int start = (int) (sr * 10);
        const int len = (int) (sr * 6);
        for (int i = 0; i < len; ++i)
        {
            if ((i % 128) == 0)
                c.setParFeedback (20.0f + 65.0f * (float) i / (float) len);
            // короткие импульсы каждые 0.5 с — слышно, как хвост растёт
            const int k = i % (int) (sr * 0.5);
            const float x = (k < 300) ? 0.8f * (float) std::sin (kTwoPi * 300.0 * k / sr) * (1.0f - k / 300.0f) : 0.0f;
            float l = x, r = x;
            c.process (l, r);
            outL[(size_t) (start + i)] += l * 0.3f;
            outR[(size_t) (start + i)] += r * 0.3f;
        }
        std::printf ("  3. COMB ParallelComb: feedback 20 → 85 %%\n");
    }

    // --- 4. CHORUS из прошивки: микс 0 → 1 (16–21 с)
    {
        FirmwareChorusFx ch; ch.prepare (sr);
        const int start = (int) (sr * 16);
        const int len = (int) (sr * 5);
        for (int i = 0; i < len; ++i)
        {
            const float u = (float) i / (float) len;
            if ((i % 64) == 0)
            {
                ch.setMix (u);
                ch.setParameters ({ { (int) (20 + 60 * u), (int) (20 + 100 * u), (int) (20 + 60 * u),
                                      127, (int) (20 + 40 * u), (int) (60 + 60 * u), 110, 100 } });
            }
            const float x = 0.35f * (float) std::sin (kTwoPi * 330.0 * i / sr)
                          + 0.25f * (float) std::sin (kTwoPi * 495.0 * i / sr);
            float l = x, r = 0.35f * (float) std::sin (kTwoPi * 331.0 * i / sr)
                        + 0.25f * (float) std::sin (kTwoPi * 497.0 * i / sr);
            ch.process (l, r);
            outL[(size_t) (start + i)] += l * 0.5f;
            outR[(size_t) (start + i)] += r * 0.5f;
        }
        std::printf ("  4. CHORUS из прошивки: mix 0 → 1\n");
    }

    // --- 5. всё вместе (21–24 с)
    {
        FxChain fx; fx.prepare (sr);
        FxChain::Settings s;
        s.drive = 0.35f; s.driveModel = (int) DriveModel::Tube;
        s.combMix = 0.55f; s.combModel = 0; s.combFreqA = 196.0f; s.combFreqB = 294.0f;
        s.combResA = 88.0f; s.combResB = 82.0f; s.combRouting = 2;
        s.chorusModel = 1; s.chorusMix = 0.4f;
        s.chorusFw = { { 30, 70, 40, 127, 35, 90, 110, 100 } };
        s.plateMix = 0.3f;
        fx.set (s);
        const int start = (int) (sr * 21);
        for (int i = 0; i < (int) (sr * 3) && start + i < n; ++i)
        {
            float l = 0.4f * saw (i / sr, 196.0), r = 0.4f * saw (i / sr, 196.5);
            fx.process (l, r);
            outL[(size_t) (start + i)] += l;
            outR[(size_t) (start + i)] += r;
        }
        std::printf ("  5. Всё вместе: drive → comb → chorus → plate\n");
    }

    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max (peak, std::max (std::fabs (outL[(size_t) i]), std::fabs (outR[(size_t) i])));
    const float g = (peak > 1.0e-6f) ? 0.89f / peak : 1.0f;
    for (int i = 0; i < n; ++i) { outL[(size_t) i] *= g; outR[(size_t) i] *= g; }

    writeWav16 ("audio/comb_chorus_demo.wav", outL, outR, (int) sr);

    double acc = 0.0; float pk = 0.0f;
    for (int i = 0; i < n; ++i) { acc += (double) outL[(size_t) i] * outL[(size_t) i]; pk = std::max (pk, std::fabs (outL[(size_t) i])); }
    std::printf ("  записано audio/comb_chorus_demo.wav: %.0f с, пик %.3f, RMS %.4f (до нормировки пик %.3f)\n",
                 secs, pk, std::sqrt (acc / n), peak);
    for (int k = 0; k < 6; ++k)
    {
        const int a = (int) (sr * 4.0 * k), b = std::min (n, a + (int) (sr * 4.0));
        double e = 0.0;
        for (int i = a; i < b; ++i) e += (double) outL[(size_t) i] * outL[(size_t) i];
        std::printf ("   %2d–%2d c: RMS %.4f\n", 4 * k, 4 * (k + 1), std::sqrt (e / std::max (1, b - a)));
    }
    return 0;
}
