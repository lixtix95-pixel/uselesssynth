// Concurrent preparation/publication of independent FX nodes while rendering.
#include "../src/instrument/fx.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

using namespace orogen;
int main()
{
    auto fx = std::make_unique<FxChain>();
    fx->prepare (48000.0);
    FxChain::CopySettings audioSettings {};
    for (auto& s : audioSettings)
    {
        s.enabled.fill (false);
        s.enabled[(size_t) FxChain::Drive] = true;
        s.enabled[(size_t) FxChain::DelayModule] = true;
        s.enabled[(size_t) FxChain::ChorusModule] = true;
        s.enabled[(size_t) FxChain::Glitch] = true;
        s.drive = .7f; s.delayMix = .4f; s.chorusMix = .3f; s.glitchMix = .2f;
        s.chorusModel = 1; s.chorusMode = 2; s.glitchTimingMode = 1;
    }
    fx->set (audioSettings[0]);
    const int modules[] = { FxChain::Drive, FxChain::DelayModule, FxChain::ChorusModule, FxChain::Glitch, FxChain::Equalizer };
    std::atomic<bool> run { true };
    std::atomic<int> frames { 0 }, bad { 0 };
    std::thread audio ([&] {
        FxChain::RackRanks ranks {};
        for (int n = 0; n < 1600; ++n)
        {
            ranks.fill (0);
            for (int i = 0; i < 5; ++i) ranks[(size_t) (FxChain::ModuleCount + modules[(i + n/50) % 5])] = i + 1;
            fx->setRack (ranks, audioSettings);
            fx->setTempo (90.0 + (n % 3) * 20.0);
            for (int i = 0; i < 64; ++i)
            {
                const int t = n * 64 + i;
                float l = .18f * std::sin (t * .031f), r = l * .8f;
                fx->process (l, r);
                if (! std::isfinite (l) || ! std::isfinite (r)) ++bad;
                ++frames;
            }
        }
        run.store (false, std::memory_order_release);
    });
    const FxChain::Settings initial = audioSettings[1];
    std::thread worker ([&] {
        for (const int m : modules) fx->ensureCopy (FxChain::ModuleCount + m, initial);
        while (run.load (std::memory_order_acquire))
            for (const int m : modules) fx->ensureCopy (FxChain::ModuleCount + m, initial);
    });
    audio.join(); worker.join();
    const bool ok = frames == 1600 * 64 && bad == 0 && fx->getCopyGeneration() == 5;
    std::printf ("FX handoff: frames=%d, prepared=%u/5, nonfinite=%d: %s\n",
                 frames.load(), fx->getCopyGeneration(), bad.load(), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
