// Голос инструмента: 8 слотов A–H, у каждого свой движок, конверт, фильтр,
// панорама и хвост. Голос не знает про полифонию и эффекты — только про слоты.
//
// Частота управления (control rate) — 64 сэмпла: на каждом шаге движкам
// вызывается prepare() с актуальными параметрами (глиссандо, модуляция,
// морф), а render() работает каждый сэмпл.
#pragma once

#include "patcher.h"
#include "engine_swap.h"
#include "../core/svf.h"
#include "../core/filters.h"
#include "../core/adsr.h"

#include <array>
#include <memory>
#include <cmath>

namespace orogen {

class Voice
{
public:
    static constexpr int kSlots = 8;
    static constexpr int kControlBlock = 128;

    void init (const EngineBank& bank, double sr, EngineRetireBin* retireBinToUse = nullptr)
    {
        sampleRate = sr;
        retireBin = retireBinToUse;
        for (int i = 0; i < kSlots; ++i)
        {
            env[(size_t) i].setSampleRate (sr);
            filter[(size_t) i].prepare (sr);        // буфер гребёнки — здесь, не в аудио-потоке
            gainSm[(size_t) i].reset (sr, 8.0);
            panL[(size_t) i] = panR[(size_t) i] = 0.0f;
        }
        controlCounter = 0;
        active = false;
        (void) bank;
    }

    // Подготовка движков слотов — ТОЛЬКО вне аудио-потока.
    //
    // Создаёт новый движок в pending[i], если тип слота отличается от того,
    // что сейчас работает (или уже подготовлен). Работающий движок не
    // трогается: аудио-поток продолжает его использовать, пока не вызовет
    // applyPendingEngines(). Раньше здесь стояла замена «на месте» — и смена
    // типа движка в работающем плагине означала аллокацию внутри processBlock.
    void stageSlots (const Patch& p, const EngineBank& bank)
    {
        uint32_t add = 0;
        const uint32_t staged = pendingMask.load (std::memory_order_acquire);
        for (int i = 0; i < kSlots; ++i)
        {
            const uint32_t bit = 1u << i;
            const bool willExist = engines[(size_t) i] != nullptr || (staged & bit) != 0;
            const EngineType willBe = (staged & bit) != 0 ? pendingType[(size_t) i]
                                                          : slotType[(size_t) i];
            if (willExist && willBe == p.slots[i].type) continue;

            // если в pending уже лежал невостребованный движок (тип сменили дважды
            // между двумя блоками), он удаляется здесь же — это message thread
            if (pending[(size_t) i]) EngineStats::deleted.fetch_add (1, std::memory_order_relaxed);
            pending[(size_t) i] = makeEngine (p.slots[i].type, bank);
            pendingType[(size_t) i] = p.slots[i].type;
            if (pending[(size_t) i]) pending[(size_t) i]->setSampleRate (sampleRate);
            add |= bit;
        }
        if (add != 0)
            pendingMask.store (staged | add, std::memory_order_release);
    }

    // Сколько слотов ждёт применения (для тестов и диагностики).
    int pendingCount() const noexcept { return popCount (pendingMask.load (std::memory_order_acquire)); }

    // Применение подготовленных движков — ТОЛЬКО аудио-поток.
    // Только обмен указателей и уборка старых в retire bin: ни одной аллокации.
    // Если места в bin не хватает, обмен откладывается на следующий блок
    // (звук при этом остаётся прежним — старые движки работают).
    void applyPendingEngines() noexcept
    {
        const uint32_t mask = pendingMask.load (std::memory_order_acquire);
        if (mask == 0) return;
        const int n = popCount (mask);
        if (retireBin != nullptr && retireBin->room() < n) return;   // подождём

        pendingMask.store (0, std::memory_order_release);
        for (int i = 0; i < kSlots; ++i)
        {
            if ((mask & (1u << i)) == 0) continue;
            if (engines[(size_t) i])
            {
                EngineBase* old = engines[(size_t) i].release();
                if (retireBin == nullptr || ! retireBin->push (old))
                {
                    delete old;                       // без bin: единственный путь
                    EngineStats::deleted.fetch_add (1, std::memory_order_relaxed);
                }
            }
            engines[(size_t) i] = std::move (pending[(size_t) i]);
            slotType[(size_t) i] = pendingType[(size_t) i];
            env[(size_t) i].reset();
            slotRunning[(size_t) i] = false;
            lastParams[(size_t) i] = EngineParams();
            lastParamsValid[(size_t) i] = false;
        }
    }

