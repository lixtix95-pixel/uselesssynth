// Память и конечность Delay при быстрой автоматизации и разных sample rate.
// Отдельный ASan/UBSan-стенд без CPU-бенчмарка и без подключения JUCE.
#include "../src/instrument/fx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace orogen;

int main()
{
    int bad = 0, scenarios = 0;
    float peak = 0.0f;
    for (int sr : { 44100, 48000, 96000 })
    {
        PingPongDelay d;
        d.prepare ((double) sr);
        d.setTime (2.0f); d.setMix (0.0f); d.setFeedback (0.95f);
        for (int i = 0; i < sr * 6; ++i)
        {
            if (i % 127 == 0)
            {
                const int k = i / 127;
                const float times[] = { 0.020f, 2.0f, 0.031f, 1.50f, 0.333f };
                d.setTime (times[k % 5]);
                d.setMix ((k % 4) < 2 ? 0.0f : 1.0f);
                d.setFeedback ((k % 3) ? 0.95f : 0.0f);
            }
            const float in = (float) (0.05 * std::sin (kTwoPi * 777.0 * i / sr));
            float l = in, r = -0.7f * in;
            d.process (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) ++bad;
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
        }
        ++scenarios;
    }
    std::printf ("ASan/UBSan Delay: %d sample rates, 18 s, NaN/Inf %d, peak %.6f\n",
                 scenarios, bad, peak);
    return scenarios == 3 && bad == 0 && peak < 2.0f ? 0 : 1;
}
