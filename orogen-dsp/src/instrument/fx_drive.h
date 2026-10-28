// Драйв-секция: 19 моделей нелинейности вместо одного tanh.
//
// ИСТОЧНИК (по разрешению автора плагина MonoJuice 0.9.5):
//   * 12 моделей (Soft Clip … Rectifier) и таблица профилей — MasterBus.h
//     MonoJuice (DriveUnit/applyDriveShape/driveProfile), код автора плагина;
//   * PurestDrive, Density, Airwindows Tape, Airwindows Tube — адаптации
//     Airwindows (MIT, © 2016 Chris Johnson), third_party/monojuice/dsp/
//     AirwindowsMasterModels.h и DriveSourceModels.h;
//   * Wavefolder и Decimator — DaisySP (MIT, © 2020 Electrosmith),
//     third_party/monojuice/dsp/DriveSourceModels.h;
//   * DiodeWdf — наш собственный WDF-клиппер из fx_wdf.h (BSD-3 chowdsp_wdf).
// Лицензии — third_party/monojuice/licenses/.
//
// Что изменено при переносе:
//   * JUCE-оверсэмплер заменён на наш полифазный Fir4x (fx_wdf.h): у него
//     измеренный алиасинг и он есть в ядре без зависимостей;
//   * один класс на оба канала (у них — вектор состояний на канал), так как
//     в OROGEN FX-цепочка обрабатывает стерео поканально;
//   * профили моделей оставлены как есть (это «voicing», подобранный на слух).
//
// Калибровка по громкости: у каждой модели свой диапазон предгейна
// (minDriveDb…maxDriveDb) и своя доля статической компенсации, поверх —
// адаптивная коррекция по RMS (±6 дБ), как в оригинале.
#pragma once

#include "fx_wdf.h"
#include "../../third_party/monojuice/dsp/AirwindowsMasterModels.h"

#include <cmath>
#include <algorithm>

