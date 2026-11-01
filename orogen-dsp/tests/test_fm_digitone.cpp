#include "../src/engines/fm.h"
#include "../src/core/fft.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace orogen;
static int failures=0;
static void check(bool ok,const char* label){std::printf("  [%s] %s\n",ok?" OK ":"FAIL",label);if(!ok)++failures;}

static EngineParams params(int algorithm)
{
    EngineParams p;
    p.fmAlgorithm=algorithm; p.detune=0; p.level=1;
    p.p[0]=(2.0f-.5f)/15.5f; p.p[1]=.45f; p.p[2]=.35f; p.p[3]=.15f;
    p.fmRatioC=1; p.fmRatioB1=3; p.fmRatioB2=2; p.fmMix=.4f;
    return p;
}
static std::vector<float> render(EngineParams p,int note=57,double sr=48000,int n=8192)
{
    FmEngine e; e.setSampleRate(sr);e.start(note,.8f);e.prepare(p);
    std::vector<float> out((size_t)n);
    for (auto& x:out) x=e.render(p);
    return out;
}
static double difference(const std::vector<float>& a,const std::vector<float>& b)
{
    double s=0;for(size_t i=0;i<a.size();++i){const double d=a[i]-b[i];s+=d*d;}
    return std::sqrt(s/(double)a.size());
}
static double aliasDb(EngineParams p,int note,double sr)
{
    constexpr int n=16384;
    const auto y=render(p,note,sr,n+2048);
    std::vector<std::complex<float>> s((size_t)n);
    for(int i=0;i<n;++i){const double t=(double)i/(n-1),w=.35875-.48829*std::cos(kTwoPi*t)+.14128*std::cos(2*kTwoPi*t)-.01168*std::cos(3*kTwoPi*t);s[(size_t)i]={y[(size_t)i+2048]*(float)w,0};}
    Fft::forward(s);double all=0,other=0,binHz=sr/n,f0=midiToHz(note);
    for(int i=1;i<n/2;++i){double f=i*binHz;if(f<25)continue;double e=std::norm(s[(size_t)i]);all+=e;double h=std::round(f/f0);if(h<1||std::abs(f-h*f0)>5*binHz)other+=e;}
    return 10*std::log10(std::max(1e-30,other)/std::max(1e-30,all));
}
int main()
{
    std::printf("OROGEN: eight Digitone-style FM routes\n");
    bool graphValid=true;
    for(int alg=0;alg<8;++alg){unsigned seen=0;for(int i=0;i<4;++i){int op=kDigitoneOrder[alg][i];graphValid&=(seen&(1u<<op))==0;graphValid&=(kDigitoneRouting[alg].mod[op]&~seen)==0;seen|=1u<<op;}graphValid&=seen==15;}
    check(graphValid,"all eight routing graphs evaluate in dependency order");
    std::array<std::vector<float>,8> audio;
    double minRms=1e9,maxRms=0,minDistance=1e9;
    int bad=0;
    for(int alg=1;alg<=8;++alg){audio[(size_t)alg-1]=render(params(alg));double ss=0;for(float v:audio[(size_t)alg-1]){ss+=(double)v*v;if(!std::isfinite(v))++bad;}const double rms=std::sqrt(ss/audio[(size_t)alg-1].size());minRms=std::min(minRms,rms);maxRms=std::max(maxRms,rms);std::printf("  algorithm %d: RMS %.6f\n",alg,rms);}
    for(int i=0;i<8;++i)for(int j=i+1;j<8;++j)minDistance=std::min(minDistance,difference(audio[(size_t)i],audio[(size_t)j]));
    std::printf("  distinctness min delta RMS %.9f, range %.6f..%.6f\n",minDistance,minRms,maxRms);
    check(bad==0&&minRms>0.001&&maxRms<0.8,"all algorithms are audible, finite and bounded");
    check(minDistance>1e-4,"all eight graphs produce distinct signals for this patch");
    auto reference=params(2); reference.p[3]=0;
    auto modified=reference; modified.fmMix=1.0f;
    const double mixDiff=difference(render(reference),render(modified));
    modified=reference;modified.fmRatioC=2;const double cDiff=difference(render(reference),render(modified));
    modified=reference;modified.fmRatioB1=4;const double b1Diff=difference(render(reference),render(modified));
    modified=reference;modified.fmRatioB2=5;const double b2Diff=difference(render(reference),render(modified));
    modified=reference;modified.p[3]=.7f;const double fbDiff=difference(render(reference),render(modified));
    std::printf("  deltas: MIX %.6f, ratio C %.6f, B1 %.6f, B2 %.6f, feedback %.6f\n",mixDiff,cDiff,b1Diff,b2Diff,fbDiff);
    check(mixDiff>1e-3&&cDiff>1e-3&&b1Diff>1e-3&&b2Diff>1e-3&&fbDiff>1e-4,"X/Y, all operator ratios and feedback reach audio");

    double worst=-300;
    for(int alg=1;alg<=8;++alg)
        for(int note:{48,60,72,84})
        {
            auto p=params(alg);p.p[1]=.25f;p.p[2]=.15f;p.p[3]=0; // integer ratios; harmonic test
            worst=std::max(worst,aliasDb(p,note,48000));
        }
    std::printf("  worst nonharmonic energy: %.2f dB (32 cases, no feedback)\n",worst);
    check(worst < -55.0,"measured harmonic scenarios retain low alias energy");

    double sweepWorst=-300.0;
    int sweepWorstRate=0,sweepWorstNote=0,sweepWorstAlgorithm=0;
    float sweepWorstFeedback=0.0f;
    int sweepCases=0;
    for(double sr:{22050.0,44100.0,48000.0,96000.0,192000.0})
        for(int note:{36,48,60,72,84})
            for(int alg=1;alg<=8;++alg)
                for(float fb:{0.0f,0.15f,0.3f,0.6f,1.0f})
                {
                    auto p=params(alg);
                    p.p[0]=(3.0f-.5f)/15.5f; p.p[1]=.25f; p.p[2]=.18f; p.p[3]=fb;
                    p.fmRatioC=1.0f; p.fmRatioB1=2.0f; p.fmRatioB2=3.0f;
                    const double alias=aliasDb(p,note,sr);
                    if(alias>sweepWorst){sweepWorst=alias;sweepWorstRate=(int)sr;sweepWorstNote=note;sweepWorstAlgorithm=alg;sweepWorstFeedback=fb;}
                    ++sweepCases;
                }
    std::printf("  wide sweep worst nonharmonic energy: %.2f dB (%d cases, 5 rates × 5 notes × 8 routes × 5 feedbacks; worst %d Hz / MIDI %d / algo %d / fb %.2f)\n",
                sweepWorst,sweepCases,sweepWorstRate,sweepWorstNote,sweepWorstAlgorithm,sweepWorstFeedback);
    check(sweepCases==1000&&sweepWorst < -55.0,"wide pitch/rate/route/feedback sweep retains low alias energy");

    int extreme=0, nonfinite=0;float peak=0;
    for(double sr:{8000.0,22050.0,48000.0,96000.0,192000.0})
        for(int note:{0,48,84,127})
            for(int alg=1;alg<=8;++alg)
            {
                auto p=params(alg);p.p[0]=p.p[1]=p.p[2]=p.p[3]=1;p.fmRatioC=p.fmRatioB1=p.fmRatioB2=16;p.fmMix=.5f;
                FmEngine e;e.setSampleRate(sr);e.start(note,1);e.prepare(p);
                for(int i=0;i<2048;++i){float x=e.render(p);if(!std::isfinite(x))++nonfinite;peak=std::max(peak,std::abs(x));}
                ++extreme;
            }
    std::printf("  extremes %d, nonfinite %d, raw peak %.6f\n",extreme,nonfinite,peak);
    check(nonfinite==0&&peak<1.2f,"sample-rate, note, ratio and feedback extremes remain finite");

    FmEngine e;auto p=params(0);e.setSampleRate(48000);e.start(60,1);e.prepare(p);
    float last=0,natural=0,transition=0;int finite=0;
    for(int i=0;i<12000;++i){const float v=e.render(p);if(i>4000)natural=std::max(natural,std::abs(v-last));last=v;}
    for(int alg=1;alg<=8;++alg){p.fmAlgorithm=alg;e.prepare(p);for(int i=0;i<3000;++i){float v=e.render(p);if(std::isfinite(v))++finite;if(i<300)transition=std::max(transition,std::abs(v-last));else natural=std::max(natural,std::abs(v-last));last=v;}}
    p.fmAlgorithm=0;e.prepare(p);for(int i=0;i<3000;++i){float v=e.render(p);if(std::isfinite(v))++finite;if(i<300)transition=std::max(transition,std::abs(v-last));else natural=std::max(natural,std::abs(v-last));last=v;}
    std::printf("  live switch max step %.8f vs natural %.8f\n",transition,natural);
    check(finite==27000&&transition<=natural*1.5f+.005f,"held-note algorithm changes are crossfaded");
    std::printf("DIGITONE-STYLE FM: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
