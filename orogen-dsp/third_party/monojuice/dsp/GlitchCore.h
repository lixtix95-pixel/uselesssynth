#pragma once

#include "FirmwarePrimitives.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(NOVA_GLITCH_USE_JUCE_RANDOM)
#include <JuceHeader.h>
#endif

namespace nova::fx {

struct GlitchSettings {
    float stutterChance = 0.0f;
    float stutterLengthMs = 80.0f;
    float foldAmount = 0.0f;
    float foldDrive = 0.0f;
    float dry = 0.0f;
    float wet = 1.0f;
    float repeats = 3.0f;
    float captureOffset = 100.0f;
    float reverseProbability = 0.0f;
    float crossfade = 100.0f;
    float tone = 24000.0f;
    float feedback = 0.0f;
    float pitchSemitones = 0.0f;
    float lengthSync = 0.0f;
    float lengthDivision = 3.0f;
    float toneType = 0.0f;
    float resonance = 0.0f;
};

// Common interface for slot effects. Implementations allocate state in prepare()
// and are expected to keep process() real-time safe.
class EffectProcessorBase {
public:
    virtual ~EffectProcessorBase() = default;
    virtual void prepare(double sampleRate, int maximumBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void set(const GlitchSettings&) noexcept = 0;
    virtual Frame process(Frame) noexcept = 0;
};

class GlitchCoreAudioProcessor final : public EffectProcessorBase {
public:
    void prepare(double sampleRate, int) override {
        sr = (std::isfinite(sampleRate) && sampleRate > 1000.0) ? sampleRate : 44100.0;
        // DAW-synced divisions reach eight beats. At high host rates the
        // complete 24-second loop is intentionally capped to protect RAM.
        const double historySeconds=sr>highRateLimit?highRateGlitchHistorySeconds:24.0;
        const auto required = static_cast<std::size_t>(std::ceil(sr * historySeconds)) + 2u;
        ring.assign(std::max<std::size_t>(64u, required), Frame{});
        parameterSmoothing = 1.0f - static_cast<float>(std::exp(-1.0 / (sr * 0.003)));
        // FOLD/DIST: пиковый конверт входа (attack ~1.5 мс, release ~120 мс) и DC-блок ~10 Гц.
        envelopeAttack = 1.0f - static_cast<float>(std::exp(-1.0 / (sr * 0.0015)));
        envelopeRelease = 1.0f - static_cast<float>(std::exp(-1.0 / (sr * 0.120)));
        dcCoefficient = 1.0f - static_cast<float>(std::exp(-6.2831853f * 10.0f / static_cast<float>(sr)));
        reset();
    }

    void reset() noexcept override {
        std::fill(ring.begin(), ring.end(), Frame{});
        writeIndex = 0;
        written = 0;
        stutterLoopStart = 0;
        stutterLoopLength = 1;
        stutterPlaybackLength = 1;
        stutterLoopPosition = 0;
        stutterLoopsRemaining = 0;
        stutterCrossfade = 1;
        stutterDirection = 1;
        triggerCountdown = 0;
        stuttering = false;
        fastEnvelope = 0.0f;
        slowEnvelope = 0.0f;
        feedbackState = {};
        distStateL = distStateR = foldStateL = foldStateR = ShapeState{};
        toneLeft = {};
        toneRight = {};
#if !defined(NOVA_GLITCH_USE_JUCE_RANDOM)
        randomState = 0x6d2b79f5u;
#endif
    }

    void set(const GlitchSettings& value) noexcept override {
        targetSettings.stutterChance = std::clamp(value.stutterChance, 0.0f, 1.0f);
        targetSettings.stutterLengthMs = std::clamp(value.stutterLengthMs, 1.0f, 500.0f);
        targetSettings.foldAmount = std::clamp(value.foldAmount, 0.0f, 10.0f);
        targetSettings.foldDrive = std::clamp(value.foldDrive, 0.0f, 20.0f);
        targetSettings.dry = std::clamp(value.dry, 0.0f, 1.0f);
        targetSettings.wet = std::clamp(value.wet, 0.0f, 1.0f);
        targetSettings.repeats = std::clamp(value.repeats, 1.0f, 16.0f);
        targetSettings.captureOffset = std::clamp(value.captureOffset, 0.0f, 100.0f);
        targetSettings.reverseProbability = std::clamp(value.reverseProbability, 0.0f, 100.0f);
        targetSettings.crossfade = std::clamp(value.crossfade, 0.0f, 100.0f);
        targetSettings.tone = std::clamp(value.tone, 200.0f, 24000.0f);
        targetSettings.feedback = std::clamp(value.feedback, 0.0f, 95.0f);
        targetSettings.pitchSemitones = std::clamp(value.pitchSemitones, -12.0f, 12.0f);
        targetSettings.lengthSync = std::clamp(value.lengthSync, 0.0f, 1.0f);
        targetSettings.lengthDivision = std::clamp(value.lengthDivision, 0.0f, 8.0f);
        targetSettings.toneType = std::clamp(value.toneType, 0.0f, 1.0f);
        targetSettings.resonance = std::clamp(value.resonance, 0.0f, 95.0f);
    }

