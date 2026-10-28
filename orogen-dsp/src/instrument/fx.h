// Цепочка эффектов инструмента:
//   Drive (19 моделей) → COMB (ODIN2 / ParallelComb) → Chorus → Ping-pong Delay →
//   Reverb → Plate → EQ → Material → Spectre → Glitch → Compressor → Limiter.
// Всё аллоцируется в prepare(), в горячем цикле — ни аллокаций, ни делений.
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
    }

    void setTime (float sec)  { timeSec = clamp (sec, 0.02f, 2.0f); }
    void setFeedback (float f){ fb = clamp (f, 0.0f, 0.92f); }
    void setMix (float m)     { mix = clamp (m, 0.0f, 1.0f); }

    inline void process (float& l, float& r) noexcept
    {
        if (mix < 1.0e-4f || bufL.empty()) return;

        const int d = (int) (timeSec * (float) sampleRate);
        int rd = write - d; if (rd < 0) rd += (int) bufL.size();
        const int rd2 = (rd + d / 2) % (int) bufL.size();

        const float dl = bufL[(size_t) rd];
        const float dr = bufR[(size_t) rd2];

        // кросс-обратная связь: левый идёт в правый — это и есть ping-pong
        lpL += 0.35f * (dr - lpL);
        lpR += 0.35f * (dl - lpR);

        bufL[(size_t) write] = l + lpL * fb;
        bufR[(size_t) write] = r + lpR * fb;
        if (++write >= (int) bufL.size()) write = 0;

        l = l + dl * mix;
        r = r + dr * mix;
    }

