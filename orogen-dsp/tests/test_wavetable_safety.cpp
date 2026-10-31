// Targeted Wavetable safety, transition and high-key alias regressions.
#include "../src/engines/wavetable.h"
#include "../src/core/fft.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
using namespace orogen;
static int failures=0;
static void check(bool ok,const char* what){std::printf("[%s] %s\n",ok?"OK":"FAIL",what);if(!ok)++failures;}
struct AliasResult { double aliasDb; double rms; };

// Алиасинг как ОТНОШЕНИЕ ЭНЕРГИЙ: сумма энергии не-гармонических бинов / полная.
// Так метрика не зависит от того, насколько тих основной тон (у формантных
// таблиц он на -70 дБ тише формант, и «относительно фундаментала» шум выглядел
// бы катастрофой при том же абсолютном уровне).
static AliasResult measureAliasing (WavetableEngine& eng, const EngineParams& p,
                                    int note, double sr, int fftSize = 16384)
{
    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);

    std::vector<float> buf ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i) buf[(size_t) i] = eng.render (p);

    std::vector<std::complex<float>> spec ((size_t) fftSize);
    double rms = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        const double t = (double) i / (fftSize - 1);
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                       + 0.14128 * std::cos (2 * kTwoPi * t)
                       - 0.01168 * std::cos (3 * kTwoPi * t);
        spec[(size_t) i] = { (float) (buf[(size_t) i] * w), 0.0f };
        rms += (double) buf[(size_t) i] * buf[(size_t) i];
    }
    rms = std::sqrt (rms / fftSize);
    Fft::forward (spec);

    const double f0 = midiToHz (note);
    const double binHz = sr / fftSize;

    double totalE = 0.0, aliasE = 0.0;
    for (int b = 1; b < fftSize / 2; ++b)
    {
        const double hz = (double) b * binHz;
        if (hz < 25.0) continue;
        const double m2 = std::norm (spec[(size_t) b]);
        totalE += m2;

        const double ratio = hz / f0;
        const double nearest = std::round (ratio);
        // допуск только в бинах: окно Блэкмана-Харриса «размазывает» партиал
        // на ±4 бина, независимо от его частоты
        const double tol = 5.0 * binHz;
        if (nearest >= 1.0 && std::fabs (ratio - nearest) * f0 < tol) continue;
        aliasE += m2;
    }
    const double db = 10.0 * std::log10 (std::max (1e-30, aliasE) / std::max (1e-30, totalE));
    return { db, rms };
}

