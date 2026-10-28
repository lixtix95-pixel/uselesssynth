// ДВИЖОК 9: CHOP — слайсер лупа.
//
// Отличие от Sample: там всегда играет весь one-shot с начала, здесь играет
// ОДИН кусок (слайс), выбранный нотой или параметром, и обрывается в конце
// куска без щелчка. Что нужно было решить:
//
//   1. Границы слайсов ищет детектор атак по сигналу (`detectSlices`), а не
//      разметка: в Terrain луп приходит снаружи, и никто не знает, где удары.
//   2. Питч. В режиме Slices (нота = слайс) высота слайса не должна зависеть
//      от ноты — иначе один и тот же удар на соседних клавишах звучит по-разному.
//      В режиме Chromatic наоборот: слайс фиксирован параметром, а нота
//      транспонирует. Оба режима проверяются тестами.
//   3. Алиасинг при транспонировании вверх — через MIP-уровни (тот же
//      MipReader, что у Sample и Granular).
//   4. Щелчки на обрыве куска: окно 1 мс на входе и 5 мс на выходе, причём
//      окно стоит в шкале ИСХОДНИКА, то есть при транспонировании вверх оно
//      короче в выходном времени ровно во столько же раз, во сколько короче
//      сам слайс.
#pragma once

#include "engine.h"
#include "mip_reader.h"
#include "../content/loop_bank.h"

namespace orogen {

class ChopEngine final : public EngineBase
{
public:
    static constexpr float kGainTrim = 0.72f;

    void setBank (const LoopBank* b) noexcept { bank = b; }

    const char* name() const override { return "CHOP"; }
    float costFactor() const override { return 1.1f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        levelSm.setTime (sr, 12.0);
        // DC-блокер 12 Гц: обрыв слайса и его окно оставляют постоянную
        // составляющую 1.6·10⁻³ (замер на пробном лупе) — для перкуссии это
        // слышимый «пуф» на каждый удар при накоплении в цепи.
        dcCoef = (float) std::exp (-kTwoPi * 12.0 / sr);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = clamp (velocity, 0.0f, 1.0f);
        pos = 0.0f;
        repeatIndex = 0;
        active = true;
        dcX1 = dcY1 = 0.0f;
        /// выбор слайса зависит от ноты, поэтому решается здесь же
        chooseSliceFromNote = true;
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        if (bank == nullptr || bank->loops.empty()) { active = false; return; }
        const Loop& lp = bank->loops[(size_t) clamp (p.table, 0, (int) bank->loops.size() - 1)];
        loop = &lp;
        shot = &lp.shot;

        const int n = (int) lp.slices.size();
        if (n == 0) { active = false; return; }

        // ---- выбор слайса
        const bool byNote = p.p[0] <= 0.02f;
        int idx;
        if (byNote)
        {
            idx = (note - lp.shot.rootNote) % n;
            if (idx < 0) idx += n;
        }
        else
        {
            idx = (int) std::lround (clamp (p.p[0], 0.0f, 1.0f) * (float) (n - 1));
        }
        if (idx != sliceIndex) { pos = 0.0f; repeatIndex = 0; }
        sliceIndex = idx;

        const int start = lp.slices[(size_t) idx];
        const int end   = (idx + 1 < n) ? lp.slices[(size_t) (idx + 1)]
                                        : lp.shot.lens[0] - 1;
        sliceStart = start;
        sliceSpan  = std::max (64, end - start);

        // длина воспроизведения: 5…100 % слайса (p[1])
        playLen = (float) sliceSpan * clamp (0.05f + 0.95f * p.p[1], 0.05f, 1.0f);

        // ---- питч
        const double cents = (double) 0.0;
        const double semis = byNote ? (double) p.tuneSemis
                                    : (double) (note - lp.shot.rootNote) + (double) p.tuneSemis;
        rate = (float) std::pow (2.0, (semis + cents / 100.0) / 12.0);
        mip.setup (lp.shot, rate);

        // повторы 1…4 и реверс — из параметров
        repeats = 1 + (int) std::lround (clamp (p.p[2], 0.0f, 1.0f) * 3.0f);
        reverse = p.p[3] > 0.5f;

        gain = kGainTrim * p.level * (0.35f + 0.65f * vel);
        levelSm.setTarget (gain);
    }

    float render (const EngineParams& p) override
    {
        if (! active || shot == nullptr) return 0.0f;

        if (pos >= playLen)
        {
            if (++repeatIndex >= repeats) { active = false; return 0.0f; }
            pos = 0.0f;
        }

        const float in  = reverse ? (playLen - pos) : pos;
        const float abs = (float) sliceStart + in;

        // окно в шкале исходника: 1 мс вход, 5 мс выход
        const float inFade  = (float) (0.001 * sampleRate);
        const float outFade = (float) (0.005 * sampleRate);
        float w = 1.0f;
        if (in < inFade)   w = in / inFade;
        if (in > playLen - outFade) w = std::min (w, (playLen - in) / outFade);

        float s = mip.read (abs) * w * levelSm.next();
        const float y = dcCoef * (dcY1 + s - dcX1);
        dcX1 = s; dcY1 = y;
        s = y;
        pos += rate;
        return s;
    }

    // ---- диагностика для тестов
    int   sliceCount() const noexcept { return loop != nullptr ? (int) loop->slices.size() : 0; }
    int   currentSlice() const noexcept { return sliceIndex; }
    int   sliceStartSample() const noexcept { return sliceStart; }
    float sliceLength() const noexcept { return playLen; }
    float playbackRate() const noexcept { return rate; }
    bool  playing() const noexcept { return active; }

private:
    const LoopBank* bank = nullptr;
    const Loop*     loop = nullptr;
    const OneShot*  shot = nullptr;
    MipReader       mip;

    double sampleRate = 48000.0;
    int    note = 60;
    float  vel  = 1.0f;
    float  pos  = 0.0f;
    int    repeatIndex = 0, repeats = 1;
    int    sliceIndex = 0, sliceStart = 0, sliceSpan = 1;
    float  playLen = 1.0f, rate = 1.0f, gain = 0.7f;
    bool   reverse = false, active = false, chooseSliceFromNote = true;
    float  dcCoef = 0.998f, dcX1 = 0.0f, dcY1 = 0.0f;
    SmoothedValue levelSm;
};

} // namespace orogen
