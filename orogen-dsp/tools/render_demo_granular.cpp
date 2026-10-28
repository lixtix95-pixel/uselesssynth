// Офлайн-рендер демо движка Granular: 3 секции по 6-7 c.
// Результат: audio/granular_demo.wav (16 бит, 48 кГц, стерео, 20 c).
//
// Секции:
//   1. 0-6.5 c  — пэд Choir Ah: длинные зёрна, низкая плотность, спред 0.18;
//   2. 6.5-13 c — облако Vox Ah + Tape Pad: средние зёрна, спред 0.45;
//   3. 13-20 c  — бас Sub Bass + текстура Wood Knock (короткие зёрна, тише).
//
// Почему параметры именно такие (было иначе и звучало как артефакт):
// короткие зёрна (0.055 с) на плотности 71/с по «щипковому» источнику дают
// 280 случайных обрывков в секунду — это не облако, а шум с металлическим
// призвуком (в 6–12 кГц было −2.6 дБ от общей энергии). Длинные зёрна
// (0.10–0.14 с) при плотности 30–40/с дают перекрытие 3–5 зёрен и звучат
// как хор, а не как гранулярный шум.
#include "../src/engines/granular.h"
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
    GranularEngine eng;
    Adsr           env;
    Svf            svf;
    int            table = 0;
    float          pan = 0.0f, amp = 1.0f;
    double         t0 = 0.0, t1 = 0.0;
    // параметры, которые «едут» во времени
    float          scan0 = 0.0f, scan1 = 0.0f;
    float          size = 0.5f, dens = 0.5f, spread = 0.3f, tone = 0.6f;
    bool           used = false, released = false;
};

static constexpr int kMaxVoices = 10;

