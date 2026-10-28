// Патчер: 8 слотов A–H в одном голосе, как в Terrain.
// Слот = движок + его параметры + свой конверт + свой фильтр + панорама.
// Патч описывает, какие движки в каких слотах, и как они складываются:
//   Stack — все включённые слоты звучат одновременно (слои);
//   Morph — по позиции 0..1 слоты кроссфейдятся друг в друга;
//   Split — каждый слот занимает свой диапазон нот (мультизон).
#pragma once

#include "../content/wavetable_bank.h"
#include "../content/sample_bank.h"
#include "../content/resynth_data.h"
#include "../content/loop_bank.h"
#include "../engines/wavetable.h"
#include "../engines/fm.h"
#include "../engines/harmonic.h"
#include "../engines/sample.h"
#include "../engines/granular.h"
#include "../engines/modal.h"
#include "../engines/resynth.h"
#include "../engines/organics.h"
#include "../engines/chop.h"
#include "engine_swap.h"

#include <memory>
#include <cstring>
#include <cmath>

namespace orogen {

enum class EngineType : int
{
    Wavetable = 0, FM, Harmonic, Sample, Granular, Modal, Resynth, Organics,
    Chop,                              // 8 — слайсер лупов (итерация 12)
    Count
};

inline const char* engineTypeName (EngineType t)
{
    switch (t)
    {
        case EngineType::Wavetable: return "Wavetable";
        case EngineType::FM:        return "FM";
        case EngineType::Harmonic:  return "Harmonic";
        case EngineType::Sample:    return "Sample";
        case EngineType::Granular:  return "Granular";
        case EngineType::Modal:     return "Modal";
        case EngineType::Resynth:   return "Resynth";
        case EngineType::Organics:  return "Organics";
        case EngineType::Chop:      return "Chop";
        default:                    return "?";
    }
}

// Общий контент на весь инструмент: голоса только читают его.
struct EngineBank
{
    WavetableBank wt;
    SampleBank    smp;
    ResynthBank   rs;

    LoopBank      loops;

    void build (double sr = 48000.0)
    {
        wt.buildFactory();
        smp.buildFactory (sr);
        rs.build (smp, sr);
        loops.buildFactory (sr);
    }

    double bytes() const
    {
        double b = (double) rs.bytes();
        for (const auto& t : wt.tables)
            for (const auto& lv : t.levels) b += (double) lv.size() * sizeof (float);
        for (const auto& s : smp.shots)
            for (const auto& lv : s.levels) b += (double) lv.size() * sizeof (float);
        for (const auto& l : loops.loops)
            for (const auto& lv : l.shot.levels) b += (double) lv.size() * sizeof (float);
        return b;
    }
};

struct SlotPatch
{
    EngineType   type = EngineType::Wavetable;
    EngineParams params;                    // p[4], level, tuneSemis, detune, table
    bool  on = true;

    // конверт слота (секунды; sustain 0..1)
    float attack = 0.005f, decay = 0.30f, sustain = 0.85f, release = 0.35f;

    // фильтр слота
    float cutoff = 1.0f;                    // 0..1 -> 120 Гц … 18 кГц (экспоненциально)
    float res    = 0.0f;                    // 0..1
    int   filterType = 0;                   // 0..15 = FilterModel (0..4 — старые id)

    // зона (для Split) и вес (для Stack отключается)
    int   loNote = 0, hiNote = 127;

    // хвост после закрытия конверта: чтобы модальный/ресінтез не обрывались щелчком
    float tail = 0.05f;

