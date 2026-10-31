// Regression tests for independent, genuinely ordered FX instances.
#include "../src/instrument/fx.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

static std::atomic<bool> watchAlloc { false };
static std::atomic<int> allocs { 0 };
void* operator new (std::size_t n) { if (watchAlloc.load()) ++allocs; if (auto* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n) { return ::operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace orogen;
static int pass = 0, fail = 0;
static void check (bool b, const char* name, double value = 0)
{
    std::printf ("[%s] %s (%.9f)\n", b ? "OK" : "FAIL", name, value);
    b ? ++pass : ++fail;
}
static FxChain::Settings settings()
{
    FxChain::Settings s;
    s.enabled.fill (false);
    s.drive = 0.75f; s.driveMix = 0.9f; s.driveTone = 0.35f;
    s.eqLow = 12.0f; s.eqHigh = -11.0f;
    s.chorusMix = 0.45f; s.chorusModel = 1; s.chorusMode = 2;
    s.delayMix = 0.45f; s.reverbMix = 0.45f; s.plateMix = 0.45f;
    s.combMix = 0.35f; s.materialMix = 0.4f; s.spectreMix = 0.5f;
    s.spectreWindow = 1; s.robinMix = 0.4f; s.robinSemis = 7.0f;
    s.glitchMix = 0.4f; s.glitchTimingMode = 1;
    return s;
}
static float input (int i)
{
    return 0.21f * std::sin (i * 0.043f) + 0.14f * std::sin (i * 0.17f) + ((i % 751) == 0 ? 0.12f : 0.0f);
}
static double diffRms (const std::vector<float>& a, const std::vector<float>& b)
{
    double sum = 0; for (size_t i = 4000; i < a.size(); ++i) { double d = a[i] - b[i]; sum += d*d; }
    return std::sqrt (sum / double (a.size() - 4000));
}
static std::vector<float> render (FxChain::RackRanks ranks, FxChain::CopySettings copies, bool prepareDuplicate)
{
    auto fx = std::make_unique<FxChain>();
    fx->prepare (48000); fx->set (copies[0]);
    if (prepareDuplicate) fx->ensureCopy (FxChain::ModuleCount + FxChain::Drive, copies[1]);
    fx->setRack (ranks, copies);
    std::vector<float> result (24000);
    for (int i = 0; i < (int) result.size(); ++i)
    {
        float l = input (i), r = 0.73f * l;
        fx->process (l, r); result[(size_t) i] = l;
    }
    return result;
}
int main()
{
    {
        auto a = std::make_unique<FxChain>(), b = std::make_unique<FxChain>();
        a->prepare (48000); b->prepare (48000);
        auto s = settings(); s.enabled.fill (true); s.spectreMix = 0; // avoid long transform in this parity run
        a->set (s); b->set (s);
        FxChain::CopySettings copies {}; copies[0] = s;
        b->setRack (FxChain::defaultRanks(), copies);
        int mismatches = 0;
        for (int i = 0; i < 32000; ++i)
        {
            float l1 = input(i), r1 = l1 * 0.8f, l2 = l1, r2 = r1;
            a->process (l1, r1); b->process (l2, r2);
            if (l1 != l2 || r1 != r2) ++mismatches;
        }
        check (mismatches == 0 && a->latency() == b->latency(), "canonical legacy graph is bit-identical", mismatches);
    }
    auto s = settings(); s.enabled[(size_t) FxChain::Drive] = true; s.enabled[(size_t) FxChain::Equalizer] = true;
    FxChain::CopySettings copies {}; copies[0] = s;
    FxChain::RackRanks a {}, b {};
    a[(size_t) FxChain::Drive] = 1; a[(size_t) FxChain::Equalizer] = 2;
    b[(size_t) FxChain::Equalizer] = 1; b[(size_t) FxChain::Drive] = 2;
    const auto first = render (a, copies, false), second = render (b, copies, false);
    const double reorderDiff = diffRms (first, second);
    check (reorderDiff > 0.001, "EQ before/after Drive changes real audio", reorderDiff);

    FxChain::RackRanks two = a; two[(size_t) (FxChain::ModuleCount + FxChain::Drive)] = 3;
    copies[1] = s; copies[1].drive = 0.18f; copies[1].enabled[(size_t) FxChain::Drive] = true;
    const auto quietCopy = render (two, copies, true);
    copies[1].drive = 0.91f;
    const auto hotCopy = render (two, copies, true);
    const double independentDiff = diffRms (quietCopy, hotCopy);
    check (independentDiff > 0.001, "second Drive has its own settings and DSP state", independentDiff);

    {
        auto switched = std::make_unique<FxChain>(), control = std::make_unique<FxChain>();
        switched->prepare (48000); control->prepare (48000);
        auto fx = settings(); fx.enabled.fill (false);
        fx.enabled[(size_t) FxChain::Drive] = fx.enabled[(size_t) FxChain::Equalizer] = true;
        fx.drive = .82f; fx.eqLow = 1.0f; fx.eqHigh = -.5f;
        FxChain::CopySettings c {}; c[0] = fx;
        FxChain::RackRanks before {}; before[FxChain::Drive] = 1; before[FxChain::Equalizer] = 2;
        auto after = before; after[FxChain::Drive] = 2; after[FxChain::Equalizer] = 1;
        switched->setRack (before, c); control->setRack (before, c);
        double previous = 0.0, maximumStep = 0.0, firstDiff = 0.0, midpoint = 1.0;
        for (int i = 0; i < 12800; ++i)
        {
            if (i == 12000) switched->setRack (after, c);
            float l = .4f * std::sin ((float) i * .017f), r = .3f * std::sin ((float) i * .013f);
            float referenceL = l, referenceR = r;
            switched->process (l, r); control->process (referenceL, referenceR);
            if (i >= 12000)
            {
                maximumStep = std::max (maximumStep, std::fabs ((double) l - previous));
                if (i == 12000) firstDiff = std::fabs ((double) l - referenceL);
                if (i == 12287) midpoint = std::fabs ((double) l) + std::fabs ((double) r);
            }
            previous = l;
        }
        check (firstDiff < .004 && midpoint < 1.0e-6 && maximumStep < .08,
               "reordering fades to silence over 6 ms then back, without a large sample jump", maximumStep);
    }
    {
        auto rack = std::make_unique<FxChain>(); rack->prepare (48000);
        auto base = settings(); base.enabled.fill (false); rack->set (base);
        FxChain::CopySettings c {}; c[0] = base; c[1] = settings(); c[1].enabled.fill (false);
        const int node = FxChain::ModuleCount + FxChain::Robin;
        c[1].enabled[(size_t) FxChain::Robin] = true;
        rack->ensureCopy (node, c[1]);
        FxChain::RackRanks ranks {}; ranks[(size_t) node] = 1;
        rack->setRack (ranks, c);
        check (rack->latency() == 2048, "base dry latency plus duplicate ROBIN are reported", rack->latency());
        ranks[(size_t) FxChain::Robin] = 2;
        rack->setRack (ranks, c);
        check (rack->latency() == 2048, "present base ROBIN is not double-counted", rack->latency());
    }
    {
        auto reference = std::make_unique<FxChain>();
        reference->prepare (48000);
        auto active = settings(); active.enabled.fill (false);
        active.enabled[(size_t) FxChain::ChorusModule] = true;
        active.enabled[(size_t) FxChain::Spectre] = true;
        reference->set (active);
        const int expectedLatency = reference->latency();
        auto rack = std::make_unique<FxChain>(); rack->prepare (48000);
        auto base = settings(); base.enabled.fill (false); rack->set (base);
        FxChain::CopySettings c {}; c[0] = base; c[1] = active;
        const int chorus = FxChain::ModuleCount + FxChain::ChorusModule;
        const int spectre = FxChain::ModuleCount + FxChain::Spectre;
        rack->ensureCopy (chorus, c[1]); rack->ensureCopy (spectre, c[1]);
        FxChain::RackRanks ranks {}; ranks[(size_t) chorus] = 1; ranks[(size_t) spectre] = 2;
        rack->setRack (ranks, c);
        check (rack->latency() == expectedLatency && expectedLatency > 1024,
               "firmware Chorus and Spectre copy latencies sum with fixed dry latency", rack->latency());
    }
    {
        auto rack = std::make_unique<FxChain>(); rack->prepare (48000);
        auto base = settings(); base.enabled.fill (false); rack->set (base);
        FxChain::CopySettings c {}; c[0] = base; c[1] = settings();
        int good = 0, nonfinite = 0;
        for (int module = 0; module < FxChain::ModuleCount; ++module)
        {
            c[1].enabled.fill (false); c[1].enabled[(size_t) module] = true;
            const int node = FxChain::ModuleCount + module;
            rack->ensureCopy (node, c[1]);
            FxChain::RackRanks ranks {}; ranks[(size_t) node] = 1;
            rack->setRack (ranks, c);
            const int before = nonfinite;
            for (int i = 0; i < 3600; ++i)
            {
                float l = input (i), r = 0.7f * l; rack->process (l, r);
                if (! std::isfinite (l) || ! std::isfinite (r)) ++nonfinite;
            }
            if (nonfinite == before) ++good;
        }
        check (good == FxChain::ModuleCount && nonfinite == 0, "all 13 duplicate types prepare and render without nonfinite output", good);
        allocs = 0; watchAlloc = true;
        for (int module = 0; module < FxChain::ModuleCount; ++module)
        {
            FxChain::RackRanks ranks {}; ranks[(size_t) (FxChain::ModuleCount + module)] = 1;
            c[1].enabled.fill (false); c[1].enabled[(size_t) module] = true;
            rack->setRack (ranks, c);
            for (int i = 0; i < 1200; ++i) { float l = input(i), r = l * 0.7f; rack->process (l, r); }
        }
        watchAlloc = false;
        check (allocs.load() == 0, "13 duplicate types: audio graph update and rendering allocate nothing", allocs.load());
    }
    std::printf ("DYNAMIC FX: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
