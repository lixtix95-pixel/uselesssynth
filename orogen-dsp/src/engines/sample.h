// ДВИЖОК 4: Sample — воспроизведение one-shot'а как инструмента.
//
// Что делает его «промышленным», а не «плеером на 20 строк»:
//   1. MIP-уровни сэмпла: при повышении ноты на октаву полоса сужается вдвое,
//      иначе ВСЁ, что выше нового Найквиста, заворачивается. Выбор уровня по
//      коэффициенту питча + кроссфейд между уровнями.
//   2. Полифазный windowed-sinc (32 отвода, 256 фаз) вместо линейной/кубической
//      интерполяции: линейная даёт образы -21 дБ, Catmull-Rom -27 дБ, sinc < -80 дБ.
//   3. Loop-регион с бесшовной склейкой (кроссфейд в точке петли) — иначе щелчок
//      на каждом обороте.
//   4. Scan (позиция чтения) со сглаживанием: «скраб» по записи без зиппера.
//   5. Плавное затухание в конце и защита от чтения за границей буфера.
#pragma once

#include "engine.h"
#include "../content/sample_bank.h"

#include "mip_reader.h"

namespace orogen {

class SampleEngine final : public EngineBase
{
public:
    static constexpr float kGainTrim = 0.62f;

    void setBank (const SampleBank* b) noexcept { bank = b; }

    // Петля выключена по умолчанию: one-shot'ы (как 2 177 записей в Terrain)
    // по определению не петлевые. Зацикленный затухающий сэмпл даёт
    // пилообразную огибающую на частоте петли, а её сайдбенды попадают
    // в промежутки между гармониками — звучит как «дрожание», а измеряется
    // как алиасинг (-10 дБ при идеально чистом спектре).
    void setLoop (bool shouldLoop) noexcept { looping = shouldLoop; }

    const char* name() const override { return "Sample"; }
    float costFactor() const override { return 1.2f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        scanSm.setTime (sr, 22.0);
        toneSm.setTime (sr, 30.0);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        pos = 0.0f;
        endFade = 1.0f;
        toneState = 0.0f;
        driftCents = rng.nextBipolar() * 5.0f;
        needSnap = true;
        active = true;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        if (bank == nullptr || bank->shots.empty()) return;
        const OneShot& s = bank->shots[(size_t) clamp (p.table, 0, (int) bank->shots.size() - 1)];
        shot = &s;

        // ---- питч: нота относительно корневой + тюн + дрейф
        const double cents = (double) driftCents * (double) p.detune * 2.0;
        const double semis = (double) note - (double) s.rootNote + (double) p.tuneSemis;
        rate = (float) (std::pow (2.0, (semis + cents / 100.0) / 12.0));

        // ---- выбор MIP-уровня: полоса сэмпла должна покрывать rate
        //   rate <= 1  -> level 0 (исходник), 1..2 -> level 1 (полоса вдвое уже)
        //   и т.д. с кроссфейдом по log2(rate).
        // MIP-выбор и чтение — в общем модуле (Granular использует отдельный
        // дробно-скоростной режим). Каждый выровненный уровень прорежен ровно
        // в 2^L раз; округление длины нечётного буфера не меняет его частоту.
        // Поэтому индекс масштабируется на 1/2^L, а не на отношение длин.
        mip.setup (s, rate);
        lenOrig = mip.lenOrig;

        // ---- Scan: позиция чтения (0..1 от длины). Сглаживается.
        const float scanT = clamp (p.p[0], 0.0f, 1.0f) * 0.85f;   // не уходим в самый хвост
        if (needSnap) { scanSm.snap (scanT); needSnap = false; }
        else            scanSm.setTarget (scanT);

        // ---- Grain: «заикание» — периодическая перемотка на кусок назад
        grainAmt = clamp (p.p[1], 0.0f, 1.0f);
        grainLen = (int) (2400.0f * (1.2f - grainAmt));          // 50 мс .. 8 мс

        // ---- Tone: однополюсный ФНЧ
        const float toneT = clamp (p.p[2], 0.0f, 1.0f);
        toneCoef = 0.02f + toneT * 0.9f;

        // ---- Air: подмешивание шума (шум «воздуха» записи)
        airAmt = clamp (p.p[3], 0.0f, 1.0f) * 0.05f;

        gain = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
        dcCoef = (float) std::exp (-kTwoPi * 6.0 / sampleRate);

        // ---- петля с кроссфейдом.
        // Склейка «конец -> начало» без кроссфейда — это разрыв сигнала,
        // то есть широкополосный щелчок на каждом обороте (при питче +3 октавы
        // сэмпл оборачивается 4 раза в секунду, и щелчки превращаются в «звон»).
        // Кроссфейд 25 мс убирает разрыв: в конце петли подмешивается начало.
        loopStart = 0.12f * (float) lenOrig;
        loopEnd   = (float) lenOrig - 64.0f;

        // Длина петли квантуется в ЦЕЛОЕ число периодов корневой ноты.
        // Иначе каждое замыкание сдвигает фазу, основной тон «размазывается»
        // (утечка ±6 бинов вместо ±4) и звучит как расстроенность. Это
        // стандартный приём в сэмплерах: loop length = N периодов.
        const double period = sampleRate / kSampleF0;
        const double len = (double) (loopEnd - loopStart);
        const double nPeriods = std::max (1.0, std::round (len / period));
        loopLen   = (float) (nPeriods * period);
        loopEnd   = loopStart + loopLen;
        xfadeLen  = std::min (0.025f * (float) lenOrig, loopLen * 0.25f);


        // флаг «читаем петлю» — если позиция уже за концом, стартуем из Scan
        if (pos <= 0.0f || pos >= (float) lenOrig) pos = 0.0f;
    }