    void setSampleRate (double sr)
    {
        sampleRate = sr;
        for (int i = 0; i < kSlots; ++i)
        {
            env[(size_t) i].setSampleRate (sr);
            filter[(size_t) i].prepare (sr);        // буфер гребёнки — здесь, не в аудио-потоке
            gainSm[(size_t) i].reset (sr, 8.0);
            if (engines[(size_t) i]) engines[(size_t) i]->setSampleRate (sr);
            if (pending[(size_t) i]) pending[(size_t) i]->setSampleRate (sr);
        }
    }

    bool isActive() const { return active; }
    int  note() const { return targetNote; }
    double age() const { return ageSamples; }
    bool isReleased() const { return released; }

    // Запуск ноты. glideFrom — откуда начинать глиссандо (в полутонах).
    void start (const Patch& p, int midiNote, float velocity, float glideFrom,
                uint32_t serial = 0)
    {
        applyPendingEngines();          // страховка: слоты должны быть готовы
        targetNote = midiNote;
        velocityGain = 0.35f + 0.65f * clamp (velocity, 0.0f, 1.0f);
        currentPitch = (p.glide > 1.0e-4f) ? glideFrom : (float) midiNote;
        targetPitch  = (float) midiNote;
        ageSamples = 0.0;
        released = false;
        active = true;
        controlCounter = 0;
        // источник Random: своё значение на КАЖДУЮ ноту. Зерно зависит от
        // порядкового номера ноты в инструменте, иначе все голоса (у них
        // одинаковое начальное зерно) выдавали бы одно и то же значение.
        noteRng.seed (0x9E3779B9u ^ (serial * 2654435761u));
        noteRandom = noteRng.nextBipolar();
        modWarpF = modDetuneF = 0.0f;

        for (int i = 0; i < kSlots; ++i)
        {
            // Слот не запускаем, если он выключен или его вес равен нулю:
            // раньше запускались все восемь движков, и семь «пустых» крутились
            // вхолостую, съедая ~75 % ядра на один голос вместо 9 %.
            if (engines[(size_t) i]) engines[(size_t) i]->stop();
            env[(size_t) i].reset();
            tailLeft[(size_t) i] = 0.0f;
            lastAmp[(size_t) i] = 0.0f;
            slotStarted[(size_t) i] = false;
            slotRunning[(size_t) i] = false;
        }

        const float morphNow = clamp (p.morph, 0.0f, 1.0f);
        for (int i = 0; i < kSlots; ++i)
        {
            const SlotPatch& s = p.slots[i];
            const float w = slotWeight (p, i, morphNow, midiNote);
            gainSm[(size_t) i].snap (w * s.params.level * velocityGain);
            const float pan = clamp (s.params.pan, -1.0f, 1.0f) * clamp (p.width, 0.0f, 1.0f);
            panL[(size_t) i] = std::cos ((pan + 1.0f) * 0.25f * (float) kPi);
            panR[(size_t) i] = std::sin ((pan + 1.0f) * 0.25f * (float) kPi);
            if (w > 1.0e-5f) startSlot (p, i);
        }
    }

    void noteOff()
    {
        if (! active) return;
        released = true;
        for (auto& e : env) if (e.isActive()) e.noteOff();
    }

    // Быстрое снятие голоса (voice stealing): 6 мс на затухание.
    void kill() { killFade = 1.0f; }

