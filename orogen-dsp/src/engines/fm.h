// ДВИЖОК 2: FM — 2 оператора + второй модулятор + feedback.
//
// Ключевая физика, без которой FM «звенит мусором»:
//
//   Правило Карсона: значимая полоса ≈ 2·(index+1)·f_mod.
//   Отсюда жёсткий предел индекса: чтобы гармоники не ушли за Найквист,
//   нужно (index+1)·f_mod < SR/2, то есть index < SR/(2·f_mod) − 1.
//   Индекс здесь — В РАДИАНАХ фазы, а не «0..100»: только так он означает
//   физически измеримую ширину спектра.
//
//   Ratio целочисленный -> гармонический спектр (орган, Rhodes, бас).
//   Ratio нецелый      -> ингармонический (колокол, металл, «стекло»).
//
// Реализация: одна таблица синуса с линейной интерполяцией на сэмпл,
// feedback через tanh (чистый DX-фидбэк при >0.7 срывается в шум).
#pragma once

#include "engine.h"

namespace orogen {

class FmEngine final : public EngineBase
{
public:
    static constexpr float kGainTrim = 0.55f;

    const char* name() const override { return "FM"; }
    float costFactor() const override { return 1.0f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        ratioSm.setTime  (sr, 20.0);
        idxSm.setTime    (sr, 15.0);
        idx2Sm.setTime   (sr, 15.0);
        fbSm.setTime     (sr, 20.0);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        ph1 = ph2 = ph3 = 0.0f;
        fbState = 0.0f;
        driftCents = rng.nextBipolar() * 5.0f;
        needSnap = true;
        active = true;
    }

    void stop() override { active = false; fbState = 0.0f; }

    void prepare (const EngineParams& p) override
    {
        // ---- ratio: 1..16 (непрерывный; 0.5 и целые — самые музыкальные точки)
        const float ratio = 0.5f + clamp (p.p[0], 0.0f, 1.0f) * 15.5f;

        const double cents = (double) driftCents * (double) p.detune * 2.0;
        const double f0 = midiToHz ((double) note + (double) p.tuneSemis) * std::pow (2.0, cents / 1200.0);

        inc1 = (float) (f0 / sampleRate);
        inc2 = inc1 * ratio;
        inc3 = inc2 * 1.5f;                       // второй модулятор — квинта к первому

        // ---- индексы в РАДИАНАХ с ограничением по Найквисту
        const double nyq = 0.5 * sampleRate;
        const double idxMax1 = std::max (0.0, nyq * 0.92 / std::max (1.0, (double) inc2 * sampleRate) - 1.0);
        const double idxMax2 = std::max (0.0, nyq * 0.92 / std::max (1.0, (double) inc3 * sampleRate) - 1.0);

        const float wantIdx1 = clamp (p.p[1], 0.0f, 1.0f) * 8.0f;
        const float wantIdx2 = clamp (p.p[2], 0.0f, 1.0f) * 3.0f;
        const float wantFb   = clamp (p.p[3], 0.0f, 1.0f) * 0.7f;

        idxTarget  = (float) std::min ((double) wantIdx1, idxMax1);
        idx2Target = (float) std::min ((double) wantIdx2, idxMax2);
        fbTarget   = wantFb;

        if (needSnap)
        {
            ratioSm.snap (ratio);
            idxSm.snap (idxTarget);
            idx2Sm.snap (idx2Target);
            fbSm.snap (fbTarget);
            ratioCur = ratio;
            needSnap = false;
        }
        else
        {
            ratioSm.setTarget (ratio);
            idxSm.setTarget (idxTarget);
            idx2Sm.setTarget (idx2Target);
            fbSm.setTarget (fbTarget);
        }

        // флаг «есть модуляция» — для быстрого пути без лишних чтений таблицы
        needsMod = (idxTarget > 1.0e-4f) || (idx2Target > 1.0e-4f) || (fbTarget > 1.0e-4f);
        gain = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
        dcCoef = (float) std::exp (-kTwoPi * 7.0 / sampleRate);
    }