    void setTempo(double bpm, bool) noexcept {
        if (std::isfinite(bpm) && bpm > 0.0)
            tempoBpm = static_cast<float>(std::clamp(bpm, 20.0, 400.0));
    }

    Frame process(Frame source) noexcept override {
        const Frame dryInput{clean(source.l), clean(source.r)};
        if (ring.empty())
            return dryInput;

        settings.stutterChance += (targetSettings.stutterChance - settings.stutterChance)
                                 * parameterSmoothing;
        settings.stutterLengthMs += (targetSettings.stutterLengthMs - settings.stutterLengthMs)
                                  * parameterSmoothing;
        settings.foldAmount += (targetSettings.foldAmount - settings.foldAmount)
                             * parameterSmoothing;
        settings.foldDrive += (targetSettings.foldDrive - settings.foldDrive)
                             * parameterSmoothing;
        settings.dry += (targetSettings.dry - settings.dry) * parameterSmoothing;
        settings.wet += (targetSettings.wet - settings.wet) * parameterSmoothing;
        settings.repeats += (targetSettings.repeats - settings.repeats) * parameterSmoothing;
        settings.captureOffset += (targetSettings.captureOffset - settings.captureOffset)
                                  * parameterSmoothing;
        settings.reverseProbability += (targetSettings.reverseProbability
                                        - settings.reverseProbability) * parameterSmoothing;
        settings.crossfade += (targetSettings.crossfade - settings.crossfade)
                              * parameterSmoothing;
        settings.tone += (targetSettings.tone - settings.tone) * parameterSmoothing;
        settings.feedback += (targetSettings.feedback - settings.feedback)
                               * parameterSmoothing;
        settings.pitchSemitones += (targetSettings.pitchSemitones - settings.pitchSemitones)
                                    * parameterSmoothing;
        settings.lengthSync += (targetSettings.lengthSync - settings.lengthSync)
                               * parameterSmoothing;
        settings.lengthDivision += (targetSettings.lengthDivision - settings.lengthDivision)
                                  * parameterSmoothing;
        settings.toneType += (targetSettings.toneType - settings.toneType)
                            * parameterSmoothing;
        settings.resonance += (targetSettings.resonance - settings.resonance)
                             * parameterSmoothing;

        const float feedbackAmount = settings.feedback * 0.01f;
        const Frame input{
            clean(dryInput.l + feedbackState.l * feedbackAmount),
            clean(dryInput.r + feedbackState.r * feedbackAmount)
        };

        ring[writeIndex] = input;
        const int available = std::min(written + 1, static_cast<int>(ring.size()));
        float loopLengthMs = settings.stutterLengthMs;
        if (settings.lengthSync >= 0.5f) {
            constexpr float divisionBeats[9]{8.0f, 4.0f, 2.0f, 1.0f, .5f, .25f,
                                             .125f, .0625f, .03125f};
            const int division = std::clamp(static_cast<int>(std::lround(settings.lengthDivision)), 0, 8);
            loopLengthMs = 60000.0f / static_cast<float>(tempoBpm) * divisionBeats[division];
        }
        const int loopLength = std::clamp(
            static_cast<int>(std::lround(loopLengthMs * 0.001 * sr)),
            1, static_cast<int>(ring.size()) - 1);

        const float level = 0.5f * (std::abs(input.l) + std::abs(input.r));
        fastEnvelope += (level - fastEnvelope) * 0.22f;
        slowEnvelope += (level - slowEnvelope) * 0.004f;
        const bool transient = level > 0.015f && fastEnvelope > slowEnvelope * 1.35f + 0.004f;

        if (!stuttering) {
            if (triggerCountdown > 0)
                --triggerCountdown;
            if (triggerCountdown <= 0 && available >= loopLength
                && settings.stutterChance > 0.0f) {
                // Transients make the burst likely; the low background
                // probability keeps a sustained tone capable of glitching too.
                const float chance = transient
                    ? std::min(1.0f, settings.stutterChance * 1.5f)
                    : settings.stutterChance * 0.05f;
                triggerCountdown = std::max(32, loopLength / 4);
                if (randomFloat() < chance) {
                    const int maxOffset = std::min(loopLength / 2, available - loopLength);
                    const int allowedOffset = static_cast<int>(std::lround(
                        float(std::max(0, maxOffset)) * settings.captureOffset * 0.01f));
                    const int offset = allowedOffset > 0
                        ? static_cast<int>(randomFloat() * float(allowedOffset + 1)) : 0;
                    stutterLoopStart = (writeIndex + static_cast<int>(ring.size())
                                        - loopLength + 1 - offset)
                                       % static_cast<int>(ring.size());
                    stutterLoopLength = loopLength;
                    stutterPlaybackLength = loopLength;
                    stutterLoopPosition = 0;
                    const int maximumCrossfade = std::min(64, std::max(1, std::min(stutterLoopLength, stutterPlaybackLength) / 8));
                    stutterCrossfade = static_cast<int>(std::lround(
                        float(maximumCrossfade) * settings.crossfade * 0.01f));
                    stutterDirection = randomFloat() < settings.reverseProbability * 0.01f ? -1 : 1;
                    stutterLoopsRemaining = std::clamp(
                        static_cast<int>(std::lround(settings.repeats)), 1, 16);
                    stuttering = true;
                }
            }
        }

        Frame shuffled = input;
        if (stuttering) {
            const float pitchRatio = std::pow(2.0f, settings.pitchSemitones / 12.0f);
            const float sourcePhase = std::fmod(
                float(stutterLoopPosition) * pitchRatio,
                float(std::max(1, stutterLoopLength)));
            const float sourcePosition = stutterDirection > 0
                ? sourcePhase : float(stutterLoopLength - 1) - sourcePhase;
            Frame grain = ringAtInterpolated(stutterLoopStart + sourcePosition);

            // Crossfade both sides of the playback cycle to avoid hard seams.
            const int seam = std::clamp(stutterCrossfade, 0,
                                        std::min(stutterLoopLength, stutterPlaybackLength) / 2);
            if (seam > 0 && stutterLoopPosition < seam) {
                const int tailPosition = stutterDirection > 0
                    ? stutterLoopLength - seam + stutterLoopPosition
                    : seam - 1 - stutterLoopPosition;
                const float blend = float(stutterLoopPosition + 1) / float(seam + 1);
                grain = mix(ringAt(stutterLoopStart + tailPosition), grain, blend);
            } else if (seam > 0 && stutterLoopPosition >= stutterPlaybackLength - seam) {
                const int headPosition = stutterLoopPosition - (stutterPlaybackLength - seam);
                const int sourceStart = stutterDirection > 0 ? headPosition : stutterLoopLength - 1 - headPosition;
                const float blend = float(headPosition + 1) / float(seam + 1);
                grain = mix(grain, ringAt(stutterLoopStart + sourceStart), blend);
            }

            shuffled = grain;

            if (stutterLoopsRemaining == 1 && seam > 0
                && stutterLoopPosition >= stutterPlaybackLength - seam) {
                const float liveBlend = float(stutterLoopPosition
                                               - (stutterPlaybackLength - seam) + 1)
                                      / float(seam);
                shuffled = mix(shuffled, input,
                               std::clamp(liveBlend, 0.0f, 1.0f));
            }

            if (++stutterLoopPosition >= stutterPlaybackLength) {
                stutterLoopPosition = 0;
                if (--stutterLoopsRemaining <= 0)
                    stuttering = false;
            }
        }

        writeIndex = (writeIndex + 1) % static_cast<int>(ring.size());
        written = std::min(written + 1, static_cast<int>(ring.size()));

        Frame toned = shuffled;
        const float nyquistCutoff = std::max(200.0f, static_cast<float>(sr * 0.49));
        // The maximum TONE value is the open end of the control in both modes.
        // Treat it as bypass so switching to HP cannot mute the default sound.
        if (settings.tone < nyquistCutoff) {
            const float cutoff = std::clamp(settings.tone, 200.0f, nyquistCutoff);
            const auto coefficients = toneCoefficients(cutoff);
            toned = {toneFilter(shuffled.l, toneLeft, coefficients),
                     toneFilter(shuffled.r, toneRight, coefficients)};
        }

        // Ручки 0..20 (DIST) и 0..10 (FOLD) задают теперь ХАРАКТЕР искажения, а не громкость:
        // вход шейперов нормируется пиковым конвертом, поэтому эффект одинаково работает
        // на тихом и на громком материале, а уровень на выходе не «прыгает» с ручкой.
        const float distAmount=std::clamp(settings.foldDrive,0.0f,20.0f);
        const Frame distorted{distortShape(toned.l,distAmount,distStateL),
                              distortShape(toned.r,distAmount,distStateR)};
        const float foldAmount=std::clamp(settings.foldAmount,0.0f,10.0f);
        const Frame processed{foldShape(distorted.l,foldAmount,foldStateL),
                              foldShape(distorted.r,foldAmount,foldStateR)};
        // Страховка как в Rack::process (returned = clamp(out, -4, 4)): что бы ни случилось
        // с ручками и LFO, петля обратной связи не может уйти в бесконечность.
        feedbackState = {std::clamp(processed.l, -4.0f, 4.0f),
                         std::clamp(processed.r, -4.0f, 4.0f)};
        return dryInput * settings.dry + processed * settings.wet;
    }

private:
    Frame ringAt(int index) const noexcept {
        const int size = static_cast<int>(ring.size());
        index %= size;
        if (index < 0)
            index += size;
        return ring[static_cast<std::size_t>(index)];
    }