    inline float render (const EngineParams&) override
    {
        if (! active || shot == nullptr) return 0.0f;

        // ВАЖНО: позиция живёт в ОРИГИНАЛЬНОЙ шкале времени (отсчёты исходника).
        // Раньше шаг был в единицах выбранного MIP-уровня, а уровень прорежен
        // в 2^L раз — сэмпл играл в 2^L раз быстрее и «звенел» алиасингом.
        // Теперь уровень — это только выбор таблицы: индекс делим на 2^L.
        pos += rate;
        if (pos < 0.0f) pos = 0.0f;

        if (! looping)
        {
            // ---- одноразовый режим: дошли до конца — короткий fade-out
            if (pos >= (float) lenOrig - 256.0f)
            {
                const float fade = clamp (((float) lenOrig - pos) / 256.0f, 0.0f, 1.0f);
                if (fade <= 0.0f) { active = false; return 0.0f; }
                endFade = fade;
            }
            else endFade = 1.0f;
        }
        else
        {
            if (pos >= loopEnd) pos -= loopLen;                 // петля
        }

        // Grain: короткая перемотка назад -> «заикание»
        if (grainAmt > 0.01f)
        {
            grainCounter++;
            if (grainCounter >= grainLen)
            {
                grainCounter = 0;
                pos -= (float) grainLen * grainAmt * 0.8f;
                if (pos < 0.0f) pos += loopLen;
            }
        }

        const float scan = scanSm.next();
        float readPos = pos + scan * loopLen;
        if (looping && readPos >= loopEnd) readPos -= loopLen;
        if (readPos > (float) lenOrig - 8.0f) readPos = (float) lenOrig - 8.0f;

        float s = mip.read (readPos);

        // кроссфейд в хвосте петли: подмешиваем начало петли (только в loop-режиме)
        const float toEnd = loopEnd - readPos;
        if (looping && toEnd < xfadeLen)
        {
            // raised-cosine: 0 в начале перехода, 1 в конце
            const float u = clamp (1.0f - toEnd / xfadeLen, 0.0f, 1.0f);
            const float w = 0.5f - 0.5f * std::cos (float (kPi) * u);
            float folded = readPos - loopLen;
            if (folded < 0.0f) folded += loopLen + loopStart;
            s = s * (1.0f - w) + mip.read (folded) * w;
        }

        // Tone
        toneState += (s - toneState) * toneCoef;
        s = toneState;

        // Air
        if (airAmt > 1.0e-4f)
        {
            noise = noise * 1103515245u + 12345u;
            s += ((float) (int32_t) noise / 2147483648.0f) * airAmt;
        }

        const float out = s * gain * endFade;
        const float y = out - dcX + dcCoef * dcY;
        dcX = out; dcY = y;
        return y;
    }

private:

    const SampleBank* bank = nullptr;
    const OneShot* shot = nullptr;
    double sampleRate = 48000.0;

    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true, looping = false;
    float  endFade = 1.0f;

    float  pos = 0.0f, rate = 1.0f;
    int    lenOrig = 1;
    MipReader mip;

    float  loopStart = 0.0f, loopEnd = 0.0f, loopLen = 1.0f, xfadeLen = 1.0f;
    float  grainAmt = 0.0f, airAmt = 0.0f, toneCoef = 0.5f, toneState = 0.0f;
    int    grainLen = 1200, grainCounter = 0;
    uint32_t noise = 0x1u;
    float  driftCents = 0.0f;
    float  gain = 0.6f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;

    SmoothedValue scanSm, toneSm;
    Rng rng { 0xBEEF01u };
};

} // namespace orogen
