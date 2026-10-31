// Цепочка эффектов инструмента:
//   Drive (19 моделей) → COMB (ODIN2 / ParallelComb) → Chorus → Ping-pong Delay →
//   Reverb → Plate → EQ → Material → Spectre → Glitch → Compressor → Limiter.
// Основной тракт готовится в prepare(), копии — в ensureCopy() вне audio;
// в аудиоколбэке нет выделений/удалений и ожидания потоков.
//
// Drive, Material, Spectre и Glitch перенесены из плагина MonoJuice 0.9.5
// (см. FX-MONOJUICE.md): Drive — 12 моделей автора плагина + Airwindows
// (MIT) + DaisySP (MIT) + наш WDF; Material — модальный резонатор;
// Spectre — MDCT-маска (BSD-3, Spectrum Pixelator); Glitch — статтер;
// COMB — две модели комб-фильтров (GPL-3.0), Chorus — прошивочное ядро OS 1.32B
// (как альтернатива нашему простому хорусу: chorusModel = 1).
//
// Лицензия проекта — GPL-3.0-or-later (см. LICENSE и NOTICE.md): в цепочке
// есть GPL-3.0 DSP (COMB — Odin 2 и Uhhyou ParallelComb).
#pragma once

#include "../core/dsp.h"
#include "../core/svf.h"
#include "fx_plate.h"
#include "fx_wdf.h"
#include "fx_drive.h"
#include "fx_material.h"
#include "fx_spectre.h"
#include "fx_spectre_classic.h"
#include "fx_glitch.h"
#include "fx_robin.h"
#include "fx_comb.h"
#include "fx_chorus_fw.h"

#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>

namespace orogen {

// 1. Shaper: drive + асимметричный tanh, с компенсацией уровня
class Shaper
{
public:
    void prepare (double sr) { sampleRate = sr; gainSm.reset (sr, 15.0); gainSm.snap (1.0f); }
    void setDrive (float d) { drive = clamp (d, 0.0f, 1.0f); }
    void setMix (float m)  { mix = clamp (m, 0.0f, 1.0f); }

    inline float process (float x) noexcept
    {
        if (drive < 1.0e-4f || mix < 1.0e-4f) return x;
        const float g = 1.0f + drive * 11.0f;
        const float comp = 1.0f / (1.0f + drive * 1.7f);
        const float wet = fastTanh (x * g) * comp;
        return x * (1.0f - mix) + wet * mix;
    }

private:
    SmoothedValue gainSm;
    double sampleRate = 48000.0;
    float drive = 0.0f, mix = 1.0f;
};

// 2. Chorus: две модулированные линии задержки, противофаза по каналам
class Chorus
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        const int maxD = (int) (sr * 0.05) + 4;
        bufL.assign ((size_t) maxD, 0.0f);
        bufR.assign ((size_t) maxD, 0.0f);
        write = 0;
        phase = 0.0f;
        updateLowpass (sr);
    }

    void setDepth (float d) { depth = clamp (d, 0.0f, 1.0f); }
    void setRate (float r)  { rateHz = clamp (r, 0.02f, 8.0f); }
    void setMix (float m)   { mix = clamp (m, 0.0f, 1.0f); }
    // остальные ручки — те же имена и шкала 0…127, что у прошивочного
    // хоруса, чтобы обе модели управлялись одинаково
    // Отсчётные точки — значения прошивки по умолчанию (DEL 40, FB 30, WID 80,
    // LP 100, INP 100): там простой хорус звучит ровно как раньше (12 мс,
    // фидбэк 0.15, без разведения каналов и без фильтра), поэтому пресеты
    // не поехали.
    void setDelay (float v)    { delayMs = 12.0f + (clamp (v, 0.0f, 127.0f) - 40.0f) / 127.0f * 32.0f; }
    void setFeedback (float v) { feedback = clamp (0.15f + (clamp (v, 0.0f, 127.0f) - 30.0f) / 127.0f * 0.6f, 0.0f, 0.85f); }
    void setWidth (float v)    { spreadMs = std::max (0.0f, 80.0f - clamp (v, 0.0f, 127.0f)) / 80.0f * 2.0f; }
    void setLowpass (float v)
    {
        // LP 100 и выше — фильтра нет (как до расширения), ниже — срез вниз:
        // 99 ≈ 20 кГц, 0 ≈ 900 Гц
        const float lp = clamp (v, 0.0f, 127.0f);
        lpHz = 20000.0f * std::pow (0.02f, (100.0f - lp) / 127.0f);
        lpBypass = lp >= 100.0f;
        updateLowpass (sampleRate);
    }
    void setInput (float v)    { inputGain = std::pow (8.0f, (clamp (v, 0.0f, 127.0f) - 100.0f) / 100.0f); }

    inline void process (float& l, float& r) noexcept
    {
        if (mix < 1.0e-4f || bufL.empty()) return;

        const float base = (float) sampleRate * (delayMs * 0.001f);
        const float mod  = (float) sampleRate * 0.004f * depth;
        const float s1 = fastSin (phase);
        const float s2 = fastSin (phase + 0.5f);

        const float dL = base + mod * (0.5f + 0.5f * s1);
        const float dR = base + mod * (0.5f + 0.5f * s2);

        // WID разводит каналы по задержке, LP — фильтр в петле обратной связи
        const float spread = spreadMs * 0.001f * (float) sampleRate;
        lpL += lpA * (readInterp (bufL, dL) - lpL);
        lpR += lpA * (readInterp (bufR, dR + spread) - lpR);
        const float yl = lpL, yr = lpR;

        const float inL = l * inputGain, inR = r * inputGain;
        bufL[(size_t) write] = inL + yl * feedback;
        bufR[(size_t) write] = inR + yr * feedback;
        if (++write >= (int) bufL.size()) write = 0;

        phase += rateHz / (float) sampleRate;
        if (phase >= 1.0f) phase -= 1.0f;

        l = inL * (1.0f - mix * 0.5f) + yl * mix;
        r = inR * (1.0f - mix * 0.5f) + yr * mix;
    }

