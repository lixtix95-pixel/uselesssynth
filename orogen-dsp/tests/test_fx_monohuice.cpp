// MonoHuice 0.9.15 control parity, legacy timing, and real audio regression.
#include "instrument/fx.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

static std::atomic<bool> trackAlloc { false };
static std::atomic<int> allocations { 0 };
void* operator new(std::size_t n) { if (trackAlloc.load()) ++allocations; if (void* p=std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace orogen;
using S = FxChain::Settings;
struct Pair { float l, r; };
static int failed=0, passed=0;
static void check(bool ok,const char* name,double metric=0)
{
    (ok ? passed : failed)++;
    std::printf("[%s] %-38s %.9f\n",ok?"OK":"FAIL",name,metric);
}
static Pair input(int i)
{
    // Nonstationary stereo, with high harmonics and short transients. This
    // exercises spectral masks, capture offsets and envelopes, not just gain.
    const float env=(i%16384 < 6200) ? .75f : .12f;
    const float transient=(i%12000 < 100) ? .17f : 0.0f;
    return {env*(.21f*std::sin(.031f*i)+.12f*std::sin(.137f*i)+.05f*std::sin(.313f*i))+transient,
            env*(.20f*std::sin(.043f*i+.4f)+.09f*std::sin(.173f*i)+.04f*std::sin(.283f*i))-transient*.7f};
}
static std::vector<Pair> render(const S& settings, double bpm=120, int samples=96000)
{
    auto fx=std::make_unique<FxChain>(); fx->prepare(48000); fx->set(settings); fx->setTempo(bpm);
    std::vector<Pair> out((size_t)samples);
    for(int i=0;i<samples;++i) { auto v=input(i); fx->process(v.l,v.r); out[(size_t)i]=v; }
    return out;
}
static double delta(const std::vector<Pair>& a,const std::vector<Pair>& b,int& bad)
{
    double e=0; bad=0;
    for(size_t i=2048;i<a.size();++i) {
        if(!std::isfinite(b[i].l)||!std::isfinite(b[i].r)) ++bad;
        const double l=a[i].l-b[i].l,r=a[i].r-b[i].r; e+=l*l+r*r;
    }
    return std::sqrt(e/(2*(a.size()-2048)));
}
static S base(int module)
{
    S s; s.enabled.fill(false); s.enabled[(size_t)module]=true;
    return s;
}
static void change(const char* name,const S& s,const std::vector<Pair>& ref,double bpm=120)
{
    int bad=0; const auto out=render(s,bpm,(int)ref.size());
    const double d=delta(ref,out,bad); check(d>1e-6 && bad==0,name,d);
}
static void parity(double sr,int mode)
{
    auto wrapper=std::make_unique<GlitchFx>(); wrapper->prepare(sr);
    nova::fx::GlitchCoreAudioProcessor core;
    core.prepare(mode ? sr : std::max(96001.0,sr),512);
    nova::fx::GlitchSettings t;
    t.stutterChance=.86f; t.stutterLengthMs=31; t.repeats=7;
    t.reverseProbability=.65f * 100.0f; t.feedback=.3f * 95.0f; t.pitchSemitones=3;
    t.foldAmount=2; t.foldDrive=1.8f; t.tone=7200; t.toneType=1;
    t.resonance=37; t.captureOffset=63; t.crossfade=57;
    t.lengthSync=0; t.lengthDivision=4;
    t.sizeDrift=28; t.pitchStep=1; t.pitchRandom=4;
    t.rerollProbability=35; t.repeatDecay=18; t.dry=.23f; t.wet=.77f;
    wrapper->setTimingMode(mode); wrapper->setChance(t.stutterChance); wrapper->setLengthMs(t.stutterLengthMs);
    wrapper->setRepeats(t.repeats); wrapper->setReverse(.65f); wrapper->setFeedback(.3f);
    wrapper->setPitch(t.pitchSemitones); wrapper->setFold(t.foldAmount); wrapper->setFoldDrive(t.foldDrive);
    wrapper->setTone(t.tone); wrapper->setToneHighpass(true); wrapper->setResonance(t.resonance);
    wrapper->setOffset(t.captureOffset); wrapper->setCrossfade(t.crossfade); wrapper->setSync(false,4);
    wrapper->setDrift(t.sizeDrift); wrapper->setPitchStep(t.pitchStep); wrapper->setPitchRandom(t.pitchRandom);
    wrapper->setReroll(t.rerollProbability); wrapper->setDecay(t.repeatDecay); wrapper->setLevels(.5f,.23f,.77f);
    wrapper->setTempo(97); core.set(t); core.setTempo(97,false);
    int differences=0,bad=0; double maxDiff=0;
    for(int i=0;i<100000;++i) {
        auto v=input(i); const auto ref=core.process({v.l,v.r}); wrapper->process(v.l,v.r);
        if(std::memcmp(&v.l,&ref.l,sizeof(float)) || std::memcmp(&v.r,&ref.r,sizeof(float))) ++differences;
        if(!std::isfinite(v.l)||!std::isfinite(v.r)) ++bad;
        maxDiff=std::max(maxDiff,(double)std::max(std::abs(v.l-ref.l),std::abs(v.r-ref.r)));
    }
    char name[80]; std::snprintf(name,sizeof(name),"Glitch %s %g Hz vs source",mode?"host":"legacy",sr);
    check(differences==0 && bad==0,name,maxDiff);
}
static void chorusAdapterParity(int mode, float mix, double sr, bool reprepare)
{
    auto fx=std::make_unique<FirmwareChorusFx>(); fx->prepare(sr); fx->setMode(mode);
    std::array<int,8> params {{ 0, 38, 22, 127, 5, 45, 101, 64 }};
    fx->setParameters(params); fx->setMix(mix);
    if (reprepare) { fx->prepare(sr); fx->setMode(mode); fx->setParameters(params); fx->setMix(mix); }
    nova::AudioAdapter source; const int latency=source.prepare(sr);
    source.setDelayZeroGuard(true); source.setSmoothing(mode != 1);
    params[3] = mode == 1 ? (int) std::lround(mix*127) : 127;
    source.setParameters(params);
    std::vector<Pair> history((size_t)latency+8); int write=0, differences=0;
    double maxDiff=0;
    for(int i=0;i<96000;++i) {
        auto v=input(i); history[(size_t)write]=v;
        int at=write-latency; if(at<0) at+=(int)history.size();
        const auto dry=history[(size_t)at]; if(++write==(int)history.size()) write=0;
        float l=0,r=0; source.processFrame(v.l,v.r,l,r,false);
        if(mode==2) { l=dry.l+(l-dry.l)*mix; r=dry.r+(r-dry.r)*mix; }
        fx->process(v.l,v.r);
        const float diff=std::max(std::abs(l-v.l),std::abs(r-v.r));
        if(diff!=0) ++differences; maxDiff=std::max(maxDiff,(double)diff);
    }
    char name[80]; std::snprintf(name,sizeof(name),"Chorus V%d mix %.1f %g Hz%s",mode,mix,sr,reprepare?" reprepare":"");
    check(differences==0,name,maxDiff);
}
static void chorusBoundary()
{
    // The upstream DEL=0 repair is opt-in via MonoJuice V1/V2. Legacy must
    // retain the old trajectory and both must have finite 16-frame outputs.
    nova::ChorusCore guarded, legacy;
    guarded.setDelayZeroGuard(true); guarded.reset(nova::ChorusCore::defaults);
    legacy.setDelayZeroGuard(false); legacy.reset(nova::ChorusCore::defaults);
    auto state=[](const nova::ChorusCore& c) {
        return (long long)nova::ChorusCore::signed24(c.readX(0x545)) * 0x1000000LL + c.readY(0x545);
    };
    std::array<int32_t,32> in{},a{},b{};
    double maxSecond=0; float prev1=0,prev2=0; int changed=0;
    for(int n=0;n<6000;++n) {
        for(int j=0;j<16;++j) {
            const auto x=(int32_t)std::lround(.05*std::sin(6.283185307179586*440*(16*n+j)/44100.0)*8388608.0);
            in[(size_t)(j*2)]=in[(size_t)(j*2+1)]=x;
        }
        guarded.process16(in.data(),a.data()); legacy.process16(in.data(),b.data());
        for(int j=0;j<16;++j) {
            const float x=a[(size_t)j*2]/8388608.0f;
            if(n>3000) maxSecond=std::max(maxSecond,(double)std::abs(x-2*prev1+prev2));
            prev2=prev1;prev1=x; if(a[(size_t)j*2]!=b[(size_t)j*2]) ++changed;
        }
    }
    check(state(guarded)==-16LL*0x1000000LL && maxSecond<.002 && changed>0,
          "Chorus DEL=0 guard / legacy",maxSecond);
}
int main()
{
    for(double sr : {44100.0,48000.0,96000.0,192000.0}) { parity(sr,0); parity(sr,1); }
    chorusBoundary();
    for (int mode : {1,2}) for (float mix : {0.0f,.5f,1.0f}) {
        chorusAdapterParity(mode,mix,44100.0,false);
        chorusAdapterParity(mode,mix,48000.0,true);
    }
    {
        S s=base(FxChain::Glitch); s.glitchTimingMode=1; s.glitchMix=.8f;
        s.glitchChance=1; s.glitchLengthMs=31; s.glitchRepeats=7; s.glitchReverse=.65f;
        s.glitchFeedback=.3f; s.glitchPitch=2; s.glitchFold=2; s.glitchTone=7200;
        auto ref=render(s);
        struct Test {const char* name; float S::*field; float value;};
        const Test t[]={{"Glitch DIST",&S::glitchFoldDrive,4},{"Glitch DRY",&S::glitchDryLevel,.7f},
            {"Glitch WET",&S::glitchWetLevel,.2f},{"Glitch OFFSET",&S::glitchOffset,0},
            {"Glitch XFADE",&S::glitchCrossfade,0},{"Glitch RESONANCE",&S::glitchResonance,80},
            {"Glitch DRIFT",&S::glitchDrift,75},{"Glitch PITCH STEP",&S::glitchPitchStep,3},
            {"Glitch PITCH RAND",&S::glitchPitchRandom,6},{"Glitch REROLL",&S::glitchReroll,100},
            {"Glitch DECAY",&S::glitchDecay,65},{"Glitch LENGTH 1..500",&S::glitchLengthExt,480}};
        for(auto v:t) { S next=s; next.*(v.field)=v.value; change(v.name,next,ref); }
        S hp=s; hp.glitchHighpass=true; change("Glitch highpass",hp,ref);
        S sync=s; sync.glitchSync=true; sync.glitchDivision=6; change("Glitch sync 1/32",sync,ref,120);
        auto syncref=render(sync,120); change("Glitch sync host BPM 73",sync,syncref,73);
        sync.glitchDivision=4; change("Glitch sync division",sync,syncref,120);
        S zero=s; zero.glitchMix=0; zero.glitchDryLevel=0; zero.glitchWetLevel=0;
        auto silence=render(zero); zero.glitchWetLevel=.8f; change("Glitch independent wet at MIX=0",zero,silence);
    }
    {
        S s=base(FxChain::Spectre); s.spectreModel=0; s.spectreMix=.9f;
        s.spectreWindow=1; s.spectrePeaks=7; s.spectreWidth=1;
        auto ref=render(s);
        struct Test {const char* name; float S::*field; float value;};
        const Test t[]={{"Spectre REJECT",&S::spectreReject,-12},{"Spectre ATTACK",&S::spectreAttack,1200},
            {"Spectre RELEASE",&S::spectreRelease,2200},{"Spectre PEAKS=0 all",&S::spectrePeaksExt,0},
            {"Spectre WIDTH=24",&S::spectreWidthExt,24},{"Spectre LOW extended",&S::spectreLowExt,12000},
            {"Spectre HIGH extended",&S::spectreHighExt,1000},{"Spectre WINDOW=65536",&S::spectreWindowExt,7}};
        for(auto v:t) { S next=s; next.*(v.field)=v.value; change(v.name,next,ref); }
        auto fx=std::make_unique<FxChain>(); fx->prepare(48000); s.spectreWindowExt=7; fx->set(s);
        check(fx->latency()==65536+1024,"Spectre 65536 latency",fx->latency());
    }
    {
        S s=base(FxChain::Comb); s.combMix=.8f; s.combModel=0; s.combRouting=2;
        s.combFreqA=250; s.combFreqB=420; s.combDepth=0; s.combEnvelope=.2f;
        auto ref=render(s);
        struct Test {const char* name; float S::*field; float value;};
        const Test common[]={{"Comb ODIN MIX",&S::combOdinMix,.2f},{"Comb drive extended",&S::combDriveExt,30},
            {"Comb output extended",&S::combOutputExt,-28},{"Comb depth extended",&S::combDepthExt,18},
            {"Comb envelope extended",&S::combEnvelopeExt,-24},{"Comb frequency A extended",&S::combFreqAExt,6400},
            {"Comb frequency B extended",&S::combFreqBExt,5200}};
        for(auto v:common) { S next=s; next.*(v.field)=v.value; change(v.name,next,ref); }
        S motion=s; motion.combDepthExt=12; auto motionRef=render(motion); motion.combRateExt=17; change("Comb speed extended",motion,motionRef);
        s.combModel=1; s.combFeedback=50; s.combDelayMs=20; auto parRef=render(s);
        const Test parallel[]={{"Comb dry extended",&S::combDryDbExt,12},{"Comb wet extended",&S::combWetDbExt,10},
            {"Comb feedback extended",&S::combFeedbackExt,95},{"Comb highpass extended",&S::combHighpassExt,5000},
            {"Comb tune extended",&S::combTuneExt,42}};
        for(auto v:parallel) { S next=s; next.*(v.field)=v.value; change(v.name,next,parRef); }
    }
    {
        auto fx=std::make_unique<FxChain>(); fx->prepare(48000); S s=base(FxChain::Glitch); s.glitchMix=.8f;
        fx->set(s); allocations=0; trackAlloc=true;
        for(int i=0;i<4000;++i) {
            if(i%40==0) { s.glitchTimingMode=1-s.glitchTimingMode; s.glitchSync=!s.glitchSync;
                s.glitchDrift=(float)(i%101); s.glitchPitchRandom=(float)(i%13); fx->set(s); fx->setTempo(73+i%200); }
            auto v=input(i); fx->process(v.l,v.r);
        }
        trackAlloc=false; check(allocations.load()==0,"FX mode/tempo automation allocations",allocations.load());
    }
    std::printf("MonoHuice FX: %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
}