    Frame ringAtInterpolated(float index) const noexcept {
        const int base = static_cast<int>(std::floor(index));
        return mix(ringAt(base), ringAt(base + 1), index - float(base));
    }

    float randomFloat() noexcept {
#if defined(NOVA_GLITCH_USE_JUCE_RANDOM)
        return random.nextFloat();
#else
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        return static_cast<float>(randomState >> 8) / 16777216.0f;
#endif
    }

    struct ToneState {float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;};
    struct ToneCoefficients {float b0=0.f,b1=0.f,b2=0.f,a1=0.f,a2=0.f;};

    ToneCoefficients toneCoefficients(float cutoff) const noexcept {
        const float omega = 6.283185307179586f * cutoff / static_cast<float>(sr);
        const float cosine = std::cos(omega);
        const float sine = std::sin(omega);
        const float q = 0.5f + 11.5f * std::clamp(settings.resonance, 0.0f, 95.0f) / 95.0f;
        const float alpha = sine / (2.0f * q);
        const float a0 = 1.0f + alpha;
        const bool highpass = settings.toneType >= 0.5f;
        const float b0 = (highpass ? 1.0f + cosine : 1.0f - cosine) * 0.5f / a0;
        const float b1 = (highpass ? -(1.0f + cosine) : 1.0f - cosine) / a0;
        const float b2 = b0;
        const float a1 = -2.0f * cosine / a0;
        const float a2 = (1.0f - alpha) / a0;
        return {b0,b1,b2,a1,a2};
    }

