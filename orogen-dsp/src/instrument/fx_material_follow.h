// Local OROGEN adaptation of MonoJuice's MaterialResonator.h:
// https://github.com/lixtix95-pixel/monohuice-0.9.15
// The original third_party file is untouched. This version replaces its
// zero-crossing pitch tracker with an externally supplied MIDI frequency,
// keeps the first mode at that frequency independently of SIZE/TONE, and uses
// double-precision poles/states for accurate low notes. Physical mode formulas
// and parameter smoothing are adapted from that source. Each retuned modal
// filter is normalized at its resonant frequency rather than relying on the
// source's wet peak guard. See FX.md for provenance and limitations.
#pragma once

#include "../../third_party/monojuice/dsp/MaterialResonator.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace nova::fx {

class MaterialMidiResonator
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr = std::isfinite (sampleRate) && sampleRate >= 8000.0 ? sampleRate : 44100.0;
        pitchSlew = 1.0 - std::exp (-1.0 / (0.005 * sr));
        target = {}; current = target;
        reset();
    }

    void reset() noexcept
    {
        left = {}; right = {}; coefficients = {};
        current = target;
        pitch = targetPitch;
        activeModes = coefficientTick = 0;
        dirty = true;
        configure();
        dirty = false;
    }

    void set (MaterialSettings value) noexcept
    {
        const int mode = std::clamp (value.mode, 0, 3);
        const bool modeChanged = mode != current.mode;
        const float tone = std::clamp (value.tone, 0.0f, 100.0f);
        const float damp = std::clamp (value.damp, 0.0f, 100.0f);
        const float size = std::clamp (value.size, 0.0f, 100.0f);
        const float feedback = std::clamp (value.feedback, 0.0f, 100.0f);
        if (target.mode != mode || target.tone != tone || target.damp != damp
            || target.size != size || target.feedback != feedback)
            dirty = true;
        target.mode = mode;
        target.excite = std::clamp (value.excite, 0.0f, 100.0f);
        target.tone = tone; target.damp = damp; target.size = size; target.feedback = feedback;
        target.mix = std::clamp (value.mix, 0.0f, 100.0f);
        target.follow = true;
        current.mode = mode;
        current.follow = true;
        if (modeChanged)
        {
            left = {}; right = {}; coefficients = {};
            activeModes = coefficientTick = 0;
            configure();
            dirty = false;
        }
    }

    // Audio thread only. The last MIDI note remains the target during release;
    // C4 is the default before the first note. Values outside the usable
    // frequency range are not folded into a different audible pitch.
    void setMidiPitchHz (double hz) noexcept
    {
        if (std::isfinite (hz) && hz > 0.0) targetPitch = hz;
    }
    double midiPitchTargetHz() const noexcept { return targetPitch; }

    Frame process (Frame in) noexcept
    {
        const float oldTone = current.tone, oldDamp = current.damp;
        const float oldSize = current.size, oldFeedback = current.feedback;
        current.excite += (target.excite - current.excite) * smooth;
        current.tone += (target.tone - current.tone) * smooth;
        current.damp += (target.damp - current.damp) * smooth;
        current.size += (target.size - current.size) * smooth;
        current.feedback += (target.feedback - current.feedback) * smooth;
        current.mix += (target.mix - current.mix) * smooth;
        const double oldPitch = pitch;
        pitch += (targetPitch - pitch) * pitchSlew;
        if (std::abs (targetPitch - pitch) < 1.0e-7) pitch = targetPitch;
        if (pitch != oldPitch || current.tone != oldTone || current.damp != oldDamp
            || current.size != oldSize || current.feedback != oldFeedback)
            dirty = true;
        if (dirty && ++coefficientTick >= 16)
        {
            configure(); coefficientTick = 0; dirty = false;
        }

        // A normalized linear modal filter is driven by the actual audio;
        // it does not create uncontrolled gain on a sustained MIDI note.
        const Frame exciter = in;
        const float inputGain = 0.05f + 0.95f * std::clamp (current.excite * 0.01f, 0.0f, 1.0f);
        double wetL = 0.0, wetR = 0.0;
        for (int i = 0; i < activeModes; ++i)
        {
            const auto index = (std::size_t) i;
            const auto& c = coefficients[index];
            double yl = (double) exciter.l * inputGain * c.drive
                      + c.feedback * left[index].one - c.radius2 * left[index].two;
            double yr = (double) exciter.r * inputGain * c.drive
                      + c.feedback * right[index].one - c.radius2 * right[index].two;
            if (std::abs (yl) < 1.0e-30) yl = 0.0;
            if (std::abs (yr) < 1.0e-30) yr = 0.0;
            left[index].two = left[index].one; left[index].one = yl;
            right[index].two = right[index].one; right[index].one = yr;
            wetL += yl * c.output; wetR += yr * c.output;
        }
        const Frame wet { (float) (wetL * outputGain), (float) (wetR * outputGain) };
        // No limiter in the MIDI-follow path: bad modal gain must remain
        // visible to the DSP tests rather than being hidden by a peak guard.
        return fx::mix (in, wet, std::clamp (current.mix * 0.01f, 0.0f, 1.0f));
    }

