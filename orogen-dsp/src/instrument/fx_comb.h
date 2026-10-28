// COMB из плагина MonoJuice 0.9.5 — две модели в одном слоте.
//
//   model 0 — ODIN2:  third_party/monojuice/dsp/OdinComb.h
//                      (© 2020-2021 TheWaveWarden, GPL-3.0-or-later)
//   model 1 — ParallelComb: third_party/monojuice/dsp/ParallelCombModel.h
//                      + third_party/monojuice/third_party/Uhhyou/
//                      (© Takamitsu Endo, GPL-3.0-only)
//   общий слой (драйв входа, LFO/env-модуляция частоты, выходной уровень):
//                      third_party/monojuice/dsp/CombColour.h
//
// Листовой DSP скопирован без правок; всё, что ниже — тонкая обвязка:
// перевод «человеческих» единиц (Гц, дБ, проценты) в структуры настроек,
// ленивое применение (set() у этих классов пересчитывает коэффициенты),
// и ранний выход при нулевом миксе.
//
// Задержки внутренних линий — часть звука, transport latency = 0 (как и в
// оригинале). 16x-оверсэмплинг ParallelComb стоит 13 % ядра, поэтому по
// умолчанию выключен.
#pragma once

// OdinComb.h содержит строку с двумя операторами под одним `while` без скобок —
// это его собственный стиль, файл копируется без правок, поэтому предупреждение
// глушим локально, а не правим сторонний код.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../../third_party/monojuice/dsp/CombColour.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "../core/dsp.h"

namespace orogen
{

class CombFx
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        colour.prepare (sr);
        prepared = true;
        dirty = true;
        push();
    }

    // --- общее
    void setModel (int m)        { if (m != c.model) { c.model = clamp (m, 0, 1); dirty = true; } }
    void setMix (float m)        { mix = clamp (m, 0.0f, 1.0f); }
    void setDriveDb (float db)   { if (db != c.drive) { c.drive = db; dirty = true; } }
    void setOutputDb (float db)  { if (db != c.output) { c.output = db; dirty = true; } }
    void setDepth (float semis)  { if (semis != c.depth) { c.depth = semis; dirty = true; } }
    void setRate (float hz)      { if (hz != c.rate) { c.rate = clamp (hz, 0.0f, 8.0f); dirty = true; } }
    void setEnvelope (float semis) { if (semis != c.envelope) { c.envelope = semis; dirty = true; } }
    void setDecayMs (float ms)   { if (ms != c.decay) { c.decay = clamp (ms, 5.0f, 4000.0f); dirty = true; } }

    // --- ODIN2
    void setOdinFreq (float a, float b) { if (a != od.freqA || b != od.freqB) { od.freqA = a; od.freqB = b; dirty = true; } }
    void setOdinRes (float a, float b)  { if (a != od.resA || b != od.resB) { od.resA = clamp (a, 0.0f, 100.0f); od.resB = clamp (b, 0.0f, 100.0f); dirty = true; } }
    void setOdinBalance (float b01)     { if (b01 != od.balance) { od.balance = clamp (b01, 0.0f, 1.0f) * 100.0f; dirty = true; } }
    void setOdinRouting (int r)         { r = clamp (r, 0, 2); if (r != od.routing) { od.routing = r; dirty = true; } }
    void setOdinNegative (bool a, bool b) { if (a != od.negativeA || b != od.negativeB) { od.negativeA = a; od.negativeB = b; dirty = true; } }

    // --- ParallelComb
    void setParDelayMs (int tap, float ms)
    {
        tap = clamp (tap, 0, 3);
        ms = clamp (ms, 0.0f, 4000.0f);
        if (pa.delayMs[(size_t) tap] != ms) { pa.delayMs[(size_t) tap] = ms; dirty = true; }
    }
    void setParMultiplier (float k)   { if (k != pa.multiplier) { pa.multiplier = clamp (k, 0.0f, 1.0f); dirty = true; } }
    void setParFeedback (float pct)   { if (pct != pa.feedback) { pa.feedback = clamp (pct, 0.0f, 100.0f); dirty = true; } }
    void setParHighpass (float hz)    { if (hz != pa.highpass) { pa.highpass = std::max (0.0f, hz); dirty = true; } }
    void setParTune (float semis)     { if (semis != pa.tune) { pa.tune = semis; dirty = true; } }
    void setParCross (float pct)      { if (pct != pa.cross) { pa.cross = clamp (pct, 0.0f, 100.0f); dirty = true; } }
    void setParAm (float pct)         { if (pct != pa.am) { pa.am = clamp (pct, 0.0f, 100.0f); dirty = true; } }
    void setParLean (float pct)       { if (pct != pa.lean) { pa.lean = clamp (pct, -100.0f, 100.0f); dirty = true; } }
    void setParGateDb (float db)      { if (db != pa.gateDB) { pa.gateDB = clamp (db, -140.0f, 0.0f); dirty = true; } }
    void setParGateMs (float ms)      { if (ms != pa.gateMs) { pa.gateMs = clamp (ms, 0.1f, 10000.0f); dirty = true; } }
    void setParLimiterMs (float ms)   { if (ms != pa.limiterMs) { pa.limiterMs = clamp (ms, 0.001f, 16000.0f); dirty = true; } }
    void setParDryWetDb (float dryDb, float wetDb) { if (dryDb != pa.dryDB || wetDb != pa.wetDB) { pa.dryDB = dryDb; pa.wetDB = wetDb; dirty = true; } }
    void setParOversample (bool on)   { if (on != pa.oversample) { pa.oversample = on; dirty = true; } }
    void setParMidSide (bool on)      { if (on != pa.midSide) { pa.midSide = on; dirty = true; } }

    // Задержки внутренних линий эффекта (не transport latency).
    float latency() const { return 0.0f; }
    bool  oversamplingActive() const { return pa.oversample; }

    inline void process (float& l, float& r) noexcept
    {
        if (mix < 1.0e-4f || ! prepared) return;
        if (dirty) push();

        const nova::fx::Frame in { l, r };
        const nova::fx::Frame wet = colour.process (in);
        // CombColour уже смешивает dry/wet по своему mix (он = 100), поэтому
        // внешний микс — простая линейная смесь с исходным сигналом.
        l = in.l + (wet.l - in.l) * mix;
        r = in.r + (wet.r - in.r) * mix;
        if (isBad (l)) l = 0.0f;
        if (isBad (r)) r = 0.0f;
    }

private:
    void push()
    {
        nova::fx::ColourSettings p;
        p.model = c.model;
        p.drive = c.drive;
        p.mix = 100.0f;                 // внешний микс считаем сами
        p.output = c.output;
        p.depth = c.depth;
        p.rate = c.rate;
        p.envelope = c.envelope;
        p.decay = c.decay;
        colour.set (pa, od, p);
        dirty = false;
    }

    nova::fx::CombColour colour;
    nova::fx::CombSettings pa;
    nova::fx::OdinSettings od;
    nova::fx::ColourSettings c;
    double sampleRate = 48000.0;
    float mix = 0.0f;
    bool prepared = false, dirty = true;
};

} // namespace orogen