    float toneFilter(float input, ToneState& state, const ToneCoefficients& c) const noexcept {
        const float output = c.b0 * input + c.b1 * state.x1 + c.b2 * state.x2
                           - c.a1 * state.y1 - c.a2 * state.y2;
        state.x2 = state.x1;
        state.x1 = input;
        state.y2 = state.y1;
        state.y1 = std::isfinite(output) ? output : 0.0f;
        return state.y1;
    }

    //--------------------------------------------------------------------------
    // FOLD / DIST.
    // Что изменилось против первой версии и почему (подробности — в PATCH_FOLD_DIST.md):
    //   1) вход нормируется пиковым конвертом -> ручка управляет характером, а не громкостью;
    //   2) добавлен ADAA (antiderivative anti-aliasing) — «песок»/алиасинг вместо звука
    //      больше не появляется уже на нескольких процентах хода ручки;
    //   3) у DIST появился DC-блок (асимметричный tanh давал постоянное смещение до -0.2),
    //      из-за которого фолдер складывал сигнал несимметрично и щёлкал при движении ручки;
    //   4) убраны «подъёмы» уровня: раньше FOLD давал до +9.6 дБ, а DIST до +16.4 дБ.
    //--------------------------------------------------------------------------
    struct ShapeState {
        float env = 0.0f;       // пиковый конверт входа шейпера
        bool  envReady = false;
        float previous = 0.0f;  // предыдущий вход шейпера (для ADAA)
        bool  previousReady = false;
        float dc = 0.0f;        // состояние DC-блока (DIST)
    };

    float trackEnvelope(float input, ShapeState& state) noexcept {
        const float magnitude = std::abs(input);
        if (!state.envReady) { state.env = magnitude; state.envReady = true; }
        else state.env += (magnitude - state.env)
                         * (magnitude > state.env ? envelopeAttack : envelopeRelease);
        // Верхний предел 1.0 — не косметика, а условие устойчивости: выход шейперов
        // нормируется конвертом, и без абсолютного предела петля FEEDBACK (до 95 %)
        // вместе с резонансным TONE-фильтром (Q до 12) могла уходить в разнос.
        // Нижний предел 1e-4 — чтобы в тишине не поднимался шум.
        return std::clamp(state.env, 1.0e-4f, 1.0f);
    }

