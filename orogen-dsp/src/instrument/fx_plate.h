// Plate reverb по схеме Джона Датторро (figure-of-eight tank).
//
// ИСТОЧНИК: https://github.com/el-visio/dattorro-verb (MIT, © 2022 Pauli Pölkki),
//           код лежит в third_party/dattorro-verb/ без изменений.
// АЛГОРИТМ: J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters",
//           JAES 45(9), 1997 — https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
//
// Отличия от референса (все — осознанные):
//   1. Переменная частота дискретизации: длины линий задержки референса подобраны
//      для 48 кГц, здесь они масштабируются как sr/48000 (и округляются до целых
//      сэмплов, чтобы читать без интерполяции).
//   2. Стерео на входе: суммируем в моно (как в референсе), но добавляем
//      L/R-декорреляцию через разные точки съёма — референс уже это делает.
//   3. Сглаживание параметров (8 мс): в референсе decay/дамп /прыгают/ мгновенно,
//      что при движении ручки даёт щелчок.
//   4. double внутри, float наружу: референс тоже double.
//
// Совпадение с референсом на 48 кГц проверяется в tests/test_fx.cpp
// (импульс → сравнение по сэмплам, допуск 1e-9).
#pragma once

#include "../core/dsp.h"

#include <vector>
#include <array>
#include <cmath>

namespace orogen {

class PlateReverb
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const double k = sr / 48000.0;          // масштаб длин линий задержки
        scale = k;

        preDelay.prepare ((int) (kMaxPreDelaySamples * k));
        inDiff[0].prepare (scaled (142));
        inDiff[1].prepare (scaled (107));
        inDiff[2].prepare (scaled (379));
        inDiff[3].prepare (scaled (277));

        decayDiff1[0].prepare (scaled (672), 32);      // 32 — запас на модуляцию
        decayDiff1[1].prepare (scaled (908), 32);

        preDamp[0].prepare (scaled (4453));
        preDamp[0].setTap (1, scaled (353));
        preDamp[0].setTap (2, scaled (3627));
        preDamp[0].setTap (3, scaled (1990));

        decayDiff2[0].prepare (scaled (1800));
        decayDiff2[0].setTap (1, scaled (187));
        decayDiff2[0].setTap (2, scaled (1228));

        postDamp[0].prepare (scaled (3720));
        postDamp[0].setTap (1, scaled (1066));
        postDamp[0].setTap (2, scaled (2673));

        preDamp[1].prepare (scaled (4217));
        preDamp[1].setTap (1, scaled (266));
        preDamp[1].setTap (2, scaled (2974));
        preDamp[1].setTap (3, scaled (2111));

        decayDiff2[1].prepare (scaled (2656));
        decayDiff2[1].setTap (1, scaled (335));
        decayDiff2[1].setTap (2, scaled (1913));

        postDamp[1].prepare (scaled (3163));
        postDamp[1].setTap (1, scaled (121));
        postDamp[1].setTap (2, scaled (1996));

        t = 0;
        preFilterState = 0.0;
        dampState[0] = dampState[1] = 0.0;

        // кэш реальных значений (без сглаживания на старте)
        decayCur = decayTarget; tankDiffCur = tankDiffTarget;
        inDiff1Cur = inDiff1Target; inDiff2Cur = inDiff2Target;
        dampCur = dampTarget; preFiltCur = preFiltTarget; preDelayCur = preDelayTarget;
        outTrimCur = outTrimTarget;

