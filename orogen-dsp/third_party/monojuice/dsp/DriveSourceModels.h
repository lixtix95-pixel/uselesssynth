/*
 * DSP adaptations from Airwindows Tape/Tube and DaisySP Wavefolder/Decimator.
 * Original projects and licenses are listed in THIRD_PARTY_NOTICES_RU.md.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nova::fx::airwindows {

struct TubeState
{
    double previousInput = 0.0, previousOutput = 0.0;

    float process (float input, float amount, double sampleRate) noexcept
    {
        const double overallScale = std::max (8000.0, sampleRate) / 44100.0;
        if (overallScale > 1.9)
        {
            const double stored = input;
            input = static_cast<float> ((input + previousInput) * 0.5);
            previousInput = stored;
        }

        const double gain = 1.0 + std::clamp (double (amount), 0.0, 1.0) * 0.2246161992650486;
        const int power = int (5.0 * (1.0 - std::clamp (double (amount), 0.0, 1.0))) + 1;
        const double x = std::clamp (double (input) * gain, -1.0, 1.0);
        double factor = x;
        for (int i = 0; i < power; ++i)
            factor *= x;
        if ((power % 2) == 1 && x != 0.0)
            factor = (factor / x) * std::abs (x);
        const double shaped = (x - factor / double (power + 1)) * (1.0 + 1.0 / double (power));
        double output = shaped;

        if (overallScale > 1.9)
        {
            output = (shaped + previousOutput) * 0.5;
            previousOutput = shaped;
        }
        return float (output);
    }
};

struct TapeBiquad
{
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void configure (double frequency, double resonance) noexcept
    {
        const double k = std::tan (3.14159265358979323846 * frequency);
        const double norm = 1.0 / (1.0 + k / resonance + k * k);
        b0 = k / resonance * norm;
        b1 = 0.0;
        b2 = -b0;
        a1 = 2.0 * (k * k - 1.0) * norm;
        a2 = (1.0 - k / resonance + k * k) * norm;
    }

    double process (double input) noexcept
    {
        const double output = input * b0 + z1;
        z1 = input * b1 - output * a1 + z2;
        z2 = input * b2 - output * a2;
        return output;
    }
};

struct TapeState
{
    std::array<double, 2> midRoller{};
    std::array<double, 2> headBump{};
    std::array<TapeBiquad, 2> headFilter{};
    std::array<TapeBiquad, 2> signalFilter{};
    double rollAmount = 0.0, headBumpFrequency = 0.0;
    double lastSample = 0.0;
    bool flip = false;

    void prepare (double sampleRate) noexcept
    {
        const double scale = std::max (8000.0, sampleRate) / 44100.0;
        rollAmount = (1.0 - 0.6180339887498948482) / scale;
        headBumpFrequency = 0.12 / scale;
        for (auto& filter : headFilter)
            filter.configure (0.0072 / scale, 0.0009);
        for (auto& filter : signalFilter)
            filter.configure (0.032 / scale, 0.0007);
    }

    float process (float sample, float bump) noexcept
    {
        if (!std::isfinite (sample))
            return 0.0f;

        const int lane = flip ? 1 : 0;
        const double dry = sample;
        midRoller[std::size_t (lane)] = midRoller[std::size_t (lane)] * (1.0 - rollAmount)
                                     + sample * rollAmount;
        const double highs = sample - midRoller[std::size_t (lane)];

        double head = headBump[std::size_t (lane)] + sample * 0.05;
        head -= head * head * head * headBumpFrequency;
        head = std::sin (head);
        head = std::clamp (headFilter[std::size_t (lane)].process (head), -1.0, 1.0);
        headBump[std::size_t (lane)] = std::asin (head);

        double shaped = std::sin (sample);
        shaped = std::clamp (signalFilter[std::size_t (lane)].process (shaped), -1.0, 1.0);
        shaped = std::asin (shaped);
        flip = !flip;

        const double ground = dry - shaped;
        const double applySoften = 1.0 - std::cos (std::min (std::abs (highs) * 1.57079633, 1.57079633));
        if (highs > 0.0) shaped -= applySoften;
        if (highs < 0.0) shaped += applySoften;
        shaped = std::clamp (shaped, -1.2533141373155, 1.2533141373155);
        const double magnitude = std::abs (shaped);
        shaped = std::sin (shaped * magnitude) / (magnitude == 0.0 ? 1.0 : magnitude);

        const double suppress = (1.0 - std::abs (shaped)) * 0.00013;
        for (double& bumpState : headBump)
        {
            if (bumpState > suppress) bumpState -= suppress;
            if (bumpState < -suppress) bumpState += suppress;
        }
        shaped += ground + (headBump[0] + headBump[1]) * std::clamp (double (bump), 0.0, 1.0) * 0.1;

        constexpr double softness = 0.6180339887498948482;
        if (lastSample >= 0.99)
            lastSample = shaped < 0.99 ? (0.99 * softness + shaped * (1.0 - softness)) : 0.99;
        if (lastSample <= -0.99)
            lastSample = shaped > -0.99 ? (-0.99 * softness + shaped * (1.0 - softness)) : -0.99;
        if (shaped > 0.99)
            shaped = lastSample < 0.99 ? (0.99 * softness + lastSample * (1.0 - softness)) : 0.99;
        if (shaped < -0.99)
            shaped = lastSample > -0.99 ? (-0.99 * softness + lastSample * (1.0 - softness)) : -0.99;
        lastSample = shaped;
        return float (std::clamp (shaped, -0.99, 0.99));
    }
};

} // namespace nova::fx::airwindows

namespace nova::fx::daisysp {

inline float wavefolder (float input) noexcept
{
    const float interval = std::floor ((input + 1.0f) * 0.5f);
    const float sign = static_cast<int> (interval) % 2 == 0 ? 1.0f : -1.0f;
    return sign * (input - 2.0f * interval);
}

struct DecimatorState
{
    float downsampled = 0.0f, crushed = 0.0f;
    std::uint32_t count = 0;

    float process (float input, float amount) noexcept
    {
        const float level = std::clamp (amount, 0.0f, 1.0f);
        const std::uint32_t threshold = std::uint32_t (level * level * 96.0f);
        if (++count > threshold)
        {
            count = 0;
            downsampled = std::clamp (input, -1.0f, 1.0f);
        }

        const auto bits = std::uint32_t (1.0f + level * 14.0f);
        std::int32_t temp = std::int32_t (downsampled * 65536.0f);
        temp = (temp >> bits) * (std::int32_t (1) << bits);
        crushed = float (temp) / 65536.0f;
        return crushed;
    }
};

} // namespace nova::fx::daisysp
