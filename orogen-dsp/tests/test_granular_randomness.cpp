// Независимость случайных положений и разброса Granular в нотах/слотах.
// При Reverse 0/100% два независимых слоя с ненулевым Spread не должны
// повторять одну и ту же последовательность pitch/position зерна.
#include "engine_suite.h"
#include "../src/instrument/voice.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
using namespace orogen;

namespace orogen {
struct GranularTestProbe
{
    static bool active (const GranularEngine& e, int i) { return e.grains[i].active; }
    static void forceNextSpawn (GranularEngine& e) { e.spawnAcc = 1.0f; }
    static double lower (const GranularEngine& e, int i)
    {
        const auto& g = e.grains[i];
        const double first = g.pos - (double) g.rate;
        return g.rate < 0.0f ? first + (double) g.dur * (double) g.rate : first;
    }
    static double speed (const GranularEngine& e, int i) { return std::fabs ((double) e.grains[i].rate); }
};
}

static Patch spreadPatch (float reverse)
{
    Patch p = patchInit(); p.width = 1.0f;
    auto& a = p.slots[0]; a.on = true; a.type = EngineType::Granular;
    a.params.table = 0; a.params.level = 0.5f; a.params.detune = 0.0f;
    a.params.p[0] = 0.4f; a.params.p[1] = 0.2f;
    a.params.p[2] = 0.7f; a.params.p[3] = 0.6f; a.params.grainReverse = reverse;
    a.attack = 0.005f; a.decay = 0.1f; a.sustain = 1.0f; a.release = 0.1f;
    a.tail = 0.0f; a.cutoff = 1.0f;
    for (int i = 1; i < 8; ++i) p.slots[i].on = false;
    return p;
}

static void startVoice (Voice& voice, const EngineBank& bank, const Patch& p, uint32_t serial)
{
    voice.init (bank, 48000.0); voice.stageSlots (p, bank); voice.applyPendingEngines();
    voice.start (p, 60, 1.0f, 60.0f, serial);
}

static double stereoLayers (const EngineBank& bank, float reverse)
{
    Patch p = spreadPatch (reverse); p.slots[0].params.pan = -1.0f;
    p.slots[1] = p.slots[0]; p.slots[1].params.pan = 1.0f;
    Voice voice; startVoice (voice, bank, p, 123);
    double delta = 0.0, energy = 0.0;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        float l = 0.0f, r = 0.0f; voice.render (p, l, r);
        delta += ((double) l - r) * ((double) l - r);
        energy += ((double) l * l + (double) r * r) * 0.5;
    }
    return std::sqrt (delta / std::max (1.0e-20, energy));
}

static double voices (const EngineBank& bank, uint32_t serialA, uint32_t serialB)
{
    Patch p = spreadPatch (0.0f); Voice a, b;
    startVoice (a, bank, p, serialA); startVoice (b, bank, p, serialB);
    double delta = 0.0, energy = 0.0;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
        a.render (p, al, ar); b.render (p, bl, br);
        delta += ((double) al - bl) * ((double) al - bl); energy += (double) al * al;
    }
    return std::sqrt (delta / std::max (1.0e-20, energy));
}