    static float cutoffHz (float norm) noexcept
    {
        return 120.0f * std::pow (150.0f, clamp (norm, 0.0f, 1.0f));
    }
};

enum class PatchMode : int { Stack = 0, Morph = 1, Split = 2 };

// Куда идёт модуляция. Первые четыре значения — исторические (их использует
// простое поле Lfo.dest), дальше — назначения мод-матрицы.
enum class ModDest : int { Cutoff = 0, Pitch = 1, Morph = 2, Level = 3,
                           Pan = 4, Resonance = 5, Warp = 6, Detune = 7, Count };

// Откуда берётся модуляция. LFO 1/2 — те же генераторы, что и в простом режиме
// (у них свои rate/shape, а amount/dest остаются «быстрым» маршрутом).
enum class ModSource : int { Lfo1 = 0, Lfo2, Env, Velocity, KeyTrack, ModWheel, Random, Bend, Count };

inline const char* modSourceName (ModSource s)
{
    switch (s)
    {
        case ModSource::Lfo1:     return "LFO 1";
        case ModSource::Lfo2:     return "LFO 2";
        case ModSource::Env:      return "Env";
        case ModSource::Velocity: return "Velocity";
        case ModSource::KeyTrack: return "Key Track";
        case ModSource::ModWheel: return "Mod Wheel";
        case ModSource::Random:   return "Random";
        case ModSource::Bend:     return "Bend";
        default:                  return "?";
    }
}

inline const char* modDestName (ModDest d)
{
    switch (d)
    {
        case ModDest::Cutoff:    return "Cutoff";
        case ModDest::Pitch:     return "Pitch";
        case ModDest::Morph:     return "Morph";
        case ModDest::Level:     return "Level";
        case ModDest::Pan:       return "Pan";
        case ModDest::Resonance: return "Resonance";
        case ModDest::Warp:      return "Warp";
        case ModDest::Detune:    return "Detune";
        default:                 return "?";
    }
}

// Один маршрут мод-матрицы. amount −1…+1, знак важен (инверсия).
// Значения: Cutoff — ±2 октавы, Pitch — ±12 полутонов, Morph/Level/Pan/
// Resonance/Warp/Detune — ±1 в своих единицах (см. voice.h, applyRoutes()).
struct ModRoute
{
    ModSource source = ModSource::Lfo1;
    ModDest   dest   = ModDest::Cutoff;
    float     amount = 0.0f;
};

static constexpr int kModRoutes = 8;

struct Lfo
{
    float rate = 1.0f;      // Гц
    float amount = 0.0f;    // 0..1
    ModDest dest = ModDest::Cutoff;
    int   shape = 0;        // 0 синус, 1 треугольник, 2 S&H (квантованный)
};

struct Patch
{
    char     name[32] = "Init";
    PatchMode mode = PatchMode::Stack;
    SlotPatch slots[8];

    float morph = 0.0f;         // 0..1 — позиция морф-кроссфейда
    float glide = 0.0f;         // секунды на полутон→цель (0 = без глиссандо)
    float width = 1.0f;         // стерео-ширина 0..1
    float gain  = 1.0f;         // калибровка громкости патча (замером, не на глаз)
    Lfo   lfo[2];
    ModRoute routes[kModRoutes];   // мод-матрица: 8 маршрутов, amount = 0 — выключен
    float bend = 0.0f;             // −1..1 — колесо высоты, источник для матрицы
    float modWheel = 0.0f;      // 0..1 — общий вход модуляции

