#include "AudioAdapter.h"
namespace nova {
void AudioAdapter::makeKernel(Kernel& table,double cutoff) noexcept {
 constexpr double pi=3.1415926535897932384626433832795;
 for(int phase=0;phase<phases;++phase){
  const double fraction=double(phase)/phases;double total=0;
  for(int tap=0;tap<taps;++tap){
   const double distance=(tap-(taps/2-1))-fraction;
   const double z=pi*distance*cutoff;
   const double sinc=std::abs(z)<1.0e-12?1.0:std::sin(z)/z;
   const double window=0.42+0.5*std::cos(pi*distance/(taps/2))+0.08*std::cos(2*pi*distance/(taps/2));
   const double c=cutoff*sinc*window;table[std::size_t(phase)][std::size_t(tap)]=float(c);total+=c;
  }
  for(float& c:table[std::size_t(phase)])c=float(c/total);
 }
}
AudioAdapter::Stereo AudioAdapter::interpolate(const Ring& ring,double position,const Kernel& kernel) noexcept {
 const auto base=std::int64_t(std::floor(position));
 const int phase=std::clamp(int((position-double(base))*phases),0,phases-1);
 Stereo result{};
 for(int tap=0;tap<taps;++tap){
  const auto s=ring.get(base+tap-(taps/2-1));const auto c=kernel[std::size_t(phase)][std::size_t(tap)];
  result.l+=s.l*c;result.r+=s.r*c;
 }
 return result;
}
int AudioAdapter::prepare(double rate) noexcept {
 hostRate=(std::isfinite(rate)&&rate>=8000&&rate<=768000)?rate:44100.0;
 paramStep=127.f/float(hostRate*.02);
 ratio=hostRate/44100.0;direct=std::abs(hostRate-44100.0)<0.001;
 latencyFrames=direct?16:int(std::ceil(taps/2+(16+taps/2)*ratio))+2;
 if(!direct){
  makeKernel(inputKernel,0.94*std::min(1.0,1.0/ratio));
  makeKernel(outputKernel,0.94*std::min(1.0,ratio));
 }
 reset(ChorusCore::defaults);return latencyFrames;
}
std::array<float,8> AudioAdapter::floatParams(const std::array<int,8>& p) noexcept {
 std::array<float,8> f{};
 for(std::size_t i=0;i<8;++i)f[i]=float(std::clamp(p[i],0,127));
 return f;
}
void AudioAdapter::updateParameters() noexcept {
 if(!smooth)return;
 bool moved=false;
 for(std::size_t i=0;i<8;++i){
  const float t=target[i];
  float c=current[i];
  if(c!=t){
   const float d=t-c;
   current[i]=c+std::clamp(d,-paramStep,paramStep);
   moved=true;
  }
 }
 if(moved)core.setParametersF(current);
}
void AudioAdapter::reset(const std::array<int,8>& parameters) noexcept {
 current=target=floatParams(parameters);
 core.reset(parameters);hostInput.clear();nativeOutput.clear();
 inputBlock.fill(0);outputBlock.fill(0);inputFill=0;nextNative=0;
}
void AudioAdapter::pushNative(Stereo sample) noexcept {
 inputBlock[std::size_t(inputFill*2)]=toFixed(sample.l);
 inputBlock[std::size_t(inputFill*2+1)]=toFixed(sample.r);
 if(++inputFill==16){
  core.process16(inputBlock.data(),outputBlock.data());inputFill=0;
  for(int i=0;i<16;++i)nativeOutput.push({float(outputBlock[std::size_t(i*2)])/8388608.0f,float(outputBlock[std::size_t(i*2+1)])/8388608.0f});
 }
}
void AudioAdapter::processFrame(float inL,float inR,float& outL,float& outR,bool bypass) noexcept {
 updateParameters();
 const auto index=hostInput.written;
 hostInput.push({std::isfinite(inL)?inL:0.0f,std::isfinite(inR)?inR:0.0f});
 Stereo result{};
 if(direct){
  result=nativeOutput.get(index-16);pushNative({inL,inR});
 }else{
  while(double(nextNative)*ratio+taps/2<=double(index)){
   pushNative(interpolate(hostInput,double(nextNative)*ratio,inputKernel));++nextNative;
  }
  result=interpolate(nativeOutput,(double(index)-latencyFrames)/ratio,outputKernel);
 }
 if(bypass)result=hostInput.get(index-latencyFrames);
 outL=result.l;outR=result.r;
}
}
