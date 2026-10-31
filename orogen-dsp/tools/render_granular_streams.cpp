// Стерео-диагностика независимых потоков Granular слотов A/B.
// Голос/фильтры и встроенный soft clip присутствуют; мастер-FX и нормировки нет.
// ./build/render_granular_streams file.wav probability [serial]
#include "../src/instrument/voice.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace orogen;

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: %s file.wav probability [serial]\n", argv[0]); return 2; }
    constexpr int sr = 48000, frames = sr * 4;
    const float chance = std::clamp ((float) std::atof (argv[2]), 0.0f, 1.0f);
    const uint32_t serial = argc > 3 ? (uint32_t) std::strtoul (argv[3], nullptr, 10) : 123u;
    EngineBank bank; bank.build (sr);
    Patch p = patchInit(); p.width = 1.0f;
    auto& a = p.slots[0];
    a.on = true; a.type = EngineType::Granular; a.params.table = 0;
    a.params.level = 0.5f; a.params.detune = 0.0f;
    a.params.p[0] = 0.4f; a.params.p[1] = 0.2f; a.params.p[2] = 1.0f; a.params.p[3] = 0.0f;
    a.params.grainReverse = chance; a.params.pan = -1.0f;
    a.attack = 0.005f; a.decay = 0.1f; a.sustain = 1.0f; a.release = 0.1f; a.tail = 0.0f; a.cutoff = 1.0f;
    p.slots[1] = a; p.slots[1].params.pan = 1.0f;
    for (int i = 2; i < 8; ++i) p.slots[i].on = false;
    Voice v; v.init (bank, sr); v.stageSlots (p, bank); v.applyPendingEngines(); v.start (p, 60, 1.0f, 60.0f, serial);
    std::vector<float> out ((size_t) frames * 2);
    double ll = 0.0, rr = 0.0, lr = 0.0, delta = 0.0; float peak = 0.0f; int bad = 0;
    for (int i = 0; i < frames; ++i)
    {
        float l = 0.0f, r = 0.0f;
        v.render (p, l, r);
        if (! std::isfinite (l) || ! std::isfinite (r)) ++bad;
        out[(size_t) (i * 2)] = l; out[(size_t) (i * 2 + 1)] = r;
        ll += (double) l * l; rr += (double) r * r; lr += (double) l * r;
        delta += ((double) l - r) * ((double) l - r);
        peak = std::max ({ peak, std::fabs (l), std::fabs (r) });
    }
    // Окончание на нулевом переходе БЛИЖАЙШЕМ К КОНЦУ по обоим каналам.
    // Сами отсчёты не меняются; оба канала заканчиваются в одном кадре.
    int end = frames;
    float best = std::fabs (out[(size_t) (2 * (frames - 1))]) + std::fabs (out[(size_t) (2 * (frames - 1) + 1)]);
    for (int i = frames - 2; i > frames - 1000; --i)
    {
        const float score = std::fabs (out[(size_t) (2 * i)]) + std::fabs (out[(size_t) (2 * i + 1)]);
        if (score < best) { best = score; end = i + 1; }
    }
    out.resize ((size_t) end * 2);
    FILE* f = std::fopen (argv[1], "wb");
    if (! f || bad) { if (f) std::fclose (f); return 1; }
    const uint32_t bytes = (uint32_t) (out.size() * sizeof (float));
    const auto u32 = [f] (uint32_t n) { std::fwrite (&n, 4, 1, f); };
    const auto u16 = [f] (uint16_t n) { std::fwrite (&n, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (48u + bytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (2); u32 (sr); u32 (sr * 8); u16 (8); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((uint32_t) end);
    std::fwrite ("data", 1, 4, f); u32 (bytes);
    const bool ok = std::fwrite (out.data(), sizeof (float), out.size(), f) == out.size();
    const bool closed = std::fclose (f) == 0;
    std::printf ("%s: %.3f с, 48 кГц stereo float32, reverse %.0f%%, serial %u\n", argv[1], (double) end / sr, chance * 100.0f, serial);
    std::printf ("   корреляция L/R %.6f, ΔRMS/RMS %.6f, peak %.6f, NaN/Inf %d\n",
                 lr / std::sqrt (std::max (1.0e-30, ll * rr)), std::sqrt (delta / std::max (1.0e-30, (ll + rr) * 0.5)), peak, bad);
    return ok && closed ? 0 : 1;
}
