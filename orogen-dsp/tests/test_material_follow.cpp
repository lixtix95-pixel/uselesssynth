#include "../src/instrument/instrument.h"
#include "../src/core/fft.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::atomic<long long> allocations { 0 };
static thread_local bool countAllocations = false;
void* operator new (std::size_t n) { if (countAllocations) ++allocations; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n) { return ::operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace orogen;
static int passed = 0, failed = 0;
static void check (bool ok, const char* label)
{
    std::printf ("  [%s] %s\n", ok ? " OK " : "FAIL", label);
    ok ? ++passed : ++failed;
}

struct Measurement { double hz = 0.0, peak = 0.0, rms = 0.0; int invalid = 0; };

static double spectralFrequency (const std::vector<float>& signal, double expected, double sr)
{
    const int samples = (int) signal.size();
    int nfft = 1; while (nfft < samples * 4) nfft *= 2;
    std::vector<std::complex<float>> spectrum ((size_t) nfft);
    for (int i = 0; i < samples; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * 3.141592653589793 * i / (samples - 1));
        spectrum[(size_t) i] = { (float) (signal[(size_t) i] * win), 0.0f };
    }
    Fft::forward (spectrum);
    const double bin = sr / nfft;
    const int lo = std::max (1, (int) std::floor (expected * 0.94 / bin));
    const int hi = std::min (nfft / 2 - 2, (int) std::ceil (expected * 1.06 / bin));
    int best = lo;
    for (int i = lo + 1; i <= hi; ++i)
        if (std::norm (spectrum[(size_t) i]) > std::norm (spectrum[(size_t) best])) best = i;
    const auto logPower = [&] (int k) { return std::log (std::max (1.0e-40, (double) std::norm (spectrum[(size_t) k]))); };
    const double a = logPower (best - 1), b = logPower (best), c = logPower (best + 1);
    const double denominator = a - 2.0 * b + c;
    const double offset = std::abs (denominator) > 1.0e-15 ? 0.5 * (a - c) / denominator : 0.0;
    return (best + std::clamp (offset, -1.0, 1.0)) * bin;
}

static Measurement measure (int mode, int note, float tone, float size, double sr = 48000.0, double seconds = 1.0)
{
    nova::fx::MaterialMidiResonator resonator;
    resonator.prepare (sr);
    nova::fx::MaterialSettings s;
    s.mode = mode; s.mix = 100; s.excite = 70; s.tone = tone * 100;
    s.size = size * 100; s.damp = 10; s.feedback = 80; s.follow = true;
    resonator.set (s);
    const double expected = midiToHz ((double) note);
    resonator.setMidiPitchHz (expected);
    for (int i = 0; i < (int) (sr * 0.5); ++i) resonator.process ({ 0, 0 });
    const int samples = (int) (sr * seconds);
    std::vector<float> signal ((size_t) samples);
    Measurement m;
    double energy = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        const float x = i == 0 ? 0.01f : 0.0f;
        const auto y = resonator.process ({ x, x });
        if (! std::isfinite (y.l) || ! std::isfinite (y.r)) ++m.invalid;
        m.peak = std::max (m.peak, (double) std::abs (y.l));
        energy += (double) y.l * y.l;
        signal[(size_t) i] = y.l;
    }
    m.rms = std::sqrt (energy / samples);
    m.hz = spectralFrequency (signal, expected, sr);
    return m;
}

