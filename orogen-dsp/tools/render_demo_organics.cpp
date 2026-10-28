// Офлайн-рендер демо движка Organics: 3 секции, 20 c.
//
// Что изменено после прослушивания: секция перкуссии (Wood Knock) была на
// 20 дБ тише остальных и звучала как треск — теперь удары короткие (0.35 с),
// а секции выровнены по RMS. Плюс движок больше не «пулемётит»: атака
// проигрывается один раз, дальше идёт петля с кроссфейдом (см. organics.h).
// audio/organics_demo.wav
#include "../src/engines/organics.h"
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
    OrganicsEngine eng;
    Adsr env;
    Svf  svf;
    float pan = 0.0f, amp = 1.0f;
    bool  used = false;
    double endT = 0.0;
};

int main()
{
    const double sr = 48000.0, length = 20.0;
    const int total = (int) (sr * length);

    SampleBank bank; bank.buildFactory();

    struct Ev { double t, dur; int note, table; float vel, pan, body, air, drift, bloom; };
    std::vector<Ev> ev;

    // 1. Пэд Choir: тело + воздух, длинные ноты (этот источник держит петлю)
    const int pad[] = { 50, 57, 62, 65 };
    for (int i = 0; i < 4; ++i)
        ev.push_back ({ 0.3, 7.0, pad[i], 2, 0.8f, -0.5f + 0.33f * i, 0.6f, 0.45f, 0.35f, 0.25f });
    // 2. Перкуссия: Wood Knock и Glass Hit, короткие ноты — «приготовленный»
    //    инструмент. Ноты по 0.35 с: раньше стояло 2.0 с, и голос после удара
    //    почти минуту висел в тишине (RMS секции 0.012 против 0.34 у баса).
    const int hits[] = { 62, 65, 69, 62, 69, 72, 65, 69, 62, 72, 76, 69 };
    for (int i = 0; i < 12; ++i)
        ev.push_back ({ 7.2 + i * 0.36, 0.35, hits[i], (i % 4 == 3) ? 3 : 5, 0.85f,
                        -0.4f + 0.27f * (i % 3), 0.55f, 0.25f, 0.2f, 0.12f });
    // 3. Бас + текстура: Sub Bass и Tape Pad
    ev.push_back ({ 12.4, 20.0, 38, 7, 0.95f, 0.0f, 0.35f, 0.05f, 0.15f, 0.1f });
    ev.push_back ({ 12.6, 20.0, 57, 4, 0.6f, -0.3f, 0.7f, 0.55f, 0.5f, 0.4f });
    ev.push_back ({ 12.6, 20.0, 64, 4, 0.55f, 0.3f, 0.7f, 0.5f, 0.45f, 0.4f });
    for (int i = 0; i < 4; ++i)
        ev.push_back ({ 15.0 + i * 1.2, 19.6, 74 + i * 2, 6, 0.5f, -0.35f + 0.25f * i, 0.5f, 0.6f, 0.3f, 0.5f });

    std::sort (ev.begin(), ev.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });

    std::deque<Voice> voices (16);
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
            slot->used = true; slot->pan = e.pan; slot->amp = e.vel * 0.75f; slot->endT = e.t + e.dur;
            slot->eng.start (e.note, e.vel);
            EngineParams p;
            p.table = e.table; p.p[0] = e.body; p.p[1] = e.air; p.p[2] = e.drift; p.p[3] = e.bloom;
            p.detune = 0.0f; p.tuneSemis = 0.0f; p.level = 1.0f;
            slot->eng.prepare (p);
            slot->svf.setParams (2200.0f + 5000.0f * e.air, 0.3f, 0.0f);
            slot->env.setParams (0.01f, 0.15f, 0.9f, 0.7f);
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
            if (! v.env.isActive() && e < 1e-5f) { v.used = false; v.eng.stop(); }
        }
        L[(size_t) i] = (float) l; R[(size_t) i] = (float) r;
    }

    // Балансировка секций по RMS (см. granular): перкуссионная секция была
    // на 20 дБ тише остальных и читалась как артефакт на их фоне.
    {
        struct Sec { double a, b; };
        const Sec secs[] = { { 0.3, 7.0 }, { 7.2, 11.8 }, { 12.4, 20.0 } };
        // Цель — фиксированный RMS на секцию (а не среднее по секциям):
        // басовая секция при среднем «тянула» баланс за собой.
        const double mean = 0.115;
        const int ramp = (int) (sr * 0.03);
        std::vector<float> gain ((size_t) total, 1.0f);
        for (const auto& sc : secs)
        {
            double acc = 0.0; int n2 = 0;
            for (int i = (int) (sc.a * sr); i < (int) (sc.b * sr) && i < total; ++i)
            { acc += (double) L[(size_t) i] * L[(size_t) i]; ++n2; }
            const float g2 = (float) clamp (mean / std::max (1.0e-6, std::sqrt (acc / std::max (1, n2))), 0.08, 8.0);
            std::printf ("   секция %.1f-%.1f с: RMS %.4f -> усиление %.2f\n", sc.a, sc.b,
                         std::sqrt (acc / std::max (1, n2)), (double) g2);
            for (int i = (int) (sc.a * sr); i < std::min (total, (int) (sc.b * sr)); ++i) gain[(size_t) i] = g2;
        }
        // Стыки — линейный кроссфейд между ПОСТОЯННЫМИ усилениями секций.
        // Наивная «сглаживай, пока соседи различаются» каскадом растягивала
        // переход на всю секцию (баланс уезжал в 3 раза).
        for (size_t k = 0; k + 1 < sizeof (secs) / sizeof (secs[0]); ++k)
        {
            const int tb = (int) (secs[k].b * sr);
            const float ga = gain[(size_t) std::max (0, tb - 1)];
            const float gb = gain[(size_t) std::min (total - 1, (int) (secs[k + 1].a * sr))];
            for (int i = std::max (0, tb - ramp / 2); i < std::min (total, tb + ramp / 2); ++i)
                gain[(size_t) i] = ga + (gb - ga) * (float) (i - (tb - ramp / 2)) / (float) std::max (1, ramp);
        }
        double applied = 0.0; int changed = 0;
        for (int i = 0; i < total; ++i) { if (gain[(size_t) i] != 1.0f) ++changed; L[(size_t) i] *= gain[(size_t) i]; R[(size_t) i] *= gain[(size_t) i]; applied += gain[(size_t) i]; }
        std::printf ("   балансировка: применено к %d из %d, средний gain %.3f\n", changed, total, applied / total);
        for (const auto& sc : secs)
        {
            double acc = 0.0; int n2 = 0;
            for (int i = (int) (sc.a * sr); i < (int) (sc.b * sr) && i < total; ++i)
            { acc += (double) L[(size_t) i] * L[(size_t) i]; ++n2; }
            std::printf ("   после: секция %.1f-%.1f: RMS %.4f\n", sc.a, sc.b, std::sqrt (acc / std::max (1, n2)));
        }
    }

    float peak = 0.0f;
    for (int i = 0; i < total; ++i) { peak = std::max (peak, std::fabs (L[(size_t) i])); peak = std::max (peak, std::fabs (R[(size_t) i])); }
    const float g = (peak > 1e-6f) ? 0.89f / peak : 1.0f;
    double sL = 0, sR = 0, dc = 0;
    for (int i = 0; i < total; ++i)
    { L[(size_t) i] *= g; R[(size_t) i] *= g; sL += (double) L[(size_t) i] * L[(size_t) i]; sR += (double) R[(size_t) i] * R[(size_t) i]; dc += L[(size_t) i]; }

    writeWav16 ("audio/organics_demo.wav", L, R, (int) sr);
    std::printf ("готово: audio/organics_demo.wav (%.1f c, брошено %d)\n", length, dropped);
    std::printf ("пик до нормализации %.3f, RMS L %.3f / R %.3f, DC %.5f\n", peak, std::sqrt (sL / total), std::sqrt (sR / total), dc / total);
    return 0;
}
