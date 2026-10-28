// ДВИЖОК 3: Harmonic — аддитивный синтез по таблице партиалов.
//
// Два пути, и это не оптимизация «на всякий случай», а принципиальное решение:
//
//   БЫСТРЫЙ (spread ≈ 0): одна фаза + рекуррентность Чебышёва
//       sin((k+1)φ) = 2·cosφ·sin(kφ) − sin((k−1)φ)
//       2 чтения таблицы на сэмпл вместо N синусов. 64 партиала почти бесплатно.
//
//   МЕДЛЕННЫЙ (spread > 0): у каждого партиала своя фаза и свой инкремент.
//       Нужен для «растянутых» партиалов (негармоничность фортепианной струны:
//       f_k = k·f0·sqrt(1 + B·k²)) — рекуррентность тут неприменима, потому что
//       партиалы больше не кратны одной частоте.
//
// Anti-alias: число партиалов ограничено 0.45·SR/f0, поэтому выше Найквиста
// ничего не рождается ПО ПОСТРОЕНИЮ (это сильная сторона аддитива).
#pragma once

#include "engine.h"

namespace orogen {

class HarmonicEngine final : public EngineBase
{
public:
    static constexpr int   kMaxPartials = 64;
    static constexpr float kGainTrim    = 0.28f;

    const char* name() const override { return "Harmonic"; }
    float costFactor() const override { return 1.6f; }   // дороже вейвтейбла: до 64 партиалов

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        tiltSm.setTime   (sr, 30.0);
        shapeSm.setTime  (sr, 30.0);
        spreadSm.setTime (sr, 40.0);
        motionSm.setTime (sr, 40.0);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        phase = 0.0f;
        driftCents = rng.nextBipolar() * 6.0f;
        for (int k = 0; k < kMaxPartials; ++k)
        {
            ph[k]  = rng.nextFloat();
            ph2[k] = rng.nextFloat();
        }
        motionPhase = 0.0f;
        needSnap = true;
        active = true;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        const double cents = (double) driftCents * (double) p.detune * 2.0;
        const double f0 = midiToHz ((double) note + (double) p.tuneSemis) * std::pow (2.0, cents / 1200.0);
        inc = (float) (f0 / sampleRate);

        // ---- сколько партиалов влезает ниже Найквиста
        const int allowed = (int) std::floor (0.45 * sampleRate / std::max (1.0, f0));
        nPartials = clamp (allowed, 1, kMaxPartials);

        if (needSnap)
        {
            // та же шкала, что и в else-ветви: 0..1 -> -1..+1 (иначе первый голос
            // звучал с tilt вдвое меньшим, чем последующие, — расхождение 12 дБ по ВЧ)
            tiltSm.snap  (clamp (p.p[0], 0.0f, 1.0f) * 2.0f - 1.0f);
            shapeSm.snap (clamp (p.p[1], 0.0f, 1.0f));
            spreadSm.snap (clamp (p.p[2], 0.0f, 1.0f));
            motionSm.snap (clamp (p.p[3], 0.0f, 1.0f));
            needSnap = false;
        }
        else
        {
            tiltSm.setTarget  (clamp (p.p[0], 0.0f, 1.0f) * 2.0f - 1.0f);
            shapeSm.setTarget (clamp (p.p[1], 0.0f, 1.0f));
            spreadSm.setTarget (clamp (p.p[2], 0.0f, 1.0f));
            motionSm.setTarget (clamp (p.p[3], 0.0f, 1.0f));
        }

