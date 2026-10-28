/*
 * C++17 adaptations of Airwindows PurestDrive, Density, Pressure4, and
 * ButterComp2 DSP algorithms. Original project: https://github.com/airwindows/airwindows
 * Copyright (c) 2016 Airwindows. MIT license; see
 * Source/third_party/Airwindows/LICENSE.
 * MonoJuice control mappings and integration are Copyright (c) 2026.
 */
#pragma once

#include "FirmwarePrimitives.h"
#include "DriveSourceModels.h"
#include <algorithm>
#include <cmath>

namespace nova::fx::airwindows {

constexpr float pi = 3.14159265358979323846f;

struct DriveState
{
    float purestPrevious = 0.0f;
    float densityLow = 0.0f;
    TapeState tape;
    TubeState tube;
    daisysp::DecimatorState decimator;
};

inline float purestDrive (float x, float amount, DriveState& state) noexcept
{
    const float shaped = std::sin (x);
    const float blend = std::clamp (0.5f * std::abs (state.purestPrevious + shaped) * amount,
                                    0.0f, 1.0f);
    state.purestPrevious = std::sin (x);
    return x + blend * (shaped - x);
}

inline float density (float x, float amount, double sampleRate, DriveState& state) noexcept
{
    const float scale = static_cast<float> (std::max (sampleRate, 8000.0) / 44100.0);
    const float iir = std::clamp (amount * amount * amount * 0.005f / scale, 0.0f, 0.25f);
    state.densityLow += iir * (x - state.densityLow);
    x -= state.densityLow;

    const float densityValue = 4.0f * amount;
    auto shape = [] (float input) noexcept
    {
        const float magnitude = std::min (std::abs (input), 1.0f) * (0.5f * pi);
        return std::copysign (std::sin (magnitude), input);
    };
    const int stages = static_cast<int> (densityValue);
    for (int stage = 0; stage < stages; ++stage)
        x = shape (x);
    const float stageMix = densityValue - static_cast<float> (stages);
    return x + stageMix * (shape (x) - x);
}

struct Pressure4State
{
    float speed[2] { 10000.0f, 10000.0f };
    float coefficient[2] { 1.0f, 1.0f };
    bool flip = false;
};

inline Frame pressure4 (Frame x, float pressure, float speedControl, float mewinessControl,
                        float outputGain, double sampleRate,
                        Pressure4State& state, float& reductionDb) noexcept
{
    const float a = std::clamp (pressure, 0.0f, 1.0f);
    const float threshold = 1.0f - a * 0.95f;
    const float makeup = 1.0f / threshold;
    const float scale = static_cast<float> (std::max (sampleRate, 8000.0) / 44100.0);
    const float speedSetting = std::clamp (speedControl, 0.0f, 1.0f);
    const float release = std::pow (1.28f - speedSetting, 5.0f) * 32768.0f / scale;
    const float fastest = std::sqrt (release);
    const float signedMewiness = std::clamp (mewinessControl, -1.0f, 1.0f);
    const bool positiveMewiness = signedMewiness >= 0.0f;
    const float mewiness = std::abs (signedMewiness);
    const float unmewiness = 1.0f - mewiness;
    const int lane = state.flip ? 0 : 1;

    const float left = std::clamp (x.l * makeup, -64.0f, 64.0f);
    const float right = std::clamp (x.r * makeup, -64.0f, 64.0f);
    const float sense = std::max (std::abs (left), std::abs (right));
    float coefficient = std::clamp (state.coefficient[lane], 0.0f, 1.0f);

    if (sense > threshold)
    {
        const float vary = threshold / std::max (sense, 1.0e-12f);
        const float attack = std::sqrt (std::max (state.speed[lane], 1.0e-6f));
        coefficient = (coefficient * (attack - 1.0f)
                     + (vary < threshold ? threshold : vary)) / attack;
    }
    else
    {
        const float speedSquared = state.speed[lane] * state.speed[lane];
        coefficient = (coefficient * (speedSquared - 1.0f) + 1.0f) / speedSquared;
    }

    state.coefficient[lane] = std::clamp (coefficient, 0.0f, 1.0f);
    const float speed = std::max (state.speed[lane], 1.0e-6f);
    state.speed[lane] = std::clamp ((speed * (speed - 1.0f)
                                  + std::abs (sense * release) + fastest) / speed,
                                    1.0f, 1.0e6f);

    coefficient = state.coefficient[lane];
    const float curved = positiveMewiness ? coefficient * coefficient : std::sqrt (coefficient);
    const float gain = curved * mewiness + coefficient * unmewiness;
    auto shape = [gain, outputGain] (float sample) noexcept
    {
        sample *= gain * std::clamp (outputGain, 0.0f, 1.0f);
        const float magnitude = std::abs (sample);
        const float curve = magnitude > 1.57079633f ? 1.0f : std::sin (magnitude);
        return std::copysign (curve, sample);
    };

    const Frame processed { shape (left), shape (right) };
    reductionDb = std::max (0.0f, -20.0f * std::log10 (std::max (gain, 1.0e-9f)));
    state.flip = ! state.flip;
    return processed;
}

struct ButterChannelState
{
    float controlApos = 1.0f, controlAneg = 1.0f;
    float controlBpos = 1.0f, controlBneg = 1.0f;
    float targetPos = 1.0f, targetNeg = 1.0f;
    float lastOutput = 0.0f;
};

struct ButterComp2State
{
    ButterChannelState channel[2];
    bool flip = false;
};

inline float butterChannel (float input, float amount, float postGain, float sampleRate,
                            ButterChannelState& state, bool flip) noexcept
{
    const float inputGain = std::pow (10.0f, amount * 14.0f / 20.0f);
    const float normalization = 1.0f + (inputGain - 1.0f) / 1.5f;
    const float compFactor = 0.012f * (amount / 135.0f);
    input = std::clamp (input * inputGain, -64.0f, 64.0f);

    const float rateScale = std::max (static_cast<float> (sampleRate), 8000.0f);
    const float divisor = std::clamp (compFactor / (1.0f + std::abs (state.lastOutput))
                                    * 44100.0f / rateScale,
                                      0.0f, 0.25f);
    const float retain = 1.0f - divisor;
    const float positive = std::max (input + 1.0f, 0.0f);
    const float negative = std::max (1.0f - input, 0.0f);
    state.targetPos = std::max (1.0e-5f, state.targetPos * retain + positive * positive * divisor);
    state.targetNeg = std::max (1.0e-5f, state.targetNeg * retain + negative * negative * divisor);
    const float calcPos = std::min (1.0f / (state.targetPos * state.targetPos), 1.0e4f);
    const float calcNeg = std::min (1.0f / (state.targetNeg * state.targetNeg), 1.0e4f);

    float& controlPos = flip ? state.controlApos : state.controlBpos;
    float& controlNeg = flip ? state.controlAneg : state.controlBneg;
    if (input >= 0.0f)
        controlPos = controlPos * retain + calcPos * divisor;
    else
        controlNeg = controlNeg * retain + calcNeg * divisor;
    controlPos = std::clamp (controlPos, 0.0f, 1.0e4f);
    controlNeg = std::clamp (controlNeg, 0.0f, 1.0e4f);

    const float outputPos = std::clamp ((input + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float outputNeg = std::clamp ((1.0f - input) * 0.5f, 0.0f, 1.0f);
    const float multiplier = controlPos * outputPos + controlNeg * outputNeg;
    const float output = input * multiplier / normalization * std::clamp (postGain, 0.0f, 2.0f);
    state.lastOutput = output;
    return output;
}

inline Frame butterComp2 (Frame x, float amount, float outputGain, float wet,
                          double sampleRate,
                          ButterComp2State& state, float& reductionDb) noexcept
{
    const float a = std::clamp (amount, 0.0f, 1.0f);
    const float beforeGain = std::pow (10.0f, a * 14.0f / 20.0f);
    const bool flip = state.flip;
    const float left = butterChannel (x.l, a, outputGain, static_cast<float> (sampleRate), state.channel[0], flip);
    const float right = butterChannel (x.r, a, outputGain, static_cast<float> (sampleRate), state.channel[1], flip);
    const float afterGain = std::max (std::abs (left / std::max (std::abs (x.l), 1.0e-6f)),
                                      std::abs (right / std::max (std::abs (x.r), 1.0e-6f)));
    reductionDb = std::max (0.0f, -20.0f * std::log10 (std::max (afterGain / beforeGain, 1.0e-9f)));
    state.flip = ! state.flip;
    const float mix = std::clamp (wet, 0.0f, 1.0f);
    return { x.l + mix * (left - x.l), x.r + mix * (right - x.r) };
}

} // namespace nova::fx::airwindows