static void testFrequencyRange()
{
    double worst = 0.0; int invalid = 0, cases = 0; double minimumRms = 1.0;
    for (double sr : { 8000.0, 22050.0, 44100.0, 96000.0, 192000.0 })
        for (int mode : { 0, 3 })
            for (int note : { 48, 60 })
            {
                const auto m = measure (mode, note, .5f, .5f, sr);
                const double cents = 1200.0 * std::log2 (m.hz / midiToHz (note));
                worst = std::max (worst, std::abs (cents)); minimumRms = std::min (minimumRms, m.rms);
                invalid += m.invalid; ++cases;
            }
    std::printf ("  %d sample-rate spectra (8–192 kHz), worst error %.4f cents, minimum RMS %.9g\n", cases, worst, minimumRms);
    check (invalid == 0 && worst < 2.0 && minimumRms > 1.0e-9,
           "C3/C4 FOLLOW stays tuned across supported sample rates");
    worst = 0; minimumRms = 1; invalid = 0; cases = 0;
    for (int mode : { 0, 3 })
        for (int note : { 0, 12, 24, 127 })
        {
            const auto m = measure (mode, note, .5f, .5f, 48000.0, note < 25 ? 3.0 : 1.0);
            const double cents = 1200.0 * std::log2 (m.hz / midiToHz (note));
            std::printf ("  mode %d MIDI %d: %.6f Hz, error %+.3f cents, RMS %.9g\n", mode, note, m.hz, cents, m.rms);
            worst = std::max (worst, std::abs (cents)); minimumRms = std::min (minimumRms, m.rms);
            invalid += m.invalid; ++cases;
        }
    check (invalid == 0 && worst < 5.0 && minimumRms > 1.0e-10,
           "very low and high MIDI notes remain tuned when below the sample-rate limit");
}

static void testOriginalOff()
{
    int mismatches = 0, invalid = 0;
    constexpr double sr = 48000.0;
    for (int mode = 0; mode < 4; ++mode)
    {
        MaterialFx wrapper; wrapper.prepare (sr);
        wrapper.setMode (mode); wrapper.setExcite (.7f); wrapper.setTone (.2f); wrapper.setDamp (.35f);
        wrapper.setSize (.8f); wrapper.setFeedback (.6f); wrapper.setMix (.85f); wrapper.setFollow (false);
        nova::fx::MaterialResonator original; original.prepare (sr);
        // Use the same float scaling as MaterialFx; decimal literals like
        // 0.6f*100 need not have the same bits as a literal 60.0f.
        nova::fx::MaterialSettings s; s.mode = mode; s.excite = .7f * 100.0f; s.tone = .2f * 100.0f; s.damp = .35f * 100.0f;
        s.size = .8f * 100.0f; s.feedback = .6f * 100.0f; s.mix = .85f * 100.0f; s.follow = false; original.set (s);
        for (int i = 0; i < 40000; ++i)
        {
            if (i == 12000)
            {
                wrapper.setTone (.9f); wrapper.setSize (.15f); wrapper.setMidiPitchHz (midiToHz (36));
                s.tone = .9f * 100.0f; s.size = .15f * 100.0f; original.set (s);
            }
            const float left = 0.08f * std::sin (0.017f * i) + ((i % 10003) == 0 ? 0.1f : 0.0f);
            const float right = 0.06f * std::sin (0.023f * i);
            auto ref = original.process ({ left, right });
            float l = left, r = right; wrapper.process (l, r);
            if (std::memcmp (&l, &ref.l, sizeof (float)) || std::memcmp (&r, &ref.r, sizeof (float))) ++mismatches;
            if (! std::isfinite (l) || ! std::isfinite (r)) ++invalid;
        }
    }
    std::printf ("  FOLLOW OFF: %d stereo frames, mismatches %d, nonfinite %d\n", 4 * 40000, mismatches, invalid);
    check (mismatches == 0 && invalid == 0, "FOLLOW OFF remains bit-identical to the unmodified MonoJuice path");
}

