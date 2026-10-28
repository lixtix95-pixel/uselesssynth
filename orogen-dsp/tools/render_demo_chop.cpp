// Офлайн-рендер демо движка CHOP (слайсер): четыре фразы.
// Результат: audio/chop_demo.wav (16 бит, 48 кГц, стерео, 20 c).
//
// Что демонстрируется:
//   1. Раскладка по клавишам: ноты 45–49 играют удары брейка по порядку.
//   2. Транспонирование (Chromatic): один удар на разных нотах — rate 1…2.
//   3. Повторы (Stutter): p2 от 0 до 1 на одном куске.
//   4. Реверс (p3) + наложение двух лупов (брейк и вокальные слоги).
//
// Движок вызывается напрямую (без Instrument), как и остальные демо движков:
// так слышно именно слайсер, а не огибающие инструмента.
#include "../src/engines/chop.h"
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

// Одна нота через движок: возвращает стерео-пару и укладывает в буферы.
struct Voice
{
    ChopEngine eng;
    Adsr env;
    EngineParams p;
    double panL = 0.707, panR = 0.707;
    int note = 45;
    bool active = false;
    const LoopBank* bank = nullptr;

    void start (int n, float vel, const EngineParams& params, double pan, double sr)
    {
        note = n; p = params; active = true;
        eng.setBank (bank);
        eng.setSampleRate (sr);
        eng.start (n, vel);
        eng.prepare (p);
        env.setSampleRate (sr);
        env.setParams (0.001f, 0.30f, 0.0f, 0.15f);
        env.noteOn();
        const double a = (pan + 1.0) * 0.25 * kPi;
        panL = std::cos (a); panR = std::sin (a);
    }
    float render (const EngineParams& params)
    {
        if (! active) return 0.0f;
        const float s = eng.render (params) * env.process();
        if (! env.isActive() && ! eng.playing()) active = false;
        return s;
    }
};

int main()
{
    const double sr = 48000.0;
    LoopBank bank;
    bank.buildFactory (sr);

    const double seconds = 20.0;
    const int total = (int) (seconds * sr);
    std::vector<float> outL ((size_t) total, 0.0f), outR ((size_t) total, 0.0f);
    std::vector<Voice> voices (24);
    for (auto& v : voices) v.bank = &bank;

    std::printf ("CHOP demo: %zu лупа, слайсов %d / %d / %d\n", bank.loops.size(),
                 (int) bank.loops[0].slices.size(), (int) bank.loops[1].slices.size(),
                 (int) bank.loops[2].slices.size());

    auto slot = [&] (int i, int note, float vel, EngineParams p, double at, double pan = 0.0)
    {
        const int at0 = (int) (at * sr);
        Voice& v = voices[(size_t) i % voices.size()];
        v.start (note, vel, p, pan, sr);
        for (int n = 0; n < total - at0; ++n)
        {
            const float s = v.render (p);
            if (! v.active) break;
            outL[(size_t) (at0 + n)] += s * (float) v.panL;
            outR[(size_t) (at0 + n)] += s * (float) v.panR;
        }
    };

    // ---- 1. раскладка по клавишам (0–5 с): удары брейка по порядку
    {
        const double beat = 60.0 / 128.0;
        for (int i = 0; i < 10; ++i)
        {
            EngineParams p; p.table = 0; p.p[0] = 0.0f; p.p[1] = 0.9f; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.level = 0.9f;
            slot (0 + i, 45 + i, 0.95f, p, 0.05 + i * beat, (i % 2 == 0) ? -0.25 : 0.25);
        }
    }
    // ---- 2. транспонирование (5–10 с): один и тот же удар на разных нотах
    {
        const double step = 0.30;
        const int notes[16] = { 45, 47, 50, 52, 55, 57, 60, 62,
                                52, 50, 47, 45, 48, 50, 52, 45 };
        for (int i = 0; i < 16; ++i)
        {
            EngineParams p; p.table = 0; p.p[0] = 0.05f; p.p[1] = 0.8f; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.level = 0.8f;
            slot (8 + i, notes[i], 0.9f, p, 5.0 + i * step, (i % 2 == 0) ? 0.4 : -0.4);
        }
    }
    // ---- 3. повторы (10–15 с): p2 растёт, кусок «заикается»
    {
        const double step = 0.5;
        for (int i = 0; i < 10; ++i)
        {
            EngineParams p; p.table = 0; p.p[0] = 0.25f; p.p[1] = 0.28f;
            p.p[2] = (float) i / 9.0f; p.p[3] = 0.0f;
            p.level = 0.85f;
            slot (16 + i, 45, 0.95f, p, 10.0 + i * step, (i % 2 == 0) ? -0.2 : 0.2);
        }
    }
    // ---- 4. реверс + два лупа (15–20 с)
    {
        const double step = 0.26;
        const int notes[6] = { 46, 48, 50, 52, 54, 46 };
        for (int i = 0; i < 6; ++i)
        {
            EngineParams p; p.table = 0; p.p[0] = 0.0f; p.p[1] = 0.85f; p.p[2] = 0.33f;
            p.p[3] = (i % 3 == 2) ? 1.0f : 0.0f;      // каждый третий — реверс
            p.level = 0.8f;
            slot (20 + i, notes[i], 0.9f, p, 15.0 + i * step, (i % 2 == 0) ? -0.3 : 0.3);
        }
        for (int i = 0; i < 10; ++i)
        {
            EngineParams p; p.table = 1; p.p[0] = 0.0f; p.p[1] = 0.9f; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.level = 0.5f;
            slot (26 + i, 45 + (i % 8), 0.7f, p, 15.2 + i * 0.42, (i % 2 == 0) ? 0.55 : -0.55);
        }
    }

    // ---- нормализация и статистика
    float peak = 0.0f;
    for (int i = 0; i < total; ++i)
    {
        peak = std::max (peak, std::fabs (outL[(size_t) i]));
        peak = std::max (peak, std::fabs (outR[(size_t) i]));
    }
    const float g = (peak > 1.0e-6f) ? 0.89f / peak : 1.0f;
    double sumL = 0.0, dcL = 0.0;
    for (int i = 0; i < total; ++i)
    {
        outL[(size_t) i] *= g;
        outR[(size_t) i] *= g;
        sumL += (double) outL[(size_t) i] * outL[(size_t) i];
        dcL += outL[(size_t) i];
    }

    writeWav16 ("audio/chop_demo.wav", outL, outR, (int) sr);
    std::printf ("готово: audio/chop_demo.wav (%.1f c)\n", seconds);
    std::printf ("пик до нормализации %.3f, RMS L %.3f, DC %.5f\n",
                 peak, std::sqrt (sumL / total), dcL / total);
    return 0;
}
