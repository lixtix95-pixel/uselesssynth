// Офлайн-рендер демо движка Sample: 4 фразы по 4 такта, 8 источников контента.
// Результат: audio/sample_demo.wav (16 бит, 48 кГц, стерео, 20 c).
//
// Что демонстрируется:
//   1. Fm Tine   — мелодия в среднем регистре (питч-шифт вниз/вверх от A2);
//   2. Karplus   — арпеджио 16-ми на верхних нотах (MIP-уровни, +2 октавы);
//   3. Choir Ah  — пэд с медленно ползущим Scan (скраб по записи);
//   4. Glass Hit — перкуссия + Scan по хвосту;
//   5. Wood Knock / Vox Ah / Sub Bass / Tape Pad — в收官ной фразе как бас и текстура.
#include "../src/engines/sample.h"
#include "../src/core/svf.h"
#include "../src/core/adsr.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <deque>
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
struct Voice
{
    SampleEngine eng;
    Adsr         env;
    Svf          svf;
    float        pan = 0.0f;
    float        amp = 1.0f;
    double       endT = 0.0;
    bool         used = false;
    bool         released = false;
};

static constexpr int kMaxVoices = 48;

int main()
{
    const double sr = 48000.0;
    const double length = 20.0;
    const int    total = (int) (sr * length);

    SampleBank bank;
    bank.buildFactory (sr);
    std::printf ("контент: %zu источников\n", bank.shots.size());

    std::deque<Voice> voices (kMaxVoices);
    for (auto& v : voices) { v.eng.setBank (&bank); v.eng.setSampleRate (sr); v.env.setSampleRate (sr); }

    // ---- партитура ------------------------------------------------------
    struct Ev { double t; int note; int table; float vel; float dur; float pan;
                float scan; float grain; float tone; float air; };
    std::vector<Ev> events;
    auto add = [&] (double t, int note, int table, float vel, float dur, float pan,
                    float scan = 0.0f, float grain = 0.0f, float tone = 1.0f, float air = 0.2f)
    { events.push_back ({ t, note, table, vel, dur, pan, scan, grain, tone, air }); };

    // Фраза 1 (0-4 c): мелодия Fm Tine, Dm9 -> Bbmaj7
    const int mel1[] = { 62, 65, 69, 72, 69, 65, 62, 58 };
    for (int i = 0; i < 8; ++i)
        add (0.25 + i * 0.45, mel1[i], 0, 0.9f, 0.40, -0.15f + 0.3f * (i % 2), 0.0f, 0.0f, 0.85f, 0.15f);

    // Фраза 2 (4-9 c): Karplus-арпеджио на верхних нотах (MIP +1..+2 октавы)
    const int arp[] = { 69, 72, 76, 79, 76, 72, 69, 72, 77, 81, 84, 81, 77, 72, 69, 65 };
    for (int i = 0; i < 32; ++i)
        add (4.0 + i * 0.156, arp[i % 16], 1, 0.75f, 0.14, (i % 2 ? 0.35f : -0.35f), 0.0f, 0.0f, 0.9f, 0.1f);

    // Фраза 3 (9-14 c): Choir-пэд, Scan медленно ползёт от 0 к 0.6
    const int pad[] = { 50, 57, 62, 65 };
    for (int i = 0; i < 4; ++i)
        add (9.0 + i * 0.03, pad[i], 2, 0.8f, 5.0, -0.5f + 0.33f * i, 0.05f * i, 0.0f, 0.6f, 0.35f);

    // Фраза 4 (14-20 c): Sub Bass + Glass Hit + Wood Knock + Vox Ah
    const int bass[] = { 38, 38, 36, 36, 34, 34, 38, 38 };
    for (int i = 0; i < 8; ++i)
        add (14.0 + i * 0.5, bass[i], 7, 0.95f, 0.45, 0.0f, 0.0f, 0.0f, 0.7f, 0.05f);
    for (int i = 0; i < 8; ++i)
        add (14.0 + i * 0.5 + 0.25, 74 + (i % 3) * 3, 3, 0.6f, 0.3, 0.45f - 0.3f * (i % 2), 0.25f, 0.0f, 1.0f, 0.2f);
    for (int i = 0; i < 16; ++i)
        add (14.0 + i * 0.25, 60 + (i % 4) * 2, 5, 0.45f, 0.2, -0.45f + 0.3f * (i % 2), 0.1f, 0.0f, 1.0f, 0.1f);
    for (int i = 0; i < 2; ++i)
        add (15.0 + i * 2.5, 55 + i * 2, 6, 0.55f, 2.0, 0.25f, 0.15f + 0.2f * i, 0.0f, 0.55f, 0.4f);

    std::sort (events.begin(), events.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });

    // ---- рендер ---------------------------------------------------------
    std::vector<float> outL ((size_t) total, 0.0f), outR ((size_t) total, 0.0f);
    size_t next = 0;
    int    dropped = 0;

    for (int i = 0; i < total; ++i)
    {
        const double t = i / sr;

        while (next < events.size() && events[next].t <= t)
        {
            const Ev& e = events[next];
            Voice* slot = nullptr;
            for (auto& v : voices) if (! v.used) { slot = &v; break; }
            if (slot == nullptr) { ++dropped; ++next; continue; }

            slot->used = true; slot->released = false;
            slot->pan = e.pan; slot->amp = e.vel * 0.55f;
            slot->endT = e.t + e.dur;
            slot->eng.start (e.note, e.vel);
            slot->svf.setSampleRate (sr);
            slot->svf.setParams (2400.0f + 4200.0f * e.tone, 0.35f, 0.0f);

            EngineParams p;
            p.table = e.table;
            p.p[0] = e.scan; p.p[1] = e.grain; p.p[2] = e.tone; p.p[3] = e.air;
            p.detune = 0.3f; p.tuneSemis = 0.0f; p.level = 1.0f;

            slot->env.setParams (0.004f, 0.05f, 0.85f, (float) std::min (1.2, e.dur * 0.6));
            slot->eng.prepare (p);
            slot->env.noteOn();

            // запоминаем подготовленные параметры в самом движке: render() их не принимает
            ++next;
        }

        double l = 0.0, r = 0.0;
        for (auto& v : voices)
        {
            if (! v.used) continue;
            if (! v.released && t >= v.endT) { v.env.noteOff(); v.released = true; }
            const EngineParams dummy {};
            const float s = v.eng.render (dummy);
            const float e = v.env.process();
            const float y = v.svf.process (s, FilterType::LP12) * e;
            const float gl = std::cos ((v.pan + 1.0f) * 0.25f * (float) kPi);
            const float gr = std::sin ((v.pan + 1.0f) * 0.25f * (float) kPi);
            l += y * gl * v.amp;
            r += y * gr * v.amp;
            if (! v.env.isActive() || e < 1.0e-5f) { v.used = false; v.released = false; v.eng.stop(); }
        }

        outL[(size_t) i] = (float) l;
        outR[(size_t) i] = (float) r;
    }

    // ---- нормализация и статистика --------------------------------------
    float peak = 0.0f;
    for (int i = 0; i < total; ++i)
    {
        peak = std::max (peak, std::fabs (outL[(size_t) i]));
        peak = std::max (peak, std::fabs (outR[(size_t) i]));
    }
    const float g = (peak > 1.0e-6f) ? 0.89f / peak : 1.0f;
    double sumL = 0.0, dcL = 0.0, sumR = 0.0;
    for (int i = 0; i < total; ++i)
    {
        outL[(size_t) i] *= g;
        outR[(size_t) i] *= g;
        sumL += (double) outL[(size_t) i] * outL[(size_t) i];
        sumR += (double) outR[(size_t) i] * outR[(size_t) i];
        dcL += outL[(size_t) i];
    }

    writeWav16 ("audio/sample_demo.wav", outL, outR, (int) sr);
    std::printf ("готово: audio/sample_demo.wav (%.1f c, голосов брошено %d)\n", length, dropped);
    std::printf ("пик до нормализации %.3f, RMS L %.3f / R %.3f, DC %.5f\n",
                 peak, std::sqrt (sumL / total), std::sqrt (sumR / total), dcL / total);
    return 0;
}
