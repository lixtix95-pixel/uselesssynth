/* Spectrum Pixelator algorithm port: Michael Nuzzo, Copyright (c) 2019.
 * BSD-3-Clause: Licenses/SpectrumPixelator-BSD-3-Clause.txt.
 * Native C++ implementation/modifications 2026 MonomachineNova contributors.
 * KBD(beta=5), MDCT/IMDCT, dB peak prominence and 50% overlap-add.
 * New block-independent streaming/FFT implementation; power-of-two window menu
 * replaces MATLAB's arbitrary even window length. No MATLAB runtime or codegen.
 */
#pragma once
#include "Spectre.h"
#include <complex>
#include <memory>
#include <array>
namespace nova::classic {
using fx::Frame;using fx::History;
inline constexpr double pi=3.1415926535897932384626433832795;
inline constexpr int spectreMax=65536;
struct SpectreSettings {float prominence=10,mix=100;int window=3;}; // 512 << index
// Shared, identical FFT/KBD plans; the 0.5 processor below is unchanged.
using MDCTPlan=fx::MDCTPlan;using MDCTPlans=fx::MDCTPlans;
class Spectre {
public:
 void prepare(const MDCTPlans& plans){bank=&plans;input.resize(spectreMax);overlap.resize(spectreMax/2);queue.resize(spectreMax/2);work.resize(spectreMax);folded.resize(spectreMax/2);coeff.resize(spectreMax/2);db.resize(spectreMax/2);left.resize(spectreMax/2);right.resize(spectreMax/2);stack.resize(spectreMax/2);valley.resize(spectreMax/2);dry.prepare(spectreMax+8);set(p);reset();}
 bool set(SpectreSettings s){s.window=std::clamp(s.window,0,7);const bool changed=!plan||p.window!=s.window;p=s;plan=&bank->get(s.window);if(changed)reset();return changed;}
 void reset(){if(!plan)return;std::fill(input.begin(),input.begin()+plan->length,Frame{});std::fill(overlap.begin(),overlap.begin()+plan->n,Frame{});std::fill(queue.begin(),queue.begin()+plan->n,Frame{});dry.clear();position=0;mix=p.mix*.01f;}
 int latency()const{return plan?plan->length:(512<<std::clamp(p.window,0,7));}
 Frame process(Frame in,bool preserveAll=false){
  const auto at=dry.sizeWritten();dry.push(in);Frame wet=queue[position];input[plan->n+position]=in;
  if(++position==plan->n){render(preserveAll);position=0;}
  mix+=(std::clamp(p.mix*.01f,0.f,1.f)-mix)*.002f;
  return fx::mix(dry.at(at-plan->length),wet,mix);
 }
 // Exact prominence bases: minimum between this bin and the nearest higher
 // value (or boundary). Equal-height peaks do not stop the search. O(N).
 static void bases(const float* values,float* result,int* indices,float* minima,int n,bool backwards){int top=0;
  for(int t=0;t<n;t++){const int i=backwards?n-1-t:t;float v=values[i];while(top>0&&values[indices[top-1]]<=values[i]){v=std::min(v,minima[--top]);}result[i]=v;indices[top]=i;minima[top++]=v;}
 }
 static void prominenceMask(float* values,int n,float threshold,float* magnitudes,float* leftBase,float* rightBase,int* indices,float* minima){
  for(int i=0;i<n;i++)magnitudes[i]=20*std::log10(std::max(1.e-15f,std::abs(values[i])));
  bases(magnitudes,leftBase,indices,minima,n,false);bases(magnitudes,rightBase,indices,minima,n,true);
  // No endpoint peaks; preserve the first bin of a flat local maximum.
  int i=1;values[0]=0;while(i<n-1){int end=i;while(end+1<n&&magnitudes[end+1]==magnitudes[i])++end;
   const bool keep=end<n-1&&magnitudes[i]>magnitudes[i-1]&&magnitudes[end]>magnitudes[end+1]&&magnitudes[i]-std::max(leftBase[i],rightBase[i])>=threshold;
   if(!keep)values[i]=0;for(int j=i+1;j<=end;j++)values[j]=0;i=end+1;
  }values[n-1]=0;
 }
private:
 void render(bool preserveAll){const int n=plan->n;
  for(int channel=0;channel<2;channel++){
   auto sample=[&](int i){return (channel?input[i].r:input[i].l)*plan->window[i];};
   for(int m=0;m<n/2;m++)folded[m]=-sample(3*n/2-1-m)-sample(3*n/2+m);
   for(int m=n/2;m<n;m++)folded[m]=sample(m-n/2)-sample(3*n/2-1-m);
   plan->dct4(folded.data(),coeff.data(),work.data());
   if(!preserveAll)prominenceMask(coeff.data(),n,std::clamp(p.prominence,1.f,100.f),db.data(),left.data(),right.data(),stack.data(),valley.data());
   plan->dct4(coeff.data(),folded.data(),work.data());
   for(int i=0;i<2*n;i++){float v;if(i<n/2)v=folded[n/2+i];else if(i<3*n/2)v=-folded[3*n/2-1-i];else v=-folded[i-3*n/2];v*=2.f/n*plan->window[i];
    if(i<n){if(channel)queue[i].r=v+overlap[i].r;else queue[i].l=v+overlap[i].l;}
    else {if(channel)overlap[i-n].r=v;else overlap[i-n].l=v;}
   }
  }
  std::copy(input.begin()+n,input.begin()+2*n,input.begin());
 }
 const MDCTPlans* bank=nullptr;const MDCTPlan* plan=nullptr;SpectreSettings p;int position=0;float mix=1;
 std::vector<Frame> input,overlap,queue;std::vector<std::complex<float>> work;std::vector<float> folded,coeff,db,left,right,valley;std::vector<int> stack;History dry;
};
}