private:
    // однополюсный ФНЧ в петле: 1 - exp(-2*pi*fc/sr)
    void updateLowpass (double sr) noexcept
    {
        if (lpBypass) { lpA = 1.0f; return; }       // фильтр выключен: y = x
        const float fc = clamp (lpHz, 20.0f, (float) sr * 0.45f);
        lpA = 1.0f - std::exp (-6.2831853f * fc / (float) sr);
    }

    float readInterp (const std::vector<float>& b, float delay) const noexcept
    {
        float pos = (float) write - delay;
        while (pos < 0.0f) pos += (float) b.size();
        const int i0 = (int) pos;
        const float f = pos - (float) i0;
        const int i1 = (i0 + 1) % (int) b.size();
        return lerp (b[(size_t) i0], b[(size_t) i1], f);
    }

    std::vector<float> bufL, bufR;
    double sampleRate = 48000.0;
    int write = 0;
    float phase = 0.0f, depth = 0.5f, rateHz = 0.35f, mix = 0.0f;
    float delayMs = 12.0f, feedback = 0.15f, spreadMs = 0.0f, inputGain = 1.0f;
    float lpHz = 12000.0f, lpA = 0.5f, lpL = 0.0f, lpR = 0.0f;
    bool  lpBypass = false;
};

// 3. Ping-pong delay с ФНЧ в петле (иначе хвост «шипит»)
class PingPongDelay
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        const int maxD = (int) (sr * 2.2) + 4;
        bufL.assign ((size_t) maxD, 0.0f);
        bufR.assign ((size_t) maxD, 0.0f);
        write = 0;
        lpL = lpR = 0.0f;
        fadeLength = std::max (1, (int) (sr * 0.020));
        requestedDelay = delayA = delayB = (int) (timeSec * (float) sampleRate);
        fadePosition = 0; fading = false; processed = false;
        mixSm.reset (sr, 8.0); mixSm.snap (mix);
        fbSm.reset (sr, 8.0); fbSm.snap (fb);
    }

    void setTime (float sec)
    {
        timeSec = clamp (sec, 0.02f, 2.0f);
        requestedDelay = (int) (timeSec * (float) sampleRate);
        if (! processed) { delayA = delayB = requestedDelay; fading = false; }
    }

    // Диапазон совпадает с параметром хоста (0…0.95); петля остаётся < 1.
    void setFeedback (float f)
    {
        fb = clamp (f, 0.0f, 0.95f);
        if (! processed) fbSm.snap (fb); else fbSm.setTarget (fb);
    }

    void setMix (float m)
    {
        mix = clamp (m, 0.0f, 1.0f);
        if (! processed) mixSm.snap (mix); else mixSm.setTarget (mix);
    }

    inline void process (float& l, float& r) noexcept
    {
        if (bufL.empty()) return;
        processed = true;

        // Время линии меняется между двумя головками за 20 мс. Если хост
        // снова меняет Time во время перехода, запоминаем последнюю цель и
        // начинаем следующий переход ПОСЛЕ текущего, без резкого перескока.
        if (! fading && requestedDelay != delayA)
        {
            delayB = requestedDelay; fadePosition = 0; fading = true;
        }
        const auto read = [this] (int d, float& left, float& right) noexcept {
            int rd = write - d; if (rd < 0) rd += (int) bufL.size();
            const int rd2 = (rd + d / 2) % (int) bufL.size();
            left = bufL[(size_t) rd]; right = bufR[(size_t) rd2];
        };
        float dl = 0.0f, dr = 0.0f;
        read (delayA, dl, dr);
        if (fading)
        {
            float nextL = 0.0f, nextR = 0.0f;
            read (delayB, nextL, nextR);
            const float w = (float) fadePosition / (float) fadeLength;
            dl = lerp (dl, nextL, w); dr = lerp (dr, nextR, w);
            if (++fadePosition >= fadeLength) { delayA = delayB; fading = false; }
        }

        // Mix=0 отключает лишь мокрый выход, а не движение линии: иначе вход
        // терялся, а старое эхо «замораживалось» и оживало после включения.
        const float wet = mixSm.next(), feedback = fbSm.next();
        lpL += 0.35f * (dr - lpL);
        lpR += 0.35f * (dl - lpR);
        // Экспоненциальный хвост не должен уходить в субнормальные float.
        if (std::fabs (lpL) < 1.0e-20f) lpL = 0.0f;
        if (std::fabs (lpR) < 1.0e-20f) lpR = 0.0f;
        bufL[(size_t) write] = l + lpL * feedback;
        bufR[(size_t) write] = r + lpR * feedback;
        if (++write >= (int) bufL.size()) write = 0;

        l += dl * wet;
        r += dr * wet;
    }

private:
    std::vector<float> bufL, bufR;
    double sampleRate = 48000.0;
    int write = 0, requestedDelay = 0, delayA = 0, delayB = 0;
    int fadeLength = 1, fadePosition = 0;
    bool fading = false, processed = false;
    float timeSec = 0.33f, fb = 0.4f, mix = 0.0f, lpL = 0.0f, lpR = 0.0f;
    SmoothedValue mixSm, fbSm;
};

// 4. Reverb: 4 гребёнки + 2 всепропускающих на канал (Freeverb-схема),
// взаимно простые длины — гребёнки не «сходятся» в один резонанс.
class Reverb
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        const float k = (float) (sr / 44100.0);
        static const int combTune[4]  = { 1116, 1188, 1277, 1356 };
        static const int apTune[2]    = { 556, 441 };
        static const int spreadL[4]   = { 0, 0, 0, 0 };
        static const int spreadR[4]   = { 23, 23, 23, 23 };

        for (int i = 0; i < 4; ++i)
        {
            combL[(size_t) i].assign ((size_t) (k * (float) (combTune[i] + spreadL[i])) + 2, 0.0f);
            combR[(size_t) i].assign ((size_t) (k * (float) (combTune[i] + spreadR[i])) + 2, 0.0f);
            idxL[(size_t) i] = idxR[(size_t) i] = 0;
            storeL[(size_t) i] = storeR[(size_t) i] = 0.0f;
        }
        for (int i = 0; i < 2; ++i)
        {
            apL[(size_t) i].assign ((size_t) (k * (float) apTune[i]) + 2, 0.0f);
            apR[(size_t) i].assign ((size_t) (k * (float) (apTune[i] + 23)) + 2, 0.0f);
            apIdxL[(size_t) i] = apIdxR[(size_t) i] = 0;
        }
    }

    void setSize (float s)  { size = clamp (s, 0.1f, 1.0f); }
    void setDamp (float d)  { damp = clamp (d, 0.0f, 0.95f); }
    void setMix (float m)   { mix = clamp (m, 0.0f, 1.0f); }

    inline void process (float& l, float& r) noexcept
    {
        if (mix < 1.0e-4f || combL[0].empty()) return;

        const float fb = 0.70f + 0.27f * size;
        const float d1 = damp * 0.6f, d2 = 1.0f - d1;
        const float input = (l + r) * 0.5f * 0.03f;

        float accL = 0.0f, accR = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            auto& bl = combL[(size_t) i];
            float& yl = storeL[(size_t) i];
            const float outL = bl[(size_t) idxL[(size_t) i]];
            yl = outL * d2 + yl * d1;
            bl[(size_t) idxL[(size_t) i]] = input + yl * fb;
            if (++idxL[(size_t) i] >= (int) bl.size()) idxL[(size_t) i] = 0;
            accL += outL;

            auto& br = combR[(size_t) i];
            float& yr = storeR[(size_t) i];
            const float outR = br[(size_t) idxR[(size_t) i]];
            yr = outR * d2 + yr * d1;
            br[(size_t) idxR[(size_t) i]] = input + yr * fb;
            if (++idxR[(size_t) i] >= (int) br.size()) idxR[(size_t) i] = 0;
            accR += outR;
        }

        for (int i = 0; i < 2; ++i)
        {
            accL = allpass (apL[(size_t) i], apIdxL[(size_t) i], accL, 0.5f);
            accR = allpass (apR[(size_t) i], apIdxR[(size_t) i], accR, 0.5f);
        }

        l = l * (1.0f - mix * 0.35f) + accL * mix * 1.6f;
        r = r * (1.0f - mix * 0.35f) + accR * mix * 1.6f;
    }

