// Контент для движка CHOP (итерация 12): лупы с ударами.
//
// В Terrain CHOP — это сэмплер-слайсер: кидаешь луп, он режется по транзиентам,
// куски раскладываются по клавишам. Лупов в поставке нет (как и сэмплов —
// всё синтезируется процедурно), поэтому каждый луп генерируется из событий:
// генератор ЗНАЕТ, где поставил удар (truth), а слайсы ищет ОТДЕЛЬНЫЙ детектор
// по готовому сигналу. Это позволяет проверить детектор честно: сравнить
// найденные границы с истинными, а не с самим собой.
//
// Уровни MIP строятся тем же кодом, что у one-shot'ов (`buildMipLevels`),
// поэтому питч-шифт слайса вверх не даёт алиасинга.
#pragma once

#include "sample_bank.h"

namespace orogen {

// ---------------------------------------------------------------------------
// Детектор атак.
//
// Что важно для слайсера:
//   • граница не должна срезать атаку — поэтому найденная точка отъезжает
//     назад на 3 мс (backtrack);
//   • между двумя ударами не должно быть двух слайсов — минимальный интервал;
//   • тихий хвост не должен считаться ударом — порог адаптивный: он растёт
//     с уровнем сигнала и падает с чувствительностью (sens);
//   • «дребезг» затухающего удара не должен давать второй слайс — гистерезис
//     сбрасывается только ниже 40 % порога, а минимальный интервал 50 мс.
// ---------------------------------------------------------------------------
struct SliceDetect
{
    std::vector<int> starts;        // позиции в отсчётах уровня 0
    float threshold = 0.0f;         // для диагностики
    int   rejected = 0;             // отброшено правилом минимального интервала
};

inline SliceDetect detectSlices (const std::vector<float>& x, double sr,
                                 float sens = 0.5f, int maxSlices = 64)
{
    SliceDetect d;
    if (x.size() < 256) return d;

    // Огибающая — пиковый детектор с удержанием (мгновенный рост, спад 30 мс).
    //
    // Первая версия была однополюсником 1.5 мс по модулю — и давала по два
    // слайса на кик: у синуса 55 Гц огибающая успевает провалиться между
    // полупериодами (каждые 9 мс), гистерезис сбрасывался, следующий полупериод
    // снова перебивал порог. Замер: 8 слайсов вместо 5 в брейке, лишние ровно
    // через 0.116 с после каждого кика.
    const float hold = std::exp (-1.0f / (0.030f * (float) sr));
    std::vector<float> env (x.size(), 0.0f);
    float e = 0.0f;
    float maxEnv = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        e = std::max (std::fabs (x[i]), e * hold);
        env[i] = e;
        maxEnv = std::max (maxEnv, e);
    }
    if (maxEnv < 1.0e-6f) return d;

    // медиана огибающей — оценка «шумового пола» (дорого, но это build-time)
    std::vector<float> sorted = env;
    std::nth_element (sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];

    // порог: доля от максимума (растёт с sens) и не ниже 2 медиан.
    // Проверено на трёх лупах: при доле 6 % от максимума (первая версия)
    // детектор находил 16 «слайсов» в брейке — каждый хэт был ударом, а хэт
    // там тише кика в 3–4 раза. При 30 % — ровно 5 ударов, как в генераторе.
    const float sMax = clamp (sens, 0.0f, 1.0f);
    d.threshold = std::max (maxEnv * (0.50f - 0.40f * sMax), median * 2.0f);

    const int minGap = (int) (0.050 * sr);      // 50 мс
    const int back   = (int) (0.003 * sr);      // откат назад, чтобы не срезать атаку

    bool armed = true;
    int lastStart = -minGap;
    for (size_t i = 1; i < env.size(); ++i)
    {
        if (armed && env[i] > d.threshold && env[i - 1] <= d.threshold)
        {
            const int pos = std::max (0, (int) i - back);
            if (pos - lastStart >= minGap && pos > 0)
            {
                if ((int) d.starts.size() >= maxSlices) break;
                d.starts.push_back (pos);
                lastStart = pos;
            }
            else if (pos > 0)
            {
                ++d.rejected;
            }
            armed = false;
        }
        else if (! armed && env[i] < d.threshold * 0.4f)
        {
            armed = true;
        }
    }
    // Если первый удар не в самом начале, добавляем слайс-«остаток» в 0:
    // в слайсере это удобно (клавиша с началом лупа), а в тестах такие
    // слайсы исключаются из сравнения как служебные.
    if (! d.starts.empty() && d.starts[0] != 0)
        d.starts.insert (d.starts.begin(), 0);
    return d;
}

