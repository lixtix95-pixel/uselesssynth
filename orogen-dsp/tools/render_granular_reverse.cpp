// Сравнение Granular с обратными зёрнами и без них.
// Два отдельных рендера с одинаковыми seed, источником и параметрами;
// только вероятность реверса задаётся аргументом. Выход — непосредственно
// двигатель: mono IEEE float32, без фильтра, FX, лимитера или нормировки файла.
// make build/render_granular_reverse
// ./build/render_granular_reverse <файл.wav> <0..1> [источник 0..7]
#include "../src/engines/granular.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace orogen;

static bool writeWav (const char* path, const std::vector<float>& samples, uint32_t sr)
{
    FILE* f = std::fopen (path, "wb");
    if (! f) return false;
    const uint32_t bytes = (uint32_t) (samples.size() * sizeof (float));
    const auto u32 = [f] (uint32_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    const auto u16 = [f] (uint16_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (48 + bytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (1); u32 (sr);
    u32 (sr * 4); u16 (4); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((uint32_t) samples.size());
    std::fwrite ("data", 1, 4, f); u32 (bytes);
    const bool ok = std::fwrite (samples.data(), sizeof (float), samples.size(), f) == samples.size();
    return std::fclose (f) == 0 && ok;
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: %s <file.wav> <probability 0..1> [source 0..7]\n", argv[0]);
        return 2;
    }
    constexpr int sr = 48000, length = sr * 6;
    const float probability = std::clamp ((float) std::atof (argv[2]), 0.0f, 1.0f);
    const int table = argc > 3 ? std::clamp (std::atoi (argv[3]), 0, 7) : 0;
    SampleBank bank; bank.buildFactory (sr);
    GranularEngine engine; engine.setBank (&bank); engine.setSampleRate (sr); engine.start (60, 1.0f);
    EngineParams p;
    p.table = table; p.p[0] = 0.35f; p.p[1] = 0.75f; p.p[2] = 0.30f; p.p[3] = 0.20f;
    p.detune = 0.0f; p.level = 1.0f; p.grainReverse = probability; engine.prepare (p);
    std::vector<float> out; out.reserve (length);
    int bad = 0;
    for (int i = 0; i < length; ++i)
    {
        const float x = engine.render (p);
        if (! std::isfinite (x)) ++bad;
        out.push_back (x);
    }
    // Как и в render_granular_raw: обрезать на последнем переходе через ноль,
    // не изменяя отсчёты и не создавая щелчок произвольного конца файла.
    for (int i = length - 1; i > length - 1000; --i)
        if ((out[(size_t) i] >= 0.0f) != (out[(size_t) (i - 1)] >= 0.0f))
        { out.resize ((size_t) i); break; }
    double sumSq = 0.0, sum = 0.0; float peak = 0.0f, step = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const float x = out[i];
        if (i > 0) step = std::max (step, std::fabs (x - out[i - 1]));
        sum += x; sumSq += (double) x * x; peak = std::max (peak, std::fabs (x));
    }
    if (bad || ! writeWav (argv[1], out, sr))
    {
        std::fprintf (stderr, "write failed or NaN/Inf=%d\n", bad); return 1;
    }
    std::printf ("%s: %.6f s, 48 kHz, mono float32, source %d, reverse %.1f%%\n",
                 argv[1], (double) out.size() / sr, table, 100.0f * probability);
    std::printf ("raw DSP: peak %.6f, RMS %.6f, DC %.6f, max |dx| %.6f, NaN/Inf %d\n",
                 peak, std::sqrt (sumSq / out.size()), sum / out.size(), step, bad);
    return 0;
}
