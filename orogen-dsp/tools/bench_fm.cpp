// Opt-in FM-only real-time cost probe. Build with:
//   c++ -O3 -std=c++17 -DNDEBUG tools/bench_fm.cpp -o /tmp/bench_fm
// Run without concurrent builds/tests. Percentages are for one CPU core and
// cover 16 continuously active FM engine instances, not a complete plugin.
#include "../src/engines/fm.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

int main()
{
    constexpr int voices = 16;
    constexpr int sr = 48000;
    constexpr int seconds = 3;
    volatile float sink = 0;
    std::printf("16 FM engines, %d Hz, %d s rendered; process CPU / real-time audio\n", sr, seconds);
    for (int algorithm = 0; algorithm <= 8; ++algorithm)
    {
        orogen::EngineParams p;
        p.fmAlgorithm = algorithm;
        p.level = 1.0f; p.detune = 0.0f;
        p.p[0] = (2.0f - 0.5f) / 15.5f;
        p.p[1] = 0.45f; p.p[2] = 0.35f; p.p[3] = 0.15f;
        p.fmRatioC = 1.0f; p.fmRatioB1 = 3.0f; p.fmRatioB2 = 2.0f; p.fmMix = 0.4f;
        std::array<orogen::FmEngine, voices> engines;
        for (int i = 0; i < voices; ++i)
        {
            engines[i].setSampleRate(sr);
            engines[i].start(48 + i, 0.8f);
            engines[i].prepare(p);
        }
        for (int n = 0; n < 4096; ++n)
            for (auto& engine : engines) sink += engine.render(p) * 0.000001f;
        const auto wallStart = std::chrono::steady_clock::now();
        const auto cpuStart = std::clock();
        double sum = 0;
        for (int n = 0; n < sr * seconds; ++n)
            for (auto& engine : engines) sum += engine.render(p);
        const double cpu = double(std::clock() - cpuStart) / CLOCKS_PER_SEC;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
        sink += float(sum * 0.000001);
        std::printf("  %-6s %d: %.2f%% of one core; CPU %.3f s; wall %.3f s\n",
                    algorithm == 0 ? "Legacy" : "Algo", algorithm, cpu * 100.0 / seconds, cpu, wall);
    }
    return std::isfinite(sink) ? 0 : 1;
}