    void setName (const char* n) { std::strncpy (name, n, sizeof (name) - 1); name[sizeof (name) - 1] = 0; }
};

// Создание движка нужного типа с привязкой контента. Вызывается вне аудио-потока
// (при загрузке патча) — аллокация здесь допустима.
inline std::unique_ptr<EngineBase> makeEngine (EngineType t, const EngineBank& b)
{
    // счётчик жизни движков: тест «создано == удалено» ловит потерю указателя
    EngineStats::created.fetch_add (1, std::memory_order_relaxed);
    switch (t)
    {
        case EngineType::Wavetable:
        {
            auto e = std::make_unique<WavetableEngine>();
            e->setBank (&b.wt);
            return e;
        }
        case EngineType::FM:       return std::make_unique<FmEngine>();
        case EngineType::Harmonic: return std::make_unique<HarmonicEngine>();
        case EngineType::Sample:
        {
            auto e = std::make_unique<SampleEngine>();
            e->setBank (&b.smp);
            return e;
        }
        case EngineType::Granular:
        {
            auto e = std::make_unique<GranularEngine>();
            e->setBank (&b.smp);
            return e;
        }
        case EngineType::Modal:    return std::make_unique<ModalEngine>();
        case EngineType::Resynth:
        {
            auto e = std::make_unique<ResynthEngine>();
            e->setBank (&b.rs);
            return e;
        }
        case EngineType::Organics:
        {
            auto e = std::make_unique<OrganicsEngine>();
            e->setBank (&b.smp);
            return e;
        }
        case EngineType::Chop:
        {
            auto e = std::make_unique<ChopEngine>();
            e->setBank (&b.loops);
            return e;
        }
        default: return nullptr;
    }
}

// ---------- фабричные патчи -------------------------------------------------
// gain — калибровка замером: RMS за первую секунду аккорда из четырёх нот
// у всех патчей 0.13 ± 1 дБ, пик < 0.9 (проверяется в tests/test_instrument.cpp).
// Калибровать по «установившемуся» RMS нельзя: у перкуссивных патчей его нет.

inline SlotPatch makeSlot (EngineType t, int table, float p0, float p1, float p2, float p3,
                           float level, float pan, float atk, float dec, float sus, float rel)
{
    SlotPatch s;
    s.type = t;
    s.params.table = table;
    s.params.p[0] = p0; s.params.p[1] = p1; s.params.p[2] = p2; s.params.p[3] = p3;
    s.params.level = level;
    s.params.pan = pan;
    s.attack = atk; s.decay = dec; s.sustain = sus; s.release = rel;
    return s;
}

inline Patch patchInit()
{
    Patch p; p.setName ("Init");
    p.gain = 0.67f;
    p.slots[0] = makeSlot (EngineType::Wavetable, 0, 0.35f, 0.2f, 0.15f, 0.3f, 0.8f, 0.0f, 0.005f, 0.4f, 0.9f, 0.4f);
    for (int i = 1; i < 8; ++i) p.slots[i].on = false;
    return p;
}

// «Terra Pad» — три слоя: таблица + резонансная модальность + воздух Organics
inline Patch patchTerraPad()
{
    Patch p; p.setName ("Terra Pad");
    p.gain = 1.49f;
    p.slots[0] = makeSlot (EngineType::Wavetable, 3, 0.25f, 0.15f, 0.2f, 0.25f, 0.55f, -0.30f, 0.9f, 1.5f, 0.85f, 1.6f);
    p.slots[0].cutoff = 0.62f; p.slots[0].res = 0.15f;
    p.slots[1] = makeSlot (EngineType::Organics, 2, 0.55f, 0.45f, 0.35f, 0.30f, 0.45f, 0.30f, 1.2f, 2.0f, 0.80f, 2.0f);
    p.slots[2] = makeSlot (EngineType::Resynth, 1, 0.30f, 0.60f, 0.20f, 0.40f, 0.35f, 0.0f, 1.5f, 2.0f, 0.70f, 2.2f);
    for (int i = 3; i < 8; ++i) p.slots[i].on = false;
    return p;
}

// «Glass Keys» — модальные клавиши с FM-слоем и щипком сэмпла
inline Patch patchGlassKeys()
{
    Patch p; p.setName ("Glass Keys");
    p.gain = 1.57f;
    p.slots[0] = makeSlot (EngineType::Modal, 0, 0.62f, 0.35f, 0.2f, 0.1f, 0.60f, -0.15f, 0.002f, 2.5f, 0.0f, 0.9f);
    p.slots[0].tail = 1.5f;
    p.slots[1] = makeSlot (EngineType::FM, 0, 0.06f, 0.30f, 0.0f, 0.05f, 0.30f, 0.20f, 0.002f, 1.8f, 0.0f, 0.8f);
    p.slots[1].tail = 1.0f;
    p.slots[2] = makeSlot (EngineType::Sample, 3, 0.20f, 0.5f, 0.0f, 0.0f, 0.28f, 0.0f, 0.001f, 1.2f, 0.0f, 0.6f);
    for (int i = 3; i < 8; ++i) p.slots[i].on = false;
    return p;
}

// «Grain Choir» — гранулярный хор + аддитивный слой, медленная атака
inline Patch patchGrainChoir()
{
    Patch p; p.setName ("Grain Choir");
    p.gain = 1.84f;
    p.slots[0] = makeSlot (EngineType::Granular, 2, 0.35f, 0.55f, 0.45f, 0.5f, 0.55f, -0.35f, 0.6f, 1.2f, 0.9f, 2.0f);
    p.slots[1] = makeSlot (EngineType::Harmonic, 3, 0.35f, 0.8f, 0.1f, 0.25f, 0.45f, 0.35f, 0.8f, 1.5f, 0.85f, 2.2f);
    p.slots[2] = makeSlot (EngineType::Organics, 6, 0.45f, 0.55f, 0.3f, 0.4f, 0.30f, 0.0f, 1.0f, 1.6f, 0.75f, 2.4f);
    for (int i = 3; i < 8; ++i) p.slots[i].on = false;
    return p;
}

// «Morph Eight» — все восемь движков в морф-режиме: одна ручка ведёт от
// табличного звука через FM/аддитив/сэмпл к шуму и телу Organics.
inline Patch patchMorphEight()
{
    Patch p; p.setName ("Morph Eight");
    p.gain = 0.49f;
    p.mode = PatchMode::Morph;
    p.morph = 0.0f;
    p.lfo[0] = { 0.08f, 0.0f, ModDest::Morph, 0 };
    p.slots[0] = makeSlot (EngineType::Wavetable, 0, 0.3f, 0.2f, 0.1f, 0.3f, 0.8f, -0.4f, 0.01f, 0.5f, 0.9f, 0.6f);
    p.slots[1] = makeSlot (EngineType::FM, 0, 0.45f, 0.55f, 0.2f, 0.0f, 0.75f, -0.25f, 0.01f, 0.6f, 0.85f, 0.7f);
    p.slots[2] = makeSlot (EngineType::Harmonic, 1, 0.5f, 0.7f, 0.2f, 0.2f, 0.7f, -0.1f, 0.01f, 0.6f, 0.9f, 0.7f);
    p.slots[3] = makeSlot (EngineType::Sample, 4, 0.3f, 0.55f, 0.0f, 0.0f, 0.7f, 0.0f, 0.01f, 0.6f, 0.85f, 0.7f);
    p.slots[4] = makeSlot (EngineType::Granular, 2, 0.4f, 0.5f, 0.4f, 0.5f, 0.7f, 0.1f, 0.05f, 0.6f, 0.9f, 0.8f);
    p.slots[5] = makeSlot (EngineType::Modal, 0, 0.7f, 0.5f, 0.2f, 0.0f, 0.7f, 0.25f, 0.01f, 0.8f, 0.4f, 0.8f);
    p.slots[6] = makeSlot (EngineType::Resynth, 1, 0.4f, 0.5f, 0.2f, 0.3f, 0.7f, 0.35f, 0.02f, 0.6f, 0.85f, 0.8f);
    p.slots[7] = makeSlot (EngineType::Organics, 2, 0.5f, 0.5f, 0.4f, 0.4f, 0.7f, 0.4f, 0.02f, 0.6f, 0.85f, 0.9f);
    return p;
}

// «Split Bass/Lead» — нижняя зона бас, верхняя — щипковый лид
inline Patch patchSplitBassLead()
{
    Patch p; p.setName ("Split Bass/Lead");
    p.mode = PatchMode::Split;
    p.gain = 1.76f;              // замер по RMS: два слота со спадом тише, чем сумма восьми
    p.slots[0] = makeSlot (EngineType::FM, 2, 0.02f, 0.45f, 0.0f, 0.35f, 0.75f, 0.0f, 0.004f, 0.35f, 0.7f, 0.25f);
    p.slots[0].loNote = 0; p.slots[0].hiNote = 59;
    p.slots[1] = makeSlot (EngineType::Sample, 1, 0.25f, 0.6f, 0.0f, 0.0f, 0.70f, 0.0f, 0.001f, 0.9f, 0.0f, 0.5f);
    p.slots[1].loNote = 60; p.slots[1].hiNote = 127;
    p.slots[1].tail = 0.8f;
    // щипок Karplus широкополосный: без фильтра лид «шипит» до 16 кГц
    // (−34 дБ от полной энергии в полосе 12–20 кГц)
    p.slots[1].cutoff = 0.72f; p.slots[1].res = 0.10f;
    for (int i = 2; i < 8; ++i) p.slots[i].on = false;
    return p;
}

// «Chop Break» — слайсер CHOP: ноты 45+ раскладывают удары брейка,
// плюс тихий Karplus для тела. Первый фабричный патч на движке 9.
inline Patch patchChopBreak()
{
    Patch p; p.setName ("Chop Break");
    p.gain = 3.45f;          // замер по RMS: перкуссивный слот со спадом 0.6 с
                             // тише тональных; поднято до общей калибровки
    // p0 = 0 -> слайс выбирается нотой; p1 = 1 -> играем слайс целиком;
    // p2 = 0 -> без повторов; p3 = 0 -> прямой ход
    p.slots[0] = makeSlot (EngineType::Chop, 0, 0.0f, 0.85f, 0.0f, 0.0f,
                           0.95f, -0.15f, 0.001f, 0.60f, 0.0f, 0.30f);
    p.slots[0].tail = 0.10f;
    // тело: короткий щипок, чтобы удары не были «сухими»
    p.slots[1] = makeSlot (EngineType::Sample, 1, 0.25f, 0.60f, 0.0f, 0.0f,
                           0.35f, 0.20f, 0.001f, 0.45f, 0.0f, 0.25f);
    p.slots[1].cutoff = 0.70f; p.slots[1].res = 0.05f;
    p.slots[1].tail = 0.30f;
    for (int i = 2; i < 8; ++i) p.slots[i].on = false;
    return p;
}

inline const Patch* factoryPatches (int& count)
{
    static Patch list[7] = { patchInit(), patchTerraPad(), patchGlassKeys(),
                             patchGrainChoir(), patchMorphEight(), patchSplitBassLead(),
                             patchChopBreak() };
    count = 7;
    return list;
}

} // namespace orogen
