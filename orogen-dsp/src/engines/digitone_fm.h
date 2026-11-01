// Four-operator phase-modulation voice for the eight Digitone-style graphs.
// The graph table is adapted from ChoralRoot FM-1 (see digitone_routing.h).
// OROGEN's macro curves, amplitude normalization, anti-alias filtering and
// output envelope are its own: this is not a bit-exact Digitone emulation.
#pragma once
#include "engine.h"
#include "halfband.h"
#include "digitone_routing.h"
#include <array>
#include <cmath>

namespace orogen {

class DigitoneFmVoice
{
public:
    void setSampleRate (double sr) noexcept
    {
        sampleRate = std::isfinite (sr) && sr >= 1000.0 ? sr : 48000.0;
        for (auto& s : ratio) s.setTime (sampleRate, 20.0);
        for (auto& s : depth) s.setTime (sampleRate, 15.0);
        feedback.setTime (sampleRate, 20.0);
        mix.setTime (sampleRate, 15.0);
        dcCoef = (float) std::exp (-kTwoPi * 7.0 / sampleRate);
    }

    void start() noexcept
    {
        phase.fill (0.0f);
        previousFeedback = dcX = dcY = 0.0f;
        first = true;
        decimA.reset(); decimB.reset();
    }

    void prepare (const EngineParams& p, int note, float driftCents) noexcept
    {
        const double cents = (double) driftCents * (double) clamp (p.detune, 0.0f, 1.0f) * 2.0;
        baseHz = midiToHz ((double) note + (double) p.tuneSemis) * std::pow (2.0, cents / 1200.0);
        if (! std::isfinite (baseHz) || baseHz < 0.0) baseHz = sampleRate;
        const float targets[4] = { clamp (p.fmRatioC, 0.25f, 16.0f),
                                   0.5f + clamp (p.p[0], 0.0f, 1.0f) * 15.5f,
                                   clamp (p.fmRatioB1, 0.25f, 16.0f),
                                   clamp (p.fmRatioB2, 0.25f, 16.0f) };
        const float a = clamp (p.p[1], 0.0f, 1.0f), b = clamp (p.p[2], 0.0f, 1.0f);
        if (first)
        {
            for (int i = 0; i < 4; ++i) ratio[(size_t) i].snap (targets[i]);
            depth[0].snap (a); depth[1].snap (b);
            feedback.snap (clamp (p.p[3], 0.0f, 1.0f));
            mix.snap (clamp (p.fmMix, 0.0f, 1.0f));
            first = false;
        }
        else
        {
            for (int i = 0; i < 4; ++i) ratio[(size_t) i].setTarget (targets[i]);
            depth[0].setTarget (a); depth[1].setTarget (b);
            feedback.setTarget (clamp (p.p[3], 0.0f, 1.0f));
            mix.setTarget (clamp (p.fmMix, 0.0f, 1.0f));
        }
        gain = 0.55f * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
    }

    float getPhase (int op) const noexcept { return phase[(size_t) clamp (op, 0, 3)]; }

    float render (int algorithm) noexcept
    {
        const int index = clamp (algorithm - 1, 0, 7);
        const auto& graph = kDigitoneRouting[index];
        const float a = depth[0].next(), b = depth[1].next();
        const float fb = feedback.next(), xy = mix.next();
        std::array<float, 4> inc {}, gate {};
        double highest = 1.0;
        for (int op = 0; op < 4; ++op)
        {
            const double hz = baseHz * ratio[(size_t) op].next();
            highest = std::max (highest, hz);
            gate[(size_t) op] = (float) clamp ((0.5 * sampleRate - hz) / (0.05 * sampleRate), 0.0, 1.0);
            inc[(size_t) op] = (float) (std::min (hz, 0.499 * sampleRate) / (4.0 * sampleRate));
        }
        // Conservative per-operator sideband budget. Cascaded PM can generate
        // more partials than one Carson bound, so share the budget between A/B.
        const float budget = (float) clamp ((0.46 * sampleRate / highest - 1.0) * 0.45, 0.0, 8.0);
        const float levels[4] = { 0.0f, std::min (a * 8.0f, budget),
                                  std::min (b * 6.0f, budget), std::min (b * 6.0f, budget) };
        const float feedbackRadians = std::min (fb * 4.0f, budget);
        // Keep the local self-PM loop slope below unity. Otherwise large
        // feedback settings can period-double and create strong subharmonics.
        const float feedbackScale = feedbackRadians > 0.6f ? 0.9f / feedbackRadians : 1.5f;
        const float carrierGain[4] = {1.0f, a, b, b};
        const auto countBits = [] (std::uint8_t m) noexcept {
            return (int) ((m & 1) != 0) + (int) ((m & 2) != 0) + (int) ((m & 4) != 0) + (int) ((m & 8) != 0);
        };
        const float normX = 1.0f / (float) std::max (1, countBits (graph.x));
        const float normY = 1.0f / (float) std::max (1, countBits (graph.y));
        for (int sub = 0; sub < 4; ++sub)
        {
            float out[4] = {};
            for (int step = 0; step < 4; ++step)
            {
                const int op = kDigitoneOrder[index][step];
                float modulation = 0.0f;
                const auto mask = graph.mod[op];
                for (int source = 0; source < 4; ++source)
                    if ((mask & (1u << source)) != 0) modulation += out[source] * levels[source];
                if (op == graph.feedback) modulation += fastTanh (previousFeedback * feedbackScale) * feedbackRadians;
                phase[(size_t) op] += inc[(size_t) op];
                if (phase[(size_t) op] >= 1.0f) phase[(size_t) op] -= 1.0f;
                float ph = phase[(size_t) op] + modulation * 0.1591549431f;
                ph -= std::floor (ph);
                out[op] = fastSin (ph) * gate[(size_t) op];
                if (op == graph.feedback) previousFeedback = out[op];
            }
            float x = 0.0f, y = 0.0f;
            for (int op = 0; op < 4; ++op)
            {
                const float v = out[op] * ((graph.enveloped & (1u << op)) ? carrierGain[op] : 1.0f);
                if ((graph.x & (1u << op)) != 0) x += v;
                if ((graph.y & (1u << op)) != 0) y += v;
            }
            const float signal = x * normX * (1.0f - xy) + y * normY * xy;
            decimA.push (signal);
            if ((sub & 1) != 0) decimB.push (decimA.decimate());
        }
        const float signal = decimB.decimate();
        const float y = signal - dcX + dcCoef * dcY;
        dcX = signal; dcY = y;
        return y * gain;
    }

private:
    double sampleRate = 48000.0, baseHz = 261.625565;
    std::array<float, 4> phase {};
    std::array<SmoothedValue, 4> ratio {};
    std::array<SmoothedValue, 2> depth {};
    SmoothedValue feedback, mix;
    HalfbandDecimator2x<31> decimA;
    HalfbandDecimator2x<63, 220> decimB;
    float previousFeedback = 0.0f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f, gain = 0.55f;
    bool first = true;
};

} // namespace orogen