// ---------------------------------------------------------------------------
enum class LoopKind { Break96, VoxChop, DubStabs, Count };

inline const char* loopName (LoopKind k)
{
    switch (k)
    {
        case LoopKind::Break96:  return "Break 96";
        case LoopKind::VoxChop:  return "Vox Chop";
        case LoopKind::DubStabs: return "Dub Stabs";
        case LoopKind::Count:    break;
    }
    return "?";
}

struct Loop
{
    OneShot shot;                    // levels + огибающая + точки петли
    std::vector<int> slices;         // найденные детектором границы (уровень 0)
    std::vector<int> truth;          // где генератор реально поставил удары
};

namespace loopgen {

struct Hit { double at; int kind; };   // kind: 0 kick, 1 snare, 2 hat, 3 stab, 4 vox

inline void addKick (std::vector<float>& out, double at, double sr, double amp = 0.9)
{
    const int s0 = (int) (at * sr);
    const int n  = (int) (0.28 * sr);
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const size_t idx = (size_t) (s0 + i);
        if (idx >= out.size()) break;
        const double t = (double) i / sr;
        const double f = 55.0 + 110.0 * std::exp (-t / 0.030);   // спад высоты
        phase += kTwoPi * f / sr;
        const double env = std::exp (-t / 0.085);
        out[idx] += (float) (amp * env * std::sin (phase));
    }
}

inline void addSnare (std::vector<float>& out, double at, double sr, Rng& rng, double amp = 0.7)
{
    const int s0 = (int) (at * sr);
    const int n  = (int) (0.20 * sr);
    for (int i = 0; i < n; ++i)
    {
        const size_t idx = (size_t) (s0 + i);
        if (idx >= out.size()) break;
        const double t = (double) i / sr;
        const double env = std::exp (-t / 0.055);
        const double tone = 0.35 * std::sin (kTwoPi * 185.0 * t);
        out[idx] += (float) (amp * env * (0.65 * rng.nextBipolar() + tone));
    }
}

inline void addHat (std::vector<float>& out, double at, double sr, Rng& rng, double amp = 0.30)
{
    const int s0 = (int) (at * sr);
    const int n  = (int) (0.06 * sr);
    float hp = 0.0f, prev = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const size_t idx = (size_t) (s0 + i);
        if (idx >= out.size()) break;
        const double t = (double) i / sr;
        const float nz = rng.nextBipolar();
        hp = 0.85f * (hp + nz - prev);        // однополюсный HP
        prev = nz;
        out[idx] += (float) (amp * std::exp (-t / 0.014) * hp);
    }
}

inline void addVox (std::vector<float>& out, double at, double sr, double f0, double amp = 0.55)
{
    // три форманта, короткий слог
    const int s0 = (int) (at * sr);
    const int n  = (int) (0.26 * sr);
    const double fm[3] = { 700.0, 1220.0, 2600.0 };
    const double g[3]  = { 1.0, 0.55, 0.25 };
    for (int i = 0; i < n; ++i)
    {
        const size_t idx = (size_t) (s0 + i);
        if (idx >= out.size()) break;
        const double t = (double) i / sr;
        const double env = std::min (1.0, t / 0.02) * std::exp (-t / 0.10);
        double acc = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            // амплитуда форманта — гауссово окно вокруг гармоник f0
            const double d = (fm[k] - f0) / 220.0;
            const double w = std::exp (-d * d);
            acc += g[k] * w * std::sin (kTwoPi * fm[k] * t);
        }
        // основной тон
        acc += 0.5 * std::sin (kTwoPi * f0 * t);
        out[idx] += (float) (amp * env * acc);
    }
}