private:
    std::vector<float> bufL, bufR;
    double sampleRate = 48000.0;
    int write = 0;
    float timeSec = 0.33f, fb = 0.4f, mix = 0.0f, lpL = 0.0f, lpR = 0.0f;
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

    // лимитер: плавное ограничение, никаких «ступенек» на выходе
    static inline void limit (float& l, float& r, float ceiling) noexcept
    {
        const float peak = std::max (std::fabs (l), std::fabs (r));
        if (peak > ceiling)
        {
            const float k = ceiling / peak;
            // мягкое колено: ограничиваем не ступенькой, а tanh-изгибом
            l = fastTanh (l * k * 1.05f) * ceiling;
            r = fastTanh (r * k * 1.05f) * ceiling;
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
    struct Settings
    {
        float drive = 0.0f;
        float chorusMix = 0.0f, chorusDepth = 0.5f, chorusRate = 0.35f;
        // 0 = наш простой хорус, 1 = прошивочное ядро OS 1.32B из MonoJuice
        int   chorusModel = 0;
        // DEL / DEP / SPD / MIX / FB / WID / LP / INP, 0…127 (MIX задаётся выше)
        std::array<int, 8> chorusFw = { 40, 60, 50, 127, 30, 80, 100, 100 };
        float delayMix = 0.0f, delayTime = 0.33f, delayFb = 0.35f;
        float reverbMix = 0.0f, reverbSize = 0.6f, reverbDamp = 0.4f;
        float plateMix = 0.0f, plateDecay = 0.75f, plateDamp = 0.6f, plateDiffusion = 0.7f;
        float platePredelay = 0.01f;
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

    void prepare (double sr)
    {
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
    }

    void set (const Settings& s)
    {
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
        s_chorusFw = s.chorusModel == 1 && s.chorusMix > 1.0e-4f;
        if (s_chorusFw) { chorusFw.setParameters (s.chorusFw); chorusFw.setMix (s.chorusMix); }
        else            { chorus.setMix (s.chorusMix); }

        comb.setModel (s.combModel);
        comb.setMix (s.combMix);
        comb.setDriveDb (s.combDriveDb); comb.setOutputDb (s.combOutputDb);
        comb.setDepth (s.combDepth); comb.setRate (s.combRate);
        comb.setEnvelope (s.combEnvelope);
        comb.setOdinFreq (s.combFreqA, s.combFreqB);
        comb.setOdinRes (s.combResA, s.combResB);
        comb.setOdinBalance (s.combBalance);
        comb.setOdinRouting (s.combRouting);
        comb.setOdinNegative (s.combNegative, s.combNegative);
        for (int i = 0; i < 4; ++i) comb.setParDelayMs (i, s.combDelayMs);
        comb.setParFeedback (s.combFeedback);
        comb.setParHighpass (s.combHighpass);
        comb.setParTune (s.combTune);
        comb.setParOversample (s.combOversample);
        comb.setParMidSide (s.combMidSide);
        s_combOn = s.combMix > 1.0e-4f;
        delay.setMix (s.delayMix); delay.setTime (s.delayTime); delay.setFeedback (s.delayFb);
        reverb.setMix (s.reverbMix); reverb.setSize (s.reverbSize); reverb.setDamp (s.reverbDamp);
        eq.setLow (s.eqLow); eq.setHigh (s.eqHigh);
        plate.setMix (s.plateMix); plate.setDecay (s.plateDecay); plate.setDamping (s.plateDamp);
        plate.setDiffusion (s.plateDiffusion); plate.setPreDelay (s.platePredelay);
        plate.setOutputTrim (0.6f);
        const int dm = (s.driveModel >= 0 && s.driveModel < (int) DriveModel::Count) ? s.driveModel : 3;
        if (dm != lastDriveModel) { drive.setModel ((DriveModel) dm); lastDriveModel = dm; }
        drive.setAmount (s.drive);
        drive.setTone (s.driveTone);
        drive.setMix (s.drive > 1.0e-4f ? 1.0f : 0.0f);
        s_driveOn = s.drive > 1.0e-4f;

        material.setMode (s.materialMode);
        material.setExcite (s.materialExcite); material.setTone (s.materialTone);
        material.setDamp (s.materialDamp); material.setSize (s.materialSize);
        material.setFeedback (s.materialFeedback); material.setFollow (s.materialFollow);
        material.setMix (s.materialMix);

        spectre.setWindow (s.spectreWindow);
        spectre.setProminence (s.spectreProminence);
        spectre.setShift (s.spectreShift);
        spectre.setPeaks (s.spectrePeaks);
        spectre.setWidth (s.spectreWidth);
        spectre.setFreeze (s.spectreFreeze);
        spectre.setRange (s.spectreLow, s.spectreHigh);      // 0 = без ограничения
        s_spectreClassic = s.spectreModel == 1;
        if (s_spectreClassic)
        {
            spectre.setMix (0.0f);                  // EXTENDED выключен
            spectreClassic.setWindow (s.spectreWindow);
            spectreClassic.setProminence (s.spectreProminence);
            spectreClassic.setMix (s.spectreMix);
        }
        else
        {
            spectre.setMix (s.spectreMix);
            spectreClassic.setMix (0.0f);
        }
        s_spectreOn = s.spectreMix > 1.0e-4f;

        robin.setSemitones (s.robinSemis);
        robin.setSize (s.robinSize);
        robin.setSpread (s.robinSpread);
        robin.setMix (s.robinMix);
        s_robinOn = s.robinMix > 1.0e-4f;

        glitch.setChance (s.glitchChance);
        glitch.setLengthMs (s.glitchLengthMs);
        glitch.setRepeats (s.glitchRepeats);
        glitch.setReverse (s.glitchReverse);
        glitch.setPitch (s.glitchPitch);
        glitch.setFeedback (s.glitchFeedback);
        glitch.setFold (s.glitchFold);
        glitch.setTone (s.glitchTone);
        glitch.setMix (s.glitchMix);
        dyn.setThreshold (s.compThreshold); dyn.setRatio (s.compRatio); dyn.setMakeup (s.compMakeup);
        width = clamp (s.width, 0.0f, 1.5f);
    }

    // Суммарная транспортная задержка цепочки, в сэмплах.
    //
    // Её должен компенсировать хост: обёртка VST3 вызывает
    // setLatencySamples (FxChain::latency()). Внутри dry/wet задержка уже
    // скомпенсирована (Spectre и прошивочный хорус задерживают свой dry),
    // но относительно входа инструмента цепочка всё равно сдвинута.
    int latency() const
    {
        int lat = 0;
        if (s_spectreOn) lat += s_spectreClassic ? spectreClassic.latency() : spectre.latency();
        if (s_chorusFw)  lat += chorusFw.latency();
        lat += robin.latency();      // постоянно, независимо от mix (сухой путь тоже задержан)
        return lat;
    }

    // innerOnly: для стендов, которым нужен только «хвост» цепочки без COMB
    inline void process (float& l, float& r, bool innerOnly = false) noexcept
    {
        // Мастер-блокер DC: shaper и компрессор асимметричны, и без него
        // на выходе накапливалось до 2.6·10⁻³ постоянной составляющей
        // (щелчки при старте/стопе и потеря запаса по пику).
        const float dl = l - dcX1L + dcCoef * dcY1L; dcX1L = l; dcY1L = dl; l = dl;
        const float dr = r - dcX1R + dcCoef * dcY1R; dcX1R = r; dcY1R = dr; r = dr;

        if (s_driveOn) { drive.process (l, r); }
        if (s_combOn && ! innerOnly) comb.process (l, r);
        if (s_chorusFw) chorusFw.process (l, r); else chorus.process (l, r);
        delay.process (l, r);
        reverb.process (l, r);
        plate.process (l, r);
        eq.process (l, r);
        material.process (l, r);
        if (s_spectreOn) { if (s_spectreClassic) spectreClassic.process (l, r); else spectre.process (l, r); }
        robin.process (l, r);          // всегда: держит постоянную латентность
        glitch.process (l, r);
        dyn.process (l, r);
        Dynamics::limit (l, r, 0.98f);

        // стерео-ширина: mid/side
        const float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * width;
        l = mid + side; r = mid - side;
    }

private:
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
    bool s_spectreOn = false, s_spectreClassic = false, s_robinOn = false;
    bool useWdf = true;
    double sampleRate = 48000.0;
    float width = 1.0f;
    float dcCoef = 0.9975f, dcX1L = 0.0f, dcY1L = 0.0f, dcX1R = 0.0f, dcY1R = 0.0f;
};

} // namespace orogen
