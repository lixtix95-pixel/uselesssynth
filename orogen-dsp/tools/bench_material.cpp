// Opt-in FOLLOW-only Material cost probe; excludes voices, other FX and GUI.
// Build: c++ -O3 -std=c++17 -DNDEBUG tools/bench_material.cpp -o /tmp/bench_material
// Run without concurrent builds/tests. Percentages refer to one CPU core.
#include "../src/core/dsp.h"
#include "../src/instrument/fx_material.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

int main()
{
    constexpr int sr = 48000, seconds = 3;
    std::array<float, sr> input {};
    for (int i = 0; i < sr; ++i)
        input[(size_t) i] = (float) (0.06 * std::sin (orogen::kTwoPi * 193.0 * i / sr));
    volatile float sink = 0;
    std::printf ("Material FOLLOW, %d Hz, %d s rendered; process CPU / audio time\n", sr, seconds);
    for (int mode = 0; mode < 4; ++mode)
        for (int copies : { 1, 4 })
            for (bool retune : { false, true })
            {
                std::array<orogen::MaterialFx, 4> fx;
                for (int j = 0; j < copies; ++j)
                {
                    fx[(size_t) j].prepare (sr);
                    fx[(size_t) j].setMode (mode); fx[(size_t) j].setTone (.5f);
                    fx[(size_t) j].setSize (.5f); fx[(size_t) j].setDamp (.3f);
                    fx[(size_t) j].setFeedback (.6f); fx[(size_t) j].setMix (.7f);
                    fx[(size_t) j].setFollow (true); fx[(size_t) j].setMidiPitchHz (orogen::midiToHz (48));
                }
                for (int i = 0; i < sr / 2; ++i)
                {
                    float l = input[(size_t) i], r = l;
                    for (int j = 0; j < copies; ++j) fx[(size_t) j].process (l, r);
                    sink += l * 0.000001f;
                }
                const auto wallStart = std::chrono::steady_clock::now(); const auto cpuStart = std::clock();
                double sum = 0;
                for (int i = 0; i < sr * seconds; ++i)
                {
                    if (retune && i % 6000 == 0)
                        for (int j = 0; j < copies; ++j)
                            fx[(size_t) j].setMidiPitchHz (orogen::midiToHz ((i / 6000) % 2 ? 60 : 48));
                    float l = input[(size_t) (i % sr)], r = l;
                    for (int j = 0; j < copies; ++j) fx[(size_t) j].process (l, r);
                    sum += l + r;
                }
                const double cpu = double (std::clock() - cpuStart) / CLOCKS_PER_SEC;
                const double wall = std::chrono::duration<double> (std::chrono::steady_clock::now() - wallStart).count();
                sink += (float) (sum * 0.000001);
                std::printf ("  mode %d, copies %d, %-6s: %.2f%% of one core (CPU %.3f s, wall %.3f s)\n",
                             mode, copies, retune ? "8 Hz" : "stable", cpu * 100.0 / seconds, cpu, wall);
            }
    return std::isfinite (sink) ? 0 : 1;
}