private:
    struct State { double one = 0.0, two = 0.0; };
    struct Coefficient { double feedback = 0.0, radius2 = 0.0, drive = 0.0, output = 0.0; };
    static constexpr int maxModes = 24;
    static constexpr double pi = 3.1415926535897932384626433832795;
    static constexpr double twoPi = 2.0 * pi;
    static constexpr float smooth = 0.001f;

    // A permanent slot per physical mode prevents state being reassigned when
    // other partials cross Nyquist during a note/size change.
    void addMode (double frequency, double radius, double driveWeight,
                  double outputWeight, double& energy) noexcept
    {
        if (activeModes >= maxModes) return;
        const auto index = (std::size_t) activeModes++;
        auto& c = coefficients[index];
        if (! std::isfinite (frequency) || frequency <= 0.0 || frequency >= sr * 0.45)
        {
            c = {}; left[index] = {}; right[index] = {};
            return;
        }
        const double angle = twoPi * frequency / sr;
        c.feedback = 2.0 * radius * std::cos (angle);
        c.radius2 = radius * radius;
        // The transfer function's denominator magnitude at the pole angle:
        // |1 - 2r cos(w)e^-jw + r²e^-j2w| = (1-r)|1-r e^-j2w|.
        // This gives each mode unity peak gain before its shape weight, at
        // every pitch and decay time, including MIDI 0 at high sample rates.
        const double poleGain = (1.0 - radius) * std::hypot (1.0 - radius * std::cos (2.0 * angle),
                                                             radius * std::sin (2.0 * angle));
        c.drive = poleGain * std::max (0.0, driveWeight);
        c.output = std::max (0.0, outputWeight);
        energy += c.output * c.output;
    }

    void addPhysical (double frequency, double t60, double driveWeight,
                      double outputWeight, double scale, double& energy) noexcept
    {
        t60 = std::clamp (t60, 0.035, 8.0);
        const double radius = std::exp (-6.90775527898 / (sr * t60));
        addMode (frequency * scale, radius, driveWeight, outputWeight, energy);
    }

    void configure() noexcept
    {
        activeModes = 0;
        double energy = 0.0;
        const double size = std::clamp ((double) current.size * 0.01, 0.0, 1.0);
        const double tone = std::clamp ((double) current.tone * 0.01, 0.0, 1.0);
        const double damping = std::clamp ((double) current.damp * 0.01, 0.0, 1.0);
        const double resonance = std::clamp ((double) current.feedback * 0.01, 0.0, 1.0);
        if (current.mode == 0)
        {
            constexpr double ratios[] = { 1.0, 1.41, 2.17, 3.08 };
            constexpr double weights[] = { 1.0, 0.78, 0.58, 0.38 };
            const double decayMs = (16.0 + (1.0 - damping) * 260.0 + resonance * 520.0) * 0.72;
            const double radius = std::exp (-1.0 / std::max (1.0, sr * decayMs * 0.001));
            double weightSum = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                addMode (pitch * ratios[i], radius, weights[i], weights[i], energy);
                if (coefficients[(std::size_t) i].output > 0.0) weightSum += weights[i];
            }
            outputGain = 0.55 / std::max (0.1, weightSum);
            return;
        }

        // Timbre follows the *retuned* modal frequency. Using the original
        // unscaled glass frequency here made TONE=0/SIZE=0 nearly silent.
        const double contactHz = 350.0 + 14500.0 * std::pow (tone, 1.4);
        if (current.mode == 1)
        {
            constexpr int masses = 24, hit = 20;
            const double q1 = pi / (2.0 * masses + 1.0);
            const double first = 80.0 / (0.65 + 0.875 * size);
            const double sqrtKOverM = twoPi * first / (2.0 * std::sin (q1 * 0.5));
            const double scale = pitch / first;
            const double shapeNorm = std::sqrt (2.0 / (masses + 0.5));
            const double baseT60 = 0.10 + (1.0 - damping) * 1.6 + resonance * 1.4;
            for (int j = 1; j <= masses; ++j)
            {
                const double q = (2.0 * j - 1.0) * pi / (2.0 * masses + 1.0);
                const double f = sqrtKOverM * std::sin (q * 0.5) / pi;
                const double shape = std::abs (std::sin (q * hit)) * shapeNorm;
                const double contact = std::exp (-std::pow ((f * scale) / contactHz, 2.0));
                const double t60 = baseT60 / (1.0 + std::pow (f / 2400.0, 1.2));
                addPhysical (f, t60, shape * contact, shape, scale, energy);
            }
        }
        else if (current.mode == 2)
        {
            constexpr double young = 69.0e9, density = 2700.0, poisson = 0.33, thickness = 0.0012;
            constexpr double lengthX = 0.27, lengthY = 0.20, hitX = 0.22, hitY = 0.31;
            const double speed = std::sqrt (young / (12.0 * density * (1.0 - poisson * poisson)));
            const double plateScale = 0.55 + 0.9 * size;
            const double coefficient = pi * thickness * speed * 0.5 / (plateScale * plateScale);
            const double first = coefficient * (1.0 / (lengthX * lengthX) + 1.0 / (lengthY * lengthY));
            const double scale = pitch / first;
            const double baseT60 = 0.10 + (1.0 - damping) * 2.5 + resonance * 1.8;
            for (int m = 1; m <= 4; ++m)
                for (int n = 1; n <= 6; ++n)
                {
                    const double f = coefficient * ((m * m) / (lengthX * lengthX) + (n * n) / (lengthY * lengthY));
                    const double shape = std::abs (std::sin (pi * m * hitX) * std::sin (pi * n * hitY));
                    const double contact = std::exp (-std::pow ((f * scale) / contactHz, 2.0));
                    const double t60 = baseT60 / (1.0 + std::pow (f / 1900.0, 1.25));
                    addPhysical (f, t60, shape * contact, shape, scale, energy);
                }
        }
        else
        {
            constexpr double young = 70.0e9, density = 2500.0, thickness = 0.0015, radius = 0.045;
            const double ringRadius = radius * (0.55 + 0.9 * size);
            const double materialSpeed = std::sqrt (young / density);
            const double coefficient = materialSpeed * thickness / (4.0 * pi * std::sqrt (3.0) * ringRadius * ringRadius);
            const double first = coefficient * (6.0 / std::sqrt (5.0));
            const double scale = pitch / first;
            const double baseT60 = 0.15 + (1.0 - damping) * 4.8 + resonance * 2.5;
            for (int order = 2; order <= 12; ++order)
            {
                const double n = (double) order;
                const double f = coefficient * n * (n * n - 1.0) / std::sqrt (n * n + 1.0);
                const double shape = std::sqrt (0.5 * std::exp (-std::pow (f / 8500.0, 2.0))
                                               / (1.0 + 0.025 * (order - 2)));
                const double contact = std::exp (-std::pow ((f * scale) / contactHz, 2.0));
                const double t60 = baseT60 / (1.0 + std::pow (f / 10500.0, 1.5));
                const double split = 0.0018 * (order - 1);
                // Keep the glass character's paired upper modes, but both
                // fundamental modes are exactly at the MIDI note.
                addPhysical (f * (order == 2 ? 1.0 : 1.0 - split), t60, shape * contact, shape, scale, energy);
                addPhysical (f * (order == 2 ? 1.0 : 1.0 + split), t60, shape * contact, shape, scale, energy);
            }
        }
        outputGain = 0.55 / std::sqrt (std::max (0.01, energy));
    }

    double sr = 44100.0, pitch = 261.6255653006, targetPitch = 261.6255653006, pitchSlew = 0.0;
    double outputGain = 0.55;
    int activeModes = 0, coefficientTick = 0;
    bool dirty = false;
    MaterialSettings current {}, target {};
    std::array<State, maxModes> left {}, right {};
    std::array<Coefficient, maxModes> coefficients {};
};

} // namespace nova::fx