inline void addStab (std::vector<float>& out, double at, double sr, double f0, double amp = 0.6)
{
    const int s0 = (int) (at * sr);
    const int n  = (int) (0.34 * sr);
    double phase = 0.0;
    float lp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const size_t idx = (size_t) (s0 + i);
        if (idx >= out.size()) break;
        const double t = (double) i / sr;
        phase += f0 / sr;
        if (phase >= 1.0) phase -= 1.0;
        const float saw = (float) (2.0 * phase - 1.0);
        // закрывающийся фильтр: стаб «гаснет» сверху
        const float cut = (float) (0.02 + 0.35 * std::exp (-t / 0.06));
        lp += cut * (saw - lp);
        const double env = std::min (1.0, t / 0.004) * std::exp (-t / 0.12);
        out[idx] += (float) (amp * env * lp);
    }
}

inline double bpmToBeat (double bpm) { return 60.0 / bpm; }

} // namespace loopgen

// ---------------------------------------------------------------------------
class LoopBank
{
public:
    std::vector<Loop> loops;

    void buildFactory (double sr = kSampleRate)
    {
        loops.clear();
        for (auto k : { LoopKind::Break96, LoopKind::VoxChop, LoopKind::DubStabs })
            loops.push_back (build (k, sr));
    }

    static Loop build (LoopKind kind, double sr = kSampleRate, float sens = 0.5f)
    {
        Loop loop;
        loop.shot.name = loopName (kind);
        loop.shot.rootNote = 45;
        loop.shot.levels.resize (1);

        std::vector<float>& x = loop.shot.levels[0];
        std::vector<int> truthSamples;
        Rng rng (0xC40Fu ^ (uint32_t) kind);

        switch (kind)
        {
            case LoopKind::Break96:
            {
                const double beat = loopgen::bpmToBeat (96.0);
                const double bar  = beat * 4.0;                 // 2.5 с
                const double dur  = bar + 0.30;
                x.assign ((size_t) (dur * sr), 0.0f);
                // 16-е доли: хэт на каждой, кик 1 и 3.5, снейр 2 и 4
                const double step = beat * 0.25;
                for (int i = 0; i < 16; ++i)
                {
                    const double at = i * step;
                    loopgen::addHat (x, at, sr, rng, (i % 4 == 0) ? 0.34 : 0.20);
                    if (i % 4 == 0) truthSamples.push_back ((int) (at * sr)); // хэты-четверти не считаем
                }
                const double kicks[3] = { 0.0, beat * 1.5, beat * 3.0 };
                const double snares[2] = { beat, beat * 3.0 + beat * 0.5 };
                for (double at : kicks)
                {
                    loopgen::addKick (x, at, sr);
                }
                for (double at : snares)
                {
                    loopgen::addSnare (x, at, sr, rng);
                }
                // истина: объединяем удары по времени и убираем те, что слиплись
                truthSamples.clear();
                for (double at : kicks) truthSamples.push_back ((int) (at * sr));
                for (double at : snares) truthSamples.push_back ((int) (at * sr));
                std::sort (truthSamples.begin(), truthSamples.end());
                break;
            }
            case LoopKind::VoxChop:
            {
                const double step = 0.34;
                const double dur  = step * 8.0 + 0.20;
                x.assign ((size_t) (dur * sr), 0.0f);
                const double f0s[8] = { 220.0, 247.0, 262.0, 294.0, 330.0, 294.0, 262.0, 220.0 };
                for (int i = 0; i < 8; ++i)
                {
                    const double at = 0.10 + i * step;
                    loopgen::addVox (x, at, sr, f0s[i], 0.5);
                    truthSamples.push_back ((int) (at * sr));
                }
                break;
            }
            case LoopKind::DubStabs:
            default:
            {
                const double step = 0.62;
                const double dur  = step * 6.0 + 0.25;
                x.assign ((size_t) (dur * sr), 0.0f);
                const double f0s[6] = { 110.0, 110.0, 146.8, 110.0, 164.8, 98.0 };
                for (int i = 0; i < 6; ++i)
                {
                    const double at = 0.08 + i * step;
                    loopgen::addStab (x, at, sr, f0s[i], 0.55);
                    truthSamples.push_back ((int) (at * sr));
                }
                break;
            }
        }

        // нормализация по пику: удары должны быть одинаково громкими независимо
        // от суммирования событий
        float peak = 0.0f;
        for (float v : x) peak = std::max (peak, std::fabs (v));
        if (peak > 0.0f)
            for (float& v : x) v *= 0.89f / peak;

        buildMipLevels (loop.shot, sr);
        loop.slices = detectSlices (loop.shot.levels[0], sr, sens).starts;
        loop.truth = truthSamples;
        return loop;
    }
};

} // namespace orogen