static void testPitches (bool quick)
{
    double worstCents = 0.0, worstOctave = 0.0, smallestRms = 1.0, maxPeak = 0.0;
    int cases = 0, invalid = 0;
    for (int mode = 0; mode < 4; ++mode)
        for (float tone : { 0.0f, 0.5f, 1.0f })
            for (float size : { 0.0f, 0.5f, 1.0f })
            {
                if (quick && (tone != .5f || size != .5f)) continue;
                const auto a = measure (mode, 48, tone, size);
                const auto b = measure (mode, 60, tone, size);
                const double errA = 1200.0 * std::log2 (a.hz / midiToHz (48));
                const double errB = 1200.0 * std::log2 (b.hz / midiToHz (60));
                const double oct = 1200.0 * std::log2 (b.hz / a.hz / 2.0);
                worstCents = std::max ({ worstCents, std::abs (errA), std::abs (errB) });
                worstOctave = std::max (worstOctave, std::abs (oct));
                smallestRms = std::min ({ smallestRms, a.rms, b.rms });
                maxPeak = std::max ({ maxPeak, a.peak, b.peak });
                invalid += a.invalid + b.invalid; cases += 2;
                if (quick) std::printf ("  mode %d: C3 %.5f Hz (%+.3f c), C4 %.5f Hz (%+.3f c), octave %+.3f c\n",
                                        mode, a.hz, errA, b.hz, errB, oct);
            }
    if (! quick)
        for (int mode = 0; mode < 4; ++mode)
            for (int note : { 49, 55, 72 })
            {
                const auto m = measure (mode, note, .5f, .5f);
                worstCents = std::max (worstCents, std::abs (1200.0 * std::log2 (m.hz / midiToHz (note))));
                smallestRms = std::min (smallestRms, m.rms); maxPeak = std::max (maxPeak, m.peak);
                invalid += m.invalid; ++cases;
            }
    std::printf ("  %d spectra: worst pitch %.4f cents, worst C3/C4 octave %.4f cents, min RMS %.9g, peak %.6f, nonfinite %d\n",
                 cases, worstCents, worstOctave, smallestRms, maxPeak, invalid);
    check (invalid == 0 && smallestRms > 1.0e-9 && worstCents < 2.0 && worstOctave < 2.0,
           "MIDI fundamental and octave remain accurate across modes, TONE and SIZE");
}

static void testLedgerAndCopies()
{
    Instrument inst; inst.prepare (48000.0, 16);
    auto close = [] (double a, double b) { return std::abs (a - b) < 1.0e-8; };
    bool ok = inst.materialFollowNote() == 60 && close (inst.materialFollowTargetHz(), midiToHz (60));
    inst.noteOn (48, .8f); ok &= inst.materialFollowNote() == 48;
    inst.noteOn (60, .8f); ok &= inst.materialFollowNote() == 60;
    inst.noteOn (55, .8f); ok &= inst.materialFollowNote() == 55;
    inst.noteOff (60); ok &= inst.materialFollowNote() == 55;
    inst.noteOff (55); ok &= inst.materialFollowNote() == 48;
    inst.noteOff (48); ok &= inst.materialFollowNote() == 48; // tail retains pitch
    inst.noteOn (60, .8f); inst.allNotesOff(); inst.noteOn (72, .8f); inst.noteOff (72);
    ok &= inst.materialFollowNote() == 72;
    check (ok, "last-note priority, held-note fallback and tail behaviour");

    FxChain::Settings s;
    s.enabled.fill (false); s.enabled[FxChain::Material] = true;
    s.materialMix = .8f; s.materialFollow = true;
    inst.setFx (s);
    inst.noteOn (48, .8f); // prepare copies *after* this note
    FxChain::RackRanks ranks {};
    FxChain::CopySettings settings {};
    for (auto& copy : settings) copy = s;
    ranks[FxChain::Material] = 1;
    for (int copy = 1; copy < FxChain::kCopiesPerModule; ++copy)
    {
        const int node = FxChain::Material + copy * FxChain::ModuleCount;
        inst.ensureFxCopy (node, s);
        ranks[(size_t) node] = copy + 1;
    }
    inst.setFxRack (ranks, settings);
    int matched = 0;
    for (int copy = 0; copy < 4; ++copy) if (close (inst.materialFollowTargetHz (copy), midiToHz (48))) ++matched;
    inst.noteOn (60, .8f);
    int moved = 0;
    for (int copy = 0; copy < 4; ++copy) if (close (inst.materialFollowTargetHz (copy), midiToHz (60))) ++moved;
    std::printf ("  four Material copies, pre-existing note %d/4, next note %d/4\n", matched, moved);
    check (matched == 4 && moved == 4, "every independent Material copy receives notes, including newly staged copies");

    Instrument arp; arp.prepare (48000.0, 4);
    ArpSettings as; as.on = true; as.mode = ArpMode::Up; as.div = ArpDiv::Sixteenth; as.octaves = 2; as.gate = .5f;
    arp.setTempo (120); arp.setArp (as); arp.noteOn (48, .8f);
    float l = 0, r = 0; arp.render (l, r);
    const int first = arp.materialFollowNote();
    for (int i = 0; i < 6000; ++i) arp.render (l, r);
    const int second = arp.materialFollowNote();
    for (int i = 0; i < 6000; ++i) arp.render (l, r);
    const int third = arp.materialFollowNote();
    std::printf ("  ARP targets %d -> %d -> %d\n", first, second, third);
    check (first == 48 && second == 60 && third == 48, "ARP-generated octaves reach Material at note-on");
    arp.allNotesOff();
    const int retained = arp.materialFollowNote();
    for (int i = 0; i < 12000; ++i) arp.render (l, r);
    check (arp.materialFollowNote() == retained, "all-notes-off stops ARP retrigger and retains the tail target");
}

