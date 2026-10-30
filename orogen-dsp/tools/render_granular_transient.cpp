// Диагностический транзиент: тихое начало источника переходит в атаку внутри
// зерна. Рендер напрямую из Granular без FX, нормировки и лимитера.
// make build/render_granular_transient
// ./build/render_granular_transient audio/granular_transient.wav
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
    const char* path = argc > 1 ? argv[1] : "audio/granular_transient.wav";
    constexpr int sr = 48000, length = sr * 4, retrigger = sr / 4;
    OneShot shot;
    shot.name = "rising source"; shot.rootNote = 45;
    shot.levels.resize (1); shot.levels[0].resize (sr);
    for (int i = 0; i < sr; ++i)
    {
        const double t = (double) i / sr;
        const double amp = t < 0.20 ? 0.06 : (t < 0.23 ? 0.06 + 0.34 * (t - 0.20) / 0.03 : 0.40);
        shot.levels[0][(size_t) i] = (float) (amp * std::sin (kTwoPi * 220.0 * t));
    }
    buildMipLevels (shot, sr);
    SampleBank bank; bank.shots.push_back (std::move (shot));
    GranularEngine e; e.setBank (&bank); e.setSampleRate (sr);
    EngineParams p;
    p.table = 0; p.p[0] = 0.22f; p.p[1] = 0.45f; p.p[2] = 0.0f; p.p[3] = 0.0f;
    p.detune = 0.0f; p.level = 1.0f;

    std::vector<float> out; out.reserve (length);
    float peak = 0.0f; double power = 0.0; int bad = 0;
    for (int i = 0; i < length; ++i)
    {
        if (i % retrigger == 0) { e.start (45, 1.0f); e.prepare (p); }
        else if (i % 128 == 0) e.prepare (p);
        const float v = e.render (p);
        if (! std::isfinite (v)) ++bad;
        peak = std::max (peak, std::fabs (v)); power += (double) v * v;
        out.push_back (v);
    }
    if (bad != 0 || ! writeFloatWav (path, out, sr)) return 1;
    std::printf ("%s: 4 c, 48 kHz mono float32; Granular без FX, нормировки и лимитера\n", path);
    std::printf ("пик %.6f, RMS %.6f, NaN/Inf %d\n", peak, std::sqrt (power / length), bad);
    return 0;
}
