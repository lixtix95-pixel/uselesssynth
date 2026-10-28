// ADSR с экспоненциальными сегментами (звучит «аналогово», в отличие от линейных).
// Атака — сглаженная экспонента до 1, decay/release — one-pole к цели.
#pragma once

#include "dsp.h"

namespace orogen {

class Adsr
{
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setSampleRate (double sr)
    {
        sampleRate = sr;
        recompute();
    }

    // все времена в секундах
    void setParams (float attack, float decay, float sustain, float release)
    {
        aTime = clamp (attack, 0.0005f, 20.0f);
        dTime = clamp (decay,  0.001f,  30.0f);
        sustain = clamp (sustain, 0.0f, 1.0f);
        rTime = clamp (release, 0.001f, 30.0f);
        sLevel = sustain;
        recompute();
    }

    void noteOn (bool retrigger = true)
    {
        if (retrigger) { value = 0.0f; }
        stage = Stage::Attack;
    }

    void noteOff() { if (stage != Stage::Idle) stage = Stage::Release; }

    void reset() { value = 0.0f; stage = Stage::Idle; }

    bool isActive() const { return stage != Stage::Idle; }

    inline float process() noexcept
    {
        switch (stage)
        {
            case Stage::Idle:
                break;

            case Stage::Attack:
                value += attackCoef * (1.0f + attackShape - value);
                // Порог, а не строгое >= 1: значение подходит к единице
                // асимптотически и на float застревает на 0.999997, из-за чего
                // стадия Decay не наступала НИКОГДА — спад и sustain просто не
                // работали (найдено при тестах мод-матрицы: Env не убывал).
                if (value >= 0.9999f) { value = 1.0f; stage = Stage::Decay; }
                break;

            case Stage::Decay:
                value = sLevel + (value - sLevel) * decayCoef;
                if (std::fabs (value - sLevel) < 1.0e-4f) { value = sLevel; stage = Stage::Sustain; }
                break;

            case Stage::Sustain:
                value = sLevel;
                if (sLevel <= 1.0e-5f) stage = Stage::Idle;
                break;

            case Stage::Release:
                value *= releaseCoef;
                if (value < 5.0e-5f) { value = 0.0f; stage = Stage::Idle; }
                break;
        }
        return value;
    }

    Stage getStage() const { return stage; }
    float getValue() const { return value; }

private:
    void recompute()
    {
        attackCoef  = 1.0f - std::exp (-1.0f / (float) std::max (1.0, aTime  * sampleRate * 0.35));
        attackShape = 0.0f;
        decayCoef   = std::exp (-1.0f / (float) std::max (1.0, dTime  * sampleRate * 0.29));
        releaseCoef = std::exp (-1.0f / (float) std::max (1.0, rTime  * sampleRate * 0.29));
    }

    double sampleRate = 48000.0;
    float aTime = 0.01f, dTime = 0.3f, sLevel = 0.8f, rTime = 0.4f;
    float sustain = 0.8f;
    float attackCoef = 0.01f, attackShape = 0.0f, decayCoef = 0.999f, releaseCoef = 0.999f;
    float value = 0.0f;
    Stage stage = Stage::Idle;
};

} // namespace orogen
