// ДВИЖОК 8: Organics — «живой» слой: запись + тело + воздух.
//
// В Terrain Organics — это не синтез с нуля, а достраивание записи до
// инструмента: к сэмплу добавляется тело (резонансы корпуса), воздух
// (шумовая составляющая) и «дыхание» (медленный дрейф).
//
// Слои:
//   1. SOURCE  — one-shot из банка через MIP-уровни (как Sample);
//   2. BODY    — 3 параллельных полосовых резонатора, частоты привязаны
//                к ноте (корпус масштабируется вместе с инструментом);
//   3. AIR     — шум, отфильтрованный ФНЧ, амплитуда ведётся огибающей
//                самой записи: шум «дышит» вместе с телом, а не живёт отдельно;
//   4. BLOOM   — нарастание атаки (запись входит не мгновенно) и медленный
//                дрейф высоты.
//
// Почему это не «просто реверберация»: тело — резонаторы с высокой
// добротностью, настроенные на ноту, поэтому оно усиливает конкретные
// гармоники записи, а не размазывает всё в кашу.
#pragma once

#include "engine.h"
#include "mip_reader.h"
#include "../content/sample_bank.h"

namespace orogen {

class OrganicsEngine final : public EngineBase
{
public:
    static constexpr float kGainTrim = 0.13f;

    void setBank (const SampleBank* b) noexcept { bank = b; }

    const char* name() const override { return "Organics"; }
    float costFactor() const override { return 2.0f; }

    void setSampleRate (double sr) override
    {
        sampleRate = sr;
        airSm.setTime (sr, 25.0);
        bodySm.setTime (sr, 35.0);
    }

    void start (int midiNote, float velocity) override
    {
        note = midiNote;
        vel  = velocity;
        pos = 0.0f;
        bloomGain = 0.0f;
        envComp = 1.0f;
        envCompCountdown = 0;
        for (auto& b : body) { b.z1 = b.z2 = 0.0f; }
        for (auto& v : airLp) v = 0.0f;
        needSnap = true;
        active = true;
        rng = Rng { 0x0A11CE5u };
        driftPhase = rng.nextFloat();
    }

    void stop() override { active = false; }

    void prepare (const EngineParams& p) override
    {
        if (bank == nullptr || bank->shots.empty()) return;
        const OneShot& s = bank->shots[(size_t) clamp (p.table, 0, (int) bank->shots.size() - 1)];
        // MIP sinc требует достаточно данных; неподготовленный/слишком короткий
        // источник безопасно даёт тишину, а не чтение вне буфера.
        if (s.levels.empty() || s.lens.empty() || s.lens[0] < SincKernel::kTaps + 8)
        {
            shot = nullptr;
            return;
        }
        shot = &s;

        baseRate = (float) std::pow (2.0, ((double) note - (double) s.rootNote + (double) p.tuneSemis) / 12.0);
        mip.setup (s, baseRate);

        // Приведение громкости источника к общему уровню: у перкуссивных
        // записей (Wood Knock) тело короче и тише, чем у пэдов — без этого
        // разброс между контентом 16 дБ (замер в tests/test_organics.cpp).
        // Нормировка по ПИКОВОМУ уровню записи, а не по медиане: у перкуссии
        // (Wood Knock) медиана огибающей равна нулю, и по ней нормировка
        // улетала в предел. Пик огибающей — устойчивая характеристика.
        // ---- Петля: весь файл, длина выровнена по периоду основного тона.
        //
        // Почему так, а не «умная» петля по ровному участку (auto-loop):
        // контент — двухсекундные one-shot'ы, и любая короткая петля на ноте 81
        // (запись проходится в 8 раз быстрее) превращается в жужжание 10–15 Гц,
        // а на затухающих записях — в пульсацию уровня. Петля по всему файлу
        // с длинным (половинным) кроссфейдом даёт ровный уровень; на высоких
        // нотах, где такая петля короче 0.3 с, кроссфейд сужается до периода,
        // чтобы не плодить боковые полосы (метрика алиасинга).
        const int envN = (int) s.env.size();
        const int winN = std::min (envN, std::max (4, envN / 4));
        const int bestStart = clamp (s.loopStart / std::max (1, s.envStep), 0, std::max (0, envN - winN));
        loopPeriod = (float) std::max (32, (int) std::lround (sampleRate / std::max (20.0, (double) midiToHz (s.rootNote))));

        const float usable = std::max (16.0f, (float) s.lens[0] - 64.0f);
        loopStartPos = 0.0f;
        const float quantizedEnd = std::floor (usable / loopPeriod) * loopPeriod;
        // Запись может быть короче периода: нулевую петлю строить нельзя.
        loopEndPos = quantizedEnd > 0.0f ? quantizedEnd : usable;
        const float loopOutSec = (loopEndPos - loopStartPos) / std::max (1.0e-3f, baseRate) / (float) sampleRate;
        loopWide = loopOutSec >= 0.3f;

        double acc = 0.0; int cnt = 0;
        for (int i = bestStart; i < bestStart + winN; ++i) { acc += (double) s.env[(size_t) i] * s.env[(size_t) i]; ++cnt; }
        // Если запись короче окна огибающей, банк уже сохранил RMS всего файла.
        const float loopRms = cnt > 0 ? (float) std::sqrt (acc / (double) cnt) : s.bodyRms;
        // потолок 12x: у «стеклянного» удара хвост тише тела на 20 дБ, и
        // при прежнем потолке 4x он оставался на 12 дБ тише остальных тембров
        srcScale = clamp (0.25f / std::max (1.0e-4f, loopRms), 0.25f, 12.0f);
        loopRefEnv = std::max (1.0e-4f, loopRms);

        // p[0] — тело, p[1] — воздух, p[2] — дыхание/дрейф, p[3] — нарастание (bloom)
        const float bodyT  = clamp (p.p[0], 0.0f, 1.0f);
        const float airT   = clamp (p.p[1], 0.0f, 1.0f);
        driftAmt           = clamp (p.p[2], 0.0f, 1.0f);
        bloomTime          = 0.002f + clamp (p.p[3], 0.0f, 1.0f) * 0.45f;

        if (needSnap) { airSm.snap (airT); bodySm.snap (bodyT); needSnap = false; }
        else          { airSm.setTarget (airT); bodySm.setTarget (bodyT); }

        // Корпус: три форманты, привязанные к ноте (инструмент больше — корпус ниже)
        const double f0 = midiToHz (note);
        const double ratios[3] = { 3.0, 5.2, 8.7 };
        const double gains[3]  = { 1.0, 0.55, 0.3 };
        for (int i = 0; i < 3; ++i)
        {
            double f = f0 * ratios[i];
            if (f > 0.44 * sampleRate) f = 0.44 * sampleRate * 0.5;    // выше Найквиста — не строим
            body[(size_t) i].freq = (float) f;
            body[(size_t) i].gain = (float) gains[i];
            body[(size_t) i].q    = 6.0f + 10.0f * bodyT;
        }

        bloomCoef = (float) (1.0 / (bloomTime * sampleRate));
        gain = kGainTrim * (0.4f + 0.6f * clamp (p.level, 0.0f, 1.0f));
        dcCoef = (float) std::exp (-kTwoPi * 8.0 / sampleRate);
        envCompCoef = 1.0f - (float) std::exp (-1.0 / (0.008 * sampleRate));   // 8 мс
    }

