// MATERIAL — мастер-эффект с четырьмя профилями: латекс, пружина, металл,
// стекло. Оригинальный MonoJuice MaterialResonator оставлен без изменений
// для FOLLOW OFF. Локальная адаптация в fx_material_follow.h использует MIDI
// вместо zero-crossing трекера; SIZE/TONE изменяют тембр, но не высоту.
#pragma once

#include "../../third_party/monojuice/dsp/MaterialResonator.h"
#include "fx_material_follow.h"

namespace orogen {

class MaterialFx
{
public:
    void prepare (double sampleRate)
    {
        sr = std::isfinite (sampleRate) && sampleRate >= 8000.0 ? sampleRate : 44100.0;
        res.prepare (sr);
        midiRes.prepare (sr);
        apply();
        followBlend = s.follow ? 1.0f : 0.0f;
        followStep = (float) (1.0 / (0.010 * sr));
    }

    void reset()
    {
        res.reset(); midiRes.reset();
        followBlend = s.follow ? 1.0f : 0.0f;
    }

    void setMode (int m)        { s.mode = m; apply(); }
    void setExcite (float x)    { s.excite = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setTone (float x)      { s.tone = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setDamp (float x)      { s.damp = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setSize (float x)      { s.size = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setFeedback (float x)  { s.feedback = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setMix (float x)       { s.mix = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setFollow (bool on)    { s.follow = on; apply(); }

    // Audio thread only. Each rack copy receives the same note independently.
    void setMidiPitchHz (double hz) noexcept { midiRes.setMidiPitchHz (hz); }
    double midiPitchTargetHz() const noexcept { return midiRes.midiPitchTargetHz(); }

    inline void process (float& l, float& r) noexcept
    {
        if (s.mix <= 0.001f) return;
        const nova::fx::Frame input { l, r };
        if (! s.follow && followBlend <= 0.0f)
        {
            const auto out = res.process (input);
            l = out.l; r = out.r;
            return;
        }
        if (s.follow && followBlend >= 1.0f)
        {
            const auto out = midiRes.process (input);
            l = out.l; r = out.r;
            return;
        }
        // Switching FOLLOW on an active signal crossfades the two actual DSP
        // paths; in steady FOLLOW OFF the upstream path is sample-identical.
        const auto a = res.process (input);
        const auto b = midiRes.process (input);
        followBlend = s.follow ? std::min (1.0f, followBlend + followStep)
                               : std::max (0.0f, followBlend - followStep);
        l = a.l + (b.l - a.l) * followBlend;
        r = a.r + (b.r - a.r) * followBlend;
    }

private:
    void apply()
    {
        auto legacy = s;
        legacy.follow = false;
        res.set (legacy);
        midiRes.set (s);
    }

    nova::fx::MaterialResonator res;
    nova::fx::MaterialMidiResonator midiRes;
    nova::fx::MaterialSettings s;
    double sr = 48000.0;
    float followBlend = 0.0f, followStep = 1.0f / 480.0f;
};

} // namespace orogen