    // Один сэмпл стерео. Возвращает false, когда голос полностью отзвучал.
    bool render (const Patch& p, float& outL, float& outR)
    {
        if (! active) return false;

        if (controlCounter == 0) updateControl (p);

        float l = 0.0f, r = 0.0f;
        bool any = false;
        const float dt = 1.0f / (float) sampleRate;

        for (int i = 0; i < kSlots; ++i)
        {
            auto& e = engines[(size_t) i];
            if (! e) continue;

            const float eVal = env[(size_t) i].process();
            float amp = 0.0f;

            if (env[(size_t) i].isActive())
            {
                amp = eVal;
                lastAmp[(size_t) i] = eVal;
                tailLeft[(size_t) i] = p.slots[i].tail;
            }
            else if (tailLeft[(size_t) i] > 0.0f && p.slots[i].tail > 1.0e-4f)
            {
                // конверт закрылся, но движок ещё звенит (модальный хвост,
                // гранулы, резонанс): пропускаем его звук через спад
                // Хвост пропускаем с огибающей 1→0, а НЕ умноженным на последнее
                // значение конверта: у перкуссивных патчей (sustain = 0) конверт
                // закрывается на −80 дБ, и умножение убивало звон модального слоя.
                tailLeft[(size_t) i] -= dt;
                amp = clamp (tailLeft[(size_t) i] / p.slots[i].tail, 0.0f, 1.0f);
            }
            else
            {
                amp = 0.0f;
            }

            // Отпущенный слот, упавший ниже −60 дБ, считаем отзвучавшим: иначе
            // голос держится 6+ секунд после «короткого» release и зря жжёт CPU.
            if (released && env[(size_t) i].isActive() && eVal < 1.0e-3f)
            {
                env[(size_t) i].reset();
                tailLeft[(size_t) i] = 0.0f;
                amp = 0.0f;
            }

            // слот с нулевым весом (морф/сплит) вообще не считаем, но конверт
            // продолжаем продвигать — иначе при возврате веса будет щелчок
            const float weightNow = gainSm[(size_t) i].getCurrent();
            const bool alive = slotStarted[(size_t) i]
                            && (amp > 0.0f || env[(size_t) i].isActive())
                            && (weightNow > 1.0e-5f || tailLeft[(size_t) i] > 0.0f);
            if (! alive)
            {
                if (slotRunning[(size_t) i]) { e->stop(); slotRunning[(size_t) i] = false; }
                continue;
            }
            slotRunning[(size_t) i] = true;

            // Параметры на блоке управления, а не на каждом сэмпле: раньше здесь
            // собиралась структура + три clamp'а на слот (8 слотов × 16 голосов
            // × 48 кГц). Движки читают параметры в prepare(), render() их не трогает.
            const EngineParams& ep = epCache[(size_t) i];
            const float sig = e->render (ep);
            if (amp <= 0.0f) { any = true; continue; }
            any = true;

            const float g = gainSm[(size_t) i].next() * amp;
            float y = filter[(size_t) i].process (sig * g, filterModelFromInt (p.slots[i].filterType));
            slotMeter[(size_t) i] = std::max (slotMeter[(size_t) i], std::fabs (y));
            l += y * panL[(size_t) i];
            r += y * panR[(size_t) i];
        }

        // глиссандо на уровне сэмпла (сглажено), чтобы не было «ступенек»
        if (p.glide > 1.0e-4f && std::fabs (targetPitch - currentPitch) > 1.0e-4f)
        {
            const float coef = 1.0f - std::exp (-dt / (p.glide * 0.35f + 1.0e-5f));
            currentPitch += (targetPitch - currentPitch) * coef;
        }

        if (killFade > 0.0f)
        {
            killFade -= 1.0f / (0.006f * (float) sampleRate);
            const float k = clamp (killFade, 0.0f, 1.0f);
            l *= k; r *= k;
            if (killFade <= 0.0f) { stopAll(); return false; }
        }

        // мягкий анти-клип на голосе: 8 слотов складываются, суммарный пик
        // может превысить 1 даже при разумных уровнях слоёв
        l = fastTanh (l * 0.85f) * 1.15f;
        r = fastTanh (r * 0.85f) * 1.15f;

        outL = l;
        outR = r;

        ageSamples += 1.0;
        if (++controlCounter >= kControlBlock) controlCounter = 0;

        if (! any) { active = false; return false; }
        return true;
    }