private:
    static inline float allpass (std::vector<float>& b, int& idx, float x, float g) noexcept
    {
        const float d = b[(size_t) idx];
        const float y = -x + d;
        b[(size_t) idx] = x + d * g;
        if (++idx >= (int) b.size()) idx = 0;
        return y;
    }

    std::vector<float> combL[4], combR[4], apL[2], apR[2];
    int idxL[4] {}, idxR[4] {}, apIdxL[2] {}, apIdxR[2] {};
    float storeL[4] {}, storeR[4] {};
    double sampleRate = 48000.0;
    float size = 0.6f, damp = 0.4f, mix = 0.0f;
};

// 5. EQ: полка низ / полка верх / середина (две полки на SVF)
class Eq
{
public:
    void prepare (double sr) { sampleRate = sr; lowSvf.setSampleRate (sr); highSvf.setSampleRate (sr); }
    void setLow (float db)  { lowGain  = db; }
    void setHigh (float db) { highGain = db; }

    inline void process (float& l, float& r) noexcept
    {
        if (std::fabs (lowGain) < 0.1f && std::fabs (highGain) < 0.1f) return;
        lowSvf.setParams (220.0f, 0.1f, 0.0f);
        highSvf.setParams (3500.0f, 0.1f, 0.0f);
        const float gl = dbToLin (lowGain), gh = dbToLin (highGain);

        const float lowL  = lowSvf.process (l, FilterType::LP12);
        const float lowR  = lowSvf.process (r, FilterType::LP12);
        const float highL = highSvf.process (l, FilterType::HP24);
        const float highR = highSvf.process (r, FilterType::HP24);
        l = (l - lowL - highL) + lowL * gl + highL * gh;
        r = (r - lowR - highR) + lowR * gl + highR * gh;
    }

private:
    static float dbToLin (float db) noexcept { return fastDbToLin (db); }
    Svf lowSvf, highSvf;
    double sampleRate = 48000.0;
    float lowGain = 0.0f, highGain = 0.0f;
};

// 6. Компрессор (пиковый детектор, мягкое колено) + 7. лимитер на выходе
class Dynamics
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        attCoef = 1.0f - std::exp (-1.0f / (float) (0.003 * sr));
        relCoef = 1.0f - std::exp (-1.0f / (float) (0.120 * sr));
        env = 1.0f;
    }

    void setThreshold (float db) { threshold = db; }
    void setRatio (float rr)     { ratio = std::max (1.0f, rr); }
    void setMakeup (float db)    { makeup = db; }

    inline void process (float& l, float& r) noexcept
    {
        const float peak = std::max (std::fabs (l), std::fabs (r));
        const float coef = (peak > env) ? attCoef : relCoef;
        env += (peak - env) * coef;

        // потолок 4·FS и пол 1e-7: без него один выброс на входе «замораживал»
        // компрессор на секунды с ослаблением −120 дБ (слышимая тишина)
        env = clamp (env, 1.0e-7f, 4.0f);
        // fastDb вместо 20·log10: libm зовётся каждый сэмпл, ошибка 0.001 дБ
        const float envDb = clamp (fastDb (env + 1.0e-9f), -120.0f, 12.0f);
        const float over = envDb - threshold;
        float grDb = 0.0f;
        if (over > 0.0f) grDb = -over * (1.0f - 1.0f / ratio);
        grDb = clamp (grDb, -36.0f, 0.0f);

        const float g = fastDbToLin (grDb + makeup);   // было std::pow(10, db/20)
        l *= g; r *= g;
    }

    // Пиковая стереозащита. Ниже потолка сигнал неизменён; выше оба канала
    // масштабируются одним коэффициентом до прежнего потолка. Старое условное
    // tanh-колено было разрывным: 0.97999 -> 0.97999, 0.98001 -> 0.77458,
    // и одновременно меняло соотношение каналов. Это исправление самого FX,
    // а не снижение громкости или сокрытие проблем движка лимитером.
    static inline void limit (float& l, float& r, float ceiling) noexcept
    {
        const float peak = std::max (std::fabs (l), std::fabs (r));
        if (peak > ceiling)
        {
            const float k = ceiling / peak;
            l *= k;
            r *= k;
        }
    }

private:
    double sampleRate = 48000.0;
    float attCoef = 0.01f, relCoef = 0.001f, env = 1.0f;
    float threshold = -8.0f, ratio = 3.0f, makeup = 2.0f;
};

// Полная цепочка. Порядок как у «мастер-шины»: сначала окрас, потом пространство.
class FxChain
{
public:
    // The order is the actual signal path, not a user-reorderable rack.
    enum Module { Drive = 0, Comb, ChorusModule, DelayModule, ReverbModule,
                  Plate, Equalizer, Material, Spectre, Robin, Glitch,
                  Compressor, StereoWidth, ModuleCount };

