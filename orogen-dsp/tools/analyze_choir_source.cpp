// Диагностика частоты фабричного Choir. Не меняет источник и не является
// тестом отсутствия клиппинга: измеряет периоды исходной формы, до движка.
#include "../src/content/sample_bank.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace orogen;

int main()
{
    constexpr int sr = 48000;
    const OneShot shot = buildOneShot (OneShotKind::Choir, sr, 2.0);
    const auto& x = shot.levels[0];
    std::vector<double> normal (x.size());
    for (size_t i = 1; i < x.size(); ++i)
    {
        const double t = (double) i / sr;
        // Убираем только известную плавную амплитудную огибающую источника,
        // чтобы одинаковый порог пиков работал и на затухающем хвосте.
        const double env = std::min (1.0, t * 6.0) * std::exp (-t * 1.1);
        normal[i] = x[i] / env;
    }
    const auto first = normal.begin() + sr / 5;
    const auto last  = normal.begin() + sr * 9 / 5;
    const double threshold = 0.65 * *std::max_element (first, last);
    std::vector<int> peakIndex;
    for (int i = 2; i < (int) normal.size() - 1; ++i)
    {
        if (normal[(size_t) i] <= threshold || normal[(size_t) i] < normal[(size_t) (i - 1)]
            || normal[(size_t) i] <= normal[(size_t) (i + 1)]) continue;
        // Один основной пик за период; при нескольких близких оставить максимум.
        if (! peakIndex.empty() && i - peakIndex.back() < sr / 180)
        {
            if (normal[(size_t) i] > normal[(size_t) peakIndex.back()]) peakIndex.back() = i;
        }
        else peakIndex.push_back (i);
    }
    std::vector<double> times;
    for (const int p : peakIndex)
    {
        const double a = normal[(size_t) (p - 1)], b = normal[(size_t) p], c = normal[(size_t) (p + 1)];
        const double denom = a - 2.0 * b + c;
        const double fraction = std::fabs (denom) > 1.0e-20 ? 0.5 * (a - c) / denom : 0.0;
        const double t = (p + fraction) / sr;
        if (t > 0.12 && t < 1.90) times.push_back (t);
    }
    constexpr double f0 = 110.0;
    const double omega = kTwoPi * 5.2;
    double lo = 1.0e9, hi = 0.0, loT = 0.0, hiT = 0.0, maxError = 0.0;
    int periods = 0, inWindow = 0;
    for (size_t i = 1; i < times.size(); ++i)
    {
        const double t = (times[i] + times[i - 1]) * 0.5;
        const double f = 1.0 / (times[i] - times[i - 1]);
        // Производная реальной фазы f0*t*(1 + .004*sin(omega*t+.7)).
        const double predicted = f0 * (1.0 + 0.004 * std::sin (omega * t + 0.7)
                                      + 0.004 * omega * t * std::cos (omega * t + 0.7));
        maxError = std::max (maxError, std::fabs (f - predicted));
        ++periods;
        if (t >= 1.4 && t <= 1.8)
        {
            if (f < lo) { lo = f; loT = t; }
            if (f > hi) { hi = f; hiT = t; }
            ++inWindow;
        }
    }
    if (periods < 100 || inWindow < 20) { std::fprintf (stderr, "недостаточно пиков для измерения\n"); return 1; }
    std::printf ("Choir, источник 48 кГц, root MIDI %d / %.1f Гц, без двигателя и FX\n", shot.rootNote, f0);
    std::printf ("%d периодов; t=1.4..1.8 с: %.3f Гц (t=%.5f) .. %.3f Гц (t=%.5f), %d периодов\n",
                 periods, lo, loT, hi, hiT, inWindow);
    std::printf ("Макс. |частота по периоду - производная фазы| = %.3f Гц; номинальный множитель vib отдельно: 109.560..110.440 Гц\n",
                 maxError);
    std::printf ("Это измерение исходника, не измерение пиков или клиппинга VST3.\n");
    return 0;
}
