// Офлайн-рендер демо движка Wavetable: пэд-аккорды + арпеджио, SVF + ADSR, стерео.
// Результат: audio/wavetable_demo.wav (16 бит, 48 кГц, стерео, 20 c).
#include "../src/engines/wavetable.h"
#include "../src/core/svf.h"
#include "../src/core/adsr.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

using namespace orogen;

static void writeWav16 (const std::string& path, const std::vector<float>& l,
                        const std::vector<float>& r, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::printf ("не могу открыть %s\n", path.c_str()); return; }
    const uint32_t n = (uint32_t) l.size();
    const uint32_t dataBytes = n * 2 * 2;
    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (1); u16 (2); u32 ((uint32_t) sr);
    u32 ((uint32_t) sr * 4); u16 (4); u16 (16);
    std::fwrite ("data", 1, 4, f); u32 (dataBytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        auto q = [] (float x) { return (int16_t) std::lrint (clamp (x, -1.0f, 1.0f) * 32767.0f); };
        const int16_t a = q (l[i]), b = q (r[i]);
        std::fwrite (&a, 2, 1, f); std::fwrite (&b, 2, 1, f);
    }
    std::fclose (f);
}

// ---------------------------------------------------------------------------
struct PadVoice
{
    WavetableEngine eng;
    Adsr            env;
    Svf             svf;
    int             note = 60;
    int             chord = 0;
    float           pan = 0.0f;
    bool            released = false;
};

struct PluckVoice
{
    WavetableEngine eng;
    Adsr            env;
    Svf             svf;
    float           pan = 0.0f;
    int             table = 1;
    bool            used = false;
};

