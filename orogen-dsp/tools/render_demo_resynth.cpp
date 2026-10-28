// Офлайн-рендер демо движка Resynth: 3 секции, 20 c.
// audio/resynth_demo.wav
#include "../src/engines/resynth.h"
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

struct Voice
{
    ResynthEngine eng;
    Svf           svf;
    Adsr          env;
    float         pan = 0.0f, amp = 1.0f;
    bool          used = false;
    double        endT = 0.0;
};

int main()
{
    const double sr = 48000.0, length = 20.0;
    const int total = (int) (sr * length);

    SampleBank samples; samples.buildFactory();
    ResynthBank bank; bank.build (samples);

    struct Ev { double t, end; int note, table; float vel, pan, stretch, pitch, tilt, drift; };
    std::vector<Ev> ev;

    // 1. Пэд со stretch 4 (медленно, тембр сохранён)
    const int pad[] = { 50, 57, 62, 65 };
    for (int i = 0; i < 4; ++i)
        ev.push_back ({ 0.3, 7.0, pad[i], 2, 0.8f, -0.5f + 0.33f * i, 0.95f, 0.5f, 0.0f, 0.25f });
    // 2. Быстрое арпеджио: stretch 0.25 (в 4 раза быстрее), наклон вверх
    const int arp[] = { 69, 72, 76, 79, 76, 72, 69, 76, 81, 84, 81, 76 };
    for (int i = 0; i < 24; ++i)
        ev.push_back ({ 7.2 + i * 0.22, 7.2 + i * 0.22 + 0.5, arp[i % 12], 1, 0.7f,
                        -0.4f + 0.27f * (i % 3), 0.06f, 0.5f, 0.35f, 0.0f });
    // 3. Голос: drift + питч-шифт вверх
    ev.push_back ({ 13.0, 20.0, 62, 6, 0.85f, -0.2f, 0.6f, 0.62f, -0.15f, 0.5f });
    ev.push_back ({ 13.4, 20.0, 55, 6, 0.7f, 0.25f, 0.6f, 0.38f, -0.2f, 0.4f });
    for (int i = 0; i < 3; ++i)
        ev.push_back ({ 16.0 + i * 1.2, 19.8, 74 + i * 3, 3, 0.55f, -0.3f + 0.3f * i, 0.4f, 0.5f, 0.2f, 0.2f });

    std::sort (ev.begin(), ev.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });

    std::deque<Voice> voices (20);
    for (auto& v : voices) { v.eng.setBank (&bank); v.eng.setSampleRate (sr); v.env.setSampleRate (sr); v.svf.setSampleRate (sr); }

    std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);
    size_t next = 0; int dropped = 0;

    for (int i = 0; i < total; ++i)
    {
        const double t = i / sr;
        while (next < ev.size() && ev[next].t <= t)
        {
            const Ev& e = ev[next];
            Voice* slot = nullptr;
            for (auto& v : voices) if (! v.used) { slot = &v; break; }
            if (! slot) { ++dropped; ++next; continue; }
            slot->used = true; slot->pan = e.pan; slot->amp = e.vel * 0.55f; slot->endT = e.end;
            slot->eng.start (e.note, e.vel);
            EngineParams p;
            p.table = e.table; p.p[0] = e.stretch; p.p[1] = e.pitch; p.p[2] = e.tilt; p.p[3] = e.drift;
            p.detune = 0.0f; p.tuneSemis = 0.0f; p.level = 1.0f;
            slot->eng.prepare (p);
            slot->svf.setParams (3000.0f + 4000.0f * e.tilt, 0.3f, 0.0f);
            slot->env.setParams (0.05f, 0.2f, 0.85f, 0.6f);
            slot->env.noteOn();
            ++next;
        }

        double l = 0.0, r = 0.0;
        for (auto& v : voices)
        {
            if (! v.used) continue;
            if (t >= v.endT) v.env.noteOff();
            const EngineParams dummy {};
            const float s = v.eng.render (dummy);
            const float e = v.env.process();
            const float y = v.svf.process (s, FilterType::LP12) * e * v.amp;
            l += y * std::cos ((v.pan + 1.0f) * 0.25f * (float) kPi);
            r += y * std::sin ((v.pan + 1.0f) * 0.25f * (float) kPi);
            if (! v.eng.isActive() || (! v.env.isActive() && e < 1e-5f)) { v.used = false; v.eng.stop(); }
        }
        L[(size_t) i] = (float) l; R[(size_t) i] = (float) r;
    }

    float peak = 0.0f;
    for (int i = 0; i < total; ++i) { peak = std::max (peak, std::fabs (L[(size_t) i])); peak = std::max (peak, std::fabs (R[(size_t) i])); }
    const float g = (peak > 1e-6f) ? 0.89f / peak : 1.0f;
    double sL = 0, sR = 0, dc = 0;
    for (int i = 0; i < total; ++i)
    { L[(size_t) i] *= g; R[(size_t) i] *= g; sL += (double) L[(size_t) i] * L[(size_t) i]; sR += (double) R[(size_t) i] * R[(size_t) i]; dc += L[(size_t) i]; }

    writeWav16 ("audio/resynth_demo.wav", L, R, (int) sr);
    std::printf ("готово: audio/resynth_demo.wav (%.1f c, брошено %d)\n", length, dropped);
    std::printf ("пик до нормализации %.3f, RMS L %.3f / R %.3f, DC %.5f\n", peak, std::sqrt (sL / total), std::sqrt (sR / total), dc / total);
    return 0;
}
