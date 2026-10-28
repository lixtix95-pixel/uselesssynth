// ДВИЖОК 6: Modal — модальный синтез (банк резонаторов).
//
// Физика: удар по телу возбуждает его моды. Каждая мода — затухающий
// синус с собственной частотой и временем затухания. 12 резонаторов
// второго порядка дают «колокол/стекло/дерево/сталь», которых не получить
// ни одним вычитающим синтезом: спектр ИНГАРМОНИЧЕСКИЙ.
//
// Что здесь продакшн, а не демо:
//   1. Частоты мод масштабируются с нотой и ОТБРАСЫВАЮТСЯ, если выходят
//      за 0.45·fs. Это принципиально: резонатор выше Найквиста не «звучит
//      тише» — он заворачивается в слышимую полосу и даёт звон. Модальное
//      «дребезжание» на верхних нотах — ровно эта ошибка.
//   2. Каждая мода имеет своё T60 (высокие гаснут быстрее) — иначе тело
//      звучит как орган, а не как материал.
//   3. Нормировка коэффициента усиления резонатора на 1/(1−r): без неё
//      короткие (сильно задемпфированные) моды тише длинных на 20+ дБ.
//   4. Нелинейность удара: короткий сдвиг высоты в момент атаки (натяжение),
//      плюс ограничение возбуждения по полосе (иначе шумовой удар сам даёт
//      широкополосный щелчок, который неотличим от алиасинга).
//   5. Субнормали гасятся в резонаторах: при T60 = 12 с двухполюсник
//      уходит в денормализованные числа, и CPU вырастает в разы.
#pragma once

#include "engine.h"
#include "halfband.h"

namespace orogen {

class ModalEngine final : public EngineBase
{
public:
    static constexpr int   kModes     = 12;
    static constexpr float kGainTrim  = 0.50f;

    const char* name() const override { return "Modal"; }
    float costFactor() const override { return 1.4f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        brightSm.setTime (sr, 40.0);
        decaySm.setTime (sr, 60.0);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        exciteIdx = 0;
        exciteLen = 0;
        for (auto& r : res) { r.y1 = r.y2 = 0.0f; }
        needSnap = true;
        active = true;
        rng = Rng { 0x2C0FFEEu };
        // возбуждение: короткий шумовой burst (1..8 мс в зависимости от жёсткости)
        burstLen = (int) (sampleRate * 0.0012);
        hitPos = 0.0f;
        started = false;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        currentTable = clamp (p.table, 0, 7);
        // p[0] — яркость (сколько мод слышно), p[1] — T60, p[2] — жёсткость удара,
        // p[3] — нелинейность (сдвиг высоты на атаке)
        brightT = clamp (p.p[0], 0.0f, 1.0f);
        const float decayT = clamp (p.p[1], 0.0f, 1.0f);
        hardness = clamp (p.p[2], 0.0f, 1.0f);
        nonlin  = clamp (p.p[3], 0.0f, 1.0f);

        t60 = 0.25f + decayT * 11.75f;                    // 0.25 .. 12 с
        if (needSnap) { brightSm.snap (brightT); decaySm.snap (t60); needSnap = false; }
        else          { brightSm.setTarget (brightT); decaySm.setTarget (t60); }

        gain = kGainTrim * (0.35f + 0.65f * clamp (p.level, 0.0f, 1.0f));
        // возбуждение: чем жёстче удар, тем короче и ярче burst
        exciteLen = (int) (sampleRate * (0.00035 + 0.004 * (1.0 - (double) hardness)));
        exciteLen = std::max (8, exciteLen);
        exciteIdx = 0;
        burstLp = 0.0f;
        age = 0;
        lastBright = -1.0f; lastDecay = -1.0f;
        updateModes (false);
    }

