// User sample pitch, safety, held-note replacement and producer/audio handoff.
#include "instrument/instrument.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
using namespace orogen;

static std::shared_ptr<const UserSample> sine (double sr, double hz, const char* name)
{
    std::vector<float> data ((size_t) sr);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (float) (0.4 * std::sin (kTwoPi * hz * (double) i / sr));
    return makeUserSample (std::move (data), sr, name);
}

// Estimate frequency from positive-going zero crossings in the first actual
// windowed grain; no assumptions about internal rate variables.
static double measure (const std::shared_ptr<const UserSample>& shot, double host, int note, int root)
{
    GranularEngine e; e.setUserSample (shot); e.setSampleRate (host);
    EngineParams p; p.grainUser = true; p.grainRootNote = root;
    p.detune = 0; p.p[0] = .5f; p.p[1] = 1; p.p[2] = 0; p.p[3] = 0;
    e.start (note, 1); e.prepare (p);
    float last = 0; double first = -1, final = -1; int cycles = 0;
    const int begin = (int) (host * .015), end = (int) (host * .185);
    for (int i = 0; i < end; ++i)
    {
        const float v = e.render (p);
        if (! std::isfinite (v)) return 0;
        if (i > begin && last < 0 && v >= 0 && v != last)
        {
            const double crossing = (double) (i - 1) + (double) (-last) / (v - last);
            if (first < 0) first = crossing; else { final = crossing; ++cycles; }
        }
        last = v;
    }
    return cycles > 5 && final > first ? host * cycles / (final - first) : 0;
}