int main()
{
    WavetableBank bank; bank.buildFactory();
    EngineParams p; p.table=0;p.level=1;p.detune=0;p.p[0]=.6f;p.p[1]=0;p.p[2]=0;p.p[3]=.3f;

    // No bank / no prepare must be silent, rather than dereference nullptr.
    WavetableEngine empty; empty.setSampleRate(48000); empty.start(60,1);empty.prepare(p);
    check(empty.render(p)==0.0f,"missing bank is silent");
    WavetableEngine notPrepared;notPrepared.setBank(&bank);notPrepared.setSampleRate(48000);notPrepared.start(60,1);
    check(notPrepared.render(p)==0.0f,"start without prepare is silent");

    int finiteCases=0, muteCases=0, bad=0, badMute=0;
    WavetableEngine sweep;sweep.setBank(&bank);
    for(double sr:{8000.,11025.,16000.,22050.,32000.,44100.,48000.,96000.,192000.})
      for(int note:{0,48,96,120,127}) for(float tune:{-24.f,0.f,24.f})
        for(int quality:{0,1,2}) for(int distortion:{0,1,2}) for(int table:{0,7})
        {
          EngineParams q=p;q.table=table;q.tuneSemis=tune;
          q.p[1]=distortion==1?.5f:0.f;q.p[2]=distortion==2?1.f:0.f;q.p[0]=1.f;
          sweep.setSampleRate(sr);sweep.setQuality(quality);sweep.start(note,1);sweep.prepare(q);
          float peak=0;for(int i=0;i<256;i++){float v=sweep.render(q);if(!std::isfinite(v))++bad;peak=std::max(peak,std::abs(v));}
          ++finiteCases;
          if(midiToHz((double)note+tune)>=.5*sr){++muteCases;if(peak>1.e-8f)++badMute;}
        }
    std::printf("extremes: %d cases, nonfinite %d, above Nyquist muted %d, failures %d\n",finiteCases,bad,muteCases,badMute);
    check(finiteCases==2430&&bad==0,"notes/tuning/sample rates/quality never read out of bounds or produce nonfinite samples");
    check(muteCases>100&&badMute==0,"fresh notes above Nyquist are silent");

    // Repeat note-on with identical RNG progression. Old filter contents must
    // not leak into the next note; new/fresh states should match bit-for-bit.
    WavetableEngine reused, clean;reused.setBank(&bank);clean.setBank(&bank);
    reused.setSampleRate(48000);clean.setSampleRate(48000);EngineParams dist=p;dist.p[1]=.4f;dist.p[2]=.7f;
    reused.start(60,1);clean.start(60,1);reused.prepare(dist);clean.prepare(dist);
    for(int i=0;i<4000;i++)(void)reused.render(dist);
    reused.stop();clean.stop();reused.start(60,1);clean.start(60,1);reused.prepare(dist);clean.prepare(dist);
    int different=0;for(int i=0;i<2048;i++){float a=reused.render(dist),b=clean.render(dist);if(std::memcmp(&a,&b,sizeof(float))!=0)++different;}
    check(different==0,"retrigger resets the nonlinear/DC filter history (bit-identical to fresh state)");

    int tests=0;double worstFirst=0,worstStep=0,worstNatural=0;
    for(int table=0;table<8;table++) for(int delay:{1000,2000,5000}) for(int kind=0;kind<7;kind++)
    {
        EngineParams a=p,b=p;a.table=b.table=table;
        if(kind==0)b.p[1]=.6f;                         // direct -> warp
        if(kind==1)b.p[2]=1.f;                         // direct -> fold
        if(kind==2){a.p[1]=b.p[1]=.2f;a.p[2]=.44f;b.p[2]=.46f;}
        if(kind==3){a.p[1]=b.p[1]=.2f;a.p[2]=.46f;b.p[2]=.44f;}
        if(kind==4)b.table=(table+3)%8;
        if(kind==5){a.p[1]=b.p[1]=.2f;b.p[2]=.001f;} // fold starts while OS is already on
        if(kind==6)b.p[3]=0.f;                          // smoothed Tilt automation
        WavetableEngine changed;changed.setBank(&bank);changed.setSampleRate(48000);changed.start(45,1);changed.prepare(a);
        float previous=0,natural=0;for(int i=0;i<delay;i++){float v=changed.render(a);if(i>delay-500)natural=std::max(natural,std::abs(v-previous));previous=v;}
        auto reference=changed;changed.prepare(b);float step=0,first=0;
        for(int i=0;i<480;i++){float expected=reference.render(a),v=changed.render(b);if(!std::isfinite(v))++bad;if(i==0)first=std::abs(v-expected);step=std::max(step,std::abs(v-previous));previous=v;}
        worstFirst=std::max(worstFirst,(double)first);worstStep=std::max(worstStep,(double)step);worstNatural=std::max(worstNatural,(double)natural);++tests;
    }
    std::printf("transitions: %d cases; first delta %.9f, max sample step %.9f (old natural max %.9f)\n",tests,worstFirst,worstStep,worstNatural);
    check(tests==168&&bad==0&&worstFirst<0.005,"table, direct/OS and 4x/8x switches have bounded first-sample discontinuity");

    // Spectral direction of P4: 0 is darker than neutral 0.3, 1 brighter.
    int tiltCases=0;double minDarkDelta=100,minBrightDelta=100;
    for(int table:{0,3,7})
    {
        double band[3]={};int idx=0;
        for(float tilt:{0.f,.3f,1.f})
        {
            EngineParams q=p;q.table=table;q.p[0]=.8f;q.p[3]=tilt;
            WavetableEngine e;e.setBank(&bank);e.setSampleRate(48000);e.start(57,1);e.prepare(q);
            for(int i=0;i<24000;i++)(void)e.render(q);
            constexpr int N=16384;std::vector<std::complex<float>> spec(N);
            for(int i=0;i<N;i++){float v=e.render(q);float w=.5f-.5f*std::cos((float)(2*kPi*i/(N-1)));spec[(size_t)i]={v*w,0.f};}
            Fft::forward(spec);double low=0,high=0;
            for(int i=1;i<N/2;i++){double hz=48000.*i/N;if(hz>3000&&hz<12000)high+=std::norm(spec[(size_t)i]);if(hz>100&&hz<2000)low+=std::norm(spec[(size_t)i]);}
            band[idx++]=10*std::log10(high/low);
        }
        minDarkDelta=std::min(minDarkDelta,band[1]-band[0]);minBrightDelta=std::min(minBrightDelta,band[2]-band[1]);
        if(band[0]<band[1]&&band[1]<band[2])++tiltCases;
    }
    std::printf("Tilt: monotonic %d/3; dark/bright high-to-low change at least %.3f/%.3f dB\n",tiltCases,minDarkDelta,minBrightDelta);
    check(tiltCases==3&&minDarkDelta>1.0&&minBrightDelta>1.0,"P4 Tilt darkens below 0.3 and brightens above it");

    double worstAlias=-300;int aliases=0;
    for(double sr:{32000.,44100.,48000.}) for(int note:{108,120,127}) for(int table=0;table<8;table++)
    {
        EngineParams q=p;q.table=table;q.p[0]=.45f;
        WavetableEngine e;e.setBank(&bank);auto r=measureAliasing(e,q,note,sr);
        worstAlias=std::max(worstAlias,r.aliasDb);++aliases;
    }
    std::printf("high-key alias: %d cases, worst %.2f dB of total energy\n",aliases,worstAlias);
    check(aliases==72&&worstAlias < -75.0,"upper MIDI notes have band-limited MIP and dense interpolation");

    double hdError=0;int hdCount=0;
    for(const auto& t:bank.tables)for(size_t l=0;l<t.highDensityLevels.size();l++)if(!t.highDensityLevels[l].empty())
    {
        const int n=t.lens[l],dest=t.highDensityLens[l],ratio=dest/n;
        for(int f=0;f<t.frames;f++)for(int i=0;i<n;i++)
            hdError=std::max(hdError,(double)std::abs(t.levels[l][(size_t)f*n+i]-t.highDensityLevels[l][(size_t)f*dest+i*ratio]));
        ++hdCount;
    }
    std::printf("dense legacy levels %d, max error at original points %.9f\n",hdCount,hdError);
    check(hdCount>0&&hdError<2.e-5,"dense resampling preserves legacy samples at original grid points");

    const std::string legacyPath="build/wt_legacy.orowt",badPath="build/wt_invalid.orowt";
    WavetableBank old=bank;for(auto& t:old.tables){t.levels.resize(kWtLegacyLevels);t.lens.resize(kWtLegacyLevels);t.harms.resize(kWtLegacyLevels);}
    bool saved=old.save(legacyPath);WavetableBank restored;bool loaded=restored.load(legacyPath);bool identical=loaded&&restored.tables.size()==bank.tables.size();
    if(identical)for(size_t i=0;i<bank.tables.size();i++)
    {
        identical&=restored.tables[i].levels.size()==bank.tables[i].levels.size();
        if(identical)for(size_t l=0;l<bank.tables[i].levels.size();l++) identical&=restored.tables[i].levels[l]==bank.tables[i].levels[l];
    }
    check(saved&&loaded&&identical,"legacy 25-level OROWT1 loads and extends without altering old samples");
    // Truncated, malformed huge table count, and invalid NaN samples must fail
    // without replacing a previously loaded bank.
    int rejected=0, intact=0;
    for(int kind=0;kind<4;kind++)
    {
        FILE* f=std::fopen(badPath.c_str(),"wb");
        if(!f){++failures;break;}
        const uint32_t nt=kind==0?0xffffffffu:1u,nf=1,nl=1,hm=1,len=8,nameLen=0;
        std::fwrite("OROWT1",1,6,f);std::fwrite(&nt,4,1,f);
        if(kind>=1){std::fwrite(&nf,4,1,f);std::fwrite(&nl,4,1,f);std::fwrite(&hm,4,1,f);std::fwrite(&len,4,1,f);}
        if(kind>=2){float samples[8]={};if(kind==2){uint32_t nan=0x7fc00000u;std::memcpy(&samples[3],&nan,4);}std::fwrite(samples,4,8,f);std::fwrite(&nameLen,4,1,f);}
        if(kind==3)std::fwrite("X",1,1,f); // unexpected trailing bytes
        std::fclose(f);
        if(!restored.load(badPath))++rejected;
        if(restored.tables.size()==bank.tables.size()&&restored.tables[0].name==bank.tables[0].name)++intact;
    }
    std::remove(legacyPath.c_str());std::remove(badPath.c_str());
    std::printf("bad OROWT1 rejected %d/4, previous bank intact %d/4\n",rejected,intact);
    check(rejected==4&&intact==4,"malformed/truncated/nonfinite OROWT1 rejected transactionally");
    std::printf("Wavetable safety: %d failures\n",failures);
    return failures==0?0:1;
}