    struct Settings
    {
        // Keep standalone DSP and legacy JSON behaviour: historically every
        // stage was present (many had mix=0). Plugin INIT explicitly clears it.
        std::array<bool, ModuleCount> enabled {{ true, true, true, true, true, true,
                                                 true, true, true, true, true, true, true }};
        float drive = 0.0f;
        float driveMix = 1.0f;                  // legacy drive was fully wet
        // Appended controls use sentinel defaults for exact old-project replay.
        // The original host parameter ranges/IDs remain untouched.
        int chorusMode = 0;                    // 0 legacy, 1 V1, 2 V2
        int glitchTimingMode = 0;              // 0 legacy, 1 actual sample rate
        bool glitchSync = false, glitchHighpass = false;
        int glitchDivision = 3;
        float combOdinMix = 1.0f, combDryDbExt = -61.0f, combWetDbExt = -61.0f;
        float combDriveExt = -1.0f, combOutputExt = -31.0f;
        float combDepthExt = -1.0f, combRateExt = -1.0f, combEnvelopeExt = -37.0f;
        float combFreqAExt = -1.0f, combFreqBExt = -1.0f, combFeedbackExt = -1.0f;
        float combHighpassExt = -1.0f, combTuneExt = -49.0f;
        float spectreReject = -120.0f, spectreAttack = 0.0f, spectreRelease = 0.0f;
        float spectreWindowExt = -1.0f, spectrePeaksExt = -1.0f, spectreWidthExt = -1.0f;
        float spectreLowExt = -1.0f, spectreHighExt = -1.0f;
        float glitchFoldDrive = 0.0f, glitchDryLevel = -1.0f, glitchWetLevel = -1.0f;
        float glitchOffset = 100.0f, glitchCrossfade = 100.0f, glitchResonance = 0.0f;
        float glitchDrift = 0.0f, glitchPitchStep = 0.0f, glitchPitchRandom = 0.0f;
        float glitchReroll = 0.0f, glitchDecay = 0.0f, glitchLengthExt = 0.0f;
        float chorusMix = 0.0f, chorusDepth = 0.5f, chorusRate = 0.35f;
        // 0 = наш простой хорус, 1 = прошивочное ядро OS 1.32B из MonoJuice
        int   chorusModel = 0;
        // DEL / DEP / SPD / MIX / FB / WID / LP / INP, 0…127 (MIX задаётся выше)
        std::array<int, 8> chorusFw = { 40, 60, 50, 127, 30, 80, 100, 100 };
        float delayMix = 0.0f, delayTime = 0.33f, delayFb = 0.35f;
        float reverbMix = 0.0f, reverbSize = 0.6f, reverbDamp = 0.4f;
        float plateMix = 0.0f, plateDecay = 0.75f, plateDamp = 0.6f, plateDiffusion = 0.7f;
        float platePredelay = 0.01f;
        // Остальные независимые органы управления Dattorro и выходной трим.
        // Значения совпадают с прежними фиксированными коэффициентами.
        float platePreFilter = 0.85f, plateInputDiff1 = 0.75f, plateInputDiff2 = 0.625f;
        float plateOutput = 0.60f;
        bool plateDc = true;
        // модель драйва: индекс DriveModel (0…18), 3 = Tanh
        int   driveModel = 3;
        float driveTone = 0.0f;                 // −1…+1, наклон ±8 дБ

        // COMB — комб-фильтры из MonoJuice
        float combMix = 0.0f;
        int   combModel = 0;                    // 0 = ODIN2, 1 = ParallelComb
        float combDriveDb = 0.0f, combOutputDb = 0.0f;
        float combDepth = 0.0f, combRate = 0.5f, combEnvelope = 0.0f;
        float combFreqA = 220.0f, combFreqB = 331.0f;
        float combResA = 75.0f, combResB = 75.0f, combBalance = 0.5f;
        int   combRouting = 0;                  // 0 A, 1 A→B, 2 A+B
        bool  combNegative = false;
        float combDelayMs = 100.0f, combFeedback = 25.0f;
        float combHighpass = 5.0f, combTune = 0.0f;
        bool  combOversample = false, combMidSide = false;
        // B: 0 follows legacy NEG A, 1 is positive, 2 is negative.
        int combNegativeB = 0;
        float combDecayMs = 180.0f;
        // -1 means "follow the legacy shared delay"; 0 ms remains selectable.
        float combTapA = -1.0f, combTapB = -1.0f, combTapC = -1.0f, combTapD = -1.0f;
        float combMultiplier = 1.0f, combParRate = 0.5f, combSmoothMs = 0.0f;
        float combCross = 0.0f, combAm = 0.0f, combLean = 0.0f;
        float combGateDb = -140.0f, combGateMs = 10.0f, combLimiterMs = 200.0f;
        float combDryDb = 0.0f, combWetDb = 0.0f;

        // MATERIAL — модальный резонатор
        float materialMix = 0.0f;
        int   materialMode = 2;                 // 0 latex, 1 spring, 2 metal, 3 glass
        float materialExcite = 0.5f, materialTone = 0.5f, materialDamp = 0.5f;
        float materialSize = 0.5f, materialFeedback = 0.4f;
        bool  materialFollow = false;

        // SPECTRE — MDCT-маска по пикам
        // 0 — EXTENDED (топ-N пиков, сдвиг, заморозка), 1 — CLASSIC (мягкая маска)
        int   spectreModel = 0;
        float spectreMix = 0.0f;
        int   spectreWindow = 3;                // 512 << index
        float spectreProminence = 10.0f;        // дБ
        float spectreShift = 0.0f;              // полутоны
        int   spectrePeaks = 0;                 // 0 = все
        int   spectreWidth = 1;                 // соседних бинов
        // полоса анализа, Гц; 0 = без ограничения (как в SpectreSettings)
        float spectreLow = 0.0f, spectreHigh = 0.0f;
        bool  spectreFreeze = false;

        // ROBIN — питч-шифтер (эффект 15/16)
        float robinMix = 0.0f;
        float robinSemis = 0.0f;                // −24…+24
        float robinSize = 0.4f;                 // 0…1 → окно 10…160 мс
        float robinSpread = 0.0f;               // расстройка правого канала, 0…1

        // GLITCHY — статтер
        float glitchMix = 0.0f, glitchChance = 0.35f, glitchLengthMs = 90.0f;
        float glitchRepeats = 3.0f, glitchReverse = 0.0f, glitchPitch = 0.0f;
        // остальные ручки GlitchCore: по умолчанию выставлены в его же
        // значения (tone 24 кГц, feedback 0, fold 0), чтобы включение этих
        // параметров не меняло звук уже сохранённых патчей
        float glitchFeedback = 0.0f, glitchFold = 0.0f, glitchTone = 24000.0f;
        float eqLow = 0.0f, eqHigh = 0.0f;
        float compThreshold = -8.0f, compRatio = 3.0f, compMakeup = 2.0f;
        float width = 1.0f;
    };