        smoothCoef = 1.0 - std::exp (-1.0 / (0.008 * sr));
        dcCoef = (float) std::exp (-2.0 * (float) kPi * 20.0f / (float) sr);
        applyPreDelay();
        recompute();
    }

    // --- параметры (все 0..1, кроме preDelay в секундах)
    void setDecay (float d)      { decayTarget   = clamp (d, 0.0f, 1.0f); }
    void setDamping (float d)    { dampTarget    = clamp (d, 0.0f, 1.0f); }
    void setPreFilter (float f)  { preFiltTarget = clamp (f, 0.0f, 1.0f); }
    void setPreDelay (float sec) { preDelayTarget = clamp (sec, 0.0f, 0.2f); }
    // Ручек диффузии в референсе три и они независимы:
    //   inputDiffusion1 (диффузоры 1-2), inputDiffusion2 (диффузоры 3-4),
    //   decayDiffusion1 (всепропускающие звенья внутри бака).
    void setDiffusion (float d) { tankDiffTarget = clamp (d, 0.0f, 1.0f); }          // = decayDiffusion1Amount
    void setInputDiffusion1 (float d) { inDiff1Target = clamp (d, 0.0f, 1.0f); }
    void setInputDiffusion2 (float d) { inDiff2Target = clamp (d, 0.0f, 1.0f); }
    void setMix (float m) { mix = clamp (m, 0.0f, 1.0f); }

    // Мгновенно применить параметры (без 8-мс сглаживания). Нужно для сверки
    // с референсом, у которого сглаживания нет; в работе не вызывается.
    void snapParams()
    {
        recompute();
        applyPreDelay();
        preDelayApplied = preDelayCur;
    }
    // 1.0 — как в референсе; 0.6 — прежний уровень в цепочке. Сглаживаем,
    // поскольку теперь выходной трим доступен автоматизации хоста.
    void setOutputTrim (float t) { outTrimTarget = clamp (t, 0.0f, 2.0f); }

    // DC-блокер 20 Гц — наше добавление (в референсе его нет). Для посэмпловой
    // сверки с оригиналом отключается, в цепочке всегда включён.
    void setDcBlocker (bool on) { dcOn = on; }

    // Стерео-вход: суммируем в моно, как в референсе (резонансный бак моно).
    void process (float& l, float& r) noexcept
    {
        const float in = (l + r) * 0.5f;

        // сглаживание параметров — раз в сэмпл, дешёвое
        smooth();

        // модуляция decayDiff1 — ровно как в референсе: пила по 2048 сэмплов,
        // знак переключается на 16-битном счётчике (у нас t 32-битный, поэтому
        // приводим к uint16_t, иначе после 32768 сэмплов модуляция «залипает»)
        if ((t & 0x07ff) == 0)
        {
            const int step = (((uint16_t) t) < (1u << 15)) ? -1 : +1;
            decayDiff1[0].bumpMain (step);
            decayDiff1[1].bumpMain (step);
        }

        double x = preDelay.process (t, in);
        preFilterState += (x - preFilterState) * preFiltCur;
        x = preFilterState;

        x = allpass (inDiff[0], t, inDiff1Cur, x);
        x = allpass (inDiff[1], t, inDiff1Cur, x);
        x = allpass (inDiff[2], t, inDiff2Cur, x);
        x = allpass (inDiff[3], t, inDiff2Cur, x);

        for (int i = 0; i < 2; ++i)
        {
            double x1 = x + postDamp[1 - i].read (0, t) * decayCur;
            x1 = allpass (decayDiff1[i], t, -tankDiffCur, x1);
            x1 = preDamp[i].process (t, x1);
            dampState[i] += (x1 - dampState[i]) * dampCur;
            x1 = dampState[i] * decayCur;
            x1 = allpass (decayDiff2[i], t, diff2TankCur, x1);   // = clamp(decay+0.15)
            postDamp[i].write (t, x1);
        }

        // В референсе счётчик инкрементируется В process(), а съёмы читаются
        // уже в getLeft()/getRight() — то есть на сэмпл позже. Повторяем это,
        // иначе получаем сдвиг на 1 сэмпл относительно оригинала.
        ++t;
        const double wetL = leftTap();
        const double wetR = rightTap();

        // DC-блокер 20 Гц: ревербератор не должен быть путём для постоянки
        float wl = (float) wetL, wr = (float) wetR;
        if (dcOn)
        {
            wl = (float) wetL - dcX1L + dcCoef * dcY1L; dcX1L = (float) wetL; dcY1L = wl;
            wr = (float) wetR - dcX1R + dcCoef * dcY1R; dcX1R = (float) wetR; dcY1R = wr;
        }
        l = l * (1.0f - mix) + wl * mix;
        r = r * (1.0f - mix) + wr * mix;
    }