namespace orogen {

enum class DriveModel : int
{
    SoftClip = 0, HardClip, Tanh, Tube, Tape, Transformer, Fuzz, Diode,
    Fold, Sine, BitCrush, Rectifier,          // 12 моделей MonoJuice
    PurestDrive, Density, AirTape, AirTube,   // Airwindows (MIT)
    Wavefolder, Decimator,                    // DaisySP (MIT)
    DiodeWdf,                                 // наш WDF-клиппер
    Count
};

inline const char* driveModelName (DriveModel m)
{
    switch (m)
    {
        case DriveModel::SoftClip:    return "Soft Clip";
        case DriveModel::HardClip:    return "Hard Clip";
        case DriveModel::Tanh:        return "Tanh";
        case DriveModel::Tube:        return "Tube";
        case DriveModel::Tape:        return "Tape";
        case DriveModel::Transformer: return "Transformer";
        case DriveModel::Fuzz:        return "Fuzz";
        case DriveModel::Diode:       return "Diode";
        case DriveModel::Fold:        return "Fold";
        case DriveModel::Sine:        return "Sine";
        case DriveModel::BitCrush:    return "Bit Crush";
        case DriveModel::Rectifier:   return "Rectifier";
        case DriveModel::PurestDrive: return "Purest Drive";
        case DriveModel::Density:     return "Density";
        case DriveModel::AirTape:     return "Air Tape";
        case DriveModel::AirTube:     return "Air Tube";
        case DriveModel::Wavefolder:  return "Wavefolder";
        case DriveModel::Decimator:   return "Decimator";
        case DriveModel::DiodeWdf:    return "Diode WDF";
        case DriveModel::Count:       break;
    }
    return "?";
}

// Профиль: предгейн, доля статической компенсации, частота раздела Tone,
// смещение перед шейпером (асимметрия → чётные гармоники), 4x оверсэмплинг.
struct DriveProfile
{
    float minDriveDb, maxDriveDb, compFraction, toneSplitHz, bias;
    bool  oversample4x;
};

inline DriveProfile driveProfile (DriveModel m) noexcept
{
    switch (m)
    {
        // 12 строк ниже — из MonoJuice MasterBus.h без изменений
        case DriveModel::SoftClip:    return {  6.0f, 24.0f, 0.85f,  700.0f, 0.00f, false };
        case DriveModel::HardClip:    return {  9.0f, 30.0f, 0.90f,  900.0f, 0.02f, true  };
        case DriveModel::Tanh:        return {  6.0f, 30.0f, 0.80f,  800.0f, 0.00f, false };
        case DriveModel::Tube:        return {  6.0f, 30.0f, 0.78f,  600.0f, 0.12f, false };
        case DriveModel::Tape:        return {  3.0f, 24.0f, 0.72f, 2200.0f, 0.00f, false };
        case DriveModel::Transformer: return {  6.0f, 30.0f, 0.75f, 1600.0f, 0.04f, false };
        case DriveModel::Fuzz:        return { 12.0f, 42.0f, 0.70f, 1100.0f, 0.10f, true  };
        case DriveModel::Diode:       return { 12.0f, 36.0f, 0.68f, 1000.0f, 0.06f, true  };
        case DriveModel::Fold:        return {  6.0f, 30.0f, 0.55f,  850.0f, 0.00f, true  };
        case DriveModel::Sine:        return {  6.0f, 30.0f, 0.55f,  850.0f, 0.00f, true  };
        case DriveModel::BitCrush:    return {  0.0f, 18.0f, 0.95f, 3000.0f, 0.00f, true  };
        case DriveModel::Rectifier:   return { 12.0f, 36.0f, 0.60f, 1200.0f, 0.00f, true  };
        // новые: подобраны по тому же принципу (громкость на выходе ≈ входу)
        case DriveModel::PurestDrive: return {  6.0f, 24.0f, 0.80f, 1600.0f, 0.00f, false };
        case DriveModel::Density:     return {  6.0f, 30.0f, 0.70f, 1200.0f, 0.00f, true  };
        case DriveModel::AirTape:     return {  3.0f, 24.0f, 0.75f, 2200.0f, 0.00f, false };
        case DriveModel::AirTube:     return {  6.0f, 30.0f, 0.75f,  900.0f, 0.00f, true  };
        case DriveModel::Wavefolder:  return {  6.0f, 30.0f, 0.60f, 1000.0f, 0.00f, true  };
        case DriveModel::Decimator:   return {  0.0f, 12.0f, 0.90f, 4000.0f, 0.00f, false };
        // WDF-клиппер уже передискретизирован внутри себя (4x), второй раз не надо
        case DriveModel::DiodeWdf:    return { 12.0f, 42.0f, 0.55f, 1200.0f, 0.00f, false };
        case DriveModel::Count:       break;
    }
    return { 6.0f, 30.0f, 0.8f, 800.0f, 0.0f, false };
}

// Калибровка громкости: у моделей разная «плотность», и при одинаковом
// amount уровень уезжал до 15 дБ. Ниже — замер RMS (220 Гц, вход 0.4 = RMS
// 0.283): поправка для amount = 0 и amount = 1, между ними линейно.
// Это наше добавление (в MonoJuice уровень модели не выравнивается), поэтому
// ручка DRIVE меняет характер, но не громкость.
inline float driveLevelTrimDb (DriveModel m, float amount) noexcept
{
    struct Trim { float at0, at1; };
    static constexpr Trim kTrim[(int) DriveModel::Count] = {
        {  +2.67f, +15.26f },  // Soft Clip
        {  +4.96f, +21.77f },  // Hard Clip
        {  +4.46f, +19.72f },  // Tanh
        {  +4.48f, +19.60f },  // Tube
        {  +1.97f, +12.33f },  // Tape
        {  +4.29f, +17.73f },  // Transformer
        {  +4.72f, +24.07f },  // Fuzz
        {  +6.20f, +17.77f },  // Diode
        {  +2.99f, +16.23f },  // Fold
        {  +2.92f, +14.66f },  // Sine
        {  -0.02f, +12.35f },  // Bit Crush
        {  +7.35f, +23.95f },  // Rectifier
        {  +4.37f,  +4.34f },  // Purest Drive
        {  +3.74f, +15.68f },  // Density
        {  +2.09f, +13.08f },  // Air Tape
        {  +2.92f, +17.21f },  // Air Tube
        {  +3.33f, +10.32f },  // Wavefolder
        {  -0.00f,  +6.57f },  // Decimator
        { +10.16f, +24.50f },  // Diode WDF
    };
    const auto& t = kTrim[(int) m];
    return lerp (t.at0, t.at1, clamp (amount, 0.0f, 1.0f));
}

class DriveFx
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        wdf.prepare (sampleRate);
        up.reset(); down.reset();
        dcCoef  = (float) std::exp (-kTwoPi * 14.0 / sr);
        rmsCoef = 1.0f - (float) std::exp (-1.0 / (sr * 0.30));
        gainCoef = 1.0f - (float) std::exp (-1.0 / (sr * 0.030));   // 30 мс на предгейн
        reset();
        setModel (model);
    }

    void reset()
    {
        state = { };
        awState = { };
        awState.tape.prepare (sr);
        toneLowL = toneLowR = 0.0f;
        dcX1L = dcY1L = dcX1R = dcY1R = 0.0f;
        preGain = preGainSm = 1.0f;
        postGainSm = outLevel = 1.0f;
        inRms = outRms = 0.0f;
        up.reset(); down.reset();
        wdf.reset();
    }

    // amount 0..1 — предгейн в диапазоне профиля модели
    void setAmount (float a) { amount = clamp (a, 0.0f, 1.0f); updateGains (true); }
    // tone -1..+1 — наклон ±8 дБ вокруг toneSplitHz профиля
    void setTone (float t) { tone = clamp (t, -1.0f, 1.0f); }
    void setMix (float m) { mix = clamp (m, 0.0f, 1.0f); }
    void setModel (DriveModel m)
    {
        model = m;
        const auto prof = driveProfile (m);
        toneSplit = prof.toneSplitHz;
        toneCoef = 1.0f - std::exp (-kTwoPi * toneSplit / (float) sr);
        wdf.setDrive (amount);
        updateGains (true);
    }
    DriveModel getModel() const { return model; }

    inline void process (float& l, float& r) noexcept
    {
        if (mix < 1.0e-4f) return;

        const float dryL = l, dryR = r;
        const auto prof = driveProfile (model);
        const float bias = prof.bias;
        preGainSm += (preGain - preGainSm) * gainCoef;
        const float g = preGainSm;

        l = l * g + bias;
        r = r * g + bias;

        if (prof.oversample4x)
        {
            float os[4];
            up.pushInput (l);
            up.up4 (os);
            for (int k = 0; k < 4; ++k)
                down.pushOversampled (shape (os[k]));
            l = down.down();

            up.pushInput (r);
            up.up4 (os);
            for (int k = 0; k < 4; ++k)
                down.pushOversampled (shape (os[k]));
            r = down.down();
        }
        else
        {
            l = shape (l);
            r = shape (r);
        }

        // Tone: наклон вокруг toneSplitHz (однополюсник делит полосы)
        const float lowG  = fastDbToLin (-8.0f * tone);
        const float highG = fastDbToLin (8.0f * tone);
        toneLowL += (l - toneLowL) * toneCoef;
        toneLowR += (r - toneLowR) * toneCoef;
        l = toneLowL * lowG + (l - toneLowL) * highG;
        r = toneLowR * lowG + (r - toneLowR) * highG;

        // DC-блокер после асимметричных моделей
        const float dl = l - dcX1L + dcCoef * dcY1L; dcX1L = l; dcY1L = dl; l = dl;
        const float dr = r - dcX1R + dcCoef * dcY1R; dcX1R = r; dcY1R = dr; r = dr;

        // адаптивная коррекция уровня по RMS (±6 дБ), как в оригинале
        const float power = 0.5f * (l * l + r * r);
        inRms  += rmsCoef * (0.5f * (dryL * dryL + dryR * dryR) - inRms);
        outRms += rmsCoef * (power - outRms);
        inRms = antiDenormal (inRms); outRms = antiDenormal (outRms);

        float adaptive = 1.0f;
        if (outRms > 1.0e-9f && inRms > 1.0e-9f)
            adaptive = clamp (std::sqrt (inRms / outRms), 0.5f, 2.0f);
        const float target = postGainSm * adaptive;
        outLevel += (target - outLevel) * 0.0005f;

        l = (dryL * (1.0f - mix)) + (l * outLevel * mix);
        r = (dryR * (1.0f - mix)) + (r * outLevel * mix);
    }

