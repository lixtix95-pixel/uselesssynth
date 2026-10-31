#include "../src/content/wavetable_import.h"
#include "../src/engines/wavetable.h"
#include <cstdio>
#include <cmath>
#include <limits>
#include <chrono>

using namespace orogen;
static int failed = 0;
static void check (bool ok, const char* s) { std::printf ("  [%s] %s\n", ok ? " OK " : "FAIL", s); if (!ok) ++failed; }

int main()
{
    std::vector<float> packed (2048 * 3);
    for (int f = 0; f < 3; ++f)
        for (int i = 0; i < 2048; ++i)
            packed[(size_t) f * 2048 + i] = (float) (0.6 * std::sin (kTwoPi * i / 2048.0)
                                                  + 0.15 * f * std::cos (kTwoPi * 3 * i / 2048.0));
    std::vector<float> raw {42.0f};
    check (unpackWavetableFrames (packed, 2048, raw) && raw.size() == packed.size(), "packed 2048: 3 frames accepted");
    double err = 0.0;
    for (size_t i = 0; i < raw.size(); ++i) err = std::max (err, std::abs ((double) raw[i] - packed[i]));
    std::printf ("  packed roundtrip max error %.9f\n", err);
    check (err < 2.0e-4, "packed spectrum and phase retained");
    const auto saved = raw;
    check (! unpackWavetableFrames (packed, 4096, raw) && raw == saved, "invalid length rejected transactionally");
    auto invalid = packed; invalid[1] = std::numeric_limits<float>::quiet_NaN();
    check (! unpackWavetableFrames (invalid, 2048, raw) && raw == saved, "non-finite samples rejected transactionally");
    std::vector<float> tiny (256);
    for (int i=0; i<256; ++i) tiny[(size_t)i] = (float) std::sin (kTwoPi * i / 256.0);
    std::vector<float> expanded;
    check (unpackWavetableFrames (tiny, 256, expanded), "256-sample cycle supported");
    err=0;
    for (int i=0; i<2048; ++i) err = std::max (err, std::abs ((double)expanded[(size_t)i] - std::sin(kTwoPi*i/2048.0)));
    std::printf ("  256->2048 max error %.9f\n", err);
    check (err < 0.0001, "spectral upsample preserves pitch");
    auto user = makeUserWavetable (saved, 3, "3 frames");
    check (user && user->table.frames == 3 && user->table.levels.size() == kWtLevels, "shared bandlimited pyramid constructed");
    bool sane=true;
    for (int level=0; level<kWtLevels; ++level)
        sane = sane && user->table.levels[(size_t)level].size() == (size_t)3 * user->table.lens[(size_t)level]
            && user->table.lens[(size_t)level] >= 2 * user->table.harms[(size_t)level];
    check (sane, "all 28 MIP levels have valid dimensions");
    // Imported harmonic tables go through the same safe MIP selection as the
    // factory bank. Measure non-harmonic energy at high played pitches.
    std::vector<float> rich (2048, 0.0f);
    for (int i=0;i<2048;++i)
        for (int k=1;k<=256;++k)
            rich[(size_t)i] += (float)(0.18*std::sin(kTwoPi*k*i/2048.0)/k);
    auto bright = makeUserWavetable (rich,1,"rich");
    double worstAlias=-300.0;
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (int note : { 72, 84, 96, 108 })
        {
            WavetableEngine osc; EngineParams ep; ep.wtUser=true; ep.detune=0;
            ep.p[0]=ep.p[1]=ep.p[2]=0; ep.p[3]=0.3f;
            osc.setUserTable (bright); osc.setSampleRate (rate); osc.start (note,1); osc.prepare (ep);
            constexpr int n=16384;
            std::vector<std::complex<float>> spectrum ((size_t)n);
            for (int i=0;i<n;++i)
            {
                const double t=(double)i/(n-1);
                const double w=.35875-.48829*std::cos(kTwoPi*t)+.14128*std::cos(2*kTwoPi*t)-.01168*std::cos(3*kTwoPi*t);
                spectrum[(size_t)i]={osc.render(ep)*(float)w,0};
            }
            Fft::forward(spectrum); double all=0,other=0;
            const double hz=rate/n, f0=midiToHz(note);
            for (int bin=1;bin<n/2;++bin)
            {
                const double f=bin*hz;
                if(f<25) continue;
                const double power=std::norm(spectrum[(size_t)bin]); all+=power;
                const double harmonic=std::round(f/f0);
                if(harmonic<1 || std::abs(f-harmonic*f0)>5*hz) other+=power;
            }
            worstAlias=std::max(worstAlias,10.0*std::log10(std::max(1.0e-30,other)/std::max(1.0e-30,all)));
        }
    std::printf ("  imported table worst non-harmonic energy %.2f dB (12 cases)\n",worstAlias);
    check (worstAlias < -70.0, "imported pyramid remains bandlimited on high notes");
    constexpr int sr=48000;
    std::vector<float> tone (sr * 2);
    for (int i=0; i<(int)tone.size(); ++i)
    {
        const double t=(double)i/sr;
        tone[(size_t)i]=(float)(0.6*std::sin(kTwoPi*110*t) + (0.1+0.2*t/2)*std::sin(kTwoPi*220*t));
    }
    const double pitch = estimateResynthPitch (tone, sr/2, sr);
    std::printf ("  110 Hz source estimated %.4f Hz\n", pitch);
    check (std::abs (pitch-110.0)<1.0, "voiced input pitch estimation");
    auto start=std::chrono::steady_clock::now();
    std::vector<float> res;
    check (resynthesizeWavetable (tone, sr, res), "ordinary audio resynthesised");
    auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::printf ("  %zu frames, %.1f ms\n", res.size()/2048, elapsed);
    check (res.size()==64*2048 && validUserFrames(res,64), "64 finite spectral snapshots");
    double frameRms=0;
    for(int i=0;i<2048;++i) frameRms+=(double)res[(size_t)i]*res[(size_t)i];
    frameRms=std::sqrt(frameRms/2048);
    std::printf("  first resynth frame RMS %.6f\n",frameRms);
    check (frameRms>0.1 && frameRms<1.0, "resynthesis produces non-silent bounded output");
    const auto before=res;
    check (!resynthesizeWavetable (invalid, sr, res) && before==res, "resynthesis rejects non-finite input transactionally");
    std::vector<float> noise (sr);
    unsigned seed=12345;
    for (auto& x : noise) { seed=1664525u*seed+1013904223u; x=(float)((int)(seed>>8)-8388608)/8388608.0f * 0.2f; }
    std::vector<float> noiseFrames;
    check (resynthesizeWavetable (noise, sr, noiseFrames, 4), "aperiodic audio uses spectral fallback");
    check (validUserFrames (noiseFrames,4), "aperiodic frames finite and bounded");
    std::printf ("WAVETABLE IMPORT: %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