static void testCopyAudio()
{
    double worstCents = 0.0, minimumRms = 1.0; int invalid = 0;
    for (int copy = 0; copy < 4; ++copy)
    {
        auto chain = std::make_unique<FxChain>();
        chain->prepare (48000.0);
        FxChain::Settings s; s.enabled.fill (false); s.enabled[FxChain::Material] = true;
        s.materialMix = 1.0f; s.materialFollow = true; s.materialMode = copy;
        s.materialExcite = .7f; s.materialTone = .2f + .2f * copy;
        s.materialSize = .15f + .2f * copy; s.materialDamp = .1f; s.materialFeedback = .8f;
        chain->set (s);
        const int note = (copy & 1) ? 60 : 48;
        const double expected = midiToHz (note);
        chain->setMaterialMidiPitchHz (expected);
        const int node = FxChain::Material + copy * FxChain::ModuleCount;
        if (copy > 0) chain->ensureCopy (node, s); // prepared after the MIDI event
        FxChain::RackRanks ranks {}; ranks[(size_t) node] = 1;
        FxChain::CopySettings copies {}; for (auto& settings : copies) settings = s;
        chain->setRack (ranks, copies);
        float l = 0, r = 0;
        for (int i = 0; i < 24000; ++i) { l = r = 0; chain->process (l, r); }
        std::vector<float> out (48000);
        double energy = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            l = r = i == 0 ? 0.01f : 0.0f;
            chain->process (l, r); out[(size_t) i] = l;
            energy += (double) l * l;
            if (! std::isfinite (l) || ! std::isfinite (r)) ++invalid;
        }
        const double hz = spectralFrequency (out, expected, 48000.0);
        const double cents = 1200.0 * std::log2 (hz / expected);
        const double rms = std::sqrt (energy / out.size());
        worstCents = std::max (worstCents, std::abs (cents)); minimumRms = std::min (minimumRms, rms);
        std::printf ("  rack Material #%d, mode %d: %.5f Hz (%+.3f cents), RMS %.9g\n", copy + 1, copy, hz, cents, rms);
    }
    check (invalid == 0 && worstCents < 2.0 && minimumRms > 1.0e-9,
           "actual audio from each of the four independent rack copies follows the MIDI pitch");
}

static void testLiveRetune()
{
    MaterialFx a, b; a.prepare (48000); b.prepare (48000);
    for (auto* fx : { &a, &b })
    {
        fx->setMode (2); fx->setExcite (.8f); fx->setTone (.7f); fx->setSize (.5f);
        fx->setDamp (.15f); fx->setFeedback (.7f); fx->setMix (1.0f);
        fx->setMidiPitchHz (midiToHz (48)); fx->setFollow (true);
    }
    double before = 0, after = 0, initialDelta = 0; int bad = 0, afterCount = 0;
    float previous = 0, natural = 0, during = 0;
    for (int i = 0; i < 72000; ++i)
    {
        if (i == 24000) a.setMidiPitchHz (midiToHz (60));
        const float x = .025f * std::sin ((float) (kTwoPi * 193.0 * i / 48000.0))
                      + ((i % 4000) == 0 ? .025f : 0.0f);
        float al = x, ar = x, bl = x, br = x;
        a.process (al, ar); b.process (bl, br);
        if (! std::isfinite (al) || ! std::isfinite (bl)) ++bad;
        const double delta = (double) al - bl;
        if (i < 24000) before += delta * delta;
        if (i == 24000) initialDelta = std::abs (delta);
        if (i >= 36000) { after += delta * delta; ++afterCount; }
        if (i > 1000 && i < 24000) natural = std::max (natural, std::abs (al - previous));
        if (i >= 24000 && i < 24500) during = std::max (during, std::abs (al - previous));
        previous = al;
    }
    const double beforeRms = std::sqrt (before / 24000.0), afterRms = std::sqrt (after / afterCount);
    std::printf ("  live target C3->C4: before ΔRMS %.9f, first Δ %.9f, after ΔRMS %.9f; step %.6f / natural %.6f\n",
                 beforeRms, initialDelta, afterRms, during, natural);
    check (bad == 0 && beforeRms < 1e-8 && initialDelta < 1e-6 && afterRms > 1e-6
           && during <= natural * 2.0f + .01f,
           "held input changes pitch smoothly when only the MIDI target changes");
}