    // Stable node identity: module + copy * ModuleCount. Copy zero keeps
    // every historical parameter ID and DSP state. Additional copies are
    // staged off the audio thread and published once, then retained until the
    // instrument is destroyed. The rack itself is deliberately bounded.
    static constexpr int kCopiesPerModule = 4;
    static constexpr int kNodeCount = ModuleCount * kCopiesPerModule;
    static constexpr int kRackLimit = 16;
    using RackRanks = std::array<int, kNodeCount>;
    using CopySettings = std::array<Settings, kCopiesPerModule>;

    static RackRanks defaultRanks() noexcept
    {
        RackRanks ranks {};
        for (int i = 0; i < ModuleCount; ++i) ranks[(size_t) i] = i + 1;
        return ranks;
    }

    FxChain() noexcept
    {
        for (auto& copy : copies) copy.store (nullptr, std::memory_order_relaxed);
        for (int i = 0; i < ModuleCount; ++i) active.nodes[(size_t) i] = { i, this };
        desired = active;
    }

    ~FxChain()
    {
        // The owner must have stopped audio before destroying the instrument.
        for (auto& copy : copies) delete copy.load (std::memory_order_relaxed);
    }

    // Message/control thread only. No active DSP object is modified here.
    void ensureCopy (int node, const Settings& initial)
    {
        if (node < ModuleCount || node >= kNodeCount) return;
        const int slot = node - ModuleCount;
        if (copies[(size_t) slot].load (std::memory_order_acquire) != nullptr) return;
        std::lock_guard<std::mutex> lock (copyMutex);
        if (copies[(size_t) slot].load (std::memory_order_relaxed) != nullptr) return;
        auto instance = std::make_unique<FxChain>();
        instance->prepareSingle (sampleRate, (Module) (node % ModuleCount));
        instance->set (initial);
        copies[(size_t) slot].store (instance.release(), std::memory_order_release);
        copyGeneration.fetch_add (1, std::memory_order_release);
    }

    unsigned getCopyGeneration() const noexcept { return copyGeneration.load (std::memory_order_acquire); }

    // Audio thread only. Pointer snapshots and fixed-size insertion sort mean
    // no allocation, deletion, locks or waiting in the callback. A change in
    // order or ready copies is faded out before the new graph is installed.
    void setRack (const RackRanks& ranks, const CopySettings& settings) noexcept
    {
        struct Ranked { int rank, code; FxChain* instance; };
        std::array<Ranked, kRackLimit> ordered {};
        int count = 0;
        for (int node = 0; node < kNodeCount; ++node)
        {
            const int rank = ranks[(size_t) node];
            if (rank <= 0) continue;
            FxChain* instance = node < ModuleCount ? this
                : copies[(size_t) (node - ModuleCount)].load (std::memory_order_acquire);
            if (instance != nullptr && instance != this)
                instance->set (settings[(size_t) (node / ModuleCount)]);
            const Ranked candidate { rank, node, instance };
            int at = 0;
            while (at < count && (ordered[(size_t) at].rank < rank
                   || (ordered[(size_t) at].rank == rank && ordered[(size_t) at].code < node))) ++at;
            if (at >= kRackLimit) continue;
            const int newCount = std::min (count + 1, kRackLimit);
            for (int j = newCount - 1; j > at; --j) ordered[(size_t) j] = ordered[(size_t) (j - 1)];
            ordered[(size_t) at] = candidate;
            count = newCount;
        }
        Layout next {};
        next.count = count;
        next.canonical = count == ModuleCount;
        for (int i = 0; i < count; ++i)
        {
            next.nodes[(size_t) i] = { ordered[(size_t) i].code, ordered[(size_t) i].instance };
            next.canonical = next.canonical && ordered[(size_t) i].code == i;
        }
        if (sameLayout (next, desired)) return;
        desired = next;
        if (! processed) { active = desired; switching = 0; transitionGain = 1.0f; }
        else if (! sameLayout (active, desired)) switching = 1; // fade out, then install
    }

    void prepare (double sr)
    {
        std::lock_guard<std::mutex> lock (copyMutex);
        sampleRate = sr;
        shaper.prepare (sr); chorus.prepare (sr); delay.prepare (sr);
        reverb.prepare (sr); eq.prepare (sr); dyn.prepare (sr);
        plate.prepare (sr); wdf.prepare (sr);
        drive.prepare (sr); material.prepare (sr); spectre.prepare (sr); glitch.prepare (sr);
        robin.prepare (sr);
        spectreClassic.prepare (sr);
        comb.prepare (sr); chorusFw.prepare (sr);
        dcCoef = (float) std::exp (-kTwoPi * 18.0 / sr);
        dcX1L = dcY1L = dcX1R = dcY1R = 0.0f;
        processed = false;
        for (auto& gain : moduleGain) { gain.reset (sr, 6.0); gain.snap (1.0f); }
        transitionStep = 1.0f / (float) std::max (1.0, sr * 0.006);
        transitionGain = 1.0f; switching = 0;
        for (int slot = 0; slot < (int) copies.size(); ++slot)
            if (auto* instance = copies[(size_t) slot].load (std::memory_order_acquire))
                instance->prepareSingle (sr, (Module) ((slot + ModuleCount) % ModuleCount));
    }

