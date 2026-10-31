// ДВИЖОК 5: Granular — гранулярный синтез из тех же one-shot'ов.
//
// Что делает его продакшн-движком, а не демо «нарежем сэмпл на куски»:
//   1. Каждое зерно имеет СВОЙ питч-рейт (спред), а значит свой MIP-уровень:
//      переиспользуется MipReader движка Sample (windowed-sinc + 5 уровней).
//      Без этого верхние ноты снова дают алиасинг, только теперь «размазанный».
//   2. Окно Ханна по фазе зерна: края зерна всегда в нуле -> нет щелчков,
//      даже если зерно стартует в середине громкой атаки.
//   3. Нормировка каждого зерна по фактическому перекрытию с учётом его
//      укороченной длительности и лимита голосов. При автоматизации Density
//      амплитуда уже звучащих зёрен не меняется скачком.
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
        gainSm.setTime (sr, 5.0);
        // Создание lookup-таблиц не должно попадать на первую ноту и первый
        // render(). setSampleRate вызывается при подготовке/создании движка,
        // вне горячего аудиоцикла; далее эти таблицы только читаются.
        (void) sincKernel();
        (void) grainSincKernel();
        (void) fastCosTbl (0.0f);
    }

    // Отдельный медленный detune на ноту; Spread по-прежнему случайный для
    // каждого зерна. Voice передаёт свой детерминированный bipolar Random.
    void setNoteRandom (float value) noexcept override
    {
        noteRandom = clamp (value, -1.0f, 1.0f);
    }

    // Отдельный seed направления не трогает генератор позиции и Spread.
    // Самостоятельный Granular без голоса получает прежний поток (seed 0).
    void setRandomStreamSeed (uint32_t seed) noexcept override { directionStreamSeed = seed; }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        // Первая гранула запускается сразу на note-on. Дальнейшие интервалы
        // задаёт Density; прежде первый запуск ждал до 250 мс при 4 зернах/с.
        spawnAcc = 1.0f;
        dcX = dcY = 0.0f;
        for (auto& g : grains) g.active = false;
        needSnap = true;
        active = true;
        rng = Rng { 0x51F3A7u };
        directionRng = Rng { 0xB79D31u ^ directionStreamSeed }; // независим от RNG позиции и питча
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

        // Detune = до ±10 центов для конкретной ноты; при 0 звук и
        // планировщик идентичны прежнему коду. Новая высота назначается
        // только новым зёрнам: активные сохраняют свои безопасные MIP/границы.
        const double cents = 10.0 * (double) noteRandom * (double) clamp (p.detune, 0.0f, 1.0f);
        baseRate = (float) std::pow (2.0, ((double) note - (double) s.rootNote
                                          + (double) p.tuneSemis + cents / 100.0) / 12.0);
        bodyRms = std::max (1.0e-4f, bodyLevel (s));

        // p[0] — позиция чтения по записи, p[1] — размер зерна, p[2] — плотность, p[3] — спред
        // Позиция задаётся в диапазоне допустимых стартов, а не в полной
        // длине файла: при длинном/быстром зерне его хвост занимает место.
        const float posT   = clamp (p.p[0], 0.0f, 1.0f);
        const float sizeT  = clamp (p.p[1], 0.0f, 1.0f);
        const float densT  = clamp (p.p[2], 0.0f, 1.0f);
        spread             = clamp (p.p[3], 0.0f, 1.0f);
        // Направление выбирается только при рождении зерна: автоматизация
        // не изменяет курс и считыватель уже играющих зёрен.
        reverseChance      = clamp (p.grainReverse, 0.0f, 1.0f);

        // размер: 8 .. 220 мс (в секундах)
        sizeSec = 0.008f + sizeT * 0.212f;
        // плотность: 4 .. 90 зёрен/с
        density = 4.0f + densT * 86.0f;

        const float gainT = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
        if (needSnap)
        {
            posSm.snap (posT); sizeSm.snap (sizeSec); gainSm.snap (gainT);
            needSnap = false;
        }
        else
        {
            posSm.setTarget (posT); sizeSm.setTarget (sizeSec); gainSm.setTarget (gainT);
        }
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
            // Накопление float-фазы может закончиться на несколько отсчётов
            // позже расчётной dur (например, при 96 кГц). Не даём следующему
            // sinc-чтению перейти край MIP и завернуться на другой конец:
            // штатное воспроизведение и окно при этом не меняются.
            if (g.phase >= 1.0f || (g.rate < 0.0f ? g.pos < g.readLimit : g.pos >= g.readLimit))
                g.active = false;
        }

        // Нормировка зафиксирована при запуске каждого зерна. Уже звучащее
        // окно не получает мгновенный скачок при автоматизации Density.
        const float out = sum * gainSm.next();
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
        // Абсолютную координату нельзя многократно наращивать во float:
        // после 2 с точность шага уже даёт до ~14 центов ошибки, на длинных
        // источниках — полутоны. Rate остаётся float, но суммируется в double.
        double pos = 0.0;
        double readLimit = 0.0; // безопасный край sinc в направлении чтения
        float rate = 1.0f, phase = 0.0f, amp = 0.0f, dur = 3840.0f;
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

        // Учитываем ВСЕ 32/64 отвода выбранного sinc, а не только центр.
        // Уровень L читает исходник с шагом 2^L, поэтому фиксированного
        // запаса в 128 исходных отсчётов недостаточно.
        if (! g->mip.setupGranular (*shot, rate)) return;
        const int half = g->mip.grainHalf();
        if (g->mip.lenA < 2 * half + 4 || g->mip.invScaleA <= 0.0f) return;
        float safeBegin = std::max (64.0f, (float) (half + 1) / g->mip.invScaleA);
        float safeEnd = std::min ((float) lenOrig - 128.0f,
                                  (float) (g->mip.lenA - half - 2) / g->mip.invScaleA);

        // Зерно занимает максимум половину доступного участка, оставляя
        // пространство для управления позицией. Два дополнительных шага
        // учитывают округление длительности. Если даже 16 отсчётов физически
        // не помещаются в короткий источник, зерно не запускается.
        const float usable = safeEnd - safeBegin;
        const float maxDur = 0.5f * usable / rate - 2.0f;
        if (maxDur < 16.0f) return;
        const float want = size * (float) sampleRate;
        g->dur = std::min (want, maxDur);
        const float span = (g->dur + 2.0f) * rate;
        const float startRange = usable - span;
        if (startRange <= 0.0f) return;

        // Джиттер используется и при spread=0, чтобы зёрна не гасили друг
        // друга. Его масштаб берём по РЕАЛЬНОМУ проходу, а не по желаемой
        // длине: при высокой ноте последнее давало произвольный обход файла и
        // почти убивало влияние Position. На краях отражаем позицию обратно,
        // а не переносим маленький отрицательный джиттер на противоположный край.
        const float jitterRange = 0.5f * g->dur * rate;
        const float posJit = rng.nextBipolar() * jitterRange
                           + spread * 0.25f * rng.nextBipolar() * startRange;
        float positionInRange = posT * startRange + posJit;
        const float foldedRange = 2.0f * startRange;
        positionInRange = std::fmod (positionInRange, foldedRange);
        if (positionInRange < 0.0f) positionInRange += foldedRange;
        if (positionInRange > startRange) positionInRange = foldedRange - positionInRange;
        const float pos = safeBegin + positionInRange;

        // MIP и обе границы выбраны по положительной |rate|. Реверс читает
        // тот же безопасный интервал с верхнего края в обратную сторону;
        // sinc получает double-координату, а скорость получает только знак.
        // При 0%/100% отдельный RNG не требуется; при промежуточной вероятности
        // он не расходует генератор Spread/Position и не меняет их поведение.
        const bool reverse = reverseChance >= 1.0f
                          || (reverseChance > 0.0f && directionRng.nextFloat() < reverseChance);
        const float readSpan = g->dur * rate;
        g->pos   = reverse ? (double) pos + (double) readSpan : (double) pos;
        g->rate  = reverse ? -rate : rate;
        // Sinc читает half-1 отсчётов слева и half справа. Правая граница
        // исключительная, левая включительная; invScaleA — степень двойки.
        g->readLimit = (double) (reverse ? half - 1 : g->mip.lenA - half)
                     / (double) g->mip.invScaleA;
        g->phase = 0.0f;

        // Учитываем энергию ВСЕГО фактически читаемого участка, взвешенную
        // квадратом окна Ханна. Иначе тихая первая 10-мс ячейка поднимает
        // громкую атаку в середине того же зерна в несколько раз. Результат
        // вычисляется только при spawn, действующие зёрна не перенормируются.
        // Не усиливаем затухающий участок сильнее прежнего: используем большее
        // из RMS в начале и RMS под окном. Это не выходной лимитер.
        // Запись короче 10 мс не имеет ячеек env. envAt() в этом случае
        // возвращает условную единицу, а не RMS; из-за неё разница в один
        // отсчёт на границе 10 мс меняла gain зерна до 12 дБ. Для короткой
        // записи используем заранее рассчитанный RMS целого источника.
        // В реверсе первая проба берётся с верхнего края. Проверяем и нижний:
        // тихий старт не должен усилить громкую атаку на противоположном
        // конце в несколько раз. Прямой путь остаётся побайтно прежним.
        float local = shot->env.empty() ? bodyRms : shot->envAt (reverse ? (float) g->pos : pos);
        if (reverse && ! shot->env.empty()) local = std::max (local, shot->envAt (pos));
        // Взвешивание Hann² обходит ячейки в порядке фактического чтения.
        if (! shot->env.empty() && shot->envStep > 0)
        {
            // Фабричные источники: прежние точечные пробы через половину
            // ячейки. На очень длинном проходе 256 точек могут пропустить
            // одиночную атаку, поэтому каждая из тех же 256 фазовых секций
            // интегрирует ВСЕ свои 10-мс ячейки через префикс суммы квадратов.
            // Hann² берём в центре секции: время ограничено, аллокаций нет.
            const int probes = clamp ((int) std::ceil (2.0f * readSpan / (float) shot->envStep), 8, 256);
            const bool integrate = readSpan > 128.0f * (float) shot->envStep;
            double sumEnergy = 0.0, sumWeight = 0.0;
            for (int i = 0; i < probes; ++i)
            {
                const float phase = ((float) i + 0.5f) / (float) probes;
                const float window = 0.5f - 0.5f * fastCosTbl (phase);
                double energy;
                if (integrate)
                {
                    const int section = reverse ? probes - 1 - i : i;
                    const double from = (double) pos + (double) readSpan * (double) section / probes;
                    const double to = (double) pos + (double) readSpan * (double) (section + 1) / probes;
                    energy = shot->meanEnvEnergy (from, to);
                }
                else
                {
                    const float samplePhase = reverse ? 1.0f - phase : phase;
                    const float env = shot->envAt (pos + samplePhase * readSpan);
                    energy = (double) env * env;
                }
                const double weight = (double) window * window;
                sumEnergy += weight * energy;
                sumWeight += weight;
            }
            if (sumWeight > 0.0)
                local = std::max (local, (float) std::sqrt (sumEnergy / sumWeight));
        }
        local = std::max (1.0e-4f, local);
        // Верхний предел 5x — чтобы не вытягивать шум из полной тишины.
        const float comp = clamp (bodyRms / local, 0.25f, 5.0f);

        // Приведение к абсолютному уровню: голос не должен зависеть от того,
        // насколько громко записан источник (пробник 0.40 RMS против Fm Tine
        // 0.14 давал разницу 11 дБ при одинаковых настройках).
        const float bodyScale = clamp (kBodyTarget / bodyRms, 0.05f, 6.0f);

        // Фактическое ожидаемое перекрытие: density × реальная длительность.
        // Прежний sqrt(dur / want) уменьшал короткие зёрна ещё сильнее;
        // общий 1/sqrt(density × want) учитывал несуществующее перекрытие.
        // При ожидаемом числе больше ёмкости учитываем только 12 живых зёрен;
        // ниже единицы не усиливаем паузы и не поднимаем единичный импульс.
        const float overlap = clamp (density * g->dur / (float) sampleRate,
                                     1.0f, (float) kMaxGrains);
        const float norm = 1.0f / std::sqrt (overlap);
        g->amp = (0.6f + 0.4f * std::min (1.0f, size / 0.05f)) * comp * bodyScale * norm;
        g->active = true;
    }

    const SampleBank* bank = nullptr;
    const OneShot*    shot = nullptr;
    Grain grains[kMaxGrains];

    double sampleRate = 48000.0;
    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true;

    // Для автономного Granular без Voice — воспроизводимое ненулевое значение;
    // в инструменте каждый note-on подставляет собственное значение голоса.
    float  noteRandom = 0.5f;
    float  baseRate = 1.0f, spread = 0.0f, sizeSec = 0.08f, density = 20.0f, bodyRms = 1.0f;
    float  reverseChance = 0.0f;
    uint32_t directionStreamSeed = 0;
    float  spawnAcc = 0.0f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;
    SmoothedValue posSm, sizeSm, gainSm;
    Rng rng { 0x51F3A7u }, directionRng { 0xB79D31u };
};

} // namespace orogen
