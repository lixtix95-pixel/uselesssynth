// ДВИЖОК 7: Resynth — ресинтез записи синусами по спектральным кадрам.
//
// Что даёт независимость параметров, которой нет у Sample:
//   • Stretch  — длина во времени, тембр НЕ меняется (кадры идут медленнее);
//   • Pitch    — высота, длительность НЕ меняется (множитель частот партиалов);
//   • Tilt     — наклон спектра (яркость);
//   • Drift    — медленное «дыхание» высоты (расстройка партиалов в центах).
//
// Продакшн-детали, без которых это звучит как «орган из синусов»:
//   1. Частоты партиалов интерполируются между кадрами ЛИНЕЙНО, а фазы
//      интегрируются (phase += 2π·f/fs). Иначе при смене кадра фаза скачет
//      и слышен щелчок на каждой десятой миллисекунде.
//   2. Амплитуды сглаживаются однополюсником: партиал, появившийся в новом
//      кадре, входит плавно, исчезнувший — плавно уходит (без «моргания»).
//   3. Партиалы выше 0.45·fs после питч-шифта ОТБРАСЫВАЮТСЯ: синус выше
//      Найквиста заворачивается и даёт фальшивую гармонику.
//   4. Пики сопоставляются по индексу (кадры отсортированы по частоте),
//      поэтому партиал остаётся «своим» от кадра к кадру.
#pragma once

#include "engine.h"
#include "../content/resynth_data.h"

namespace orogen {

class ResynthEngine final : public EngineBase
{
public:
    static constexpr float kGainTrim = 0.50f;

    void setBank (const ResynthBank* b) noexcept { bank = b; }

    const char* name() const override { return "Resynth"; }
    float costFactor() const override { return 3.0f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        ampSmooth = (float) std::exp (-1.0 / (0.004 * sr));      // 4 мс
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        // фаза в ОБОРОТАХ (0..1) — так её ждёт fastSin (табличный синус).
        // В радианах он давал частоту в 2π раз выше и негармонический спектр
        // (измерено -17 дБ «алиасинга» на идеально чистом ресинтезе).
        for (int j = 0; j < kRsPartials; ++j) { phase[j] = rng.nextFloat(); amp[j] = 0.0f; }
        framePos = 0.0f;
        active = true;
        driftPhase = 0.0f;
        needSnap = true;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        if (bank == nullptr || bank->sources.empty()) return;
        src = &bank->sources[(size_t) clamp (p.table, 0, (int) bank->sources.size() - 1)];

        // p[0] — stretch (0.25x .. 4x), p[1] — питч (±24 полутонов),
        // p[2] — tilt (-1 .. +1), p[3] — drift (центов)
        const float stretchT = clamp (p.p[0], 0.0f, 1.0f);
        stretch = 0.25f + stretchT * 3.75f;
        pitchSemis = (clamp (p.p[1], 0.0f, 1.0f) - 0.5f) * 48.0f + p.tuneSemis;
        tilt = clamp (p.p[2], -1.0f, 1.0f);
        driftCents = clamp (p.p[3], 0.0f, 1.0f) * 25.0f;

        // нота относительно корневой: ресинтез транспонируется целиком
        pitchRatio = (float) std::pow (2.0, ((double) note - (double) src->rootNote + (double) pitchSemis) / 12.0);

        gain = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));

        if (needSnap) { framePos = 0.0f; needSnap = false; }
    }

    inline float render (const EngineParams&) override
    {
        if (! active || src == nullptr || src->frames.empty()) return 0.0f;

        const int nFrames = (int) src->frames.size();
        const int i0 = (int) framePos;
        if (i0 >= nFrames - 1) { active = false; return 0.0f; }
        const int i1 = i0 + 1;
        const float t = framePos - (float) i0;

        const PartialFrame& A = src->frames[(size_t) i0];
        const PartialFrame& B = src->frames[(size_t) i1];

        // drift: медленное «дыхание» высоты
        driftPhase += (float) kTwoPi * 0.7f / (float) sampleRate;
        if (driftPhase > (float) kTwoPi) driftPhase -= (float) kTwoPi;
        const float drift = 1.0f + driftCents * 0.0005776f * std::sin (driftPhase);   // 1 цент = 0.0578 % (точный: разность фаз копится секундами)

        const int count = std::max (A.count, B.count);

        // tilt — это pow(freq/1000, tilt) на каждый партиал каждый сэмпл.
        // Частоты партиалов меняются медленно (дрейф + ход по кадрам), поэтому
        // коэффициент пересчитывается раз в 16 сэмплов, а не 32 раза за сэмпл.
        if (--tiltCountdown <= 0)
        {
            tiltCountdown = 16;
            for (int j = 0; j < count; ++j)
            {
                const float fa = A.freq[j];
                const float fb = (B.freq[j] > 0.0f) ? B.freq[j] : A.freq[j];
                const float f = (fa + (fb - fa) * t) * pitchRatio * drift;
                tiltGain[(size_t) j] = (tilt == 0.0f) ? 1.0f
                                     : std::pow (std::max (f, 20.0f) / 1000.0f, tilt);
            }
        }
        const float nyqLimit = 0.45f * (float) sampleRate;
        const float inc = 1.0f / (float) sampleRate;      // Гц -> обороты за сэмпл

        float sum = 0.0f;
        for (int j = 0; j < count; ++j)
        {
            const float fa = A.freq[j];
            const float fb = (B.freq[j] > 0.0f) ? B.freq[j] : A.freq[j];
            const float aa = A.amp[j];
            const float ab = B.amp[j];

            const float freq = (fa + (fb - fa) * t) * pitchRatio * drift;
            if (freq <= 20.0f || freq >= nyqLimit) { amp[j] *= ampSmooth; continue; }

            // tilt: наклон спектра вокруг 1 кГц (коэффициент — из кэша)
            float a = (aa + (ab - aa) * t) * tiltGain[(size_t) j];

            amp[j] = amp[j] * ampSmooth + a * (1.0f - ampSmooth);      // сглаживание появления/исчезновения

            phase[j] += inc * freq;
            if (phase[j] >= 1.0f) phase[j] -= 1.0f;
            sum += amp[j] * fastSin (phase[j]);
        }

        // сдвиг кадров: чем больше stretch, тем медленнее идём по времени
        framePos += 1.0f / (float) (kRsHop * sampleRate * (double) stretch);

        const float out = sum * gain;
        const float y = out - dcX + dcCoef * dcY;
        dcX = out; dcY = y;
        return y;
    }

    // для тестов: позиция чтения кадров и признак активности
    float frameProgress (int nFrames) const noexcept
    {
        return (nFrames > 0) ? framePos / (float) nFrames : 0.0f;
    }
    bool isActive() const noexcept { return active; }
    float framesElapsed() const noexcept { return framePos; }

private:
    const ResynthBank* bank = nullptr;
    const ResynthSource* src = nullptr;

    double sampleRate = 48000.0;
    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true;

    float  framePos = 0.0f, stretch = 1.0f, pitchRatio = 1.0f, pitchSemis = 0.0f;
    float  tilt = 0.0f, driftCents = 0.0f, driftPhase = 0.0f;
    std::array<float, kRsPartials> tiltGain { };
    int    tiltCountdown = 0;
    float  gain = 1.0f, ampSmooth = 0.99f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;

    float  phase[kRsPartials] {};
    float  amp[kRsPartials] {};
    Rng    rng { 0x7E5A17u };
};

} // namespace orogen
