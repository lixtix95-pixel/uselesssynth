// Офлайн-рендер демо движков FM и Harmonic (два файла за один прогон).
//   audio/fm_demo.wav        — 6 пресетов FM по 3.3 c
//   audio/harmonic_demo.wav  — 4 тембра аддитивного движка по 5 c
#include "../src/engines/fm.h"
#include "../src/engines/harmonic.h"
#include "../src/core/svf.h"
#include "../src/core/adsr.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>
#include <cmath>
#include <array>

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
    EngineBase* eng = nullptr;
    Adsr  env;
    Svf   svf;
    float pan = 0.0f, amp = 1.0f;
    bool  used = false;
    double endT = 0.0;
};

template <typename Engine>
static void renderFile (const char* path, const std::vector<std::array<float, 4>>& settings,
                        const std::vector<std::vector<int>>& phrases, double total,
                        float tone)
{
    const double sr = 48000.0;
    const int n = (int) (sr * total);
    std::vector<float> L ((size_t) n, 0.0f), R ((size_t) n, 0.0f);
    std::deque<Voice> voices (16);
    // пик до 1.9 при 16 голосах: усиление на голос снижено, чтобы не клиппило до нормировки
    const float voiceGain = 0.42f;
    std::vector<Engine> engines (16);
    for (int i = 0; i < 16; ++i) { voices[(size_t) i].eng = &engines[(size_t) i]; voices[(size_t) i].eng->setSampleRate (sr); voices[(size_t) i].env.setSampleRate (sr); voices[(size_t) i].svf.setSampleRate (sr); }

    struct Note { double t, dur; int note; int idx; };
    std::vector<Note> notes;
    const double seg = total / (double) phrases.size();
    for (size_t p = 0; p < phrases.size(); ++p)
        for (size_t k = 0; k < phrases[p].size(); ++k)
            notes.push_back ({ p * seg + 0.15 + k * (seg - 0.3) / (double) phrases[p].size(),
                               (seg - 0.3) / (double) phrases[p].size() * 1.6,
                               phrases[p][k], (int) p });

    size_t next = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        while (next < notes.size() && notes[next].t <= t)
        {
            const Note& nt = notes[next];
            Voice* slot = nullptr;
            for (auto& v : voices) if (! v.used) { slot = &v; break; }
            if (! slot) { ++next; continue; }
            slot->used = true; slot->endT = nt.t + nt.dur; slot->amp = voiceGain;
            slot->pan = -0.35f + 0.7f * (float) ((nt.idx * 7 + (int) (nt.note % 5)) % 4) / 3.0f;
            slot->eng->start (nt.note, 0.9f);
            EngineParams p;
            p.table = nt.idx; p.detune = 0.15f; p.level = 1.0f;
            const auto& s = settings[(size_t) (nt.idx % (int) settings.size())];
            p.p[0] = s[0]; p.p[1] = s[1]; p.p[2] = s[2]; p.p[3] = s[3];
            slot->eng->prepare (p);
            slot->svf.setParams (tone, 0.3f, 0.0f);
            slot->env.setParams (0.006f, 0.12f, 0.8f, 0.5f);
            slot->env.noteOn();
            ++next;
        }

        double l = 0.0, r = 0.0;
        for (auto& v : voices)
        {
            if (! v.used) continue;
            if (t >= v.endT) v.env.noteOff();
            const EngineParams dummy {};
            const float s = v.eng->render (dummy);
            const float e = v.env.process();
            const float y = v.svf.process (s, FilterType::LP12) * e * v.amp;
            l += y * std::cos ((v.pan + 1.0f) * 0.25f * (float) kPi);
            r += y * std::sin ((v.pan + 1.0f) * 0.25f * (float) kPi);
            if (! v.env.isActive() && e < 1e-5f) { v.used = false; v.eng->stop(); }
        }
        L[(size_t) i] = (float) l; R[(size_t) i] = (float) r;
    }

    float peak = 0.0f;
    for (int i = 0; i < n; ++i) { peak = std::max (peak, std::fabs (L[(size_t) i])); peak = std::max (peak, std::fabs (R[(size_t) i])); }
    const float g = (peak > 1e-6f) ? 0.89f / peak : 1.0f;
    double sL = 0, sR = 0, dc = 0;
    for (int i = 0; i < n; ++i)
    { L[(size_t) i] *= g; R[(size_t) i] *= g; sL += (double) L[(size_t) i] * L[(size_t) i]; sR += (double) R[(size_t) i] * R[(size_t) i]; dc += L[(size_t) i]; }
    writeWav16 (path, L, R, (int) sr);
    std::printf ("готово: %s (%.1f c)\n", path, total);
    std::printf ("   пик до норм %.3f, RMS L %.3f / R %.3f, DC %.5f\n", peak, std::sqrt (sL / n), std::sqrt (sR / n), dc / n);
}

int main()
{
    // FM: 6 пресетов (ratio, index, index2, feedback)
    std::vector<std::array<float, 4>> fm = {
        { 0.06f, 0.30f, 0.00f, 0.05f },   // Tine
        { 0.45f, 0.55f, 0.20f, 0.00f },   // Bell
        { 0.02f, 0.45f, 0.00f, 0.35f },   // Bass
        { 0.75f, 0.85f, 0.40f, 0.20f },   // Metal
        { 0.55f, 0.25f, 0.60f, 0.00f },   // Glass
        { 0.20f, 0.35f, 0.10f, 0.10f },   // Organ
    };
    std::vector<std::vector<int>> fmPhrases = {
        { 57, 60, 64, 67 }, { 69, 72, 76, 79 }, { 45, 45, 52, 45 },
        { 76, 79, 83, 86 }, { 64, 71, 76, 83 }, { 60, 64, 67, 72 },
    };
    renderFile<FmEngine> ("audio/fm_demo.wav", fm, fmPhrases, 20.0, 9000.0f);

    // Harmonic: 4 тембра (tilt, shape=чётные, spread=растяжка, motion)
    std::vector<std::array<float, 4>> hm = {
        { 0.15f, 1.00f, 0.00f, 0.10f },   // тёмный орган
        { 0.80f, 1.00f, 0.00f, 0.20f },   // яркая пила
        { 0.35f, 0.70f, 0.45f, 0.35f },   // расстроенный ансамбль
        { 0.65f, 0.85f, 0.85f, 0.60f },   // «стеклянный» с движением
    };
    std::vector<std::vector<int>> hmPhrases = {
        { 50, 57, 62, 65 }, { 62, 65, 69, 74 }, { 45, 52, 57, 64 }, { 67, 71, 74, 79 },
    };
    renderFile<HarmonicEngine> ("audio/harmonic_demo.wav", hm, hmPhrases, 20.0, 11000.0f);
    return 0;
}
