// Независимость направления Granular между слотами и MIDI-нотами.
// Звук проходит через настоящий Voice, его фильтры и равномощную панораму.
#include "engine_suite.h"
#include "../src/instrument/voice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
using namespace orogen;

namespace orogen {
struct GranularTestProbe {
    static bool active (const GranularEngine& e, int i) { return e.grains[i].active; }
    static float rate (const GranularEngine& e, int i) { return e.grains[i].rate; }
    static double lower (const GranularEngine& e, int i) {
        const auto& g = e.grains[i];
        const double start = g.pos - (double) g.rate;
        return g.rate < 0.0f ? start + (double) g.dur * (double) g.rate : start;
    }
};
}

static Patch makePatch (float chance)
{
    Patch p = patchInit();
    p.width = 1.0f;
    auto& a = p.slots[0];
    a.on = true; a.type = EngineType::Granular;
    a.params.table = 0; a.params.level = 0.5f; a.params.detune = 0.0f;
    a.params.p[0] = 0.4f; a.params.p[1] = 0.2f;
    a.params.p[2] = 1.0f; a.params.p[3] = 0.0f;
    a.params.grainReverse = chance;
    a.attack = 0.005f; a.decay = 0.1f; a.sustain = 1.0f;
    a.release = 0.1f; a.tail = 0.0f; a.cutoff = 1.0f;
    for (int i = 1; i < 8; ++i) p.slots[i].on = false;
    return p;
}

static void startVoice (Voice& voice, const EngineBank& bank, const Patch& p, uint32_t serial)
{
    voice.init (bank, 48000.0);
    voice.stageSlots (p, bank);
    voice.applyPendingEngines();
    voice.start (p, 60, 1.0f, 60.0f, serial);
}

// Два идентичных слота, разведённые строго L и R. Разница слышимого сигнала
// означает разное направление отдельных зёрен, а не Detune/Spread/эффекты.
static double twinSlots (const EngineBank& bank, float chance)
{
    Patch p = makePatch (chance);
    p.slots[0].params.pan = -1.0f;
    p.slots[1] = p.slots[0]; p.slots[1].params.pan = 1.0f;
    Voice v; startVoice (v, bank, p, 123);
    double delta = 0.0, energy = 0.0;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        float l = 0.0f, r = 0.0f;
        v.render (p, l, r);
        delta += ((double) l - r) * ((double) l - r);
        energy += ((double) l * l + (double) r * r) * 0.5;
    }
    return std::sqrt (delta / std::max (1.0e-20, energy));
}

static double twinVoices (const EngineBank& bank, float chance, uint32_t serialA, uint32_t serialB)
{
    Patch p = makePatch (chance);
    Voice a, b;
    startVoice (a, bank, p, serialA); startVoice (b, bank, p, serialB);
    double delta = 0.0, energy = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
        a.render (p, al, ar); b.render (p, bl, br);
        delta += ((double) al - bl) * ((double) al - bl);
        energy += (double) al * al;
    }
    return std::sqrt (delta / std::max (1.0e-20, energy));
}