    inline float render (const EngineParams&) override
    {
        if (! active || shot == nullptr) return 0.0f;

        // ---- 1. источник (с дрейфом высоты)
        driftPhase += (float) kTwoPi * 0.55f / (float) sampleRate;
        if (driftPhase > (float) kTwoPi) driftPhase -= (float) kTwoPi;
        const float drift = 1.0f + driftAmt * 0.006f * std::sin (driftPhase);   // exact: на границе метрики алиасинга

        const float rate = baseRate * drift;

        // Заворот записи — с кроссфейдом.
        //
        // Было: `pos = 0` по достижении конца. На коротких источниках (Wood
        // Knock, Glass Hit) склейка случалась десятки раз в секунду, и каждый
        // раз сигнал прыгал с хвоста на атаку — это слышалось как треск
        // и «пулемёт» вместо инструмента. Теперь последние `fade` отсчётов
        // записи плавно подмешивают её начало, а позиция после склейки
        // перескакивает ровно на `fade` — выход непрерывен.
        // Петля — по ХВОСТУ записи, а не с нуля: атака проигрывается один раз,
        // дальше идёт участок [loopStart, len). Для перкуссивных источников
        // (Wood Knock) заворот «в начало» давал повторный удар каждые полсекунды
        // — тот самый «пулемёт».
        const float loopStart = loopStartPos, loopEnd = loopEndPos;
        const float loopLength = loopEnd - loopStart;
        const float wantedFade = loopWide ? std::max (256.0f, loopLength * 0.5f)
                                         : std::max (64.0f, loopPeriod);
        // На короткой записи fade не может занимать всю петлю: тогда период
        // после склейки становится нулевым и позиция улетает за её конец.
        const float fade = std::min (wantedFade, loopLength * 0.5f);
        float src = mip.read (pos);
        if (pos >= loopEnd - fade)
        {
            // равномощный кроссфейд: сумма двух копий не «просаживается» в
            // середине (линейный давал −3 дБ в точке склейки — слышно как впадина)
            const float w = (pos - (loopEnd - fade)) / fade;
            const float wa = 0.5f + 0.5f * fastCosTbl (w * 0.5f);
            const float wb = 0.5f - 0.5f * fastCosTbl (w * 0.5f);
            // Вторая ветвь идёт от начала петли к loopStart + fade.
            // Отрицательная позиция pos-loopEnd раньше заворачивалась обратно
            // в ХВОСТ у MipReader, а затем скачком перескакивала к началу.
            const float nextPos = loopStart + (pos - (loopEnd - fade));
            src = src * wa + mip.read (nextPos) * wb;
        }
        pos += rate;
        if (pos >= loopEnd)
        {
            // Остаток шага сохраняем; на короткой петле один шаг может
            // перескочить несколько оборотов. Обычный случай обходится без fmod.
            float overshoot = pos - loopEnd;
            const float cycle = loopLength - fade;
            if (overshoot >= cycle) overshoot = std::fmod (overshoot, cycle);
            pos = loopStart + fade + overshoot;
        }

        // У источника короче окна 10 мс огибающей нет: его RMS уже вычислен
        // при сборке банка, вместо фиктивной единицы используем это значение.
        const float env = shot->env.empty() ? shot->bodyRms : shot->envAt (pos);

        // Компенсация огибающей внутри петли.
        //
        // Записи — затухающие one-shot'ы. Петля по хвосту возвращает уровень
        // скачком: у Fm Tine это давало пульсацию ~10 дБ раз в секунду сразу
        // после затухания атаки — слышно как «дыхание» не в такт и как артефакт.
        // Умножаем источник на loopRef/env: громкость петли постоянна, а тембр
        // остаётся от записи. Обновляем раз в 32 отсчёта (огибающая медленная),
        // потолок 4x — чтобы не вытягивать шум из тишины.
        if (--envCompCountdown <= 0)
        {
            envCompCountdown = 32;
            envCompTarget = clamp (loopRefEnv / std::max (1.0e-4f, env), 0.35f, 12.0f);
        }
        // плавно, а не ступенькой раз в 32 отсчёта: ступенька давала боковые
        // полосы (метрика алиасинга на ноте 69 падала с −64 до −50 дБ)
        envComp += (envCompTarget - envComp) * envCompCoef;

        // ---- 2. bloom: нарастание атаки
        if (bloomGain < 1.0f) { bloomGain += bloomCoef; if (bloomGain > 1.0f) bloomGain = 1.0f; }

        // ---- 3. тело: три параллельных BP
        const float bodyAmt = bodySm.next();
        float bodyOut = 0.0f;
        if (bodyAmt > 0.01f)
        {
            for (auto& b : body)
            {
                // полосовой резонатор (двухполюсный) с нормировкой на резонанс
                const float w = (float) kTwoPi * b.freq / (float) sampleRate;
                const float r = std::exp (-w / (2.0f * b.q));
                const float a1 = 2.0f * r * std::cos (w);
                const float a2 = -r * r;
                const float drive = (1.0f - r) * 4.0f;
                float y = drive * src + a1 * b.z1 + a2 * b.z2;
                y = antiDenormal (y);
                b.z2 = b.z1; b.z1 = y;
                bodyOut += y * b.gain;
            }
            bodyOut *= 3.0f;
        }

        // ---- 4. воздух: шум по огибающей записи, ФНЧ 6 кГц
        const float airAmt = airSm.next();
        float air = 0.0f;
        if (airAmt > 0.001f)
        {
            // 4 каскада однополюсника: 24 дБ/окт. Два каскада оставляли шум
            // на 9–20 кГц, и это измерялось как «алиасинг -22 дБ» —
            // на самом деле просто неотфильтрованный шум.
            const float n = rng.nextBipolar();
            airLp[0] += (n - airLp[0]) * 0.35f;
            airLp[1] += (airLp[0] - airLp[1]) * 0.35f;
            airLp[2] += (airLp[1] - airLp[2]) * 0.35f;
            airLp[3] += (airLp[2] - airLp[3]) * 0.35f;
            airLp[4] += (airLp[3] - airLp[4]) * 0.35f;
            airLp[5] += (airLp[4] - airLp[5]) * 0.35f;
            air = airLp[5] * env * airAmt * 28.0f;
        }

        const float out = (src * srcScale * envComp * (1.0f - 0.25f * bodyAmt) + bodyOut * bodyAmt + air) * bloomGain;
        const float y = out - dcX + dcCoef * dcY;
        dcX = out; dcY = y;
        return y * gain;
    }

private:
    friend struct OrganicsTestProbe;
    struct Body
    {
        float freq = 300.0f, q = 10.0f, gain = 1.0f;
        float z1 = 0.0f, z2 = 0.0f;
    };

    const SampleBank* bank = nullptr;
    const OneShot*    shot = nullptr;
    MipReader         mip;
    Body              body[3];

    double sampleRate = 48000.0;
    int    note = 60;
    float  vel = 1.0f;
    bool   active = false, needSnap = true;

    float  pos = 0.0f, baseRate = 1.0f;
    float  driftAmt = 0.3f, driftPhase = 0.0f;
    float  bloomTime = 0.02f, bloomCoef = 0.001f, bloomGain = 0.0f;
    float  airLp[6] {};
    float  srcScale = 1.0f, loopRefEnv = 0.25f, envComp = 1.0f, envCompTarget = 1.0f, envCompCoef = 0.01f;
    float  loopStartPos = 48000.0f, loopEndPos = 96000.0f, loopPeriod = 436.0f;
    bool   loopWide = false;
    int    envCompCountdown = 0;
    float  gain = 0.55f, dcX = 0.0f, dcY = 0.0f, dcCoef = 0.999f;

    SmoothedValue airSm, bodySm;
    Rng rng { 0x0A11CE5u };
};

} // namespace orogen
