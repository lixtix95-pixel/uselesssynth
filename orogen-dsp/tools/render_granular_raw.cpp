// Неподкрашенный диагностический рендер Granular.
//
// 0–2 с: Density 0 (4 зерна/с); в 2 с — ступень Density на 1 (90/с);
// в 4 с Position меняется с 0.35 на 0.90 (сглаживание — внутри движка).
// Файл содержит сами отсчёты движка: нет фильтра, FX, компрессора, лимитера,
// нормировки, поканальной балансировки или изменения усиления после рендера.
// IEEE float32 сохраняет реальные значения, в том числе при возможном пике >1.
//
// make build/render_granular_raw
// ./build/render_granular_raw audio/granular_raw.wav [номер источника 0..7]
#include "../src/engines/granular.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace orogen;

static bool writeFloatWav (const char* path, const std::vector<float>& samples, int sr)
{
    FILE* f = std::fopen (path, "wb");
    if (f == nullptr) return false;
    const uint32_t bytes = (uint32_t) (samples.size() * sizeof (float));
    const auto u32 = [f] (uint32_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    const auto u16 = [f] (uint16_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (48u + bytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (1); u32 ((uint32_t) sr);
    u32 ((uint32_t) sr * 4); u16 (4); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((uint32_t) samples.size());
    std::fwrite ("data", 1, 4, f); u32 (bytes);
    const bool ok = std::fwrite (samples.data(), sizeof (float), samples.size(), f) == samples.size();
    return std::fclose (f) == 0 && ok;
}

int main (int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "audio/granular_raw.wav";
    constexpr int sr = 48000, length = 6 * sr;
    SampleBank bank;
    bank.buildFactory (sr);
    GranularEngine e;
    e.setBank (&bank); e.setSampleRate (sr); e.start (60, 1.0f);
    EngineParams p;
    p.table = argc > 2 ? std::clamp (std::atoi (argv[2]), 0, (int) bank.shots.size() - 1) : 2;
    p.p[0] = 0.35f; p.p[1] = 0.45f; p.p[2] = 0.0f; p.p[3] = 0.20f;
    p.level = 1.0f; p.tuneSemis = 0.0f; p.detune = 0.0f;
    e.prepare (p);

    std::vector<float> out;
    out.reserve ((size_t) length);
    int bad = 0;
    for (int i = 0; i < length; ++i)
    {
        if (i == 2 * sr) p.p[2] = 1.0f;
        if (i == 4 * sr) p.p[0] = 0.90f;
        if (i % 128 == 0) e.prepare (p);
        const float x = e.render (p);
        if (! std::isfinite (x)) ++bad;
        out.push_back (x);
    }

    // Обрезка на последнем переходе через ноль не изменяет ни один отсчёт;
    // конец файла не создаёт щелчок из-за произвольного разрыва проигрывателя.
    for (int i = length - 1; i > length - 1000; --i)
        if ((out[(size_t) i] >= 0.0f) != (out[(size_t) (i - 1)] >= 0.0f))
        { out.resize ((size_t) i); break; }

    double energy = 0.0, sum = 0.0;
    float peak = 0.0f, maxStep = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
    {
        energy += (double) out[i] * out[i]; sum += out[i];
        peak = std::max (peak, std::fabs (out[i]));
        if (i > 0) maxStep = std::max (maxStep, std::fabs (out[i] - out[i - 1]));
    }
    if (bad != 0 || ! writeFloatWav (path, out, sr))
    {
        std::fprintf (stderr, "не удалось записать %s; NaN/Inf %d\n", path, bad);
        return 1;
    }
    std::printf ("%s: %.3f с, источник %d, 48 kHz, mono float32, без FX и нормировки\n",
                 path, (double) out.size() / sr, p.table);
    std::printf ("пик %.6f, RMS %.6f, DC %.6f, max |dx| %.6f, NaN %d\n",
                 peak, std::sqrt (energy / out.size()), sum / out.size(), maxStep, bad);
    std::printf ("Density шаг: |dx| %.6f; Position шаг: |dx| %.6f\n",
                 std::fabs (out[(size_t) (2 * sr)] - out[(size_t) (2 * sr - 1)]),
                 std::fabs (out[(size_t) (4 * sr)] - out[(size_t) (4 * sr - 1)]));
    return 0;
}