static void testAutomationAndFollowSwitch()
{
    double worstCents = 0.0; int spectra = 0, invalid = 0;
    for (int mode = 0; mode < 4; ++mode)
    {
        nova::fx::MaterialMidiResonator resonator; resonator.prepare (48000.0);
        nova::fx::MaterialSettings s; s.mode = mode; s.mix = 100; s.excite = 70;
        s.damp = 10; s.feedback = 80; s.follow = true;
        resonator.set (s); resonator.setMidiPitchHz (midiToHz (60));
        for (int i = 0; i < 24000; ++i) resonator.process ({ 0, 0 });
        for (const auto change : { std::pair<float, float> { 0, 0 }, { 100, 100 }, { 20, 80 } })
        {
            // Same held note and same resonator state; only TONE and SIZE change.
            s.tone = change.first; s.size = change.second; resonator.set (s);
            for (int i = 0; i < 18000; ++i) resonator.process ({ 0, 0 });
            std::vector<float> data (48000);
            for (int i = 0; i < 48000; ++i)
            {
                const float in = i == 0 ? .01f : 0.0f;
                const auto y = resonator.process ({ in, in });
                data[(size_t) i] = y.l;
                if (! std::isfinite (y.l) || ! std::isfinite (y.r)) ++invalid;
            }
            const double frequency = spectralFrequency (data, midiToHz (60), 48000.0);
            worstCents = std::max (worstCents, std::abs (1200.0 * std::log2 (frequency / midiToHz (60))));
            ++spectra;
        }
    }
    std::printf ("  TONE/SIZE automation on a held note: %d spectra, worst %.4f cents, nonfinite %d\n",
                 spectra, worstCents, invalid);
    check (invalid == 0 && worstCents < 2.0, "live TONE/SIZE changes preserve the held MIDI frequency");

    MaterialFx switched, reference; switched.prepare (48000); reference.prepare (48000);
    for (auto* fx : { &switched, &reference })
    {
        fx->setMode (2); fx->setMix (.7f); fx->setExcite (.7f); fx->setTone (.5f);
        fx->setSize (.5f); fx->setMidiPitchHz (midiToHz (48));
    }
    double firstDelta = 0, late = 0; int bad = 0;
    float last = 0, beforeStep = 0, switchStep = 0;
    for (int i = 0; i < 72000; ++i)
    {
        if (i == 24000) switched.setFollow (true);
        const float input = (float) (.025 * std::sin (kTwoPi * 130.8128 * i / 48000.0));
        float a = input, ar = input, b = input, br = input;
        switched.process (a, ar); reference.process (b, br);
        if (! std::isfinite (a) || ! std::isfinite (b)) ++bad;
        if (i == 24000) firstDelta = std::abs ((double) a - b);
        if (i >= 48000) late += (a - b) * (a - b);
        if (i > 1000 && i < 24000) beforeStep = std::max (beforeStep, std::abs (a - last));
        if (i >= 24000 && i < 24480) switchStep = std::max (switchStep, std::abs (a - last));
        last = a;
    }
    const double lateRms = std::sqrt (late / 24000.0);
    std::printf ("  FOLLOW OFF->ON first Δ %.8f, later ΔRMS %.8f, transition step %.6f / natural %.6f\n",
                 firstDelta, lateRms, switchStep, beforeStep);
    check (bad == 0 && firstDelta < .005 && lateRms > 1.0e-5 && switchStep < beforeStep * 2.0f + .01f,
           "switching FOLLOW on a held signal crossfades the real paths");
}

