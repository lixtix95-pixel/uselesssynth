// Сравнение двух независимых слоёв Granular в одном настоящем Voice.
// 48 кГц, stereo float32, без мастер-FX и пост-нормировки; после 1.5 с Note Off.
// Voice содержит штатные фильтры и soft clip; это не сырой выход Granular.
// make build/render_granular_spread
// ./build/render_granular_spread <file.wav> [spread 0..1] [reverse 0..1]
#include "../src/instrument/voice.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace orogen;

static bool writeWav (const char* path, const std::vector<float>& samples, uint32_t sr)
{
    FILE* f = std::fopen (path, "wb"); if (f == nullptr) return false;
    const uint32_t bytes = (uint32_t) (samples.size() * sizeof (float));
    const auto u32 = [f] (uint32_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    const auto u16 = [f] (uint16_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (48u + bytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (2); u32 (sr);
    u32 (sr * 8); u16 (8); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((uint32_t) (samples.size() / 2));
    std::fwrite ("data", 1, 4, f); u32 (bytes);
    const bool ok = std::fwrite (samples.data(), sizeof (float), samples.size(), f) == samples.size();
    return std::fclose (f) == 0 && ok;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: %s <file.wav> [spread] [reverse]\n", argv[0]); return 2; }
    constexpr int sr = 48000, frames = sr * 2;
    const float spread = argc > 2 ? clamp ((float) std::atof (argv[2]), 0.0f, 1.0f) : 0.6f;
    const float reverse = argc > 3 ? clamp ((float) std::atof (argv[3]), 0.0f, 1.0f) : 0.0f;
    EngineBank bank; bank.build (sr); Patch patch = patchInit(); patch.width = 1.0f;
    auto& a = patch.slots[0]; a.on = true; a.type = EngineType::Granular;
    a.params.table = 0; a.params.level = 0.5f; a.params.detune = 0.0f;
    a.params.pan = -1.0f; a.params.p[0] = 0.4f; a.params.p[1] = 0.2f;
    a.params.p[2] = 0.7f; a.params.p[3] = spread; a.params.grainReverse = reverse;
    a.attack = 0.005f; a.decay = 0.1f; a.sustain = 1.0f; a.release = 0.1f;
    a.tail = 0.0f; a.cutoff = 1.0f;
    patch.slots[1] = a; patch.slots[1].params.pan = 1.0f;
    for (int i = 2; i < 8; ++i) patch.slots[i].on = false;
    Voice voice; voice.init (bank, sr); voice.stageSlots (patch, bank); voice.applyPendingEngines();
    voice.start (patch, 60, 1.0f, 60.0f, 123);
    std::vector<float> data ((size_t) frames * 2); double ll=0, rr=0, lr=0, diff=0; float peak=0; int bad=0;
    for (int i = 0; i < frames; ++i)
    {
        if (i == sr * 3 / 2) voice.noteOff();
        float l = 0.0f, r = 0.0f; voice.render (patch, l, r);
        if (! std::isfinite (l) || ! std::isfinite (r)) ++bad;
        data[(size_t) 2*i] = l; data[(size_t) 2*i + 1] = r;
        ll += (double) l*l; rr += (double) r*r; lr += (double) l*r;
        diff += ((double) l-r) * ((double) l-r);
        peak = std::max ({ peak, std::fabs (l), std::fabs (r) });
    }
    if (bad || ! writeWav (argv[1], data, sr)) return 1;
    std::printf ("%s: stereo float32 %d Hz, %d frames, Spread %.3f Reverse %.3f\n", argv[1], sr, frames, spread, reverse);
    std::printf ("peak %.8f, L/R RMS %.8f/%.8f, corr %.8f, ΔRMS/RMS %.8f, NaN/Inf %d\n",
                 peak, std::sqrt (ll/frames), std::sqrt (rr/frames), lr/std::sqrt (std::max (1.0e-30,ll*rr)),
                 std::sqrt (diff/std::max (1.0e-30,(ll+rr)*0.5)), bad);
    return 0;
}
