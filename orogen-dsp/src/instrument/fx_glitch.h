// GLITCH: MonoJuice GlitchCore (0.9.15). The legacy OROGEN adapter prepared
// the core at max(host rate, 96001 Hz), changing physical times and filters.
// Two cores are prepared off the audio thread: old sessions retain that path;
// new INIT patches use the real host rate and the upstream history policy
// (24 seconds through 96 kHz, 4 seconds above 96 kHz). Mode changes and all
// parameter/tempo updates are allocation-free in the audio callback.
#pragma once

#include "../../third_party/monojuice/dsp/GlitchCore.h"

namespace orogen {

class GlitchFx
{
public:
    void prepare (double sampleRate)
    {
        const double sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
        legacy.prepare (std::max (96001.0, sr), 512);
        accurate.prepare (sr, 512);
        apply();
    }

    void setTimingMode (int mode) { accurateTiming = mode != 0; }
    void setChance (float x)     { s.stutterChance = clamp (x, 0.0f, 1.0f); apply(); }
    void setLengthMs (float ms)  { s.stutterLengthMs = clamp (ms, 1.0f, 500.0f); apply(); }
    void setRepeats (float n)    { s.repeats = clamp (n, 1.0f, 16.0f); apply(); }
    void setReverse (float x)    { s.reverseProbability = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setFeedback (float x)   { s.feedback = clamp (x, 0.0f, 1.0f) * 95.0f; apply(); }
    void setPitch (float semis)  { s.pitchSemitones = clamp (semis, -12.0f, 12.0f); apply(); }
    void setFold (float amount)  { s.foldAmount = clamp (amount, 0.0f, 10.0f); apply(); }
    void setFoldDrive (float v)  { s.foldDrive = clamp (v, 0.0f, 20.0f); apply(); }
    void setTone (float hz)      { s.tone = clamp (hz, 200.0f, 24000.0f); apply(); }
    void setToneHighpass (bool b){ s.toneType = b ? 1.0f : 0.0f; apply(); }
    void setResonance (float v)  { s.resonance = clamp (v, 0.0f, 95.0f); apply(); }
    void setOffset (float v)     { s.captureOffset = clamp (v, 0.0f, 100.0f); apply(); }
    void setCrossfade (float v)  { s.crossfade = clamp (v, 0.0f, 100.0f); apply(); }
    void setSync (bool b, int division) { s.lengthSync = b ? 1.0f : 0.0f; s.lengthDivision = (float) clamp (division, 0, 8); apply(); }
    void setDrift (float v)      { s.sizeDrift = clamp (v, -100.0f, 100.0f); apply(); }
    void setPitchStep (float v)  { s.pitchStep = clamp (v, -12.0f, 12.0f); apply(); }
    void setPitchRandom (float v){ s.pitchRandom = clamp (v, 0.0f, 12.0f); apply(); }
    void setReroll (float v)     { s.rerollProbability = clamp (v, 0.0f, 100.0f); apply(); }
    void setDecay (float v)      { s.repeatDecay = clamp (v, 0.0f, 90.0f); apply(); }
    // -1 means follow the old single MIX. Nonnegative levels independently
    // override dry/wet exactly as in the source. They are not extra gain stages.
    // Keep the original standalone adapter API as the linked-level mode.
    void setMix (float mix) { setLevels (mix, -1.0f, -1.0f); }
    void setLevels (float mix, float dry, float wet)
    {
        wetN = clamp (mix, 0.0f, 1.0f);
        independentLevels = dry >= 0.0f || wet >= 0.0f;
        s.dry = dry < 0.0f ? 1.0f - wetN : clamp (dry, 0.0f, 1.0f);
        s.wet = wet < 0.0f ? wetN : clamp (wet, 0.0f, 1.0f);
        apply();
    }
    void setTempo (double bpm) { legacy.setTempo (bpm, false); accurate.setTempo (bpm, false); }

    inline void process (float& l, float& r) noexcept
    {
        if (wetN <= 0.0005f && ! independentLevels) return;
        const nova::fx::Frame out = accurateTiming ? accurate.process ({ l, r }) : legacy.process ({ l, r });
        l = out.l; r = out.r;
    }

private:
    void apply() { legacy.set (s); accurate.set (s); }
    nova::fx::GlitchCoreAudioProcessor legacy, accurate;
    nova::fx::GlitchSettings s;
    float wetN = 0.0f;
    bool accurateTiming = false, independentLevels = false;
};

} // namespace orogen