    // Треугольный фолдинг (точная форма) и его первообразная для ADAA.
    static float triangleFold(float u) noexcept {
        float p = u + 1.0f; p -= 4.0f * std::floor(p * 0.25f);
        float v = p - 1.0f;
        if (v > 2.0f) v = 4.0f - v;
        return v - 1.0f;
    }
    static float triangleFoldIntegral(float u) noexcept {
        float p = u + 1.0f; p -= 4.0f * std::floor(p * 0.25f);
        const float v = p - 1.0f;
        return v <= 1.0f ? 0.5f * v * v : 0.5f + 2.0f * (v - 1.0f) - 0.5f * (v * v - 1.0f);
    }
    // log(cosh(u)) для ADAA-версии tanh; при |u|>10 работаем асимптотикой (экономим exp/log).
    static float logCosh(float u) noexcept {
        const float a = std::abs(u);
        if (a > 10.0f) return a - 0.6931472f;
        return a - 0.6931472f + std::log1p(std::exp(-2.0f * a));
    }

    // amount 0..10. Нулевое значение — честный bypass.
    float foldShape(float input, float amount, ShapeState& state) noexcept {
        if (amount <= 0.0f) { state.previous = input; state.previousReady = true; return input; }
        const float norm = trackEnvelope(input, state);
        const float drive = 1.0f + 2.0f * amount;
        const float u = input / norm * drive;
        const float previous = state.previousReady ? state.previous : u;
        const float delta = u - previous;
        const float shaped = std::abs(delta) > 1.0e-5f
            ? (triangleFoldIntegral(u) - triangleFoldIntegral(previous)) / delta
            : triangleFold(u);
        state.previous = u; state.previousReady = true;
        // Выход треугольного фолдера ограничен +-1, поэтому результат не превышает
        // недавний пик входа: петля FEEDBACK (до 95 %) остаётся устойчивой.
        // Отдельного makeup-трима здесь нет намеренно — он бы давал усиление > 1
        // в замкнутой петле (проверено фаззингом: 240 комбинаций параметров).
        return shaped * norm;
    }

    // amount 0..20 (ручка DIST). Нулевое значение — честный bypass.
    float distortShape(float input, float amount, ShapeState& state) noexcept {
        if (amount <= 0.0f) { state.previous = input; state.previousReady = true; state.dc = 0.0f; return input; }
        const float normalised = amount / 20.0f;
        const float norm = trackEnvelope(input, state);
        const float gain = 1.0f + 40.0f * normalised;
        const float bias = -(0.02f + 0.16f * normalised);
        const float u = input / norm * gain + bias;
        const float previous = state.previousReady ? state.previous : u;
        const float delta = u - previous;
        const float shaped = std::abs(delta) > 1.0e-5f
            ? (logCosh(u) - logCosh(previous)) / delta
            : std::tanh(u);
        state.previous = u; state.previousReady = true;
        float output = input + normalised * (shaped * norm - input);          // dry -> shaped
        state.dc += (output - state.dc) * dcCoefficient;                      // убираем смещение
        output -= state.dc;
        return output / (1.0f + 0.46f * normalised + 0.20f * normalised * normalised);
    }

    double sr = 44100.0;
    GlitchSettings settings{};
    GlitchSettings targetSettings{};
    float parameterSmoothing = 0.05f;
    float envelopeAttack = 0.01f, envelopeRelease = 0.001f, dcCoefficient = 0.001f;
    Frame feedbackState{};
    ShapeState distStateL{}, distStateR{}, foldStateL{}, foldStateR{};
    ToneState toneLeft{}, toneRight{};
    std::vector<Frame> ring;
    int writeIndex = 0;
    int written = 0;
    int stutterLoopStart = 0;
    int stutterLoopLength = 1;
    int stutterPlaybackLength = 1;
    int stutterLoopPosition = 0;
    int stutterLoopsRemaining = 0;
    int stutterCrossfade = 2;
    int stutterDirection = 1;
    int triggerCountdown = 0;
    bool stuttering = false;
    float fastEnvelope = 0.0f;
    float slowEnvelope = 0.0f;
    float tempoBpm = 120.0f;
#if defined(NOVA_GLITCH_USE_JUCE_RANDOM)
    juce::Random random;
#else
    std::uint32_t randomState = 0x6d2b79f5u;
#endif
};

} // namespace nova::fx
