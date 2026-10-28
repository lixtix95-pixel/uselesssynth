// SPECTRE — спектральный эффект на MDCT (модифицированное DCT-IV, окно
// Кайзера β=5, перекрытие 50%): маска по «выступающим» пикам, сдвиг по
// частоте, заморозка, диапазон и ширина.
//
// ИСТОЧНИК: MonoJuice 0.9.5, Source/dsp/Spectre.h — порт алгоритма
// Spectrum Pixelator (Michael Nuzzo, BSD-3-Clause). Файл лежит в
// third_party/monojuice/dsp/Spectre.h без правок; лицензия — BSD-3
// (licenses/SpectrumPixelator-BSD-3-Clause.txt в архиве плагина).
//
// Окну соответствует 512 << index сэмплов: 512 … 65536. Чем больше окно, тем
// точнее частотное разрешение и тем больше задержка (латентность = длина окна).
#pragma once

// Сторонний код написан в плотном стиле без скобок у однострочных if/for —
// это даёт -Wmisleading-indentation. Заголовок не правим (важен как эталон),
// поэтому глушим предупреждение только на время включения.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../core/dsp.h"
#include "../../third_party/monojuice/dsp/Spectre.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace orogen {

class SpectreFx
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        plans.prepare();            // 8 планов MDCT, считается один раз
        fx.prepare (plans, sampleRate);
        prepared = true;            // до этого set() некуда применять: планов нет
        apply();
    }

    void setWindow (int index)     { s.window = index; apply(); }
    void setProminence (float db)  { s.prominence = clamp (db, 1.0f, 100.0f); apply(); }
    void setMix (float x)          { mixN = clamp (x, 0.0f, 1.0f); s.mix = mixN * 100.0f; apply(); }
    void setRange (float loHz, float hiHz) { s.low = loHz; s.high = hiHz; apply(); }
    void setPeaks (int n)          { s.peaks = n; apply(); }
    void setWidth (int bins)       { s.width = bins; apply(); }
    void setShift (float semis)    { s.shift = semis; apply(); }
    void setFreeze (bool on)       { s.freeze = on; apply(); }

    // Латентность в сэмплах: ровно длина окна, её надо компенсировать
    // задержкой сухого сигнала (FX-цепочка этого пока не делает).
    int latency() const { return fx.latency(); }

    inline void process (float& l, float& r) noexcept
    {
        // при нулевом миксе не тратим MDCT (и не вносим задержку окна)
        if (mixN <= 0.0005f) return;
        const nova::fx::Frame out = fx.process ({ l, r });
        // как и в CLASSIC: не-финитный кадр не должен попасть в тракт
        if (! std::isfinite (out.l) || ! std::isfinite (out.r)) { l = 0.0f; r = 0.0f; return; }
        l = out.l; r = out.r;
    }

private:
    // set() можно звать до prepare(): плагин заполняет параметры раньше, чем
    // хост вызовет prepareToPlay(). Раньше это был segfault: Spectre::reset()
    // обращался к ещё не посчитанным планам MDCT.
    void apply() { if (prepared) fx.set (s); }

    nova::fx::MDCTPlans plans;
    nova::fx::Spectre fx;
    nova::fx::SpectreSettings s;
    float mixN = 0.0f;
    double sr = 48000.0;
    bool prepared = false;
};

} // namespace orogen
