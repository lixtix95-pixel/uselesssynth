/* MonoJuice creative control layer, GPL-3.0-only, 2026 contributors.
 * Original ODIN / ParallelComb kernels remain separate and unchanged (except
 * the ODIN settings type name). Drive and envelope/LFO tuning are new features.
 */
#pragma once
#include "ParallelCombModel.h"
#include "OdinComb.h"
namespace nova::fx {
struct ColourSettings {int model=0;float drive=0,mix=100,output=0,depth=0,rate=.5f,envelope=0,decay=180;};
class CombColour {
public:
 void prepare(double rate){sr=rate;parallel.prepare(rate);odin.prepare(rate);smooth=float(1-std::exp(-1/(rate*.01)));attack=float(1-std::exp(-1/(rate*.003)));ready=true;set(a,b,p);reset();}
 void set(CombSettings aa,OdinSettings bb,ColourSettings s){a=aa;b=bb;p=s;if(!ready)return;targetGain=std::pow(10.f,p.drive*.05f);targetLevel=std::pow(10.f,p.output*.05f);release=float(1-std::exp(-1/(sr*.001*std::max(5.f,p.decay))));assign(currentMod);}
 void reset(){if(!ready)return;phase=0;env=0;counter=0;currentMod=0;assign(0);parallel.reset();odin.reset();gain=std::pow(10.f,p.drive*.05f);level=std::pow(10.f,p.output*.05f);amount=p.mix*.01f;driveBlend=p.drive>0?1.f:0.f;}
 Frame process(Frame in){
  const float target=std::min(1.f,4.f*std::max(std::abs(in.l),std::abs(in.r)));env+=(target-env)*(target>env?attack:release);
  const bool mod=p.depth!=0||p.envelope!=0;
   if(mod){phase+=p.rate/sr;phase-=std::floor(phase);if(counter++%16==0){currentMod=p.depth*float(std::sin(6.283185307179586*phase))+p.envelope*env;assign(currentMod);}}
  else if(currentMod!=0){currentMod=0;assign(0);}
  gain+=(targetGain-gain)*smooth;level+=(targetLevel-level)*smooth;amount+=(p.mix*.01f-amount)*smooth;
  driveBlend+=((p.drive>0?1.f:0.f)-driveBlend)*smooth;Frame excite=in;if(driveBlend>.000001f)excite=fx::mix(in,{std::tanh(in.l*gain),std::tanh(in.r*gain)},driveBlend);
  auto wet=p.model==0?odin.process(excite):parallel.process(excite);
  return fx::mix(in,wet*level,amount);
 }
private:
 void assign(float semitones){if(p.model==0){auto v=b;const float factor=std::exp2(semitones/12);v.freqA*=factor;v.freqB*=factor;odin.set(v);}else{auto v=a;v.tune+=semitones;parallel.set(v);}}
 double sr=44100,phase=0;float env=0,attack=1,release=1,smooth=1,gain=1,level=1,targetGain=1,targetLevel=1,amount=1,currentMod=0,driveBlend=0;uint32_t counter=0;bool ready=false;
 CombSettings a;OdinSettings b;ColourSettings p;ParallelCombModel parallel;OdinComb odin;
};
}
