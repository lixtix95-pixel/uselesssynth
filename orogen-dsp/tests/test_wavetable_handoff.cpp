#include "instrument/instrument.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace orogen;

static std::shared_ptr<const UserWavetable> table (int harmonics, const char* name)
{
    std::vector<float> samples (kUserWtFrameSamples);
    for (int i=0;i<kUserWtFrameSamples;++i)
        for (int h=1;h<=harmonics;++h)
            samples[(size_t)i]+=(float)(0.35*std::sin(kTwoPi*i*h/kUserWtFrameSamples)/h);
    return makeUserWavetable (samples,1,name);
}

int main()
{
    auto a=table(1,"sine"), b=table(7,"seven harmonics");
    if (!a || !b) return 2;
    Instrument inst; inst.prepare (48000, 4);
    Patch p=patchInit();
    for (int i=1;i<8;++i) p.slots[i].on=false;
    p.slots[0].params.wtUser=true; p.slots[0].params.detune=0;
    p.slots[0].params.p[0]=0; p.slots[0].params.p[1]=0; p.slots[0].params.p[2]=0;
    p.slots[0].params.p[3]=0.3f; p.slots[0].attack=.005f; p.slots[0].sustain=1;
    Instrument::UserTables assets{}; assets[0]=a;
    inst.stageUserWavetables(assets,p); inst.setPatch(p); inst.noteOn(60,.8f);
    float last=0, l=0, r=0, natural=0, naturalAfter=0, transition=0;
    double before=0, after=0;
    for(int i=0;i<16000;++i) { inst.render(l,r); if(i>12000) { natural=std::max(natural,std::abs(l-last)); before+=l*l; } last=l; }
    assets[0]=b; inst.stageUserWavetables(assets,p); inst.applyPatch(p);
    for(int i=0;i<6000;++i) { inst.render(l,r); if(i<1800) transition=std::max(transition,std::abs(l-last)); if(i>2000) { after+=l*l; naturalAfter=std::max(naturalAfter,std::abs(l-last)); } last=l; }
    before=std::sqrt(before/3999); after=std::sqrt(after/3999);
    std::printf("held-note table swap: natural max step %.9f / %.9f, transition %.9f, RMS %.6f -> %.6f\n",natural,naturalAfter,transition,before,after);
    bool ok=std::isfinite(transition) && transition < std::max(natural,naturalAfter)*1.10f+0.0001f && before>0.001 && after>0.001;
    inst.drainRetired();

    std::atomic<bool> done{false};
    std::thread producer([&] {
        for(int i=0;i<240;++i) { inst.stageUserWavetable(0,(i&1)?a:b,p); if((i&3)==0) inst.drainRetired(); }
        done.store(true,std::memory_order_release);
    });
    int samples=0, bad=0;
    while(!done.load(std::memory_order_acquire))
    {
        inst.applyPatch(p);
        for(int i=0;i<32;++i) { inst.render(l,r); ++samples; if(!std::isfinite(l)||!std::isfinite(r)) ++bad; }
    }
    producer.join();
    for(int i=0;i<2048;++i) { if((i&31)==0) inst.applyPatch(p); inst.render(l,r); if(!std::isfinite(l)||!std::isfinite(r)) ++bad; }
    inst.drainRetired();
    std::printf("concurrent table handoff: 240 updates, %d frames, nonfinite %d, pending %d\n",samples,bad,inst.pendingEngineSwaps());
    ok = ok && samples>0 && bad==0 && inst.pendingEngineSwaps()==0;
    std::printf("WAVETABLE HANDOFF: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