int main()
{
    const double sr = 48000.0;
    const double length = 20.0;
    const int    total = (int) (sr * length);

    SampleBank bank;
    bank.buildFactory (sr);
    std::printf ("контент: %zu источников\n", bank.shots.size());

    struct Ev { double t0, t1; int note; int table; float vel; float pan;
                float scan0, scan1, size, dens, spread, tone; };
    std::vector<Ev> events;

    // Секция 1: пэд из Choir Ah — 3 голоса, Scan ползёт вверх
    const int pad[] = { 50, 57, 64 };
    for (int i = 0; i < 3; ++i)
        events.push_back ({ 0.25, 6.6, pad[i], 2, 0.85f, -0.45f + 0.45f * i,
                            0.15f, 0.55f, 0.62f, 0.40f, 0.18f, 0.50f });

    // Секция 2: облако Vox Ah + Tape Pad, средние зёрна, умеренный спред
    const int cloud[] = { 64, 69, 72 };
    const int cloudTables[] = { 6, 6, 4 };
    for (int i = 0; i < 3; ++i)
        events.push_back ({ 6.7 + i * 0.12, 13.2, cloud[i], cloudTables[i], 0.8f, -0.5f + 0.5f * i,
                            0.30f, 0.60f, 0.55f, 0.35f, 0.45f, 0.55f });

    // Секция 3: бас + текстура (короткие зёрна — тише, как слой, а не как тело)
    events.push_back ({ 13.4, 20.0, 38, 7, 1.0f, 0.0f, 0.05f, 0.1f, 0.70f, 0.50f, 0.0f, 0.40f });
    events.push_back ({ 13.6, 19.6, 45, 7, 0.7f, 0.25f, 0.10f, 0.2f, 0.60f, 0.32f, 0.05f, 0.45f });
    for (int i = 0; i < 3; ++i)
        events.push_back ({ 14.0 + i * 2.0, 19.8, 62 + i * 3, 5, 0.40f, -0.3f + 0.3f * i,
                            0.30f, 0.40f, 0.35f, 0.30f, 0.55f, 0.35f });

    std::deque<Voice> voices (kMaxVoices);
    for (auto& v : voices) { v.eng.setBank (&bank); v.eng.setSampleRate (sr); v.env.setSampleRate (sr); }

    std::vector<float> outL ((size_t) total, 0.0f), outR ((size_t) total, 0.0f);
    size_t next = 0;
    int dropped = 0;

    for (int i = 0; i < total; ++i)
    {
        const double t = i / sr;

        while (next < events.size() && events[next].t0 <= t)
        {
            const Ev& e = events[next];
            Voice* slot = nullptr;
            for (auto& v : voices) if (! v.used) { slot = &v; break; }
            if (slot == nullptr) { ++dropped; ++next; continue; }

            slot->used = true; slot->released = false;
            slot->table = e.table; slot->pan = e.pan; slot->amp = e.vel * 0.30f;
            slot->t0 = e.t0; slot->t1 = e.t1;
            slot->scan0 = e.scan0; slot->scan1 = e.scan1;
            slot->size = e.size; slot->dens = e.dens; slot->spread = e.spread; slot->tone = e.tone;

            slot->eng.start (e.note, e.vel);
            slot->svf.setSampleRate (sr);
            slot->svf.setParams (1800.0f + 5200.0f * e.tone, 0.4f, 0.0f);

            EngineParams p;
            p.table = e.table; p.p[0] = e.scan0; p.p[1] = e.size;
            p.p[2] = e.dens; p.p[3] = e.spread;
            p.detune = 0.0f; p.tuneSemis = 0.0f; p.level = 1.0f;
            slot->eng.prepare (p);

            slot->env.setParams (0.02f, 0.3f, 0.9f, 0.9f);
            slot->env.noteOn();
            ++next;
        }

        double l = 0.0, r = 0.0;
        for (auto& v : voices)
        {
            if (! v.used) continue;

            // параметры едут во времени (Scan, плотность) — как автоматизация в DAW
            const double u = clamp ((t - v.t0) / std::max (0.001, v.t1 - v.t0), 0.0, 1.0);
            EngineParams p;
            p.table = v.table;
            p.p[0] = (float) (v.scan0 + (v.scan1 - v.scan0) * u);
            p.p[1] = v.size;
            p.p[2] = (float) (v.dens + 0.2 * std::sin (kTwoPi * 0.13 * t));
            p.p[3] = v.spread;
            p.detune = 0.0f; p.tuneSemis = 0.0f; p.level = 1.0f;
            v.eng.prepare (p);

            if (! v.released && t >= v.t1) { v.env.noteOff(); v.released = true; }

            const float s = v.eng.render (p);
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

    // Балансировка секций: раньше уровень секций различался на 19 дБ
    // (пэд RMS 0.19 против облака 0.07), и «тихие» секции слушались как
    // артефакт на фоне громких. Выравниваем RMS каждой секции к общему.
    {
        struct Sec { double a, b; };
        const Sec secs[] = { { 0.3, 6.5 }, { 6.7, 13.2 }, { 13.4, 20.0 } };
        double mean = 0.0; int cnt = 0;
        for (const auto& sc : secs)
        {
            double acc = 0.0; int n2 = 0;
            for (int i = (int) (sc.a * sr); i < (int) (sc.b * sr) && i < total; ++i)
            { acc += (double) outL[(size_t) i] * outL[(size_t) i]; ++n2; }
            mean += std::sqrt (acc / std::max (1, n2)); ++cnt;
        }
        mean /= cnt;
        const int ramp = (int) (sr * 0.03);
        std::vector<float> gain ((size_t) total, 1.0f);
        for (const auto& sc : secs)
        {
            double acc = 0.0; int n2 = 0;
            for (int i = (int) (sc.a * sr); i < (int) (sc.b * sr) && i < total; ++i)
            { acc += (double) outL[(size_t) i] * outL[(size_t) i]; ++n2; }
            const double rms = std::sqrt (acc / std::max (1, n2));
            const float g2 = (float) clamp (mean / std::max (1.0e-6, rms), 0.5, 2.0);
            const int i0 = (int) (sc.a * sr), i1 = std::min (total, (int) (sc.b * sr));
            for (int i = i0; i < i1; ++i) gain[(size_t) i] = g2;
        }
        // сглаживание стыков — 30 мс, чтобы не было ступеньки
        for (int pass = 0; pass < ramp; ++pass) { }
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
        for (int i = 0; i < total; ++i) { outL[(size_t) i] *= gain[(size_t) i]; outR[(size_t) i] *= gain[(size_t) i]; }
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

    writeWav16 ("audio/granular_demo.wav", outL, outR, (int) sr);
    std::printf ("готово: audio/granular_demo.wav (%.1f c, голосов брошено %d)\n", length, dropped);
    std::printf ("пик до нормализации %.3f, RMS L %.3f / R %.3f, DC %.5f\n",
                 peak, std::sqrt (sumL / total), std::sqrt (sumR / total), dcL / total);
    return 0;
}
