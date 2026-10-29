// ДВИЖОК 5: Granular — гранулярный синтез из тех же one-shot'ов.
//
// Что делает его продакшн-движком, а не демо «нарежем сэмпл на куски»:
//   1. Каждое зерно имеет СВОЙ питч-рейт (спред), а значит свой MIP-уровень:
//      переиспользуется MipReader движка Sample (windowed-sinc + 5 уровней).
//      Без этого верхние ноты снова дают алиасинг, только теперь «размазанный».
//   2. Окно Ханна по фазе зерна: края зерна всегда в нуле -> нет щелчков,
//      даже если зерно стартует в середине громкой атаки.
//   3. Нормировка на перекрытие: амплитуда = 1/sqrt(density · size).
//      Некогерентные зёрна складываются по мощности, поэтому уровень
//      не зависит от плотности — иначе ручка Density работала бы как громкость.
//   4. Псевдослучайный планировщик с фиксированным seed: два прогона
//      с одинаковыми параметрами дают бит-в-бит одинаковый результат (тест).
//   5. Сглаживание позиции/размера: ручки не «зипперят» и не щёлкают.
#pragma once

#include "engine.h"
#include "mip_reader.h"
#include "../content/sample_bank.h"

namespace orogen {

class GranularEngine final : public EngineBase
{
public:
    static constexpr int   kMaxGrains = 12;
    static constexpr float kBodyTarget = 0.15f;    // «эталонная» громкость тела записи
    static constexpr float kGainTrim  = 2.3f;    // калибровка под RMS 0.20 (замер в tests/test_granular.cpp)

    void setBank (const SampleBank* b) noexcept { bank = b; }

    const char* name() const override { return "Granular"; }
    float costFactor() const override { return 4.0f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        posSm.setTime (sr, 45.0);
        sizeSm.setTime (sr, 60.0);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        spawnAcc = 0.0f;
        dcX = dcY = 0.0f;
        for (auto& g : grains) g.active = false;
        needSnap = true;
        active = true;
        rng = Rng { 0x51F3A7u };
    }

    void stop() override
    {
        active = false;
        for (auto& g : grains) g.active = false;
    }

    void prepare (const EngineParams& p) override
    {
        if (bank == nullptr || bank->shots.empty()) return;
        const OneShot& s = bank->shots[(size_t) clamp (p.table, 0, (int) bank->shots.size() - 1)];
        shot = &s;

        baseRate = (float) std::pow (2.0, ((double) note - (double) s.rootNote + (double) p.tuneSemis) / 12.0);
        bodyRms = std::max (1.0e-4f, bodyLevel (s));

        // p[0] — позиция чтения по записи, p[1] — размер зерна, p[2] — плотность, p[3] — спред
        const float posT   = clamp (p.p[0], 0.0f, 1.0f) * 0.85f;
        const float sizeT  = clamp (p.p[1], 0.0f, 1.0f);
        const float densT  = clamp (p.p[2], 0.0f, 1.0f);
        spread             = clamp (p.p[3], 0.0f, 1.0f);

        // размер: 8 .. 220 мс (в секундах)
        sizeSec = 0.008f + sizeT * 0.212f;
        // плотность: 4 .. 90 зёрен/с
        density = 4.0f + densT * 86.0f;

        if (needSnap) { posSm.snap (posT); sizeSm.snap (sizeSec); needSnap = false; }
        else          { posSm.setTarget (posT); sizeSm.setTarget (sizeSec); }

        gain = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
        dcCoef = (float) std::exp (-kTwoPi * 6.0 / sampleRate);
    }

    inline float render (const EngineParams&) override
    {
        if (! active || shot == nullptr) return 0.0f;

        const float posT  = posSm.next();
        const float size  = sizeSm.next();
        const int   lenOrig = shot->lens[0];

        // ---------------- планировщик зёрен
        spawnAcc += density / (float) sampleRate;
        while (spawnAcc >= 1.0f)
        {
            spawnAcc -= 1.0f;
            spawn (posT, size, lenOrig);
        }

        // ---------------- сумма активных зёрен
        float sum = 0.0f;
        for (auto& g : grains)
        {
            if (! g.active) continue;

            // окно Ханна по фазе зерна
            const float w = 0.5f - 0.5f * fastCosTbl (g.phase);   // было std::cos на зерно на сэмпл
            sum += g.mip.read (g.pos) * w * g.amp;

            g.pos   += g.rate;
            g.phase += 1.0f / g.dur;
            if (g.phase >= 1.0f) g.active = false;
        }

        // Нормировка на перекрытие: сумма некогерентных зёрен растёт как sqrt(N).
        // Ожидаемое перекрытие = density · size, при 1 зерне нормировка = 1.
        // 1/sqrt(overlap) — пересчитываем не каждый сэмпл: density/size ползут
        // медленно, а sqrt в цикле заметен
        if (--normCountdown <= 0)
        {
            normCountdown = 16;
            normCur = 1.0f / std::sqrt (std::max (1.0f, density * size));
        }
        const float norm = normCur;

        const float out = sum * norm * gain;
        const float y = out - dcX + dcCoef * dcY;
        dcX = out; dcY = y;
        return y;
    }

private:
    // Тестовый пробник читает фактические границы активных зёрен без участия
    // в аудиопотоке и без изменения алгоритма.
    friend struct GranularTestProbe;
    struct Grain
    {
        MipReader mip;
        float pos = 0.0f, rate = 1.0f, phase = 0.0f, amp = 0.0f, dur = 3840.0f;
        bool  active = false;
    };

