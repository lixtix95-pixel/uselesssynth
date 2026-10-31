// Added effect controls must change DSP, not merely serialize/UI-bind.
#include "instrument/fx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
using namespace orogen;
struct Pair { float l,r; };
using S = FxChain::Settings;

static S base()
{
    S s; s.enabled.fill(false); s.enabled[(size_t) FxChain::Comb]=true;
    s.combModel=1; s.combMix=0.8f; s.combDelayMs=25; s.combFeedback=62;
    s.combHighpass=30; s.combDriveDb=3; s.combEnvelope=1.0f;
    s.combGateDb=-33; s.combGateMs=8; s.combWetDb=-8; s.combDryDb=-9;
    return s;
}
static std::vector<Pair> render(const S& s)
{
    auto fx=std::make_unique<FxChain>(); fx->prepare(48000); fx->set(s);
    std::vector<Pair> result(96000);
    for (int i=0;i<(int) result.size();++i)
    {
        const float env=(i%12000 < 5000) ? 1.0f : 0.055f;
        float l=env*(.37f*std::sin(.051f*i)+.19f*std::sin(.131f*i));
        float r=env*(.30f*std::sin(.042f*i+.4f)+.18f*std::sin(.117f*i));
        fx->process(l,r); result[(size_t)i]={l,r};
    }
    return result;
}
static double delta(const std::vector<Pair>& a,const std::vector<Pair>& b,int& bad)
{
    double e=0; bad=0;
    for(size_t i=2048;i<a.size();++i){
        if(!std::isfinite(b[i].l)||!std::isfinite(b[i].r)) ++bad;
        const double l=a[i].l-b[i].l,r=a[i].r-b[i].r;e+=l*l+r*r;
    }
    return std::sqrt(e/(2*(a.size()-2048)));
}
int main()
{
    struct Test { const char* name; float S::*field; float value; };
    const Test tests[] = {
        {"Env Decay", &S::combDecayMs, 800}, {"Tap A", &S::combTapA, 45},
        {"Tap A zero", &S::combTapA, 0}, {"Tap B", &S::combTapB, 60}, {"Tap C", &S::combTapC, 80}, {"Tap D", &S::combTapD, 100},
        {"Multiplier", &S::combMultiplier, .42f}, {"Parallel Rate", &S::combParRate, 3},
        {"Smooth ms", &S::combSmoothMs, 900}, {"Cross", &S::combCross, 85},
        {"AM", &S::combAm, 80}, {"Lean", &S::combLean, 70},
        {"Gate dB", &S::combGateDb, -8}, {"Gate ms", &S::combGateMs, 300},
        {"Limiter ms", &S::combLimiterMs, 5}, {"Dry dB", &S::combDryDb, -30}, {"Wet dB", &S::combWetDb, -35}
    };
    const S original=base(); const auto reference=render(original);
    int fail=0;
    for(const auto& test:tests){
        S next=original; next.*(test.field)=test.value; int bad=0;
        const double d=delta(reference,render(next),bad);
        const bool ok=d>1.0e-6 && bad==0; if(!ok) ++fail;
        std::printf("[%s] %-15s delta RMS %.9f / nonfinite %d\n",ok?"OK":"FAIL",test.name,d,bad);
    }
    {
        S a; a.enabled.fill(false); a.enabled[(size_t)FxChain::Drive]=true; a.drive=.8f;
        S b=a; b.driveMix=.25f; int bad=0; const double d=delta(render(a),render(b),bad);
        const bool ok=d>1.0e-6&&bad==0; if(!ok)++fail;
        std::printf("[%s] Drive Mix      delta RMS %.9f / nonfinite %d\n",ok?"OK":"FAIL",d,bad);
    }
    {
        S a=base(); a.combModel=0; a.combRouting=2; a.combFreqA=180; a.combFreqB=420;
        S b=a; b.combNegativeB=2; int bad=0; const double d=delta(render(a),render(b),bad);
        const bool ok=d>1.0e-6&&bad==0; if(!ok)++fail;
        std::printf("[%s] Negative B     delta RMS %.9f / nonfinite %d\n",ok?"OK":"FAIL",d,bad);
    }
    {
        S a; a.enabled.fill(false); a.enabled[(size_t)FxChain::Plate]=true; a.plateMix=.9f;
        S b=a; b.plateDc=false; int bad=0; const double d=delta(render(a),render(b),bad);
        const bool ok=d>1.0e-7&&bad==0; if(!ok)++fail;
        std::printf("[%s] Plate DC       delta RMS %.9f / nonfinite %d\n",ok?"OK":"FAIL",d,bad);
    }
    std::printf("FX extra failures: %d\n",fail); return fail?1:0;
}
