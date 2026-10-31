// Race/stress regression: rapidly replace prepared engines from two non-audio
// producers while the audio thread consumes them and renders held notes.
#include "instrument/instrument.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace orogen;
int main()
{
    EngineStats::reset();
    Instrument inst; inst.prepare (48000.0, 4);
    Patch live = patchInit();
    inst.setPatch (live); inst.noteOn (60, .8f);
    std::atomic<int> done {0}, staged {0};
    auto producer = [&] (int offset)
    {
        for (int n=0;n<600;++n)
        {
            Patch p=patchInit();
            for (int slot=0;slot<8;++slot)
                p.slots[slot].type=(EngineType)((n+slot*3+offset) % (int)EngineType::Count);
            inst.stagePatch (p);
            if ((n&1)==0) inst.drainRetired();
            staged.fetch_add (1, std::memory_order_relaxed);
        }
        done.fetch_add (1, std::memory_order_release);
    };
    std::thread ui1(producer,0), ui2(producer,4);
    long long samples=0, bad=0; int consumed=0;
    while (done.load (std::memory_order_acquire)<2)
    {
        consumed += inst.pendingEngineSwaps();
        inst.applyPatch (live);
        for (int i=0;i<32;++i)
        {
            float l=0,r=0; inst.render(l,r); ++samples;
            if (!std::isfinite(l) || !std::isfinite(r)) ++bad;
        }
    }
    ui1.join(); ui2.join();
    // A known final publication must win over any unconsumed older request.
    inst.drainRetired();
    Patch last=patchInit();
    for (int s=0;s<8;++s) last.slots[s].type=(EngineType)((s+1)%9);
    inst.stagePatch(last); inst.applyPatch(last); inst.drainRetired();
    int correct=0;
    for (const auto& v:inst.voiceList())
        for (int s=0;s<8;++s) correct += v.activeSlotType(s)==last.slots[s].type;
    const auto remaining=inst.pendingEngineSwaps();
    const auto alive=EngineStats::live();
    const bool ok=staged==1200 && consumed>0 && samples>0 && bad==0
               && correct==32 && remaining==0 && alive==32;
    std::printf("engine handoff: requests %d, observed pending %d, audio samples %lld, nonfinite %lld\n",
                staged.load(),consumed,samples,bad);
    std::printf("final types %d/32, pending %d, live engines %lld/32: %s\n",
                correct,remaining,alive,ok?"PASS":"FAIL");
    return ok?0:1;
}