int main()
{
    std::printf ("OROGEN — Granular: независимые Position/Spread потоков\n");
    EngineBank bank; bank.build (48000.0);
    const double fwd = stereoLayers (bank, 0.0f), rev = stereoLayers (bank, 1.0f);
    const double same = voices (bank, 123, 123), different = voices (bank, 123, 124);
    std::printf ("   стереослоты ΔRMS/RMS forward %.9f, reverse %.9f; ноты тот же/другой serial %.9f / %.9f\n",
                 fwd, rev, same, different);
    suite::check (fwd > 0.1 && rev > 0.1,
                  "Spread даёт независимые зёрна двум слотам без помощи Reverse");
    suite::check (same < 1.0e-7 && different > 0.1,
                  "те же нота/слот/serial воспроизводимы, разные ноты независимы");

    // Считаем реальные рождения, отдельно различия скорости и исходной
    // позиции: одной лишь разницы в аудио для диагностики Spread недостаточно.
    GranularEngine layers[8]; EngineParams layerParams;
    layerParams.p[0] = 0.4f; layerParams.p[1] = 0.2f;
    layerParams.p[2] = 0.7f; layerParams.p[3] = 0.6f; layerParams.detune = 0.0f;
    for (int s = 0; s < 8; ++s)
    {
        layers[s].setBank (&bank.smp); layers[s].setSampleRate (48000.0);
        layers[s].setRandomStreamSeed (noteSlotStreamSeed (123, s));
        layers[s].start (60, 1.0f); layers[s].prepare (layerParams);
    }
    std::array<std::array<bool, GranularEngine::kMaxGrains>, 8> layerPrev {};
    int comparisons = 0, distinctPos = 0, distinctPitch = 0, mismatchedBirths = 0;
    for (int t = 0; t < 48000 * 3; ++t)
    {
        for (auto& layer : layers) (void) layer.render (layerParams);
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            bool born[8];
            for (int s = 0; s < 8; ++s)
            {
                const bool active = GranularTestProbe::active (layers[s], i);
                born[s] = active && ! layerPrev[(size_t) s][(size_t) i];
                layerPrev[(size_t) s][(size_t) i] = active;
            }
            if (born[0])
                for (int s = 1; s < 8; ++s)
                {
                    if (! born[s]) { ++mismatchedBirths; continue; }
                    ++comparisons;
                    if (std::fabs (GranularTestProbe::lower (layers[0], i)
                                 - GranularTestProbe::lower (layers[s], i)) > 0.5) ++distinctPos;
                    if (std::fabs (GranularTestProbe::speed (layers[0], i)
                                 - GranularTestProbe::speed (layers[s], i)) > 1.0e-6) ++distinctPitch;
                }
            else for (int s = 1; s < 8; ++s) if (born[s]) ++mismatchedBirths;
        }
    }
    std::printf ("   A–H, сравнений %d: разные Position %d, pitch %d, несовпадение времени %d\n",
                 comparisons, distinctPos, distinctPitch, mismatchedBirths);
    suite::check (comparisons >= 1000 && mismatchedBirths == 0
                  && distinctPos > comparisons * 0.98 && distinctPitch > comparisons * 0.98,
                  "семь других слотов независимо выбирают Position Spread и pitch Spread");

    // Перевод Spread из нуля ровно в момент рождения нового зерна не должен
    // прерывать уже звучащие окна. Дополнительно проверяем окрестность нуля:
    // отдельный поток не должен переключаться на другую случайную величину
    // при самом маленьком ненулевом значении параметра.
    {
        GranularEngine e[3]; EngineParams ep[3];
        for (int k = 0; k < 3; ++k)
        {
            ep[k].p[0] = 0.4f; ep[k].p[1] = 0.2f; ep[k].p[2] = 0.7f;
            ep[k].p[3] = 0.0f; ep[k].detune = 0.0f; ep[k].level = 1.0f;
            e[k].setBank (&bank.smp); e[k].setSampleRate (48000.0);
            e[k].setRandomStreamSeed (noteSlotStreamSeed (123, 0));
            e[k].start (60, 1.0f); e[k].prepare (ep[k]);
        }
        for (int t = 0; t < 12000; ++t)
            for (int k = 0; k < 3; ++k) (void) e[k].render (ep[k]);
        std::array<bool, GranularEngine::kMaxGrains> before {};
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
            before[(size_t) i] = GranularTestProbe::active (e[0], i);
        ep[1].p[3] = 0.0001f; ep[2].p[3] = 1.0f;
        for (int k = 0; k < 3; ++k) { e[k].prepare (ep[k]); GranularTestProbe::forceNextSpawn (e[k]); }
        float first[3]; for (int k = 0; k < 3; ++k) first[k] = e[k].render (ep[k]);
        int newborn = -1;
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
            if (! before[(size_t) i] && GranularTestProbe::active (e[0], i)) { newborn = i; break; }
        double positionDelta = 1.0e9, rateDelta = 1.0e9;
        if (newborn >= 0 && GranularTestProbe::active (e[1], newborn))
        {
            positionDelta = std::fabs (GranularTestProbe::lower (e[0], newborn) - GranularTestProbe::lower (e[1], newborn));
            rateDelta = std::fabs (GranularTestProbe::speed (e[0], newborn) - GranularTestProbe::speed (e[1], newborn));
        }
        double diffSq = 0.0; int bad = 0;
        for (int t = 0; t < 24000; ++t)
        {
            const float a = e[0].render (ep[0]), b = e[2].render (ep[2]);
            if (! std::isfinite (a) || ! std::isfinite (b)) ++bad;
            diffSq += ((double) a - b) * ((double) a - b);
        }
        const double firstDelta = std::max (std::fabs ((double) first[0] - first[1]),
                                            std::fabs ((double) first[0] - first[2]));
        const double laterRms = std::sqrt (diffSq / 24000.0);
        std::printf ("   Spread 0 -> 0.0001/1 на рождении: первый Δ %.9g, малый Δpos %.6f, Δrate %.9g; далее ΔRMS %.6f, NaN %d\n",
                     firstDelta, positionDelta, rateDelta, laterRms, bad);
        suite::check (newborn >= 0 && firstDelta < 1.0e-7 && positionDelta < 2.0 && rateDelta < 1.0e-4
                      && laterRms > 1.0e-4 && bad == 0,
                      "Spread плавен от нуля, новые зёрна меняются без скачка старых");
    }

    // Независимость seed не должна связывать выбор направления с положением
    // и высотой: один seed + те же параметры -> один отрезок при 0/50/100%.
    GranularEngine engines[3]; EngineParams p[3];
    for (int i = 0; i < 3; ++i)
    {
        p[i].p[0] = 0.4f; p[i].p[1] = 0.2f; p[i].p[2] = 0.7f; p[i].p[3] = 0.6f;
        p[i].detune = 0.0f; p[i].grainReverse = 0.5f * i;
        engines[i].setBank (&bank.smp); engines[i].setSampleRate (48000.0);
        engines[i].setRandomStreamSeed (noteSlotStreamSeed (123, 0));
        engines[i].start (60, 1.0f); engines[i].prepare (p[i]);
    }
    std::array<std::array<bool, GranularEngine::kMaxGrains>, 3> prev {};
    int births = 0, errors = 0; double maxPos = 0.0, maxRate = 0.0;
    for (int t = 0; t < 48000 * 3; ++t)
    {
        for (int k = 0; k < 3; ++k) (void) engines[k].render (p[k]);
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            bool born[3];
            for (int k = 0; k < 3; ++k)
            {
                const bool active = GranularTestProbe::active (engines[k], i);
                born[k] = active && ! prev[(size_t) k][(size_t) i];
                prev[(size_t) k][(size_t) i] = active;
            }
            if (born[0])
            {
                ++births;
                for (int k = 1; k < 3; ++k)
                {
                    if (! born[k]) { ++errors; continue; }
                    maxPos = std::max (maxPos, std::fabs (GranularTestProbe::lower (engines[0], i)
                                                         - GranularTestProbe::lower (engines[k], i)));
                    maxRate = std::max (maxRate, std::fabs (GranularTestProbe::speed (engines[0], i)
                                                           - GranularTestProbe::speed (engines[k], i)));
                }
            }
            else if (born[1] || born[2]) ++errors;
        }
    }
    std::printf ("   0/50/100%% реверса при одном seed: %d зёрен, Δposition %.9f отсч., Δrate %.9f, ошибки времени %d\n",
                 births, maxPos, maxRate, errors);
    suite::check (births >= 150 && errors == 0 && maxPos < 0.02 && maxRate == 0.0,
                  "направление не меняет случайный отрезок, высоту и время рождения");
    std::printf ("\n%s (провалов: %d)\n", suite::gFailures ? "ЕСТЬ ПРОВАЛЫ" : "ВСЕ ТЕСТЫ ПРОЙДЕНЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