int main()
{
    bool ok = true;
    double worstCents = 0; int pitchTests = 0, quiet = 0;
    if (! std::getenv ("OROGEN_USER_HANDOFF_ONLY"))
    for (double source : { 8000., 22050., 44100., 48000., 96000., 192000. })
    {
        auto sample = sine (source, 220, "pitch");
        if (! sample) return 2;
        for (double host : { 44100., 48000., 96000. })
            for (const auto settings : { std::pair<int,int> { 60, 60 }, { 72, 60 }, { 60, 48 } })
            {
                const double wanted = 220.0 * std::pow (2.0, (settings.first - settings.second) / 12.0);
                const double got = measure (sample, host, settings.first, settings.second);
                if (got <= 0) { ++quiet; continue; }
                const double cents = std::abs (1200.0 * std::log2 (got / wanted));
                worstCents = std::max (worstCents, cents); ++pitchTests;
            }
    }
    ok = ok && quiet == 0 && (std::getenv ("OROGEN_USER_HANDOFF_ONLY") || (pitchTests == 54 && worstCents < .25));
    std::printf ("source/host/root pitch: %d/54, silent %d, worst error %.6f cents\n", pitchTests, quiet, worstCents);

    std::vector<float> x (1000, 0.3f);
    const bool valid = validUserSample (x, 48000);
    auto bad = x; bad[42] = std::numeric_limits<float>::quiet_NaN();
    const bool rejected = ! validUserSample (bad, 48000)
        && ! validUserSample (x, std::numeric_limits<double>::infinity())
        && ! validUserSample (std::vector<float> (511), 48000)
        && ! validUserSample (std::vector<float> (kUserSampleMaxFrames + 1), 48000);
    ok = ok && valid && rejected;
    std::printf ("validation: %s\n", valid && rejected ? "PASS" : "FAIL");

    int extremeCases = 0, extremeBad = 0; float extremePeak = 0;
    if (! std::getenv ("OROGEN_USER_HANDOFF_ONLY"))
    {
        for (double source : { 8000., 48000., 192000. })
            for (int length : { 512, 8000 })
            {
                std::vector<float> impulse ((size_t) length, 0.0f);
                for (int i = 0; i < length; i += 97) impulse[(size_t) i] = i & 1 ? -8.0f : 8.0f;
                const auto asset = makeUserSample (std::move (impulse), source, "extreme");
                if (! asset) { ++extremeBad; continue; }
                for (double host : { 8000., 48000., 192000. })
                    for (int root : { 0, 60, 127 }) for (int note : { 0, 60, 127 })
                        for (float tune : { -24.f, 24.f }) for (float reverse : { 0.f, 1.f })
                        {
                            GranularEngine e; e.setUserSample (asset); e.setSampleRate (host);
                            EngineParams ep; ep.grainUser = true; ep.grainRootNote = root;
                            ep.detune = 1; ep.tuneSemis = tune; ep.grainReverse = reverse;
                            ep.p[0] = 1; ep.p[1] = 1; ep.p[2] = 1; ep.p[3] = 1;
                            e.start (note, 1); e.prepare (ep);
                            for (int i = 0; i < 512; ++i)
                            {
                                const float v = e.render (ep);
                                if (! std::isfinite (v)) ++extremeBad;
                                else extremePeak = std::max (extremePeak, std::fabs (v));
                            }
                            ++extremeCases;
                        }
            }
        ok = ok && extremeCases == 648 && extremeBad == 0;
        std::printf ("imported sample extremes: %d cases, nonfinite %d, raw peak %.6f\n",
                     extremeCases, extremeBad, extremePeak);
    }

    auto a = sine (48000, 220, "first"), b = sine (48000, 660, "second");
    if (! a || ! b) return 2;
    Instrument inst; inst.prepare (48000, 4);
    Patch p = patchInit();
    for (int i = 1; i < 8; ++i) p.slots[i].on = false;
    auto& slot = p.slots[0]; slot.type = EngineType::Granular;
    slot.params.grainUser = true; slot.params.grainRootNote = 60; slot.params.detune = 0;
    slot.params.p[0] = .5f; slot.params.p[1] = .6f; slot.params.p[2] = .8f; slot.params.p[3] = 0;
    slot.attack = .005f; slot.sustain = 1;
    inst.stageUserSample (0, a, p); inst.stageUserSample (2, b, p);
    auto slots = inst.getUserSamples();
    const bool isolated = slots[0] == a && slots[2] == b && ! slots[1];
    inst.setPatch (p); inst.noteOn (60, .8f);
    float last = 0, l = 0, r = 0, naturalBefore = 0, naturalAfter = 0, transition = 0;
    double before = 0, after = 0;
    for (int i = 0; i < 24000; ++i)
    {
        inst.render (l, r);
        if (i > 16000) { naturalBefore = std::max (naturalBefore, std::fabs (l-last)); before += l*l; }
        last = l;
    }
    inst.stageUserSample (0, b, p); inst.applyPatch (p);
    for (int i = 0; i < 24000; ++i)
    {
        inst.render (l, r);
        if (i < 2000) transition = std::max (transition, std::fabs (l-last));
        if (i > 8000) { naturalAfter = std::max (naturalAfter, std::fabs (l-last)); after += l*l; }
        last = l;
    }
    before = std::sqrt (before / 7999); after = std::sqrt (after / 15999);
    const bool smooth = isolated && before > 1.e-4 && after > 1.e-4 && std::isfinite (transition)
        && transition < std::max (naturalBefore, naturalAfter) * 1.20f + .0002f;
    ok = ok && smooth;
    std::printf ("held sample swap: isolated %d, RMS %.6f -> %.6f, step %.9f vs natural %.9f / %.9f\n",
                 (int) isolated, before, after, transition, naturalBefore, naturalAfter);
    inst.drainRetired();

    std::atomic<bool> done { false };
    std::thread producer ([&] {
        for (int i = 0; i < 240; ++i) { inst.stageUserSample (0, (i & 1) ? a : b, p); if ((i & 3) == 0) inst.drainRetired(); }
        done.store (true, std::memory_order_release);
    });
    int count = 0, nonfinite = 0;
    while (! done.load (std::memory_order_acquire))
    {
        inst.applyPatch (p);
        for (int i = 0; i < 32; ++i) { inst.render (l, r); ++count; if (! std::isfinite (l) || ! std::isfinite (r)) ++nonfinite; }
    }
    producer.join();
    for (int i = 0; i < 4096; ++i)
    {
        if ((i & 31) == 0) inst.applyPatch (p);
        inst.render (l, r); if (! std::isfinite (l) || ! std::isfinite (r)) ++nonfinite;
    }
    inst.drainRetired();
    const bool handoff = count > 0 && nonfinite == 0 && inst.pendingEngineSwaps() == 0;
    ok = ok && handoff;
    std::printf ("concurrent handoff: 240 updates, %d frames, nonfinite %d, pending %d\n",
                 count, nonfinite, inst.pendingEngineSwaps());
    std::printf ("USER GRANULAR: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
