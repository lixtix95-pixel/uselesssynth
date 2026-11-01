// User Granular steady-state DSP benchmark: run alone, not during a build.
// make build/bench_user_granular && ./build/bench_user_granular
#include "engines/granular.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
using namespace orogen;

int main()
{
    constexpr double hostRate = 48000.0, sourceRate = 44100.0;
    constexpr int voices = 16, frames = 48000 * 3, repetitions = 3;
    std::vector<float> samples ((size_t) sourceRate * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = (float) (0.35 * std::sin (kTwoPi * 220.0 * (double) i / sourceRate));
    auto sample = makeUserSample (std::move (samples), sourceRate, "benchmark");
    if (! sample) return 2;
    volatile float sink = 0;
    int invalid = 0;
    for (float density : { 0.55f, 0.90f })
    {
        double elapsed[repetitions] {};
        for (int run = 0; run < repetitions; ++run)
        {
            EngineParams p;
            p.grainUser = true; p.grainRootNote = 60;
            p.p[0] = .4f; p.p[1] = .8f; p.p[2] = density; p.p[3] = .3f;
            p.grainReverse = .5f;
            std::vector<std::unique_ptr<GranularEngine>> engines;
            for (int i = 0; i < voices; ++i)
            {
                auto engine = std::make_unique<GranularEngine>();
                engine->setUserSample (sample); engine->setSampleRate (hostRate);
                engine->setRandomStreamSeed (noteSlotStreamSeed (i + 1, 0));
                engine->start (48 + i, .8f); engine->prepare (p);
                engines.push_back (std::move (engine));
            }
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < frames; ++f)
            {
                float mixed = 0;
                for (auto& engine : engines) mixed += engine->render (p);
                if (! std::isfinite (mixed)) ++invalid;
                sink = sink + mixed;
            }
            const auto end = std::chrono::steady_clock::now();
            elapsed[run] = 100.0 * std::chrono::duration<double> (end - start).count() / (frames / hostRate);
        }
        std::sort (elapsed, elapsed + repetitions);
        std::printf ("density %.2f, 16 engines, 44.1->48 kHz: %.2f / %.2f / %.2f%% core; median %.2f%%\n",
                     density, elapsed[0], elapsed[1], elapsed[2], elapsed[1]);
    }
    std::printf ("nonfinite %d, sink finite %d\n", invalid, (int) std::isfinite (sink));
    return invalid == 0 && std::isfinite (sink) ? 0 : 1;
}
