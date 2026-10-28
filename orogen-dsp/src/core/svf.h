// TPT State-Variable Filter (Zavalishin / «Cytomic style»):
// устойчив на быстрых модуляциях, даёт LP/BP/HP/Notch из одного ядра,
// без «взрыва» при высоком резонансе. Один фильтр на голос.
#pragma once

#include "dsp.h"

namespace orogen {

enum class FilterType { LP12 = 0, LP24, BP, HP24, Notch };

class Svf
{
public:
    void reset() noexcept
    {
        ic1eq = ic2eq = 0.0f;
        // денормал-защита: слабый шум в состоянии
        ic1eq = 1.0e-20f;
    }

    void setSampleRate (double sr) noexcept { sampleRate = sr; }

    // cutoff Hz, res 0..1 (0 -> Q≈0.7, 1 -> Q≈25), drive 0..1
    void setParams (float cutoffHz, float res, float driveAmount) noexcept
    {
        const float fc = clamp (cutoffHz, 20.0f, (float) sampleRate * 0.45f);
        g  = std::tan ((float) kPi * fc / (float) sampleRate);
        k  = 1.0f / (0.707f + std::pow (clamp (res, 0.0f, 0.98f), 2.0f) * 24.0f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        drive = 1.0f + driveAmount * 8.0f;
        // деление и компенсация зависят только от drive — считаем их при смене
        // параметров, а не 8 слотов × 16 голосов раз в сэмпл
        driveTrim = 1.0f / drive + 0.25f;
    }

    inline float process (float x, FilterType type) noexcept
    {
        x = fastTanh (x * drive) * driveTrim;

        const float v3 = x - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0f * v1 - ic1eq;
        ic2eq = 2.0f * v2 - ic2eq;

        float out;
        switch (type)
        {
            case FilterType::LP12:  out = v2; break;
            case FilterType::BP:    out = v1; break;
            case FilterType::HP24:  out = x - k * v1 - v2; break;
            case FilterType::Notch: out = x - k * v1; break;
            case FilterType::LP24:
            default:
            {
                // второй каскад для 24 дБ/окт
                const float u3 = v2 - ic2b;
                const float u1 = a1 * ic1b + a2 * u3;
                const float u2 = ic2b + a2 * ic1b + a3 * u3;
                ic1b = 2.0f * u1 - ic1b;
                ic2b = 2.0f * u2 - ic2b;
                out = u2;
                break;
            }
        }
        ic1eq = antiDenormal (ic1eq);
        ic2eq = antiDenormal (ic2eq);
        return out;
    }

private:
    double sampleRate = 48000.0;
    float g = 0.1f, k = 1.4f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float drive = 1.0f, driveTrim = 1.25f;
    float ic1eq = 0.0f, ic2eq = 0.0f;
    float ic1b = 0.0f, ic2b = 0.0f;
};

} // namespace orogen