    void spawn (float posT, float size, int lenOrig) noexcept
    {
        Grain* g = nullptr;
        for (auto& x : grains) if (! x.active) { g = &x; break; }
        if (g == nullptr) return;                       // все заняты — зерно пропускается

        // питч зерна: базовый рейт ноты + случайный спред (до ±7 полутонов)
        const float jitter = spread * 7.0f * rng.nextBipolar();
        const float rate = baseRate * (float) std::pow (2.0, (double) jitter / 12.0);

        // Длительность зерна в отсчётах. Если источник короче зерна, зерно
        // растягивается по времени — иначе оно читало бы за концом записи
        // и MipReader заворачивал бы чтение на атаку ПРЯМО ВНУТРИ окна
        // (щелчок в середине зерна, слышимый как грязь на коротких источниках).
        // Зерно должно оставлять место для позиции: занимаем не больше половины
        // записи, иначе все зёрна сбиваются в её начало (у коротких источников
        // уровень падал до −57 дБ относительно длинных).
        const float maxSpan = 0.5f * ((float) lenOrig - 128.0f);
        const float want = size * (float) sampleRate;
        g->dur = std::max (16.0f, std::min (want, maxSpan / rate));
        // Используем скорость именно нового зерна: g->rate здесь ещё содержит
        // скорость предыдущего зерна в переиспользованной ячейке (или 1.0).
        const float span = g->dur * rate;

        // Позиция старта: базовый Scan + джиттер.
        //
        // Джиттер нужен ВСЕГДА, даже при spread = 0. Без него зёрна читают
        // один и тот же материал с одинаковым шагом, и их взаимная фаза
        // жёстко определяется плотностью: при density 51/с и ноте 69 (440 Гц)
        // соседние зёрна расходились на 216° и гасили друг друга — уровень
        // падал на 27 дБ (замер: RMS 0.054 при ноте 45 против 0.0023 при 69).
        // Джиттер в пределах половины зерна делает фазы независимыми,
        // и сумма 12 зёрен растёт как sqrt(N) — как и предполагает нормировка.
        const float jitterRange = 0.5f * size * (float) sampleRate * rate;
        const float posJit = rng.nextBipolar() * jitterRange
                           + spread * 0.25f * rng.nextBipolar() * (float) lenOrig;
        float pos = posT * (float) lenOrig + posJit;
        // зерно должно целиком уложиться в запись: без заворота внутри окна
        const float limit = std::max (64.0f, (float) lenOrig - 128.0f - span);
        pos = std::fmod (pos, limit);
        if (pos < 0.0f) pos += limit;

        g->mip.setup (*shot, rate);
        g->pos   = pos;
        g->rate  = rate;
        g->phase = 0.0f;

        // Нормировка по огибающей источника: зерно из хвоста затухающей записи
        // поднимается к уровню «тела». Без этого RMS голоса зависел от позиции
        // и питч-рейта в 27 дБ (замер: нота 45 -> 0.054, нота 69 -> 0.0023).
        // Верхний предел 5x — чтобы не вытягивать шум из полной тишины.
        const float local = std::max (1.0e-4f, shot->envAt (pos));
        const float comp  = clamp (bodyRms / local, 0.25f, 5.0f);

        // Приведение к абсолютному уровню: голос не должен зависеть от того,
        // насколько громко записан источник (пробник 0.40 RMS против Fm Tine
        // 0.14 давал разницу 11 дБ при одинаковых настройках).
        const float bodyScale = clamp (kBodyTarget / bodyRms, 0.05f, 6.0f);

        // компенсация: короткие зёрна звучат тише при равной амплитуде
        // компенсация укороченного зерна: перекрытие = density · dur/sr, а не
        // density · size — иначе короткие зерна звучат тише пропорционально
        const float durScale = std::sqrt (std::max (0.05f, g->dur / std::max (16.0f, want)));
        g->amp   = (0.6f + 0.4f * std::min (1.0f, size / 0.05f)) * comp * bodyScale * durScale;
        g->active = true;
    }

    const SampleBank* bank = nullptr;
    const OneShot*    shot = nullptr;
    Grain grains[kMaxGrains];

    double sampleRate = 48000.0;
    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true;

    float  baseRate = 1.0f, spread = 0.0f, sizeSec = 0.08f, density = 20.0f, bodyRms = 1.0f;
    float  spawnAcc = 0.0f, gain = 0.5f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;
    float  normCur = 1.0f;
    int    normCountdown = 0;
    SmoothedValue posSm, sizeSm;
    Rng rng { 0x51F3A7u };
};

} // namespace orogen