int main()
{
    constexpr int    sr     = 48000;
    constexpr double length = 20.0;
    constexpr double chordDur = 4.0;
    const int total = (int) (sr * length);

    std::printf ("построение банка…\n");
    WavetableBank bank;
    bank.buildFactory (6);
    bank.save ("build/factory.orowt");

    // аккорды: Dm9 -> Bbmaj7 -> F/A -> C(add9)
    const int chords[4][4] = {
        { 50, 57, 60, 65 },
        { 46, 53, 57, 62 },
        { 45, 52, 57, 60 },
        { 48, 55, 60, 64 },
    };

    std::vector<PadVoice> pads;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i)
        {
            pads.push_back ({});
            PadVoice& v = pads.back();
            v.eng.setBank (&bank);
            v.eng.setSampleRate (sr);
            v.note  = chords[c][i] + 12;
            v.chord = c;
            v.pan   = (float) (i - 1.5) * 0.5f;
            v.env.setSampleRate (sr);
            v.env.setParams (1.4f, 1.8f, 0.72f, 2.2f);
            v.svf.setSampleRate (sr);
            v.svf.setParams (900.0f, 0.22f, 0.08f);
        }

    std::vector<PluckVoice> plucks (48);          // пул голосов арпеджио
    for (auto& pv : plucks) { pv.eng.setBank (&bank); pv.eng.setSampleRate (sr);
                              pv.env.setSampleRate (sr); pv.svf.setSampleRate (sr); }

    std::vector<float> outL ((size_t) total, 0.0f), outR ((size_t) total, 0.0f);

    int    curChord = -1;
    size_t nextPluck = 0;
    Rng    rng (777);

    for (int i = 0; i < total; ++i)
    {
        const double t = (double) i / sr;
        const int    chord = std::min (3, (int) (t / chordDur));

        // ---- смена аккорда: старые в release, новые в note-on
        if (chord != curChord)
        {
            if (curChord >= 0)
                for (auto& v : pads)
                    if (v.chord == curChord) { v.env.noteOff(); v.released = true; }

            for (auto& v : pads)
                if (v.chord == chord)
                {
                    EngineParams p;
                    p.table = 0;
                    p.p[0] = 0.22f + 0.06f * (float) chord;
                    p.p[1] = 0.35f;          // warp: фазовая дисторсия
                    p.p[2] = 0.0f;
                    p.p[3] = 0.45f;
                    p.detune = 0.5f;
                    v.eng.start (v.note, 0.85f);
                    v.eng.prepare (p);
                    v.env.reset();
                    v.env.noteOn();
                    v.svf.reset();
                }
            curChord = chord;
        }

        float l = 0.0f, r = 0.0f;

        // ---- пэды
        for (auto& v : pads)
        {
            if (! v.env.isActive()) continue;
            EngineParams p;
            p.table = 0;
            p.p[0] = 0.22f + 0.06f * (float) chord + 0.05f * (float) std::sin (t * 0.21 + v.note * 0.13);
            p.p[1] = 0.35f;
            p.p[2] = 0.0f;
            p.p[3] = 0.45f;
            p.detune = 0.5f;
            if ((i & 63) == 0) v.eng.prepare (p);

            const float s = v.eng.render (p);
            const float e = v.env.process();

            if ((i & 31) == 0)
                v.svf.setParams (650.0f + 1500.0f * (0.5f + 0.5f * (float) std::sin (t * 0.33)),
                                 0.24f, 0.10f);
            const float f = v.svf.process (s, FilterType::LP24);

            l += f * e * (0.55f - 0.35f * v.pan);
            r += f * e * (0.55f + 0.35f * v.pan);
        }

        // ---- арпеджио: восьмые, раз в 0.5 c
        if (t >= (double) nextPluck * 0.5)
        {
            for (auto& pv : plucks)
            {
                if (pv.env.isActive()) continue;
                const int step = (int) nextPluck % 4;
                const int note = chords[chord][step] + 24 + 12 * (int) ((nextPluck / 4) % 2);
                EngineParams p;
                p.table  = 1 + (int) (nextPluck % 5);
                p.p[0]   = 0.35f + 0.4f * rng.nextFloat();
                p.p[1]   = 0.45f;
                p.p[2]   = 0.12f;
                p.p[3]   = 0.3f;
                p.detune = 0.3f;
                pv.eng.start (note, 0.8f);
                pv.eng.prepare (p);
                pv.env.setParams (0.004f, 0.45f, 0.0f, 0.5f);
                pv.env.reset();
                pv.env.noteOn();
                pv.svf.reset();
                pv.svf.setParams (3800.0f, 0.3f, 0.22f);
                pv.pan   = (rng.nextFloat() * 2.0f - 1.0f) * 0.7f;
                pv.table = p.table;
                break;
            }
            ++nextPluck;
        }

        for (auto& pv : plucks)
        {
            if (! pv.env.isActive()) continue;
            EngineParams p;
            p.table = pv.table; p.p[0] = 0.3f; p.p[1] = 0.45f; p.p[2] = 0.12f; p.p[3] = 0.3f;
            p.detune = 0.3f;
            if ((i & 63) == 0) pv.eng.prepare (p);
            const float s = pv.eng.render (p);
            const float e = pv.env.process();
            const float f = pv.svf.process (s, FilterType::LP12);
            l += f * e * 0.40f * (0.5f - 0.4f * pv.pan);
            r += f * e * 0.40f * (0.5f + 0.4f * pv.pan);
        }

        outL[(size_t) i] = l;
        outR[(size_t) i] = r;
    }

    // ---- мастер: мягкое ограничение только если реально нужно + нормализация
    float peak = 0.0f;
    for (int i = 0; i < total; ++i)
        peak = std::max (peak, std::max (std::fabs (outL[(size_t) i]), std::fabs (outR[(size_t) i])));
    const float drive = peak > 0.9f ? 0.9f / peak : 1.0f;
    for (int i = 0; i < total; ++i)
    {
        outL[(size_t) i] = fastTanh (outL[(size_t) i] * drive * 1.05f);
        outR[(size_t) i] = fastTanh (outR[(size_t) i] * drive * 1.05f);
    }
    peak = 0.0f;
    for (int i = 0; i < total; ++i)
        peak = std::max (peak, std::max (std::fabs (outL[(size_t) i]), std::fabs (outR[(size_t) i])));
    const float g = peak > 0.0f ? 0.89f / peak : 1.0f;
    for (int i = 0; i < total; ++i) { outL[(size_t) i] *= g; outR[(size_t) i] *= g; }

    writeWav16 ("audio/wavetable_demo.wav", outL, outR, sr);
    std::printf ("готово: audio/wavetable_demo.wav (%.1f c, пик до нормализации %.2f)\n", length, peak);
    return 0;
}