private:
    void updateGains (bool snap)
    {
        const auto prof = driveProfile (model);
        const float driveDb = lerp (prof.minDriveDb, prof.maxDriveDb, amount);
        const float compDb  = driveDb * prof.compFraction;
        preGain    = fastDbToLin (driveDb);
        postGainSm = fastDbToLin (-compDb + driveLevelTrimDb (model, amount));
        if (snap) { preGainSm = preGain; outLevel = postGainSm; }
    }

    // статический шейпер (x уже с предгейном и bias)
    inline float shape (float x) noexcept
    {
        switch (model)
        {
            case DriveModel::SoftClip:
            {
                const float a = std::fabs (x);
                if (a >= 1.0f) return x < 0.0f ? -1.0f : 1.0f;
                return x * (1.5f - 0.5f * x * x);
            }
            case DriveModel::HardClip:
            {
                constexpr float t = 0.97f;
                return clamp (x, -t, t) * (1.0f / t);
            }
            case DriveModel::Tanh:
                return fastTanh (x * 1.2f) * 0.9f;

            case DriveModel::Tube:
            {
                const float y = (x >= 0.0f) ? fastTanh (x * 0.85f)
                                            : fastTanh (x * 1.3f) * 0.68f;
                return y + 0.05f * y * y;
            }
            case DriveModel::Tape:
            {
                const float target = fastTanh (x * 1.15f);
                const float k = 0.30f - 0.10f * amount;
                state.lag += k * (target - state.lag);
                return state.lag * (0.45f + 0.2f * amount) + target * (0.55f - 0.2f * amount);
            }
            case DriveModel::Transformer:
            {
                const float t = clamp (x, -3.0f, 3.0f);
                const float t3 = t * t * t;
                const float t5 = t3 * t * t;
                const float y = fastTanh (0.95f * t - 0.20f * t3 + 0.035f * t5);
                state.lag += 0.03f * (y - state.lag);
                return y * 0.85f + state.lag * 0.15f;
            }
            case DriveModel::Fuzz:
            {
                constexpr float gate = 0.10f;
                const float a = std::max (std::fabs (x) - gate, 0.0f) / (1.0f - gate);
                const float s = (x < 0.0f ? -1.0f : 1.0f) * std::pow (std::min (a, 1.7f), 1.4f);
                return clamp (s, -1.0f, 1.0f);
            }
            case DriveModel::Diode:
            {
                constexpr float k = 0.72f;
                return (x >= 0.0f) ? 1.0f - std::exp (-x)
                                   : -(1.0f - std::exp (x * k)) / k;
            }
            case DriveModel::Fold:
            {
                float y = std::fmod (x + 1.0f, 4.0f);
                if (y < 0.0f) y += 4.0f;
                y = std::fabs (y - 2.0f) - 1.0f;
                return fastTanh (y * 1.25f) * 0.82f;
            }
            case DriveModel::Sine:
                return fastSin (clamp (x, -7.0f, 7.0f) * 0.15915494f);

            case DriveModel::BitCrush:
            {
                const float bits = 12.0f - 9.0f * amount;
                const float levels = std::pow (2.0f, bits) * 0.5f;
                return std::round (clamp (x, -1.0f, 1.0f) * levels) / levels;
            }
            case DriveModel::Rectifier:
            {
                const float s = clamp (x, -1.6f, 1.6f) * 0.625f;
                return std::fabs (s);
            }
            case DriveModel::PurestDrive:
                return nova::fx::airwindows::purestDrive (x, amount, awState);

            case DriveModel::Density:
                return nova::fx::airwindows::density (x, amount, sr, awState);

            case DriveModel::AirTape:
                return awState.tape.process (x, amount);

            case DriveModel::AirTube:
                return awState.tube.process (x, amount, sr);

            case DriveModel::Wavefolder:
                return nova::fx::daisysp::wavefolder (x * 0.4f) * 2.0f;

            case DriveModel::Decimator:
                return awState.decimator.process (x, amount);

            case DriveModel::DiodeWdf:
                return wdf.process (x * 0.5f);

            case DriveModel::Count:
                break;
        }
        return x;
    }

    struct LagState { float lag = 0.0f; };

    WdfClipper wdf;
    Fir4x<161, 1125, 68> up, down;

    LagState state;
    nova::fx::airwindows::DriveState awState;

    DriveModel model = DriveModel::Tanh;
    double sr = 48000.0;
    float amount = 0.35f, tone = 0.0f, mix = 1.0f;
    float preGain = 1.0f, preGainSm = 1.0f, postGainSm = 1.0f, outLevel = 1.0f;
    float toneSplit = 800.0f, toneCoef = 0.1f, gainCoef = 0.001f;
    float toneLowL = 0.0f, toneLowR = 0.0f;
    float dcCoef = 0.997f, dcX1L = 0.0f, dcY1L = 0.0f, dcX1R = 0.0f, dcY1R = 0.0f;
    float rmsCoef = 1.0e-4f, inRms = 0.0f, outRms = 0.0f;
};

} // namespace orogen
