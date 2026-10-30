// Проверка pitch на поздней позиции длинного источника.
// Прямой выход Granular в float WAV: без FX, нормировки и лимитера.
// make build/render_granular_pitch
// ./build/render_granular_pitch audio/granular_pitch.wav
#include "../src/engines/granular.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
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
    const char* path = argc > 1 ? argv[1] : "audio/granular_pitch.wav";
    constexpr int sr = 96000, sourceSamples = sr * 26, length = sr * 3, retrigger = sr / 4;
    constexpr double sourceHz = 500.0;
    OneShot shot; shot.name = "pitch precision"; shot.rootNote = 45;
    shot.levels.resize (1); shot.levels[0].resize (sourceSamples);
    for (int i = 0; i < sourceSamples; ++i)
        shot.levels[0][(size_t) i] = (float) (0.30 * std::sin (kTwoPi * sourceHz * i / sr));
    buildMipLevels (shot, sr);
    SampleBank bank; bank.shots.push_back (std::move (shot));
    GranularEngine e; e.setBank (&bank); e.setSampleRate (sr);
    EngineParams p;
    p.table = 0; p.p[0] = 0.95f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f;
    p.detune = 0.0f; p.level = 1.0f;

    std::vector<float> out; out.reserve (length);
    double power = 0.0, firstCross = 0.0, lastCross = 0.0;
    float peak = 0.0f, previous = 0.0f; int bad = 0, crosses = 0;
    for (int i = 0; i < length; ++i)
    {
        if (i % retrigger == 0) { e.start (36, 1.0f); e.prepare (p); }
        const float v = e.render (p);
        if (! std::isfinite (v)) ++bad;
        peak = std::max (peak, std::fabs (v)); power += (double) v * v;
        // Частота первого зерна по пересечениям нуля, внутри его Hann-окна.
        if (i > sr / 50 && i < 18 * sr / 100 && previous < 0.0f && v >= 0.0f)
        {
            const double cross = i - 1.0 + (double) (-previous) / (double) (v - previous);
            if (crosses++ == 0) firstCross = cross;
            lastCross = cross;
        }
        previous = v; out.push_back (v);
    }
    if (bad != 0 || crosses < 5 || ! writeFloatWav (path, out, sr)) return 1;
    const double measured = (crosses - 1) * (double) sr / (lastCross - firstCross);
    const double target = sourceHz * std::pow (2.0, -9.0 / 12.0);
    std::printf ("%s: 3 с, 96 kHz mono float32; Granular без FX, нормировки и лимитера\n", path);
    std::printf ("частота %.6f Гц, эталон %.6f Гц, ошибка %+.4f цента; пик %.6f, RMS %.6f, NaN %d\n",
                 measured, target, 1200.0 * std::log2 (measured / target), peak, std::sqrt (power / length), bad);
    return 0;
}