    // Пиковый уровень каждого слота за последнее окно управления (для метринга).
    const std::array<float, kSlots>& slotMeters() const { return slotMeter; }
    void decayMeters() { for (auto& m : slotMeter) m *= 0.9f; }

    float cpuCost() const
    {
        float c = 0.0f;
        for (int i = 0; i < kSlots; ++i)
            if (engines[(size_t) i] && env[(size_t) i].isActive()) c += engines[(size_t) i]->costFactor();
        return c;
    }

private:
    void stopAll()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            if (engines[(size_t) i]) engines[(size_t) i]->stop();
            env[(size_t) i].reset();
            tailLeft[(size_t) i] = 0.0f;
            lastAmp[(size_t) i] = 0.0f;
            slotRunning[(size_t) i] = false;
        }
        active = false;
    }

    // вес слота: Stack — вкл/выкл, Morph — кроссфейд по позиции, Split — зона
    float slotWeight (const Patch& p, int i, float morph, int note) const
    {
        const SlotPatch& s = p.slots[i];
        if (! s.on) return 0.0f;

        // Диапазон клавиш действует во всех режимах: слой, ограниченный
        // зоной, должен молчать вне неё и в Stack, и в Morph (раньше lo/hi
        // учитывались только в Split — ручки в интерфейсе не работали).
        const bool inRange = (note >= s.loNote && note <= s.hiNote);

        switch (p.mode)
        {
            case PatchMode::Stack:
                return inRange ? 1.0f : 0.0f;

            case PatchMode::Split:
                return inRange ? 1.0f : 0.0f;

            case PatchMode::Morph:
            {
                if (! inRange) return 0.0f;
                const float pos = clamp (morph, 0.0f, 1.0f) * (float) (kSlots - 1);
                const float d = std::fabs (pos - (float) i);
                return clamp (1.0f - d, 0.0f, 1.0f);
            }
        }
        return 0.0f;
    }

    EngineParams slotParams (const Patch& p, int i)
    {
        EngineParams ep = p.slots[i].params;
        ep.tuneSemis += currentPitch - (float) targetNote + pitchMod;
        ep.p[0] = clamp (ep.p[0] + modWarpF, 0.0f, 1.0f);
        ep.detune = clamp (ep.detune + modDetuneF * 0.5f, 0.0f, 1.0f);
        ep.level = clamp (ep.level, 0.0f, 1.0f);
        return ep;
    }

    static bool paramsEqual (const EngineParams& a, const EngineParams& b) noexcept
    {
        if (a.table != b.table) return false;
        for (int k = 0; k < 4; ++k) if (std::fabs (a.p[k] - b.p[k]) > 1.0e-4f) return false;
        return std::fabs (a.tuneSemis - b.tuneSemis) < 1.0e-3f
            && std::fabs (a.detune - b.detune) < 1.0e-4f
            && std::fabs (a.level - b.level) < 1.0e-3f;
    }

    // Значение конверта голоса для мод-матрицы: максимум по слотам, чтобы
    // «Env» следовал за слышимым звуком, а не за конкретным слотом.
    float envValue() const noexcept
    {
        float v = 0.0f;
        for (int i = 0; i < kSlots; ++i)
            if (slotRunning[(size_t) i]) v = std::max (v, env[(size_t) i].getValue());
        return v;
    }

    void startSlot (const Patch& p, int i)
    {
        auto& e = engines[(size_t) i];
        if (! e) return;
        const SlotPatch& s = p.slots[i];
        env[(size_t) i].setParams (s.attack, s.decay, s.sustain, s.release);
        env[(size_t) i].noteOn();
        filter[(size_t) i].reset();
        tailLeft[(size_t) i] = s.tail;
        lastAmp[(size_t) i] = 0.0f;
        e->start (targetNote, velocityGain);
        EngineParams ep = slotParams (p, i);
        e->prepare (ep);
        lastParams[(size_t) i] = ep;
        lastParamsValid[(size_t) i] = true;
        epCache[(size_t) i] = ep;
        slotStarted[(size_t) i] = true;
        slotRunning[(size_t) i] = true;
    }

    void updateControl (const Patch& p)
    {
        // --- низкочастотные модуляторы
        const float dt = (float) kControlBlock / (float) sampleRate;
        for (int m = 0; m < 2; ++m)
        {
            const Lfo& lfo = p.lfo[m];
            lfoPhase[(size_t) m] += lfo.rate * dt;
            if (lfoPhase[(size_t) m] >= 1.0f) { lfoPhase[(size_t) m] -= 1.0f; lfoHeld[(size_t) m] = lfoRandom.nextBipolar(); }
        }

        float modCutoff = 0.0f, modPitch = 0.0f, modMorph = 0.0f, modLevel = 0.0f;
        float vLfo[2] = { 0.0f, 0.0f };
        for (int m = 0; m < 2; ++m)
        {
            const Lfo& lfo = p.lfo[m];
            switch (lfo.shape)
            {
                case 1: vLfo[m] = 4.0f * std::fabs (lfoPhase[(size_t) m] - 0.5f) - 1.0f; break;  // треугольник
                case 2: vLfo[m] = lfoHeld[(size_t) m]; break;                                    // sample&hold
                default: vLfo[m] = fastSin (lfoPhase[(size_t) m]); break;
            }
            if (lfo.amount <= 1.0e-4f) continue;
            const float a = lfo.amount * clamp (0.4f + 0.6f * p.modWheel, 0.0f, 1.0f) * vLfo[m];
            switch (lfo.dest)
            {
                case ModDest::Cutoff: modCutoff += a; break;
                case ModDest::Pitch:  modPitch  += a * 2.0f; break;
                case ModDest::Morph:  modMorph  += a; break;
                case ModDest::Level:  modLevel  += a; break;
                default: break;
            }
        }

        // --- мод-матрица: 8 маршрутов ---------------------------------------
        // Источники считаются один раз на блок управления, затем каждый маршрут
        // добавляет свою долю в нужное назначение. Маршруты складываются с
        // простыми LFO (они остаются «быстрым» путём и полностью совместимы
        // со старыми патчами: при amount = 0 матрица ничего не меняет).
        float srcVal[(int) ModSource::Count];
        srcVal[(int) ModSource::Lfo1]     = vLfo[0];
        srcVal[(int) ModSource::Lfo2]     = vLfo[1];
        srcVal[(int) ModSource::Env]      = envValue();
        srcVal[(int) ModSource::Velocity] = clamp ((velocityGain - 0.6f) * 2.5f, -1.0f, 1.0f);
        srcVal[(int) ModSource::KeyTrack] = clamp (((float) targetNote - 60.0f) / 48.0f, -1.0f, 1.0f);
        srcVal[(int) ModSource::ModWheel] = clamp (p.modWheel, 0.0f, 1.0f);
        srcVal[(int) ModSource::Random]   = noteRandom;
        srcVal[(int) ModSource::Bend]     = clamp (p.bend, -1.0f, 1.0f);

        float modPan = 0.0f, modRes = 0.0f, modWarp = 0.0f, modDetune = 0.0f;
        for (int r = 0; r < kModRoutes; ++r)
        {
            const ModRoute& rt = p.routes[r];
            if (std::fabs (rt.amount) < 1.0e-4f) continue;
            const int si = (int) rt.source;
            if (si < 0 || si >= (int) ModSource::Count) continue;
            const float a = srcVal[si] * rt.amount;
            switch (rt.dest)
            {
                case ModDest::Cutoff:    modCutoff += a; break;
                case ModDest::Pitch:     modPitch  += a * 12.0f; break;   // ±12 полутонов
                case ModDest::Morph:     modMorph  += a; break;
                case ModDest::Level:     modLevel  += a; break;
                case ModDest::Pan:       modPan    += a; break;
                case ModDest::Resonance: modRes    += a; break;
                case ModDest::Warp:      modWarp   += a; break;
                case ModDest::Detune:    modDetune += a; break;
                default: break;
            }
        }
        modWarpF = modWarp;
        modDetuneF = modDetune;

        pitchMod = modPitch;
        morphMod = modMorph;
        const float morphNow = clamp (p.morph + modMorph, 0.0f, 1.0f);

        for (int i = 0; i < kSlots; ++i)
        {
            auto& e = engines[(size_t) i];
            if (! e) continue;

            const SlotPatch& s = p.slots[i];
            float w = slotWeight (p, i, morphNow, targetNote);
            w = clamp (w * (1.0f + modLevel * 0.5f), 0.0f, 2.0f);
            gainSm[(size_t) i].setTarget (w * s.params.level * velocityGain);

            const float cut = clamp (s.cutoff * std::pow (2.0f, modCutoff * 2.0f), 0.0f, 1.0f);
            const float resNow = clamp (s.res + std::fabs (modCutoff) * 0.2f + modRes, 0.0f, 0.95f);
            filter[(size_t) i].setParams (SlotPatch::cutoffHz (cut), resNow);

            if (modPan != 0.0f)
            {
                const float pan = clamp (s.params.pan + modPan, -1.0f, 1.0f) * clamp (p.width, 0.0f, 1.0f);
                panL[(size_t) i] = std::cos ((pan + 1.0f) * 0.25f * (float) kPi);
                panR[(size_t) i] = std::sin ((pan + 1.0f) * 0.25f * (float) kPi);
            }

            // prepare() у движков — тяжёлый (степени, MIP-уровни, пересчёт мод).
            // Вызываем его только когда параметры реально изменились: без этого
            // инструмент тратил 88 % ядра на 8 голосов вместо 16 %.
            if (w > 1.0e-5f && ! slotStarted[(size_t) i] && ! released) startSlot (p, i);

            if (slotStarted[(size_t) i])
            {
                const EngineParams ep = slotParams (p, i);
                // кэш для сэмплового цикла: render() получает ту же структуру,
                // что ушла в prepare(), и не пересобирает её 128 раз зря
                epCache[(size_t) i] = ep;
                if (! lastParamsValid[(size_t) i] || ! paramsEqual (ep, lastParams[(size_t) i]))
                {
                    e->prepare (ep);
                    lastParams[(size_t) i] = ep;
                    lastParamsValid[(size_t) i] = true;
                }
            }
        }
    }

    std::array<std::unique_ptr<EngineBase>, kSlots> engines;
    std::array<EngineType, kSlots> slotType {};
    // подготовленные движки (пишет message thread, забирает аудио-поток)
    std::array<std::unique_ptr<EngineBase>, kSlots> pending;
    std::array<EngineType, kSlots> pendingType {};
    std::atomic<uint32_t> pendingMask { 0 };
    EngineRetireBin* retireBin = nullptr;
    std::array<Adsr, kSlots> env;
    std::array<SlotFilter, kSlots> filter;
    std::array<SmoothedValue, kSlots> gainSm;
    std::array<float, kSlots> panL {}, panR {};
    std::array<float, kSlots> tailLeft {}, lastAmp {}, slotMeter {};
    std::array<bool, kSlots> slotRunning {}, slotStarted {};
    std::array<EngineParams, kSlots> lastParams {};
    // после подмены движка кэш параметров недействителен: новый движок обязан
    // получить prepare() даже если параметры совпали с прежними
    std::array<bool, kSlots> lastParamsValid {};
    std::array<EngineParams, kSlots> epCache {};   // параметры, актуальные на текущем блоке
    std::array<float, 2> lfoPhase {}, lfoHeld {};
    Rng lfoRandom { 0x5eedu };
    Rng noteRng { 0x1234u };

    double sampleRate = 48000.0;
    double ageSamples = 0.0;

    int   targetNote = 60;
    float targetPitch = 60.0f, currentPitch = 60.0f;
    float pitchMod = 0.0f, morphMod = 0.0f;
    float modWarpF = 0.0f, modDetuneF = 0.0f;
    float noteRandom = 0.0f;       // источник Random: своё значение на каждую ноту
    float velocityGain = 1.0f;
    float killFade = 0.0f;

    int  controlCounter = 0;
    bool active = false, released = false;
};

} // namespace orogen
