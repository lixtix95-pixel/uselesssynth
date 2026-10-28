/* ParallelComb 0.2.14 DSP adaptation. SPDX-License-Identifier: GPL-3.0-only
 * Copyright Takamitsu Endo (ryukau@gmail.com); adaptation 2026 MonomachineNova.
 * Pinned original DSPCore and identical helper headers: Reference/Uhhyou.
 * Replaced VST parameter/MIDI plumbing with physical-value settings; local
 * smoothing coefficients remove upstream cross-instance static state. Core
 * tap, limiter, gate, highpass and 16x resampling algorithms are retained.
 */
#pragma once
#include "FirmwarePrimitives.h"
#include "../third_party/Uhhyou/common/dsp/basiclimiter.hpp"
#include "../third_party/Uhhyou/common/dsp/multirate.hpp"
#include "../third_party/Uhhyou/ParallelComb/source/dsp/easygate.hpp"
#include "../third_party/Uhhyou/ParallelComb/source/dsp/parallelcomb.hpp"
namespace nova::fx {
struct CombSettings {
 std::array<float,4> delayMs{{100,100,100,100}};
 float multiplier=1,feedback=25,highpass=5,limiterMs=200,rate=.5f,smoothMs=0,lean=0,cross=0,am=0,gateDB=-140,gateMs=10,dryDB=0,wetDB=0,tune=0;
 bool midSide=false,oversample=false;
};
class ParallelCombModel {
 using OS=SomeDSP::OverSampler16<float>;
public:
  void prepare(double rate){sr=float(rate);oversampleAvailable=rate<=highRateLimit;const float maxFold=oversampleAvailable?16.f:1.f;for(auto& c:comb)c.setup(sr*maxFold,4.f);for(auto& l:limit)l.resize(size_t(sr*maxFold*(64.f/48000.f))+1);gateHost.setup(sr,.001f);gateOversampled.setup(sr*maxFold,.001f);prepared=true;set(p);reset();}
  void set(CombSettings s){s.oversample=s.oversample&&oversampleAvailable;const bool mode=s.oversample!=p.oversample||s.midSide!=p.midSide;p=s;if(!prepared)return;assign(false);if(mode)reset();}
  void reset(){if(!prepared)return;assign(true);delayOut={};for(auto& o:os)o.reset();gateHost.reset();gateOversampled.reset();for(auto& c:comb)c.reset();for(auto& h:highpass)h.reset();for(auto& l:limit)l.reset();}
 Frame process(Frame in){float a=in.l,b=in.r;if(p.midSide){const float m=a+b;b=a-b;a=m;}
  if(p.oversample){os[0].push(a);os[1].push(b);for(size_t i=0;i<16;i++){const auto v=internal(os[0].at(i),os[1].at(i));os[0].inputBuffer[i]=v.l;os[1].inputBuffer[i]=v.r;}a=os[0].process();b=os[1].process();}
  else {const auto v=internal(a,b);a=v.l;b=v.r;}
  if(p.midSide){const float l=.5f*(a+b);b=.5f*(a-b);a=l;}return {clean(a),clean(b)};
 }
private:
 static float db(float d,float floor){return d<=floor?0.f:std::pow(10.f,d*.05f);}
 void assign(bool resetValues){const float fold=p.oversample?16.f:1.f,up=fold*sr;kp=float(SomeDSP::EMAFilter<double>::cutoffToP(up,std::clamp<double>(1.0/float(.04),0.0,double(up)*.5)));
  const float lean=std::clamp(p.lean*.01f,-1.f,1.f),l=lean<0?1+lean:1,r=lean<0?1:1-lean;
  const float mul=std::clamp(p.multiplier,0.f,1.f)*std::exp2(-p.tune/12);
  for(size_t i=0;i<4;i++){const float t=mul*up*std::clamp(p.delayMs[i]*.001f,0.f,4.f);if(resetValues){comb[0].time.resetAt(i,t*l);comb[1].time.resetAt(i,t*r);}else{comb[0].time.pushAt(i,t*l);comb[1].time.pushAt(i,t*r);}}
  // Defensive clamps for LFO-modulated values. No-ops for every in-spec knob
  // value; limiterMs 0 previously divided by zero (1/0 release) and poisoned
  // the limiter with a sticky NaN (permanent silence until CLEAR). 0.001 ms
  // keeps the intended "fastest release" without the division by zero.
   const float rate=std::clamp(p.rate,0.f,4.f),seconds=std::clamp(p.smoothMs,0.f,100000.f)*.001f;
    const float feedback=std::clamp(p.feedback,0.f,100.f),highpassCutoff=std::clamp(p.highpass,0.f,up*.5f);
  const float cross=std::clamp(p.cross,0.f,100.f),am=std::clamp(p.am,0.f,100.f);
  const float gateMs=std::clamp(p.gateMs,.1f,10000.f),limiterMs=std::clamp(p.limiterMs,.001f,16000.f);
   const float gateCutoff=std::min(1.f/(gateMs*.001f),up*.5f);
   const float v[]={rate,seconds==0?1.f:float(SomeDSP::EMAFilter<double>::cutoffToP(up,1.f/seconds)),feedback*.01f,float(SomeDSP::EMAFilter<double>::cutoffToP(up,highpassCutoff)),cross*.01f,am,float(SomeDSP::EMAFilter<double>::cutoffToP(up,gateCutoff)),db(p.dryDB,-60),db(p.wetDB,-60)};
  for(int i=0;i<9;i++){if(resetValues)smoothers[i].reset(v[i]);else smoothers[i].push(v[i]);}
   activeGate().prepare(up,db(p.gateDB,-140));
  for(auto& lmt:limit)lmt.prepare(up,16.f*(64.f/48000.f)/fold,limiterMs*.001f,1.f);
 }
  SomeDSP::EasyGate<float>& activeGate(){return p.oversample?gateOversampled:gateHost;}
  Frame internal(float a,float b){std::array<float,9> v;for(int i=0;i<9;i++)v[i]=smoothers[i].process(kp);
   const float g=activeGate().process(std::max(std::abs(a),std::abs(b)),v[6]);
  const float c0=delayOut.l+v[4]*(delayOut.r-delayOut.l),c1=delayOut.r+v[4]*(delayOut.l-delayOut.r);
  const float d0=g*comb[0].process(a+v[2]*c0,v[0]+v[5]*c0,v[1]);
  const float d1=g*comb[1].process(b+v[2]*c1,v[0]+v[5]*c1,v[1]);
  delayOut={limit[0].process(highpass[0].process(d0,v[3])),limit[1].process(highpass[1].process(d1,v[3]))};
  return {v[7]*a+v[8]*delayOut.l,v[7]*b+v[8]*delayOut.r};
 }
 CombSettings p;float sr=44100,kp=1;bool prepared=false;Frame delayOut;
 std::array<SomeDSP::ExpSmootherLocal<float>,9> smoothers;std::array<OS,2> os;
  SomeDSP::EasyGate<float> gateHost,gateOversampled;std::array<SomeDSP::ParallelComb<float,4>,2> comb;
  std::array<SomeDSP::EMAHighpass<float,4>,2> highpass;std::array<SomeDSP::BasicLimiter<float>,2> limit;bool oversampleAvailable=false;
};
}
