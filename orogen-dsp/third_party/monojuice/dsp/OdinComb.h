/* Adapted from Odin 2 CombFilter.cpp/.h
 * Copyright (C) 2020-2021 TheWaveWarden; modifications 2026 MonomachineNova contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * See Licenses/ODIN2-GPL-3.0.txt and Reference/odin2.
 * Removed synth modulation pointers; added stereo, independent A/B routing,
 * preallocated rate-sized buffers, bounded input/history and a 99.5% resonance ceiling.
 */
#pragma once
#include "FirmwarePrimitives.h"
#include <array>
namespace nova::fx {
struct OdinSettings {float freqA=220,resA=75,freqB=331,resB=75,balance=50,mix=100;int routing=0;bool negativeA=false,negativeB=false;};
class OdinCombLine {
public:
 void prepare(double rate){sr=float(rate);buffer.assign(std::size_t(std::ceil(rate/40*1.1)+8),0);reset();}
 void reset(){std::fill(buffer.begin(),buffer.end(),0);write=0;smooth=control;restart=true;}
 void set(float frequency,float resonance,bool negative){control=1./std::clamp(frequency,40.f,std::min(16000.f,sr*.45f));feedback=std::clamp(resonance*.01f,0.f,.995f);sign=negative?-1:1;}
 float process(float input){
  if(restart){smooth=control;restart=false;}
  smooth=(smooth-control)*.999+control;
  const float delayTime=float(smooth);const float read=float(write)-delayTime*sr;
  int lo=int(std::floor(read)),hi=lo+1;const float frac=read-float(lo);const int size=int(buffer.size());
  while(lo<0)lo+=size;while(hi<0)hi+=size;while(hi>=size)hi-=size;
  const float output=(1-frac)*buffer[std::size_t(lo)]+frac*buffer[std::size_t(hi)];
  input=std::clamp(clean(input),-8.f,8.f);
  buffer[std::size_t(write)]=std::clamp(input+output*feedback*float(sign),-16.f,16.f);
  if(++write>=size)write=0;
  // Upstream computes a DC-blocker result but discards it. Its returned signal
  // is exactly this expression; do not silently insert a different DC filter.
  return (input+output)*.5f;
 }
private:std::vector<float> buffer;int write=0,sign=1;float sr=44100,feedback=0;double control=1./220,smooth=1./220;bool restart=true;
};
class OdinComb {
public:
 void prepare(double rate){for(auto& f:a)f.prepare(rate);for(auto& f:b)f.prepare(rate);smoothing=float(1-std::exp(-1/(rate*.02)));reset();}
 void reset(){for(auto& f:a)f.reset();for(auto& f:b)f.reset();amount=p.mix*.01f;balance=p.balance*.01f;}
 void set(OdinSettings s){s.routing=std::clamp(s.routing,0,2);if(s.routing!=p.routing){for(auto& f:b)f.reset();}p=s;for(auto& f:a)f.set(s.freqA,s.resA,s.negativeA);for(auto& f:b)f.set(s.freqB,s.resB,s.negativeB);}
 Frame process(Frame in){amount+=(std::clamp(p.mix*.01f,0.f,1.f)-amount)*smoothing;balance+=(std::clamp(p.balance*.01f,0.f,1.f)-balance)*smoothing;
  const Frame first{a[0].process(in.l),a[1].process(in.r)};Frame wet=first;
  if(p.routing==1)wet={b[0].process(first.l),b[1].process(first.r)};
  if(p.routing==2)wet=fx::mix(first,{b[0].process(in.l),b[1].process(in.r)},balance);
  return fx::mix(in,wet,amount);
 }
private:std::array<OdinCombLine,2> a,b;OdinSettings p;float amount=1,balance=.5f,smoothing=.001f;
};
}
