// Проверка ЧАСТОТЫ фабричных вокальных источников до движка и FX.
// Частота измеряется по периодам отрендеренного сигнала, а не чтением фазы
// генератора. Вибрато на 110 Гц должно быть ограничено указанной глубиной и
// не увеличиваться с течением времени.
#include "../src/content/sample_bank.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace orogen;

struct Measurement
{
    int periods = 0, bad = 0;
    double minHz = 1.0e9, maxHz = 0.0, maxError = 0.0;
    double lateMin = 1.0e9, lateMax = 0.0, peak = 0.0;
};

static Measurement measure (OneShotKind kind, int sr)
{
    const bool choir = kind == OneShotKind::Choir;
    const double attack = choir ? 6.0 : 8.0, decay = choir ? 1.1 : 1.4;
    const double depth = choir ? 0.004 : 0.002;
    const double omega = kTwoPi * (choir ? 5.2 : 0.7), phase = choir ? 0.7 : 0.0;
    std::vector<float> source;
    renderOneShot (kind, source, (double) sr, 2.0);
    std::vector<double> normal (source.size(), 0.0);
    Measurement m;
    for (size_t i = 1; i < source.size(); ++i)
    {
        const double t = (double) i / sr;
        const double env = std::min (1.0, t * attack) * std::exp (-t * decay);
        if (! std::isfinite (source[i])) ++m.bad;
        m.peak = std::max (m.peak, std::fabs ((double) source[i]));
        normal[i] = source[i] / env;
    }
    const double threshold = 0.65 * *std::max_element (normal.begin() + sr / 5,
                                                        normal.begin() + 9 * sr / 5);
    std::vector<int> indices;
    for (int i = 2; i < (int) normal.size() - 1; ++i)
    {
        if (normal[(size_t) i] <= threshold || normal[(size_t) i] < normal[(size_t) (i - 1)]
            || normal[(size_t) i] <= normal[(size_t) (i + 1)]) continue;
        if (! indices.empty() && i - indices.back() < sr / 180)
        {
            if (normal[(size_t) i] > normal[(size_t) indices.back()]) indices.back() = i;
        }
        else indices.push_back (i);
    }
    std::vector<double> times;
    for (const int i : indices)
    {
        const double a = normal[(size_t) (i - 1)], b = normal[(size_t) i], c = normal[(size_t) (i + 1)];
        const double den = a - 2.0 * b + c;
        const double frac = std::fabs (den) > 1.0e-20 ? 0.5 * (a - c) / den : 0.0;
        const double t = (i + frac) / sr;
        if (t > 0.12 && t < 1.90) times.push_back (t);
    }
    for (size_t i = 1; i < times.size(); ++i)
    {
        const double t = (times[i] + times[i - 1]) * 0.5;
        const double hz = 1.0 / (times[i] - times[i - 1]);
        const double expected = 110.0 * (1.0 + depth * std::sin (omega * t + phase));
        ++m.periods;
        m.minHz = std::min (m.minHz, hz);
        m.maxHz = std::max (m.maxHz, hz);
        m.maxError = std::max (m.maxError, std::fabs (hz - expected));
        if (t >= 1.4 && t <= 1.8)
        {
            m.lateMin = std::min (m.lateMin, hz);
            m.lateMax = std::max (m.lateMax, hz);
        }
    }
    return m;
}

int main()
{
    int failed = 0;
    for (int sr : { 44100, 48000, 96000 })
        for (auto kind : { OneShotKind::Choir, OneShotKind::VoxAh })
        {
            const auto m = measure (kind, sr);
            const bool choir = kind == OneShotKind::Choir;
            // Порог 0.12 Гц учитывает квантование пика и меняющиеся форманты;
            // ошибка старой фазы t*vib достигает 25.8 / 1.4 Гц.
            const bool ok = m.periods >= 175 && m.maxError < 0.12 && m.bad == 0 && m.peak <= 0.95001;
            std::printf ("  [%s] %s %d Hz: %d periods, all %.3f..%.3f Hz, late %.3f..%.3f Hz, "
                         "max |err| %.3f Hz, peak %.6f, NaN %d\n",
                         ok ? " OK " : "FAIL", choir ? "Choir" : "Vox", sr, m.periods,
                         m.minHz, m.maxHz, m.lateMin, m.lateMax, m.maxError, m.peak, m.bad);
            if (! ok) ++failed;
        }
    // Отдельно — производная интегрированной фазы, в том числе неподвижное
    // вибрато при rate=0. Проверяем на временах до 10 с, не только в начале.
    double worstDerivativeError = 0.0;
    for (double rate : { 0.0, 0.7, 5.2, 20.0 })
        for (double t : { 0.1, 0.8, 1.8, 10.0 })
        {
            constexpr double eps = 1.0e-5;
            const auto phaseAt = [rate] (double x) {
                return sinusoidalVibratoCycles (110.0, x, rate, 0.004, 0.7);
            };
            const double derivative = (phaseAt (t + eps) - phaseAt (t - eps)) / (2.0 * eps);
            const double frequency = 110.0 * (1.0 + 0.004 * std::sin (kTwoPi * rate * t + 0.7));
            worstDerivativeError = std::max (worstDerivativeError, std::fabs (derivative - frequency));
        }
    const bool integralOk = worstDerivativeError < 1.0e-5
        && sinusoidalVibratoCycles (110.0, 0.0, 5.2, 0.004, 0.7) == 0.0;
    std::printf ("  [%s] derivative/zero-rate, 16 cases: max |err| %.8f Hz\n",
                 integralOk ? " OK " : "FAIL", worstDerivativeError);
    if (! integralOk) ++failed;
    std::printf ("Vocal vibrato: %d OK, %d FAIL\n", 7 - failed, failed);
    return failed ? 1 : 0;
}