int main()
{
    std::printf ("OROGEN — Granular: независимые потоки реверса\n=================================================\n");
    EngineBank bank; bank.build (48000.0);
    const double slot0 = twinSlots (bank, 0.0f);
    const double slot50 = twinSlots (bank, 0.5f);
    const double slot100 = twinSlots (bank, 1.0f);
    const double same = twinVoices (bank, 0.5f, 123, 123);
    const double different = twinVoices (bank, 0.5f, 123, 124);
    const double forward = twinVoices (bank, 0.0f, 123, 124);
    const double backward = twinVoices (bank, 1.0f, 123, 124);
    std::printf ("   A/B, ΔRMS/RMS при 0/50/100%%: %.9f / %.9f / %.9f\n", slot0, slot50, slot100);
    std::printf ("   две ноты, одинаковый/разный serial 50%%: %.9f / %.9f; разный serial 0/100%%: %.9f / %.9f\n",
                 same, different, forward, backward);
    suite::check (slot0 < 1.0e-5 && slot100 < 1.0e-5,
                  "0% и 100% не зависят от номера слота");
    suite::check (slot50 > 0.1, "при 50% одинаковые слоты выбирают направление независимо",
                  suite::num (slot50, 6));
    suite::check (same < 1.0e-7, "те же нота, слот и serial детерминированы");
    suite::check (different > 0.1, "при 50% разные ноты получают разные потоки реверса",
                  suite::num (different, 6));
    suite::check (forward < 1.0e-7 && backward < 1.0e-7,
                  "направление 0% и 100% не зависит от serial");

    // Для восьми реальных seed из Voice считаем именно рождения и знаки
    // скоростей, а не только изменение аудио. Позиции и Spread не должны
    // зависеть от нового потока направления.
    GranularEngine engines[8];
    EngineParams ep; ep.p[0] = 0.4f; ep.p[1] = 0.2f; ep.p[2] = 1.0f;
    ep.p[3] = 0.35f; ep.detune = 0.0f; ep.grainReverse = 0.5f;
    for (int s = 0; s < 8; ++s)
    {
        engines[s].setBank (&bank.smp); engines[s].setSampleRate (48000.0);
        engines[s].setRandomStreamSeed (noteSlotStreamSeed (123, s));
        engines[s].start (60, 1.0f); engines[s].prepare (ep);
    }
    std::array<std::array<bool, GranularEngine::kMaxGrains>, 8> prev {};
    int count[8] {}, rev[8] {}, agree = 0, pairs = 0, syncErrors = 0;
    double worstPos = 0.0, worstRate = 0.0;
    for (int t = 0; t < 48000 * 4; ++t)
    {
        for (auto& engine : engines) (void) engine.render (ep);
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            bool born[8] {};
            for (int s = 0; s < 8; ++s)
            {
                const bool active = GranularTestProbe::active (engines[s], i);
                born[s] = active && ! prev[(size_t) s][(size_t) i];
                if (born[s]) { ++count[s]; if (GranularTestProbe::rate (engines[s], i) < 0.0f) ++rev[s]; }
                prev[(size_t) s][(size_t) i] = active;
            }
            if (born[0])
            {
                for (int s = 1; s < 8; ++s)
                {
                    if (! born[s]) { ++syncErrors; continue; }
                    worstPos = std::max (worstPos, std::fabs (GranularTestProbe::lower (engines[0], i)
                                                             - GranularTestProbe::lower (engines[s], i)));
                    worstRate = std::max (worstRate, std::fabs (std::fabs ((double) GranularTestProbe::rate (engines[0], i))
                                                               - std::fabs ((double) GranularTestProbe::rate (engines[s], i))));
                    ++pairs;
                    if ((GranularTestProbe::rate (engines[0], i) < 0.0f)
                        == (GranularTestProbe::rate (engines[s], i) < 0.0f)) ++agree;
                }
            }
            else for (int s = 1; s < 8; ++s) if (born[s]) ++syncErrors;
        }
    }
    double minFraction = 1.0, maxFraction = 0.0;
    for (int s = 0; s < 8; ++s)
    {
        const double f = count[s] > 0 ? (double) rev[s] / count[s] : 0.0;
        minFraction = std::min (minFraction, f); maxFraction = std::max (maxFraction, f);
    }
    const double agreement = pairs > 0 ? (double) agree / pairs : 1.0;
    std::printf ("   8 слотов, зёрен/слот %d, reverse %.2f..%.2f%%; совпадение направлений %.2f%%; Δposition %.6f, Δrate %.9f, desync %d\n",
                 count[0], minFraction * 100.0, maxFraction * 100.0, agreement * 100.0,
                 worstPos, worstRate, syncErrors);
    suite::check (count[0] >= 300 && minFraction > 0.38 && maxFraction < 0.62
                  && agreement > 0.38 && agreement < 0.62,
                  "все восемь потоков сохраняют вероятность и различаются");
    suite::check (syncErrors == 0 && worstPos < 0.02 && worstRate == 0.0,
                  "семена направления не меняют время, Position и Spread");
    std::printf ("\n%s (провалов: %d)\n", suite::gFailures ? "ЕСТЬ ПРОВАЛЫ" : "ВСЕ ТЕСТЫ ПРОЙДЕНЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