private:
    static constexpr int kMaxPreDelaySamples = 9600;   // 200 мс при 48 кГц, как у параметра хоста

    int scaled (int samples48k) const
    {
        return std::max (2, (int) std::lround ((double) samples48k * scale));
    }

    // --- кольцевой буфер, как в референсе (степень двойки + маска), но C++
    struct DelayLine
    {
        // delay — рабочая длина линии, extra — запас под модуляцию (он
        // увеличивает буфер, но НЕ саму задержку; иначе длина уезжает)
        void prepare (int delay, int extra = 0)
        {
            int size = 1;
            while (size < delay + extra + 8) size <<= 1;
            buf.assign ((size_t) size, 0.0);
            mask = (uint32_t) (size - 1);
            mainOffset = mask + 1 - (uint32_t) delay;
            tapOffset[1] = tapOffset[2] = tapOffset[3] = mainOffset;
        }

        void setTap (int id, int delay) { tapOffset[id] = mask + 1 - (uint32_t) delay; }
        void bumpMain (int step) { mainOffset = (uint32_t) ((int) mainOffset + step); }

        // id 0 — основная задержка линии (mainOffset), 1..3 — съёмы для выходных тапов
        double read (int id, uint32_t t) const
        {
            const uint32_t off = (id == 0) ? mainOffset : tapOffset[id];
            return buf[(t + off) & mask];
        }

        double process (uint32_t t, double in) noexcept
        {
            buf[t & mask] = in;
            return buf[(t + mainOffset) & mask];
        }

        void write (uint32_t t, double in) noexcept { buf[t & mask] = in; }

        std::vector<double> buf;
        uint32_t mask = 0, mainOffset = 0;
        uint32_t tapOffset[4] {};
    };

    static double allpass (DelayLine& dl, uint32_t t, double gain, double in) noexcept
    {
        const double delayed = dl.read (0, t);
        in += delayed * -gain;
        dl.write (t, in);
        return delayed + in * gain;
    }

    double leftTap() const
    {
        double a = preDamp[1].read (1, t);
        a += preDamp[1].read (2, t);
        a -= decayDiff2[1].read (2, t);
        a += postDamp[1].read (2, t);
        a -= preDamp[0].read (3, t);
        a -= decayDiff2[0].read (1, t);
        a += postDamp[0].read (1, t);
        return a * outTrimCur;              // семь съёмов суммируются — нужен трим
    }

    // DattorroVerb_getRight: preDampingDelay[0] OUT1+OUT2, −decayDiffusion2[0] OUT2,
    // +postDampingDelay[0] OUT2, −preDampingDelay[1] OUT3, −decayDiffusion2[1] OUT1,
    // +postDampingDelay[1] OUT1
    double rightTap() const
    {
        double a = preDamp[0].read (1, t);
        a += preDamp[0].read (2, t);
        a -= decayDiff2[0].read (2, t);
        a += postDamp[0].read (2, t);
        a -= preDamp[1].read (3, t);
        a -= decayDiff2[1].read (1, t);
        a += postDamp[1].read (1, t);
        return a * outTrimCur;
    }

    void applyPreDelay()
    {
        const int d = clamp ((int) std::lround ((double) preDelayCur * sr), 0, (int) preDelay.buf.size() - 8);
        preDelay.mainOffset = preDelay.mask + 1 - (uint32_t) d;
    }

    void smooth() noexcept
    {
        const auto step = [this] (double& cur, double target)
        {
            cur += (target - cur) * smoothCoef;
        };
        step (decayCur, decayTarget);
        step (tankDiffCur, tankDiffTarget);
        step (inDiff1Cur, inDiff1Target);
        step (inDiff2Cur, inDiff2Target);
        step (dampCur, targetFromNorm (dampTarget));
        step (preFiltCur, targetFromNorm (preFiltTarget));
        step (preDelayCur, preDelayTarget);
        step (outTrimCur, outTrimTarget);

        diff2TankCur = clamp (decayCur + 0.15, 0.25, 0.50);  // decayDiffusion2Amount в референсе

        if (std::fabs (preDelayCur - preDelayApplied) > 1.0e-4)
        {
            preDelayApplied = preDelayCur;
            applyPreDelay();
        }
    }

    // Нормализация в коэффициенты как в референсе:
    //   decay 0..1 -> 0.75 (по умолчанию); он же множитель обратной связи
    //   damping    -> частота однополюсника 0.05…1 (1 = без фильтра)
    //   preFilter  -> частота однополюсника
    static double targetFromNorm (double n) noexcept
    {
        // В референсе dampingAmount и preFilterAmount — это непосредственно
        // коэффициенты однополюсника (0.95 и 0.85 по умолчанию), поэтому
        // маппинг единичный: так адаптер даёт те же коэффициенты, что и оригинал.
        return clamp (n, 0.02, 1.0);
    }

    void recompute() noexcept
    {
        decayCur = decayTarget;
        tankDiffCur = tankDiffTarget;
        inDiff1Cur = inDiff1Target;
        inDiff2Cur = inDiff2Target;
        dampCur = targetFromNorm (dampTarget);
        preFiltCur = targetFromNorm (preFiltTarget);
        diff2TankCur = clamp (decayCur + 0.15, 0.25, 0.50);
        preDelayCur = preDelayTarget;
        preDelayApplied = preDelayCur;
        outTrimCur = outTrimTarget;
    }

    DelayLine preDelay, inDiff[4], decayDiff1[2], preDamp[2], decayDiff2[2], postDamp[2];
    double preFilterState = 0.0, dampState[2] { 0.0, 0.0 };
    double decayCur = 0.75, dampCur = 0.95, preFiltCur = 0.85;
    double inDiff1Cur = 0.75, inDiff2Cur = 0.625, tankDiffCur = 0.70;
    double diff2TankCur = 0.9;   // = clamp(decay + 0.15), как decayDiffusion2Amount
    double preDelayCur = 0.01, preDelayApplied = 0.01;
    double smoothCoef = 0.003;

    float decayTarget = 0.75f, dampTarget = 0.95f, preFiltTarget = 0.85f;
    float tankDiffTarget = 0.70f, preDelayTarget = 0.01f;
    float inDiff1Target = 0.75f, inDiff2Target = 0.625f;
    float mix = 0.0f;
    double outTrimCur = 0.6, outTrimTarget = 0.6;
    bool dcOn = true;
    float dcCoef = 0.997f, dcX1L = 0.0f, dcY1L = 0.0f, dcX1R = 0.0f, dcY1R = 0.0f;
    double sr = 48000.0, scale = 1.0;
    uint32_t t = 0;
};

} // namespace orogen