    void set (const Settings& s)
    {
        for (int i = 0; i < ModuleCount; ++i)
        {
            const float value = s.enabled[(size_t) i] ? 1.0f : 0.0f;
            if (! processed) moduleGain[(size_t) i].snap (value);
            else             moduleGain[(size_t) i].setTarget (value);
        }
        // Latency-changing stages cannot be cross-faded with an unaligned dry
        // signal. Their host latency changes with the switch, as it already did
        // with mix/model changes in legacy versions.
        if (s.chorusModel == 1) moduleGain[(size_t) ChorusModule].snap (s.enabled[(size_t) ChorusModule] ? 1.0f : 0.0f);
        moduleGain[(size_t) Spectre].snap (s.enabled[(size_t) Spectre] ? 1.0f : 0.0f);
        shaper.setDrive (s.drive);
        chorus.setMix (s.chorusMix); chorus.setDepth (s.chorusDepth); chorus.setRate (s.chorusRate);
        // те же ручки, что у прошивочного хоруса (DEL/FB/WID/LP/INP)
        chorus.setDelay    ((float) s.chorusFw[(size_t) 0]);
        chorus.setFeedback ((float) s.chorusFw[(size_t) 4]);
        chorus.setWidth    ((float) s.chorusFw[(size_t) 5]);
        chorus.setLowpass  ((float) s.chorusFw[(size_t) 6]);
        chorus.setInput    ((float) s.chorusFw[(size_t) 7]);
        // прошивочный хорус работает только при ненулевом миксе: иначе он
        // вносил бы задержку 87 отсчётов «вхолостую»
        s_chorusFw = s.enabled[(size_t) ChorusModule] && s.chorusModel == 1
                     && (s.chorusMode != 0 || s.chorusMix > 1.0e-4f);
        chorusFw.setMode (s.chorusMode);
        if (s_chorusFw) { chorusFw.setParameters (s.chorusFw); chorusFw.setMix (s.chorusMix); }
        else            { chorus.setMix (s.chorusMix); }

        comb.setModel (s.combModel);
        comb.setMix (s.combMix);
        comb.setDriveDb (s.combDriveExt < 0 ? s.combDriveDb : s.combDriveExt);
        comb.setOutputDb (s.combOutputExt <= -31 ? s.combOutputDb : s.combOutputExt);
        comb.setDepth (s.combDepthExt < 0 ? s.combDepth : s.combDepthExt);
        comb.setRate (s.combRateExt < 0 ? s.combRate : s.combRateExt);
        comb.setEnvelope (s.combEnvelopeExt <= -37 ? s.combEnvelope : s.combEnvelopeExt); comb.setDecayMs (s.combDecayMs);
        comb.setOdinFreq (s.combFreqAExt < 0 ? s.combFreqA : s.combFreqAExt,
                          s.combFreqBExt < 0 ? s.combFreqB : s.combFreqBExt);
        comb.setOdinMix (s.combOdinMix);
        comb.setOdinRes (s.combResA, s.combResB);
        comb.setOdinBalance (s.combBalance);
        comb.setOdinRouting (s.combRouting);
        comb.setOdinNegative (s.combNegative,
                              s.combNegativeB == 0 ? s.combNegative : s.combNegativeB == 2);
        const float taps[] = { s.combTapA, s.combTapB, s.combTapC, s.combTapD };
        for (int i = 0; i < 4; ++i) comb.setParDelayMs (i, taps[i] >= 0.0f ? taps[i] : s.combDelayMs);
        comb.setParMultiplier (s.combMultiplier); comb.setParRate (s.combParRate);
        comb.setParSmoothMs (s.combSmoothMs); comb.setParCross (s.combCross);
        comb.setParAm (s.combAm); comb.setParLean (s.combLean);
        comb.setParGateDb (s.combGateDb); comb.setParGateMs (s.combGateMs);
        comb.setParLimiterMs (s.combLimiterMs);
        comb.setParDryWetDb (s.combDryDbExt <= -61 ? s.combDryDb : s.combDryDbExt,
                             s.combWetDbExt <= -61 ? s.combWetDb : s.combWetDbExt);
        comb.setParFeedback (s.combFeedbackExt < 0 ? s.combFeedback : s.combFeedbackExt);
        comb.setParHighpass (s.combHighpassExt < 0 ? s.combHighpass : s.combHighpassExt);
        comb.setParTune (s.combTuneExt <= -49 ? s.combTune : s.combTuneExt);
        comb.setParOversample (s.combOversample);
        comb.setParMidSide (s.combMidSide);
        s_combOn = s.combMix > 1.0e-4f;
        delay.setMix (s.delayMix); delay.setTime (s.delayTime); delay.setFeedback (s.delayFb);
        reverb.setMix (s.reverbMix); reverb.setSize (s.reverbSize); reverb.setDamp (s.reverbDamp);
        eq.setLow (s.eqLow); eq.setHigh (s.eqHigh);
        plate.setMix (s.plateMix); plate.setDecay (s.plateDecay); plate.setDamping (s.plateDamp);
        plate.setDiffusion (s.plateDiffusion); plate.setPreDelay (s.platePredelay);
        plate.setPreFilter (s.platePreFilter);
        plate.setInputDiffusion1 (s.plateInputDiff1); plate.setInputDiffusion2 (s.plateInputDiff2);
        plate.setOutputTrim (s.plateOutput); plate.setDcBlocker (s.plateDc);
        const int dm = (s.driveModel >= 0 && s.driveModel < (int) DriveModel::Count) ? s.driveModel : 3;
        if (dm != lastDriveModel) { drive.setModel ((DriveModel) dm); lastDriveModel = dm; }
        drive.setAmount (s.drive);
        drive.setTone (s.driveTone);
        drive.setMix (s.drive > 1.0e-4f ? s.driveMix : 0.0f);
        s_driveOn = s.drive > 1.0e-4f;

        material.setMode (s.materialMode);
        material.setExcite (s.materialExcite); material.setTone (s.materialTone);
        material.setDamp (s.materialDamp); material.setSize (s.materialSize);
        material.setFeedback (s.materialFeedback); material.setFollow (s.materialFollow);
        material.setMix (s.materialMix);

        const int spectreWindow = s.spectreWindowExt < 0 ? s.spectreWindow : (int) std::lround (s.spectreWindowExt);
        spectre.setWindow (spectreWindow);
        spectre.setProminence (s.spectreProminence);
        spectre.setShift (s.spectreShift);
        spectre.setPeaks (s.spectrePeaksExt < 0 ? s.spectrePeaks : (int) std::lround (s.spectrePeaksExt));
        spectre.setWidth (s.spectreWidthExt < 0 ? s.spectreWidth : (int) std::lround (s.spectreWidthExt));
        spectre.setReject (s.spectreReject); spectre.setAttack (s.spectreAttack); spectre.setRelease (s.spectreRelease);
        spectre.setFreeze (s.spectreFreeze);
        spectre.setRange (s.spectreLowExt < 0 ? s.spectreLow : s.spectreLowExt,
                          s.spectreHighExt < 0 ? s.spectreHigh : s.spectreHighExt);      // 0 = без ограничения
        s_spectreClassic = s.spectreModel == 1;
        if (s_spectreClassic)
        {
            spectre.setMix (0.0f);                  // EXTENDED выключен
            spectreClassic.setWindow (spectreWindow);
            spectreClassic.setProminence (s.spectreProminence);
            spectreClassic.setMix (s.spectreMix);
        }
        else
        {
            spectre.setMix (s.spectreMix);
            spectreClassic.setMix (0.0f);
        }
        s_spectreOn = s.enabled[(size_t) Spectre] && s.spectreMix > 1.0e-4f;

        robin.setSemitones (s.robinSemis);
        robin.setSize (s.robinSize);
        robin.setSpread (s.robinSpread);
        robinBaseMix = clamp (s.robinMix, 0.0f, 1.0f);
        robin.setMix (robinBaseMix * moduleGain[(size_t) Robin].getCurrent());

        glitch.setTimingMode (s.glitchTimingMode);
        glitch.setChance (s.glitchChance);
        glitch.setLengthMs (s.glitchLengthExt <= 0 ? s.glitchLengthMs : s.glitchLengthExt);
        glitch.setRepeats (s.glitchRepeats);
        glitch.setReverse (s.glitchReverse);
        glitch.setPitch (s.glitchPitch);
        glitch.setFeedback (s.glitchFeedback);
        glitch.setFold (s.glitchFold); glitch.setFoldDrive (s.glitchFoldDrive);
        glitch.setTone (s.glitchTone); glitch.setToneHighpass (s.glitchHighpass);
        glitch.setResonance (s.glitchResonance);
        glitch.setOffset (s.glitchOffset); glitch.setCrossfade (s.glitchCrossfade);
        glitch.setSync (s.glitchSync, s.glitchDivision);
        glitch.setDrift (s.glitchDrift); glitch.setPitchStep (s.glitchPitchStep);
        glitch.setPitchRandom (s.glitchPitchRandom); glitch.setReroll (s.glitchReroll);
        glitch.setDecay (s.glitchDecay);
        glitch.setLevels (s.glitchMix, s.glitchDryLevel, s.glitchWetLevel);
        dyn.setThreshold (s.compThreshold); dyn.setRatio (s.compRatio); dyn.setMakeup (s.compMakeup);
        width = clamp (s.width, 0.0f, 1.5f);
    }