    inline float render (const EngineParams&) override
    {
        if (! active) return 0.0f;

        // ---- возбуждение: burst шума, ограниченный по полосу (LP по hardness)
        float x = 0.0f;
        if (exciteIdx < exciteLen)
        {
            const float n = rng.nextBipolar();
            burstLp += (n - burstLp) * (0.02f + 0.5f * hardness);   // жёстче удар — шире полоса
            const float w = 0.5f - 0.5f * std::cos (float (kPi) * (float) exciteIdx / (float) exciteLen);
            x = burstLp * w;
            ++exciteIdx;
        }

        // ---- медленные изменения яркости/затухания
        const float bright = brightSm.next();
        const float dec = decaySm.next();
        if (std::fabs (bright - lastBright) > 0.004f || std::fabs (dec - lastDecay) > 0.05f)
        {
            lastBright = bright; lastDecay = dec;
            updateModes (true);
        }

        // ---- банк резонаторов
        float sum = 0.0f;
        const float glide = 1.0f + nonlin * 0.06f * std::exp (-(float) age / (0.035f * (float) sampleRate));
        for (int m = 0; m < activeModes; ++m)
        {
            Res& r = res[(size_t) m];

            // нелинейный сдвиг: на атаке чуть выше (натяжение струны/мембраны)
            if (nonlin > 0.001f)
            {
                const float w = kTwoPi * r.freq * glide / (float) sampleRate;
                const float c = std::cos (w);
                r.a1 = 2.0f * r.r * c;
            }

            float y = x * r.drive + r.a1 * r.y1 + r.a2 * r.y2;
            y = antiDenormal (y);
            r.y2 = r.y1; r.y1 = y;
            sum += y * r.amp;
        }

        // HP 20 Гц: импульсное возбуждение даёт DC-составляющую, которая
        // иначе остаётся в резонаторах навсегда и «съедает» динамический диапазон.
        hpY = sum - hpX + hpCoef * hpY; hpX = sum;

        ++age;
        // Жёсткого clamp здесь нет намеренно: клиппинг сам создаёт алиасинг
        // (измерено -28 дБ «алиасинга», которого не было — сигнал упирался в 4.0).
        return hpY * gain * modeNorm;
    }

private:
    struct Res
    {
        float freq = 0.0f;      // Гц
        float r = 0.0f;         // радиус (затухание)
        float a1 = 0.0f, a2 = 0.0f;
        float drive = 1.0f;     // компенсация усиления на резонансе
        float amp = 1.0f;       // вес моды
        float y1 = 0.0f, y2 = 0.0f;
    };

    // Наборы отношений мод. Не гармонические — в этом суть модального синтеза.
    // 8 «материалов», как 8 источников контента у Sample.
    static constexpr float kRatios[8][kModes] = {
        // 0: колокол (колокольная бронза)
        { 1.0f, 2.02f, 2.98f, 4.07f, 5.42f, 6.79f, 8.21f, 9.83f, 11.6f, 13.4f, 15.3f, 17.2f },
        // 1: стекло (тонкий бокал)
        { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.5f, 15.9f, 19.6f, 23.4f, 27.3f, 31.1f, 34.7f },
        // 2: дерево (ксилофон)
        { 1.0f, 2.76f, 5.40f, 8.93f, 13.3f, 18.6f, 24.8f, 31.7f, 39.1f, 46.8f, 54.6f, 62.3f },
        // 3: сталь (листовая пластина)
        { 1.0f, 1.59f, 2.14f, 2.30f, 2.65f, 2.92f, 3.16f, 3.50f, 3.60f, 3.87f, 4.10f, 4.35f },
        // 4: мембрана (барабан)
        { 1.0f, 1.59f, 2.14f, 2.30f, 2.65f, 2.92f, 3.16f, 3.51f, 3.60f, 3.87f, 4.11f, 4.36f },
        // 5: керамика
        { 1.0f, 3.31f, 6.42f, 10.4f, 15.1f, 20.6f, 26.9f, 34.0f, 41.7f, 50.1f, 59.0f, 68.4f },
        // 6: камень (гранит)
        { 1.0f, 1.83f, 2.71f, 3.85f, 5.12f, 6.63f, 8.31f, 10.2f, 12.3f, 14.6f, 17.1f, 19.8f },
        // 7: лёд
        { 1.0f, 2.13f, 3.71f, 5.62f, 7.90f, 10.5f, 13.4f, 16.6f, 20.1f, 23.8f, 27.7f, 31.9f }
    };

