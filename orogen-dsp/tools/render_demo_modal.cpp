// Офлайн-рендер демо движка Modal: 4 секции по 5 c.
// Результат: audio/modal_demo.wav (16 бит, 48 кГц, стерео, 20 c).
//   1. 0-5 c   — колокол: мелодия с длинным T60, яркость растёт;
//   2. 5-10 c  — стекло: быстрые высокие удары (моды до 12 кГц);
//   3. 10-15 c — дерево: ксилофон, короткий T60, жёсткий удар;
//   4. 15-20 c — сталь + камень: медленный «гонг», нелинейность 0.6.
#include "../src/engines/modal.h"
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

struct Voice
{
    ModalEngine eng;
    Svf         svf;
    float       pan = 0.0f, amp = 1.0f;
    bool        used = false;
    double      endT = 0.0, tail = 0.0;
};

static constexpr int kMaxVoices = 24;

int main()
{
    const double sr = 48000.0;
    const double length = 20.0;
    const int    total = (int) (sr * length);

    struct Ev { double t; int note; int table; float vel; float pan;
                float bright, decay, hardness, nonlin; };
    std::vector<Ev> events;

    // 1. Колокол — мелодия, T60 длинный
    const int bell[] = { 57, 60, 64, 67, 64, 60, 55, 60 };
    for (int i = 0; i < 8; ++i)
        events.push_back ({ 0.2 + i * 0.55, bell[i], 0, 0.85f, -0.2f + 0.35f * (i % 2),
                            0.35f + 0.05f * i, 0.62f, 0.35f, 0.25f });

    // 2. Стекло — быстрые высокие удары
    const int glass[] = { 76, 79, 83, 81, 84, 88, 86, 83 };
    for (int i = 0; i < 8; ++i)
        events.push_back ({ 5.2 + i * 0.45, glass[i], 1, 0.7f, 0.35f - 0.4f * (i % 2),
                            0.85f, 0.25f, 0.8f, 0.1f });

    // 3. Дерево — короткий T60, жёсткий удар, арпеджио
    const int wood[] = { 64, 67, 71, 74, 71, 67, 64, 60, 64, 67, 72, 76 };
    for (int i = 0; i < 12; ++i)
        events.push_back ({ 10.2 + i * 0.34, wood[i], 2, 0.8f, -0.45f + 0.28f * (i % 3),
                            0.6f, 0.12f, 0.9f, 0.15f });

    // 4. Сталь/камень — медленный гонг
    events.push_back ({ 15.0, 45, 3, 0.9f, -0.15f, 0.5f, 0.95f, 0.4f, 0.65f });
    events.push_back ({ 15.0, 52, 6, 0.8f, 0.25f, 0.7f, 0.9f, 0.5f, 0.5f });
    events.push_back ({ 17.5, 48, 3, 0.7f, 0.0f, 0.45f, 0.8f, 0.4f, 0.45f });

    std::sort (events.begin(), events.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });

    std::deque<Voice> voices (kMaxVoices);
    for (auto& v : voices) { v.eng.setSampleRate (sr); v.svf.setSampleRate (sr); }

    std::vector<float> outL ((size_t) total, 0.0f), outR ((size_t) total, 0.0f);
    size_t next = 0;
    int dropped = 0;

    for (int i = 0; i < total; ++i)
    {
        const double t = i / sr;

        while (next < events.size() && events[next].t <= t)
        {
            const Ev& e = events[next];
            Voice* slot = nullptr;
            for (auto& v : voices) if (! v.used) { slot = &v; break; }
            if (slot == nullptr) { ++dropped; ++next; continue; }

            slot->used = true;
            slot->tail = 0.0;                  // иначе голос «умирает» на первом сэмпле
            slot->pan = e.pan; slot->amp = e.vel * 0.55f;
            slot->endT = e.t + 0.06;
            slot->eng.start (e.note, e.vel);
            slot->svf.setParams (2500.0f + 6000.0f * e.bright, 0.3f, 0.0f);

            EngineParams p;
            p.table = e.table; p.p[0] = e.bright; p.p[1] = e.decay;
            p.p[2] = e.hardness; p.p[3] = e.nonlin;
            p.detune = 0.0f; p.tuneSemis = 0.0f; p.level = 1.0f;
            slot->eng.prepare (p);
            ++next;
        }

        double l = 0.0, r = 0.0;
        for (auto& v : voices)
        {
            if (! v.used) continue;
            const EngineParams dummy {};
            const float s = v.eng.render (dummy);
            const float y = v.svf.process (s, FilterType::LP12) * v.amp;
            const float gl = std::cos ((v.pan + 1.0f) * 0.25f * (float) kPi);
            const float gr = std::sin ((v.pan + 1.0f) * 0.25f * (float) kPi);
            l += y * gl;
            r += y * gr;
            if (std::fabs (s) < 1.0e-5f) { v.tail += 1.0 / sr; if (v.tail > 0.05) v.used = false; }
            else v.tail = 0.0;
        }

        outL[(size_t) i] = (float) l;
        outR[(size_t) i] = (float) r;
    }

    float peak = 0.0f;
    for (int i = 0; i < total; ++i)
    {
        peak = std::max (peak, std::fabs (outL[(size_t) i]));
        peak = std::max (peak, std::fabs (outR[(size_t) i]));
    }
    const float g = (peak > 1.0e-6f) ? 0.89f / peak : 1.0f;
    double sumL = 0.0, sumR = 0.0, dcL = 0.0;
    for (int i = 0; i < total; ++i)
    {
        outL[(size_t) i] *= g;
        outR[(size_t) i] *= g;
        sumL += (double) outL[(size_t) i] * outL[(size_t) i];
        sumR += (double) outR[(size_t) i] * outR[(size_t) i];
        dcL += outL[(size_t) i];
    }

    writeWav16 ("audio/modal_demo.wav", outL, outR, (int) sr);
    std::printf ("готово: audio/modal_demo.wav (%.1f c, голосов брошено %d)\n", length, dropped);
    std::printf ("пик до нормализации %.3f, RMS L %.3f / R %.3f, DC %.5f\n",
                 peak, std::sqrt (sumL / total), std::sqrt (sumR / total), dcL / total);
    return 0;
}
