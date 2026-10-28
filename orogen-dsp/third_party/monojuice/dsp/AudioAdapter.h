#pragma once
#include "ChorusCore.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace nova {
// Host I/O only; ChorusCore always runs at the original 44100 Hz.
// 64-tap windowed-sinc conversion is bypassed entirely at 44100 Hz.
class AudioAdapter {
public:
 static constexpr int taps=64, phases=1024, ringSize=1024;
 int prepare(double sampleRate) noexcept;
 void reset(const std::array<int,8>& parameters) noexcept;
 // Smoothed mode (default): stores the target only; values ramp toward it
 // (~20 ms full sweep) inside processFrame so knob moves and automation
 // don't zipper. Steady state and reset() stay bit-exact with the DSP core.
 // Direct mode (pre-fix CHORUS v1): writes reach the core immediately.
 void setSmoothing(bool on) noexcept { if(smooth==on)return; smooth=on; if(!smooth){ current=target; core.setParametersF(current); } }
 void setParameters(const std::array<int,8>& p) noexcept { const auto f=floatParams(p); target=f; if(!smooth){ current=f; core.setParameters(p); } }
 void setParametersF(const std::array<float,8>& p) noexcept { auto f=p; for(auto& v:f)v=std::clamp(v,0.f,127.f); target=f; if(!smooth){ current=f; core.setParametersF(f); } }
 void processFrame(float inL,float inR,float& outL,float& outR,bool bypass) noexcept;
 int latency() const noexcept { return latencyFrames; }
 double hostSampleRate() const noexcept { return hostRate; }
private:
 struct Stereo { float l=0,r=0; };
 struct Ring {
  std::array<Stereo,ringSize> data{};
  std::int64_t written=0;
  void clear() noexcept { data.fill({});written=0; }
  void push(Stereo s) noexcept { data[std::size_t(written++)&(ringSize-1)]=s; }
  Stereo get(std::int64_t at) const noexcept {
   if(at<0 || at>=written || at<written-ringSize)return {};
   return data[std::size_t(at)&(ringSize-1)];
  }
 } hostInput,nativeOutput;
 using Kernel=std::array<std::array<float,taps>,phases>;
 Kernel inputKernel{},outputKernel{};
 ChorusCore core;
 std::array<std::int32_t,32> inputBlock{},outputBlock{};
 int inputFill=0,latencyFrames=16;
 double hostRate=44100.0,ratio=1.0;
 std::int64_t nextNative=0;
 bool direct=true;
 std::array<float,8> current=floatParams(ChorusCore::defaults),target=current;
 float paramStep=0;bool smooth=true;
 static void makeKernel(Kernel&,double cutoff) noexcept;
 static Stereo interpolate(const Ring&,double position,const Kernel&) noexcept;
 static std::array<float,8> floatParams(const std::array<int,8>& p) noexcept;
 void updateParameters() noexcept;
 void pushNative(Stereo) noexcept;
 static std::int32_t toFixed(float v) noexcept {
  if(!std::isfinite(v))return 0;
  const auto scaled=std::clamp(double(v)*8388608.0,-8388608.0,8388607.0);
  return std::int32_t(std::round(scaled));
 }
};
}