    void setTempo (double bpm)
    {
        glitch.setTempo (bpm);
        for (int copy = 1; copy < kCopiesPerModule; ++copy)
            if (auto* c = copies[(size_t) ((copy - 1) * ModuleCount + Glitch)].load (std::memory_order_acquire))
                c->glitch.setTempo (bpm);
    }

    // Суммарная транспортная задержка цепочки, в сэмплах.
    //
    // Её должен компенсировать хост: обёртка VST3 вызывает
    // setLatencySamples (FxChain::latency()). Внутри dry/wet задержка уже
    // скомпенсирована (Spectre и прошивочный хорус задерживают свой dry),
    // но относительно входа инструмента цепочка всё равно сдвинута.
    int latency() const
    {
        if (! active.canonical)
        {
            int lat = 0;
            bool baseRobinPresent = false;
            for (int i = 0; i < active.count; ++i)
            {
                const auto& node = active.nodes[(size_t) i];
                const auto module = (Module) (node.code % ModuleCount);
                if (node.code == Robin) baseRobinPresent = true;
                if (node.instance != nullptr) lat += node.instance->stageLatency (module);
            }
            if (! baseRobinPresent) lat += robin.latency();
            return lat;
        }
        int lat = 0;
        if (s_spectreOn) lat += s_spectreClassic ? spectreClassic.latency() : spectre.latency();
        if (s_chorusFw)  lat += chorusFw.latency();
        lat += robin.latency();      // постоянно, независимо от mix (сухой путь тоже задержан)
        return lat;
    }

    // innerOnly: для стендов, которым нужен только «хвост» цепочки без COMB
    inline void process (float& l, float& r, bool innerOnly = false) noexcept
    {
        if (active.canonical) processOriginal (l, r, innerOnly);
        else                  processFlexible (l, r, innerOnly);
        if (switching != 0)
        {
            if (switching == 1)
            {
                transitionGain = std::max (0.0f, transitionGain - transitionStep);
                if (transitionGain <= 0.0f) { active = desired; switching = 2; }
            }
            else
            {
                transitionGain = std::min (1.0f, transitionGain + transitionStep);
                if (transitionGain >= 1.0f) switching = 0;
            }
            l *= transitionGain; r *= transitionGain;
        }
    }

    inline void processOriginal (float& l, float& r, bool innerOnly = false) noexcept
    {
        // Мастер-блокер DC: shaper и компрессор асимметричны, и без него
        // на выходе накапливалось до 2.6·10⁻³ постоянной составляющей
        // (щелчки при старте/стопе и потеря запаса по пику).
        const float dl = l - dcX1L + dcCoef * dcY1L; dcX1L = l; dcY1L = dl; l = dl;
        const float dr = r - dcX1R + dcCoef * dcY1R; dcX1R = r; dcY1R = dr; r = dr;

        processed = true;
        // Smooth power switching without changing any saved wet/mix values.
        // When fully bypassed, expensive DSP is skipped; existing delay/reverb
        // state is retained (and therefore may resume when re-enabled).
        const auto stage = [&] (Module which, auto&& processStage)
        {
            const float gain = moduleGain[(size_t) which].next();
            if (gain <= 0.0f) return;
            if (gain >= 1.0f) { processStage(); return; }
            const float dryL = l, dryR = r;
            processStage();
            l = dryL + (l - dryL) * gain;
            r = dryR + (r - dryR) * gain;
        };
        stage (Drive, [&] { if (s_driveOn) drive.process (l, r); });
        stage (Comb, [&] { if (s_combOn && ! innerOnly) comb.process (l, r); });
        stage (ChorusModule, [&] { if (s_chorusFw) chorusFw.process (l, r); else chorus.process (l, r); });
        // Keep the delay clock/history running while bypassed, just as the
        // legacy mix=0 path did. Otherwise a frozen echo returns minutes later.
        {
            const float gain = moduleGain[(size_t) DelayModule].next();
            const float dryL = l, dryR = r;
            delay.process (l, r);
            if (gain <= 0.0f) { l = dryL; r = dryR; }
            else if (gain < 1.0f) { l = dryL + (l - dryL) * gain; r = dryR + (r - dryR) * gain; }
        }
        stage (ReverbModule, [&] { reverb.process (l, r); });
        stage (Plate, [&] { plate.process (l, r); });
        stage (Equalizer, [&] { eq.process (l, r); });
        stage (Material, [&] { material.process (l, r); });
        stage (Spectre, [&] { if (s_spectreOn) { if (s_spectreClassic) spectreClassic.process (l, r); else spectre.process (l, r); } });
        // ROBIN's dry delay is mandatory even when bypassed; fade its wet mix.
        robin.setMix (robinBaseMix * moduleGain[(size_t) Robin].next());
        robin.process (l, r);
        stage (Glitch, [&] { glitch.process (l, r); });
        stage (Compressor, [&] { dyn.process (l, r); });
        Dynamics::limit (l, r, 0.98f); // fixed output safety, not the compressor

        const float gain = moduleGain[(size_t) StereoWidth].next();
        const float effectiveWidth = gain >= 1.0f ? width : gain <= 0.0f ? 1.0f : 1.0f + (width - 1.0f) * gain;
        const float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * effectiveWidth;
        l = mid + side; r = mid - side;
    }

private:
    struct Node { int code = 0; FxChain* instance = nullptr; };
    struct Layout
    {
        std::array<Node, kRackLimit> nodes {};
        int count = ModuleCount;
        bool canonical = true;
    };
    static bool sameLayout (const Layout& a, const Layout& b) noexcept
    {
        if (a.count != b.count || a.canonical != b.canonical) return false;
        for (int i = 0; i < a.count; ++i)
            if (a.nodes[(size_t) i].code != b.nodes[(size_t) i].code
                || a.nodes[(size_t) i].instance != b.nodes[(size_t) i].instance) return false;
        return true;
    }

