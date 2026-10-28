// GLITCHY — статтер/глитч: захват куска истории, повторы, реверс, питч,
// тон и фолд поверх захваченного.
//
// ИСТОЧНИК: MonoJuice 0.9.5, Source/dsp/GlitchCore.h (код автора плагина).
// Файл лежит в third_party/monojuice/dsp/GlitchCore.h без правок.
//
// ВАЖНО про память: история — до 24 секунд стерео (при 48 кГц это ~9 МБ).
// Для «приборной» истории это много, поэтому адаптер по умолчанию просит
// 4 секунды (isHighRate → тот же путь, что в оригинале на 96+ кГц).
// Если нужен полный диапазон длины 8 тактов — поднять kHistorySeconds.
#pragma once

#include "../../third_party/monojuice/dsp/GlitchCore.h"

namespace orogen {

class GlitchFx
{
public:
    static constexpr double kHistorySeconds = 4.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // GlitchCoreAudioProcessor сам берёт 24 с при sr <= 96 кГц и 4 с выше;
        // чтобы не держать 9 МБ, готовим на «высокой» частоте — история 4 с.
        fx.prepare (sampleRate > 1000.0 ? std::max (96001.0, sampleRate) : 96001.0, 512);
        fx.set (s);
    }

    void setChance (float x)     { s.stutterChance = clamp (x, 0.0f, 1.0f); apply(); }
    void setLengthMs (float ms)  { s.stutterLengthMs = clamp (ms, 1.0f, 500.0f); apply(); }
    void setRepeats (float n)    { s.repeats = clamp (n, 1.0f, 16.0f); apply(); }
    void setReverse (float x)    { s.reverseProbability = clamp (x, 0.0f, 1.0f) * 100.0f; apply(); }
    void setFeedback (float x)   { s.feedback = clamp (x, 0.0f, 1.0f) * 95.0f; apply(); }
    void setPitch (float semis)  { s.pitchSemitones = clamp (semis, -12.0f, 12.0f); apply(); }
    void setFold (float amount)  { s.foldAmount = clamp (amount, 0.0f, 10.0f); apply(); }
    void setTone (float hz)      { s.tone = clamp (hz, 200.0f, 24000.0f); apply(); }
    void setMix (float wet)      { wetN = clamp (wet, 0.0f, 1.0f); s.wet = wetN; s.dry = 1.0f - wetN; apply(); }
    void setTempo (double bpm)   { fx.setTempo (bpm, false); }

    inline void process (float& l, float& r) noexcept
    {
        if (wetN <= 0.0005f) return;
        const nova::fx::Frame out = fx.process ({ l, r });
        l = out.l; r = out.r;
    }

private:
    void apply() { fx.set (s); }

    nova::fx::GlitchCoreAudioProcessor fx;
    nova::fx::GlitchSettings s;
    float wetN = 0.0f;
    double sr = 48000.0;
};

} // namespace orogen