    inline float render (const EngineParams&) override
    {
        if (! active) return 0.0f;

        const float idx  = idxSm.next();
        const float idx2 = idx2Sm.next();
        const float fb   = fbSm.next();

        // ratio меняется медленно; инкременты пересчитываем при заметном сдвиге
        const float r = ratioSm.next();
        if (std::fabs (r - ratioCur) > 1.0e-4f)
        {
            ratioCur = r;
            inc2 = inc1 * r;
            inc3 = inc2 * 1.5f;
        }

        ph1 += inc1; if (ph1 >= 1.0f) ph1 -= 1.0f;
        float out;

        if (! needsMod && idx < 1.0e-5f && idx2 < 1.0e-5f && fb < 1.0e-5f)
        {
            out = fastSin (ph1);                                  // чистый синус
        }
        else
        {
            ph2 += inc2; if (ph2 >= 1.0f) ph2 -= 1.0f;
            float mod = fastSin (ph2) * idx;
            if (idx2 > 1.0e-5f)
            {
                ph3 += inc3; if (ph3 >= 1.0f) ph3 -= 1.0f;
                mod += fastSin (ph3) * idx2;
            }
            float ph = ph1 + mod * 0.15915494f;                    // радианы -> обороты
            ph -= std::floor (ph);
            out = fastSin (ph);

            if (fb > 1.0e-4f)
            {
                // feedback: оператор модулирует сам себя по предыдущему выходу.
                // tanh не даёт сорваться в шум на больших глубинах.
                const float fbs = fastTanh (fbState * 2.0f) * fb;
                fbState = out;
                float ph2f = ph1 + (mod + fbs * 6.0f) * 0.15915494f;
                ph2f -= std::floor (ph2f);
                out = fastSin (ph2f);
            }
        }

        // DC-блокер (FM с нецелым ratio даёт постоянку)
        const float y = out - dcX + dcCoef * dcY;
        dcX = out; dcY = y;
        return y * gain;
    }

private:
    double sampleRate = 48000.0;
    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true, needsMod = false;

    float  ph1 = 0.0f, ph2 = 0.0f, ph3 = 0.0f, fbState = 0.0f;
    float  inc1 = 0.0f, inc2 = 0.0f, inc3 = 0.0f;
    float  ratioCur = 1.0f;
    float  idxTarget = 0.0f, idx2Target = 0.0f, fbTarget = 0.0f;
    float  driftCents = 0.0f;
    float  gain = 0.5f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;

    SmoothedValue ratioSm, idxSm, idx2Sm, fbSm;
    Rng rng { 0x2468ACEu };
};

// ---------------------------------------------------------------------------
// Табличные пресеты движка FM (то, что в Terrain называется «Fm Tine», «Split»…)
// p[0] = ratio 0..1, p[1] = FM1, p[2] = FM2, p[3] = feedback
// ---------------------------------------------------------------------------
struct FmVoicePreset
{
    const char* name;
    float p[4];
};

inline const FmVoicePreset& fmPreset (int i)
{
    static const FmVoicePreset presets[] = {
        { "Tine",      { 0.10f, 0.30f, 0.05f, 0.10f } },   // ratio ~2.0 — Rhodes
        { "Bell",      { 0.55f, 0.55f, 0.20f, 0.00f } },   // ratio ~9 (нецелый) — колокол
        { "Bass",      { 0.02f, 0.45f, 0.00f, 0.25f } },   // ratio ~1 — суб-бас
        { "Metal",     { 0.72f, 0.70f, 0.35f, 0.45f } },   // ratio ~11 + feedback
        { "Glass",     { 0.32f, 0.40f, 0.25f, 0.00f } },   // ratio ~5.5
        { "Organ",     { 0.10f, 0.08f, 0.00f, 0.00f } },   // ratio 2, почти без модуляции
    };
    return presets[clamp (i, 0, 5)];
}

} // namespace orogen
