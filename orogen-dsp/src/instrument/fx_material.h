// MATERIAL — модальный резонатор как эффект: подставляет инструменту
// «тело» (латекс, пружина, металл, стекло) и умеет следить за высотой входа.
//
// ИСТОЧНИК: MonoJuice 0.9.5, Source/dsp/MaterialResonator.h (код автора
// плагина), лежит в third_party/monojuice/dsp/MaterialResonator.h без правок.
//
// Четыре профиля:
//   0 latex  — прежний банк из 4 мод (не пересчитывается на сэмпле);
//   1 spring — 24 массы, моды ~ sqrt(k/m), удар в точке 20;
//   2 metal  — тонкая алюминиевая пластина (простая опора по краям, моды ~ k²);
//   3 glass  — обод тонкой стеклянной чаши, расщеплённые пары мод.
// Параметры MonoJuice (0…100) пересчитаны в 0…1; FOLLOW — трекинг высоты
// входа по переходам через ноль.
#pragma once

#include "../../third_party/monojuice/dsp/MaterialResonator.h"

namespace orogen {

class MaterialFx
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        res.prepare (sampleRate);
        apply();
    }

    void reset() { res.reset(); }

    void setMode (int m)        { s.mode = m; apply(); }
    void setExcite (float x)    { s.excite = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setTone (float x)      { s.tone = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setDamp (float x)      { s.damp = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setSize (float x)      { s.size = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setFeedback (float x)  { s.feedback = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setMix (float x)       { s.mix = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setFollow (bool on)    { s.follow = on; apply(); }

    inline void process (float& l, float& r) noexcept
    {
        if (s.mix <= 0.001f) return;
        const nova::fx::Frame out = res.process ({ l, r });
        l = out.l; r = out.r;
    }

private:
    void apply()
    {
        // MaterialResonator::set() сам сглаживает параметры и помечает
        // коэффициенты к пересчёту — звать его каждый параметр безопасно.
        res.set (s);
    }

    nova::fx::MaterialResonator res;
    nova::fx::MaterialSettings s;
    double sr = 48000.0;
};

} // namespace orogen