static void testSafety()
{
    int invalid = 0, cases = 0;
    double peak = 0;
    for (double sr : { 8000.0, 22050.0, 48000.0, 96000.0, 192000.0 })
        for (int mode = 0; mode < 4; ++mode)
            for (int note : { 0, 24, 48, 84, 127 })
            {
                nova::fx::MaterialMidiResonator r; r.prepare (sr);
                nova::fx::MaterialSettings s; s.mode = mode; s.mix = 100; s.excite = 100;
                s.tone = 100; s.size = 0; s.damp = 0; s.feedback = 100; s.follow = true;
                r.set (s); r.setMidiPitchHz (midiToHz (note));
                for (int i = 0; i < 6000; ++i)
                {
                    const float x = ((i % 997) == 0) ? .05f : 0.0f;
                    const auto y = r.process ({ x, -0.7f * x });
                    if (! std::isfinite (y.l) || ! std::isfinite (y.r)) ++invalid;
                    peak = std::max ({ peak, (double) std::abs (y.l), (double) std::abs (y.r) });
                }
                ++cases;
            }
    std::printf ("  extremes %d, nonfinite %d, raw peak %.6f (no Material limiter)\n", cases, invalid, peak);
    check (invalid == 0 && peak < 1.0, "MIDI/sample-rate/physical-mode extremes remain finite without a limiter");

    double sustainedPeak = 0.0, sustainedEnergy = 0.0; int sustainedBad = 0;
    for (int mode = 0; mode < 4; ++mode)
    {
        nova::fx::MaterialMidiResonator modal; modal.prepare (48000.0);
        nova::fx::MaterialSettings s; s.mode = mode; s.mix = 100; s.excite = 100;
        s.tone = 100; s.size = 30; s.damp = 0; s.feedback = 100; s.follow = true;
        modal.set (s); modal.setMidiPitchHz (midiToHz (48));
        for (int i = 0; i < 24000; ++i) modal.process ({ 0, 0 });
        double phase = 0.0;
        for (int i = 0; i < 96000; ++i)
        {
            const double hz = midiToHz (i < 48000 ? 48 : 60);
            if (i == 48000) modal.setMidiPitchHz (hz);
            phase += kTwoPi * hz / 48000.0;
            const float x = (float) (.25 * std::sin (phase));
            const auto y = modal.process ({ x, x });
            if (! std::isfinite (y.l) || ! std::isfinite (y.r)) ++sustainedBad;
            sustainedPeak = std::max (sustainedPeak, (double) std::abs (y.l));
            sustainedEnergy += (double) y.l * y.l;
        }
    }
    const double sustainedRms = std::sqrt (sustainedEnergy / (4.0 * 96000.0));
    std::printf ("  sustained C3/C4 input .25: raw peak %.6f, RMS %.6f, nonfinite %d (no Material limiter)\n",
                 sustainedPeak, sustainedRms, sustainedBad);
    check (sustainedBad == 0 && sustainedPeak < .8 && sustainedRms > .001,
           "resonant sustained notes remain audible and bounded before any limiter");

    Instrument inst; inst.prepare (48000.0, 4);
    FxChain::Settings s; s.enabled.fill (false); s.enabled[FxChain::Material] = true;
    s.materialMix = .8f; s.materialFollow = true; inst.setFx (s);
    inst.noteOn (48, .8f);
    float l = 0, r = 0;
    for (int i = 0; i < 2000; ++i) inst.render (l, r);
    allocations = 0; countAllocations = true;
    for (int i = 0; i < 12000; ++i)
    {
        if (i % 1000 == 0) { inst.noteOff (48 + ((i / 1000) & 1) * 12); inst.noteOn (48 + (((i / 1000) + 1) & 1) * 12, .8f); }
        inst.render (l, r);
    }
    countAllocations = false;
    std::printf ("  audio-thread C++ allocations during note/retune %lld\n", allocations.load());
    check (allocations.load() == 0, "note tracking and FOLLOW do not allocate in this audio-thread scenario");
}

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    std::printf ("OROGEN Material: MIDI FOLLOW\n");
    testOriginalOff(); testPitches (quick); testLedgerAndCopies(); testCopyAudio(); testLiveRetune();
    if (! quick) { testFrequencyRange(); testAutomationAndFollowSwitch(); testSafety(); }
    std::printf ("Material MIDI FOLLOW: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