    void prepareSingle (double sr, Module module)
    {
        sampleRate = sr;
        switch (module)
        {
            case Drive: drive.prepare (sr); break;
            case Comb: comb.prepare (sr); break;
            case ChorusModule: chorus.prepare (sr); chorusFw.prepare (sr); break;
            case DelayModule: delay.prepare (sr); break;
            case ReverbModule: reverb.prepare (sr); break;
            case Plate: plate.prepare (sr); break;
            case Equalizer: eq.prepare (sr); break;
            case Material: material.prepare (sr); break;
            case Spectre: spectre.prepare (sr); spectreClassic.prepare (sr); break;
            case Robin: robin.prepare (sr); break;
            case Glitch: glitch.prepare (sr); break;
            case Compressor: dyn.prepare (sr); break;
            case StereoWidth: break;
            default: break;
        }
        processed = false;
        for (auto& gain : moduleGain) { gain.reset (sr, 6.0); gain.snap (1.0f); }
    }

    int stageLatency (Module module) const noexcept
    {
        if (module == Robin) return robin.latency();
        if (module == Spectre && s_spectreOn) return s_spectreClassic ? spectreClassic.latency() : spectre.latency();
        if (module == ChorusModule && s_chorusFw) return chorusFw.latency();
        return 0;
    }

    inline void processStage (Module which, float& l, float& r, bool innerOnly) noexcept
    {
        processed = true;
        if (which == Robin)
        {
            robin.setMix (robinBaseMix * moduleGain[(size_t) Robin].next());
            robin.process (l, r);
            return;
        }
        if (which == StereoWidth)
        {
            const float gain = moduleGain[(size_t) StereoWidth].next();
            const float w = gain >= 1.0f ? width : gain <= 0.0f ? 1.0f : 1.0f + (width - 1.0f) * gain;
            const float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * w;
            l = mid + side; r = mid - side;
            return;
        }
        const float gain = moduleGain[(size_t) which].next();
        if (gain <= 0.0f && which != DelayModule) return;
        const float dryL = l, dryR = r;
        switch (which)
        {
            case Drive: if (s_driveOn) drive.process (l, r); break;
            case Comb: if (s_combOn && ! innerOnly) comb.process (l, r); break;
            case ChorusModule: if (s_chorusFw) chorusFw.process (l, r); else chorus.process (l, r); break;
            case DelayModule: delay.process (l, r); break;
            case ReverbModule: reverb.process (l, r); break;
            case Plate: plate.process (l, r); break;
            case Equalizer: eq.process (l, r); break;
            case Material: material.process (l, r); break;
            case Spectre: if (s_spectreOn) { if (s_spectreClassic) spectreClassic.process (l, r); else spectre.process (l, r); } break;
            case Glitch: glitch.process (l, r); break;
            case Compressor: dyn.process (l, r); break;
            default: break;
        }
        if (gain <= 0.0f) { l = dryL; r = dryR; }
        else if (gain < 1.0f) { l = dryL + (l - dryL) * gain; r = dryR + (r - dryR) * gain; }
    }

    inline void processFlexible (float& l, float& r, bool innerOnly) noexcept
    {
        const float dl = l - dcX1L + dcCoef * dcY1L; dcX1L = l; dcY1L = dl; l = dl;
        const float dr = r - dcX1R + dcCoef * dcY1R; dcX1R = r; dcY1R = dr; r = dr;
        processed = true;
        bool baseRobinPresent = false;
        for (int i = 0; i < active.count; ++i)
        {
            const auto& node = active.nodes[(size_t) i];
            if (node.code == Robin) baseRobinPresent = true;
            if (node.instance != nullptr)
                node.instance->processStage ((Module) (node.code % ModuleCount), l, r, innerOnly);
        }
        // Preserve the historic fixed 1024-sample dry latency even if the
        // original ROBIN node was removed. Extra ROBIN nodes each add 1024.
        if (! baseRobinPresent) { robin.setMix (0.0f); robin.process (l, r); }
        Dynamics::limit (l, r, 0.98f);
    }

    std::array<std::atomic<FxChain*>, (kCopiesPerModule - 1) * ModuleCount> copies {};
    std::atomic<unsigned> copyGeneration { 0 };
    std::mutex copyMutex;
    Layout active {}, desired {};
    int switching = 0; // 0 stable, 1 fade out, 2 fade in
    float transitionGain = 1.0f, transitionStep = 1.0f / 288.0f;

    Shaper shaper;
    Chorus chorus;
    PingPongDelay delay;
    Reverb reverb;
    Eq eq;
    Dynamics dyn;
    PlateReverb plate;
    WdfClipper wdf;
    DriveFx drive;
    MaterialFx material;
    SpectreFx spectre;
    SpectreClassicFx spectreClassic;
    GlitchFx glitch;
    RobinFx robin;
    CombFx comb;
    FirmwareChorusFx chorusFw;
    int lastDriveModel = -1;
    bool s_driveOn = false, s_combOn = false, s_chorusFw = false;
    bool s_spectreOn = false, s_spectreClassic = false;
    std::array<SmoothedValue, ModuleCount> moduleGain {};
    bool processed = false;
    float robinBaseMix = 0.0f;
    bool useWdf = true;
    double sampleRate = 48000.0;
    float width = 1.0f;
    float dcCoef = 0.9975f, dcX1L = 0.0f, dcY1L = 0.0f, dcX1R = 0.0f, dcY1R = 0.0f;
};

} // namespace orogen
