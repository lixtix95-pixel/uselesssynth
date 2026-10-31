// FX power switches: real audio bypass, latency, fade and delay history.
#include "instrument/fx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace orogen;
static int failures = 0;
static void check (bool ok, const char* text)
{
    std::printf ("  [%s] %s\n", ok ? "OK" : "FAIL", text);
    if (!ok) ++failures;
}
struct Stereo { float l, r; };

static FxChain::Settings settings()
{
    FxChain::Settings s;
    s.enabled.fill (false);
    s.drive = 0.75f; s.driveMix = 0.8f;
    s.combMix = 0.7f; s.combFreqA = 280.0f; s.combResA = 85.0f;
    s.chorusMix = 0.8f; s.chorusDepth = 0.7f;
    s.delayMix = 0.7f; s.delayTime = 0.08f; s.delayFb = 0.5f;
    s.reverbMix = 0.8f; s.reverbSize = 0.8f;
    s.plateMix = 0.8f;
    s.eqLow = 9.0f; s.eqHigh = -9.0f;
    s.materialMix = 0.75f; s.materialExcite = 0.8f;
    s.spectreMix = 0.85f; s.spectreWindow = 2; s.spectreShift = 5.0f;
    s.spectrePeaks = 5;
    s.robinMix = 0.8f; s.robinSemis = 7.0f;
    s.glitchMix = 0.9f; s.glitchChance = 1.0f; s.glitchReverse = 1.0f; s.glitchLengthMs = 55;
    s.compThreshold = -30.0f; s.compRatio = 6.0f; s.compMakeup = 1.0f;
    s.width = 0.1f;
    return s;
}

static std::vector<Stereo> render (const FxChain::Settings& s, int& latency, int n = 72000)
{
    auto fx = std::make_unique<FxChain>();
    fx->prepare (48000.0); fx->set (s); latency = fx->latency();
    std::vector<Stereo> out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float env = 0.25f + 0.75f * (float) ((i % 4800) < 2700);
        float l = env * (0.24f * std::sin (0.057595866f * i) + 0.14f * std::sin (0.133f * i));
        float r = env * (0.21f * std::sin (0.047f * i + 0.7f) + 0.11f * std::sin (0.117f * i));
        fx->process (l, r);
        out[(size_t) i] = { l, r };
    }
    return out;
}

int main()
{
    std::puts ("--- FX POWER / AUDIO / LATENCY ---");
    const auto allOff = settings();
    int dryLatency = 0;
    const auto dry = render (allOff, dryLatency);
    check (dryLatency == 1024, "all OFF retains ROBIN's 1024-sample dry latency");

    auto differentlyConfigured = FxChain::Settings();
    differentlyConfigured.enabled.fill (false);
    differentlyConfigured.width = 1.5f;
    differentlyConfigured.compMakeup = 12.0f;
    differentlyConfigured.drive = 0.9f;
    differentlyConfigured.delayMix = 1.0f;
    int otherLatency = 0;
    const auto dry2 = render (differentlyConfigured, otherLatency);
    double maxOffDiff = 0.0;
    for (size_t i = 0; i < dry.size(); ++i)
        maxOffDiff = std::max ({ maxOffDiff, std::fabs ((double) dry[i].l - dry2[i].l),
                                std::fabs ((double) dry[i].r - dry2[i].r) });
    std::printf ("   all OFF: max delta with radically different controls %.9g\n", maxOffDiff);
    check (otherLatency == dryLatency && maxOffDiff == 0.0, "OFF really bypasses all 13 settings");

    const char* names[] = { "Drive", "Comb", "Chorus", "Delay", "Reverb", "Plate", "EQ",
                            "Material", "Spectre", "Robin", "Glitch", "Dynamics", "Width" };
    for (int m = 0; m < FxChain::ModuleCount; ++m)
    {
        auto s = allOff; s.enabled[(size_t) m] = true;
        int latency = 0; const auto wet = render (s, latency);
        const int shift = latency - dryLatency;
        double e = 0.0; size_t count = 0; int bad = 0;
        for (int i = 16000 + std::max (0, shift); i < (int) wet.size(); ++i)
        {
            const int k = i - shift;
            if (k < 0 || k >= (int) dry.size()) continue;
            if (! std::isfinite (wet[(size_t) i].l) || ! std::isfinite (wet[(size_t) i].r)) ++bad;
            const double dl = wet[(size_t) i].l - dry[(size_t) k].l;
            const double dr = wet[(size_t) i].r - dry[(size_t) k].r;
            e += dl*dl + dr*dr; count += 2;
        }
        const double delta = std::sqrt (e / (double) std::max ((size_t) 1, count));
        std::printf ("   %-10s delta RMS %.7f / added latency %d / nonfinite %d\n", names[m], delta, shift, bad);
        char text[100]; std::snprintf (text, sizeof text, "%s ON changes audio; output finite", names[m]);
        check (delta > 1.0e-4 && bad == 0, text);
    }

    // Power changes while audio is running: after a 6-ms exponential fade,
    // the bypass is transparent and the first switched sample is continuous.
    auto fx = std::make_unique<FxChain>();
    auto reference = std::make_unique<FxChain>();
    auto s = allOff; s.enabled[(size_t) FxChain::Drive] = true;
    fx->prepare (48000); fx->set (s); reference->prepare (48000); reference->set (s);
    float firstDelta = 0.0f, lateDelta = 0.0f, beforeDelta = 0.0f;
    for (int i = 0; i < 18000; ++i)
    {
        float l = 0.35f * std::sin (0.0106f * i), r = 0.20f * std::sin (0.0106f * i + 0.3f);
        float refL = l, refR = r;
        if (i == 8000) { s.enabled[(size_t) FxChain::Drive] = false; fx->set (s); }
        fx->process (l, r); reference->process (refL, refR);
        const float difference = std::fabs (l - refL);
        if (i < 8000 + dryLatency) beforeDelta = std::max (beforeDelta, difference);
        if (i == 8000 + dryLatency) firstDelta = difference;
        if (i > 12000) lateDelta = std::max (lateDelta, difference);
    }
    std::printf ("   Drive OFF: pre %.9f, first audible sample %.9f, later %.6f\n",
                 beforeDelta, firstDelta, lateDelta);
    check (beforeDelta < 1.0e-6f && firstDelta < 0.003f && lateDelta > 0.01f,
           "Drive bypass is audible and its first output sample is smoothed");

    // Bypassed delay must advance its clock so an old echo cannot freeze.
    auto delay = std::make_unique<FxChain>();
    auto d = allOff; d.delayMix = 1.0f; d.delayTime = 0.02f; d.delayFb = 0.7f;
    d.enabled[(size_t) FxChain::DelayModule] = true;
    delay->prepare (48000); delay->set (d);
    float peakAfter = 0.0f;
    for (int i = 0; i < 160000; ++i)
    {
        float l = i == 0 ? 0.8f : 0.0f, r = l;
        if (i == 400) { d.enabled[(size_t) FxChain::DelayModule] = false; delay->set (d); }
        if (i == 145000) { d.enabled[(size_t) FxChain::DelayModule] = true; delay->set (d); }
        delay->process (l, r);
        if (i >= 145000) peakAfter = std::max (peakAfter, std::max (std::fabs (l), std::fabs (r)));
    }
    std::printf ("   delay bypass for 3 s: post-reenable peak %.9f\n", peakAfter);
    check (peakAfter < 1.0e-3f, "delay history does not freeze while bypassed");
    std::printf ("FX bypass failures: %d\n", failures);
    return failures ? 1 : 0;
}
