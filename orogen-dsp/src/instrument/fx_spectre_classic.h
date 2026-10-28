// SPECTRE CLASSIC — «классический» вариант того же спектрального эффекта:
// маска по выступающим пикам без ранжирования, фиксированная ширина,
// без сдвига и заморозки. Звучит мягче EXTENDED: не выбирает «топ-N» пиков,
// а глушит всё, что не выделяется над соседями на заданное число дБ.
//
// ИСТОЧНИК: MonoJuice 0.9.5, Source/dsp/SpectreClassic.h — порт алгоритма
// Spectrum Pixelator (Michael Nuzzo, © 2019, BSD-3-Clause). Файл лежит в
// third_party/monojuice/dsp/SpectreClassic.h без правок; он переиспользует
// MDCT-планы из Spectre.h (та же BSD-3), поэтому ничего не дублируется.
//
// Латентность равна длине окна (512 << index), как и у EXTENDED.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../core/dsp.h"
#include "../../third_party/monojuice/dsp/SpectreClassic.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace orogen
{

class SpectreClassicFx
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        plans.prepare();
        fx.prepare (plans);
        prepared = true;
        apply();
    }

    void setWindow (int index)    { s.window = index; apply(); }
    void setProminence (float db) { s.prominence = clamp (db, 1.0f, 100.0f); apply(); }
    void setMix (float x)         { mixN = clamp (x, 0.0f, 1.0f); s.mix = mixN * 100.0f; apply(); }

    // Латентность в сэмплах — длина окна.
    int latency() const { return fx.latency(); }

    inline void process (float& l, float& r) noexcept
    {
        if (mixN <= 0.0005f) return;              // без микса — без MDCT и без задержки
        const nova::fx::Frame out = fx.process ({ l, r });
        if (isBad (out.l) || isBad (out.r)) { l = 0.0f; r = 0.0f; return; }
        l = out.l; r = out.r;
    }

private:
    void apply() { if (prepared) fx.set (s); }

    nova::fx::MDCTPlans plans;
    nova::classic::Spectre fx;
    nova::classic::SpectreSettings s;
    float mixN = 0.0f;
    double sr = 48000.0;
    bool prepared = false;
};

} // namespace orogen