        // ---- амплитуды партиалов: наклон + чёт/нечет + «формантная» подсветка
        const float tilt  = tiltSm.getCurrent();
        const float shape = shapeSm.getCurrent();
        // Наклон спектра: a_k = k^(-p).
        //   tilt = −1 -> p = 2.3  (почти синус, «стекло/флейта»)
        //   tilt =  0 -> p = 1.4  (как пила: все партиалы, но с спадом)
        //   tilt = +1 -> p = 0.5  (ярко, квадрат-подобно)
        // Раньше здесь был растущий профиль (a_k = k^+0.2) — спектр «жужжал»,
        // а crest factor достигал 8.7 (peak 2.17 при RMS 0.3) и клиповал.
        const float pExp = 1.4f - tilt * 0.9f;
        const float evenGain = clamp (shape * 2.0f, 0.0f, 1.0f);
        float energy = 0.0f;
        for (int k = 1; k <= kMaxPartials; ++k)
        {
            float a = std::pow ((float) k, -pExp);
            if ((k & 1) == 0) a *= evenGain;
            if (k > nPartials) a = 0.0f;
            amps[(size_t) (k - 1)] = a;
            energy += a * a;
        }
        normGain = energy > 1.0e-9f ? kGainTrim / std::sqrt (energy) : 0.0f;
        gain = normGain * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));

        // ---- медленный путь (растянутые партиалы)
        const float spread = spreadSm.getCurrent();
        slowPath = spread > 0.02f;
        if (slowPath)
        {
            // Коэффициент негармоничности модели жёсткой струны: f_k = k·f0·sqrt(1+B·k²).
            // Раньше было B = spread·0.0016 — при k = 64 это растяжка +72 %,
            // то есть 64-я гармоника уезжала на полторы октавы вверх и вываливалась
            // за Найквист (спектр демо был на 40 дБ темнее, чем у быстрого пути).
            // Теперь максимум +5 % на 64-м партиале — как у реальной струны.
            const float B = spread * 2.0e-5f;
            int allowed = 0;
            for (int k = 1; k <= nPartials; ++k)
            {
                const float stretch = std::sqrt (1.0f + B * (float) (k * k));
                // k обязателен: без него все 64 партиала играли на f0
                // (RMS 0.17 при спектре целиком ниже 1 кГц).
                const float f = inc * (float) k * stretch;
                if (f > 0.45f) break;                            // выше Найквиста не создаём
                pinc[(size_t) (k - 1)] = f;
                ++allowed;
            }
            nPartials = std::max (1, allowed);
        }
        dcCoef = (float) std::exp (-kTwoPi * 6.0 / sampleRate);
    }

    inline float render (const EngineParams&) override
    {
        if (! active) return 0.0f;

        // медленная «жизнь»: движение наклона спектра
        const float motion = motionSm.next();
        motionPhase += 0.00004f + motion * 0.00012f;
        if (motionPhase >= 1.0f) motionPhase -= 1.0f;
        const float mPhase = motionPhase + phase * 0.25f;

        float out = 0.0f;
        if (! slowPath)
        {
            // ---- Две серии Чебышёва: sin(kφ) и cos(kφ).
            // Зачем две: если все партиалы начинают с фазы 0, они складываются
            // в «иглу» — crest factor ≈ N (у 64 партиалов peak в 8.7 раз выше RMS,
            // и любой prefader клипует). Раздав чётные партиалы в cos-серию,
            // получаем сдвиг фаз на 90° у половины спектра: crest падает до ~4,
            // а стоимость — одно лишнее умножение на партиал (те же 2 чтения таблицы).
            const float sinP = fastSin (phase);
            const float cosP = fastSin (phase + 0.25f);
            const float twoCos = 2.0f * cosP;

            float prevS = 0.0f, curS = sinP;      // sin(kφ):     start (0, sinφ)
            float prevC = 1.0f, curC = cosP;      // cos(kφ):     start (1, cosφ)
            for (int k = 1; k <= nPartials; ++k)
            {
                float a = amps[(size_t) (k - 1)];
                if (motion > 1.0e-4f) a *= 1.0f - motion * 0.35f * (0.5f + 0.5f * fastSin (mPhase * (float) k));
                const bool odd = (k & 1) != 0;
                out += a * (odd ? curS : curC);
                const float nextS = twoCos * curS - prevS;
                prevS = curS; curS = nextS;
                const float nextC = twoCos * curC - prevC;
                prevC = curC; curC = nextC;
            }
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        else
        {
            // ---- медленный путь: свои фазы у партиалов
            for (int k = 1; k <= nPartials; ++k)
            {
                float& pk = ph[(size_t) (k - 1)];
                pk += pinc[(size_t) (k - 1)];
                if (pk >= 1.0f) pk -= 1.0f;
                out += amps[(size_t) (k - 1)] * fastSin (pk);
            }
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
        }

        out *= gain;

        // DC-блокер
        const float y = out - dcX + dcCoef * dcY;
        dcX = out; dcY = y;
        return y;
    }

private:
    double sampleRate = 48000.0;
    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true, slowPath = false;

    int    nPartials = 32;
    float  inc = 0.0f, phase = 0.0f;
    float  amps[kMaxPartials] {}, pinc[kMaxPartials] {}, ph[kMaxPartials] {}, ph2[kMaxPartials] {};
    float  gain = 0.5f, normGain = 0.5f;
    float  motionPhase = 0.0f;
    float  driftCents = 0.0f;
    float  dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;

    SmoothedValue tiltSm, shapeSm, spreadSm, motionSm;
    Rng rng { 0x13579BDFu };
};

} // namespace orogen