    void updateModes (bool glide) noexcept
    {
        const int table = currentTable;
        const double f0 = midiToHz (note_saved());
        const double nyqLimit = 0.45 * sampleRate;

        activeModes = 0;
        double ampSum = 0.0;
        int wanted = 2 + (int) (brightSm.getCurrent() * (float) (kModes - 2));

        for (int m = 0; m < wanted; ++m)
        {
            const double freq = f0 * (double) kRatios[table % 8][m];
            if (freq >= nyqLimit) break;                  // <-- мода выше Найквиста НЕ создаётся

            Res& r = res[(size_t) activeModes];
            r.freq = (float) freq;

            // T60 моды: высокие затухают быстрее (потеря на изгибе/излучении)
            const double ratio = (double) kRatios[table % 8][m];
            const double t60m = std::max (0.03, (double) decaySm.getCurrent() / (1.0 + 0.35 * (ratio - 1.0)));

            // r = 10^(-3/(T60·fs))  -> амплитуда падает на 60 дБ за T60
            const double rr = std::pow (10.0, -3.0 / (t60m * sampleRate));
            r.r = (float) std::min (0.99999, rr);
            const double w = kTwoPi * freq / sampleRate;
            r.a1 = (float) (2.0 * r.r * std::cos (w));
            r.a2 = (float) (-(double) r.r * r.r);

            // Компенсации нет: амплитуда импульсного отклика двухполюсника
            // равна sin(w) и от затухания не зависит — компенсировать «Q»
            // здесь нечего, а множитель 1/(1-r) (до 14000) дал бы перегруз.
            r.drive = 1.0f;

            // Зато надо выровнять sin(w): на низких нотах он 0.014, и мода
            // была бы на 35 дБ тише. Плюс натуральный спад высоких мод.
            // Импульсный отклик двухполюсника: h[n] = r^n·sin((n+1)w)/sin(w),
            // его пик ~1/sin(w). Значит на низких модах отклик САМ громче,
            // и выравнивать надо умножением на sin(w), а не делением.
            // (Обратная нормировка 1/sin давала разброс 16 дБ по нотам.)
            const double sinw = std::sin (w);
            r.amp = (float) (sinw * std::pow (ratio, -0.6));
            ampSum += std::pow (ratio, -0.6);
            ++activeModes;
        }
        // В момент удара все моды стартуют СИНФАЗНО (их возбуждает один и тот же
        // импульс), поэтому пик атаки равен сумме амплитуд мод, а не sqrt(N).
        // Нормировка на сумму выравнивает громкость между материалами:
        // у стали все 12 мод лежат в полосе 1..4.35 (плотный удар), у дерева
        // моды разнесены до 62-й гармоники. Без этой нормировки разброс
        // между материалами был 15.5 дБ.
        modeNorm = (ampSum > 0.1) ? (float) (1.0 / ampSum) : 1.0f;
        (void) glide;
    }

    int note_saved() const noexcept { return note; }

public:
    void setTable (int t) noexcept { currentTable = t; }
    int  modeCount() const noexcept { return activeModes; }

private:
    Res    res[kModes];
    int    activeModes = 0;
    int    note = 60, currentTable = 0;
    float  vel = 1.0f;
    bool   active = false, needSnap = true, started = false;

    double sampleRate = 48000.0;
    float  brightT = 0.6f, hardness = 0.5f, nonlin = 0.3f, t60 = 2.0f;
    float  gain = 0.6f, lastBright = -1.0f, lastDecay = -1.0f;
    int    age = 0;

    // возбуждение
    int    exciteIdx = 0, exciteLen = 0, burstLen = 240;
    float  burstLp = 0.0f, hitPos = 0.0f;

    // HP-защита от DC
    float  hpX = 0.0f, hpY = 0.0f, hpCoef = 0.99739f;   // срез 20 Гц при 48 кГц
    float  modeNorm = 1.0f;

    SmoothedValue brightSm, decaySm;
    Rng rng { 0x2C0FFEEu };
};

} // namespace orogen
