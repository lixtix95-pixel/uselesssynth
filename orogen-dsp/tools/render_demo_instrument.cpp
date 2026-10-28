// Демо инструмента: 40 секунд, пять секций, все 8 движков в слотах A–H.
//   audio/instrument_demo.wav
// Секции: пэд → клавиши → морф всех восьми движков → сплит бас/лид → кода.
#include "../src/instrument/instrument.h"

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

struct Ev
{
    double t, dur;
    int note;
    float vel;
    int patch;
    float morph;        // для морф-секции
    int fx;             // номер набора эффектов
};

int main()
{
    const double sr = 48000.0, length = 40.0;
    const int total = (int) (sr * length);

    int patchCount = 0;
    const Patch* patches = factoryPatches (patchCount);

    std::vector<Patch> patchList (patches, patches + patchCount);

    // --- события ---------------------------------------------------------
    std::vector<Ev> ev;

    // 1. 0–8 c: Terra Pad, аккорды по 3.5 c
    const int padChords[3][4] = { { 45, 52, 57, 60 }, { 43, 50, 55, 59 }, { 41, 48, 53, 57 } };
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 4; ++k)
            ev.push_back ({ 0.4 + c * 2.6, 3.4, padChords[c][k], 0.75f, 1, 0.0f, 0 });

    // 2. 8–16 c: Glass Keys, арпеджио поверх пэда
    const int glassNotes[] = { 69, 72, 76, 79, 76, 72, 74, 77, 81, 77, 74, 69 };
    for (int i = 0; i < 12; ++i)
        ev.push_back ({ 8.2 + i * 0.62, 1.2, glassNotes[i], 0.8f, 2, 0.0f, 1 });
    ev.push_back ({ 8.0, 7.4, 45, 0.6f, 1, 0.0f, 1 });
    ev.push_back ({ 8.0, 7.4, 52, 0.6f, 1, 0.0f, 1 });
    ev.push_back ({ 8.0, 7.4, 57, 0.6f, 1, 0.0f, 1 });

    // 3. 16–27 c: Morph Eight — морф едет от 0 к 1 и обратно
    const int morphNotes[] = { 52, 57, 60, 64, 67, 64, 60, 57 };
    for (int i = 0; i < 8; ++i)
    {
        const double t = 16.4 + i * 1.3;
        ev.push_back ({ t, 2.6, morphNotes[i], 0.8f, 4, (float) (i % 4) / 3.0f, 2 });
    }

    // 4. 27–36 c: Split Bass/Lead — бас и лид в разных зонах
    const int bassLine[] = { 33, 33, 36, 33, 38, 36, 33, 31, 33, 36, 38, 33, 31, 33 };
    for (int i = 0; i < 14; ++i)
        ev.push_back ({ 27.2 + i * 0.55, 0.55, bassLine[i], 0.9f, 5, 0.0f, 3 });
    const int leadLine[] = { 69, 72, 76, 79, 76, 72, 74, 71, 69, 76, 79, 74, 72, 71 };
    for (int i = 0; i < 14; ++i)
        ev.push_back ({ 27.4 + i * 0.55, 0.7, leadLine[i], 0.7f, 5, 0.0f, 3 });

    // 5. 36–40 c: кода, длинный аккорд на Terra Pad
    for (int k = 0; k < 4; ++k)
        ev.push_back ({ 34.6, 5.4, padChords[0][k] + 12, 0.6f, 1, 0.0f, 4 });

    std::sort (ev.begin(), ev.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });

    // --- наборы эффектов по секциям --------------------------------------
    FxChain::Settings fxSet[5];
    fxSet[0].reverbMix = 0.30f; fxSet[0].reverbSize = 0.7f;  fxSet[0].delayMix = 0.10f;  // пэд
    fxSet[1].reverbMix = 0.28f; fxSet[1].delayMix = 0.30f; fxSet[1].delayTime = 0.31f;   // клавиши
    fxSet[2].reverbMix = 0.35f; fxSet[2].drive = 0.25f;      fxSet[2].delayMix = 0.22f;  // морф
    fxSet[3].reverbMix = 0.18f; fxSet[3].drive = 0.15f;      fxSet[3].delayMix = 0.28f;  // сплит
    fxSet[4].reverbMix = 0.55f; fxSet[4].reverbSize = 0.9f;  fxSet[4].chorusMix = 0.25f; // кода
    for (auto& s : fxSet) { s.compThreshold = -10.0f; s.compRatio = 2.5f; s.compMakeup = 2.0f; s.width = 1.1f; }
    fxSet[3].compThreshold = -8.0f;

    Instrument inst;
    inst.prepare (sr, 16);
    inst.setFx (fxSet[0]);

    std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);
    size_t next = 0;
    int currentFx = -1, currentPatch = -1;

    // снятие нот по времени
    struct Off { double t; int note; };
    std::vector<Off> offs;
    for (const auto& e : ev) offs.push_back ({ e.t + e.dur, e.note });
    std::sort (offs.begin(), offs.end(), [] (const Off& a, const Off& b) { return a.t < b.t; });
    size_t offNext = 0;

    for (int i = 0; i < total; ++i)
    {
        const double t = (double) i / sr;

        while (next < ev.size() && ev[next].t <= t)
        {
            const Ev& e = ev[next];
            if (e.patch != currentPatch || (e.patch == 4))
            {
                Patch p = patchList[(size_t) e.patch];
                if (e.patch == 4) p.morph = e.morph;
                inst.setPatch (p);
                currentPatch = e.patch;
            }
            if (e.fx != currentFx) { inst.setFx (fxSet[(size_t) e.fx]); currentFx = e.fx; }
            inst.noteOn (e.note, e.vel);
            ++next;
        }

        while (offNext < offs.size() && offs[offNext].t <= t)
        {
            inst.noteOff (offs[offNext].note);
            ++offNext;
        }

        float l = 0.0f, r = 0.0f;
        inst.render (l, r);
        L[(size_t) i] = l; R[(size_t) i] = r;
    }

    // --- нормализация и статистика ---------------------------------------
    float peak = 0.0f;
    for (int i = 0; i < total; ++i) peak = std::max (peak, std::max (std::fabs (L[(size_t) i]), std::fabs (R[(size_t) i])));
    const float g = peak > 1e-6f ? 0.89f / peak : 1.0f;
    double accL = 0.0, accR = 0.0, dc = 0.0;
    for (int i = 0; i < total; ++i)
    {
        L[(size_t) i] *= g; R[(size_t) i] *= g;
        accL += (double) L[(size_t) i] * L[(size_t) i];
        accR += (double) R[(size_t) i] * R[(size_t) i];
        dc += L[(size_t) i];
    }

    writeWav16 ("audio/instrument_demo.wav", L, R, (int) sr);
    std::printf ("   слотов в патчах: Terra Pad 3, Glass Keys 3, Morph Eight 8, Split 2\n");
    std::printf ("готово: audio/instrument_demo.wav (%.0f c, %d нот, воров голоса %d)\n",
                 length, (int) ev.size(), inst.stolenCount());
    std::printf ("   пик до нормировки %.3f, RMS L %.3f / R %.3f, DC %.5f\n",
                 peak, std::sqrt (accL / total), std::sqrt (accR / total), dc / total);
    for (int s = 0; s < 8; ++s)
    {
        double acc = 0.0;
        const int a = s * total / 8, b = (s + 1) * total / 8;
        for (int i = a; i < b; ++i) acc += (double) L[(size_t) i] * L[(size_t) i];
        std::printf ("   %2d–%2d c: RMS %.4f\n", s * 5, (s + 1) * 5, std::sqrt (acc / (b - a)));
    }
    return 0;
}
