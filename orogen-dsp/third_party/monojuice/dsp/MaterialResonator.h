#pragma once
#include "FirmwarePrimitives.h"
#include <array>
#include <cmath>

namespace nova::fx {

struct MaterialSettings {
  int mode=0;
  float excite=45,tone=50,damp=48,size=55,feedback=32,mix=65;
  bool follow=false;
};

// Latex keeps the original four-mode bank. Spring, metal and glass use
// low-order physical modal approximations derived from simple geometries.
class MaterialResonator {
 public:
  void prepare(double sampleRate){sr=std::isfinite(sampleRate)&&sampleRate>=8000?sampleRate:44100.;pitchPole=1.f-std::exp(-twoPi*2200.f/float(sr));target={};current=target;reset();}
  void reset() noexcept {left={};right={};current=target;inputLevel=0;pitchLow=0;pitchEnvelope=0;trackedHz=0;lastPitchSample=0;pitchPeriod=0;activeModes=0;physicalGain=.55f;coefficientTick=0;coefficientsDirty=false;if(current.mode>0)configurePhysical();}
  void set(MaterialSettings value) noexcept {
    const int nextMode=std::clamp(value.mode,0,3);
    const bool modeChanged=nextMode!=current.mode;
    const float nextTone=std::clamp(value.tone,0.f,100.f),nextDamp=std::clamp(value.damp,0.f,100.f);
    const float nextSize=std::clamp(value.size,0.f,100.f),nextFeedback=std::clamp(value.feedback,0.f,100.f);
    const bool coefficientSettingsChanged=target.mode!=nextMode||target.tone!=nextTone||target.damp!=nextDamp||target.size!=nextSize||target.feedback!=nextFeedback||target.follow!=value.follow;
    target.mode=nextMode;
    target.excite=std::clamp(value.excite,0.f,100.f);
    target.tone=nextTone;
    target.damp=nextDamp;
    target.size=nextSize;
    target.feedback=nextFeedback;
    target.mix=std::clamp(value.mix,0.f,100.f);
    target.follow=value.follow;
    if(current.follow!=target.follow){pitchLow=0;pitchEnvelope=0;trackedHz=0;lastPitchSample=0;pitchPeriod=0;}
    current.mode=target.mode;
    current.follow=target.follow;
    if(current.mode>0){if(coefficientSettingsChanged)coefficientsDirty=true;}else coefficientsDirty=false;
    if(modeChanged){left={};right={};activeModes=0;coefficientTick=0;if(current.mode>0){configurePhysical();coefficientsDirty=false;}}
  }
  Frame process(Frame in) noexcept {
    const float previousTone=current.tone,previousDamp=current.damp,previousSize=current.size,previousFeedback=current.feedback;
    current.excite+=(target.excite-current.excite)*smooth;
    current.tone+=(target.tone-current.tone)*smooth;
    current.damp+=(target.damp-current.damp)*smooth;
    current.size+=(target.size-current.size)*smooth;
    current.feedback+=(target.feedback-current.feedback)*smooth;
    current.mix+=(target.mix-current.mix)*smooth;

    bool trackedChanged=false;
    if(current.follow){
      const float pitchInput=.5f*(in.l+in.r);pitchLow+=(pitchInput-pitchLow)*pitchPole;++pitchPeriod;
      const float pitchLevel=.5f*(std::abs(in.l)+std::abs(in.r));pitchEnvelope+=(pitchLevel-pitchEnvelope)*(pitchLevel>pitchEnvelope?.01f:.0005f);
      if(pitchLow>0.f&&lastPitchSample<=0.f){
        if(pitchEnvelope>.001f&&pitchPeriod>=16&&pitchPeriod<=2048){const float candidate=float(sr)/float(pitchPeriod);const float updated=trackedHz<=0?candidate:trackedHz+(candidate-trackedHz)*.2f;trackedChanged=updated!=trackedHz;trackedHz=updated;}
        pitchPeriod=0;
      }
      pitchPeriod=std::min(pitchPeriod,4096);lastPitchSample=pitchLow;
    }

    const float level=std::clamp(.5f*(std::abs(in.l)+std::abs(in.r)),0.f,4.f);
    const float rise=std::max(0.f,level-inputLevel-.002f);
    inputLevel+=(level-inputLevel)*(level>inputLevel ? .0015f : .0003f);
    const float strike=std::clamp(rise*160.f,0.f,1.f);
    const Frame exciter=in*(.015f+.985f*strike);
    const int mode=std::clamp(current.mode,0,3);
    const float size=std::clamp(current.size*.01f,0.f,1.f);
    const float tone=std::clamp(current.tone*.01f,0.f,1.f);
    const float followSize=std::pow(2.f,(.5f-size)*2.f);
    const float followTone=.6f+.8f*std::clamp(current.tone*.01f,0.f,1.f);
    if(mode==0){
      const float root=current.follow&&trackedHz>30.f?trackedHz*followSize*followTone:55.f*std::pow(22.f,1.f-size)*(.45f+1.55f*tone);
      const float base=std::clamp(root,30.f,float(sr*.23));
      const float inputGain=.05f+.95f*std::clamp(current.excite*.01f,0.f,1.f);
      const float damping=std::clamp(current.damp*.01f,0.f,1.f),resonance=std::clamp(current.feedback*.01f,0.f,1.f);
      Frame wet{};float normalise=0;
      for(int i=0;i<4;i++){
        const float weight=modeWeights[0][i],freq=std::clamp(base*ratios[0][i],30.f,float(sr*.24));
        const float decayMs=(16.f+(1.f-damping)*260.f+resonance*520.f)*modeDecay[0];
        const float radius=std::exp(-1.f/std::max(1.f,float(sr)*decayMs*.001f));
        const float gain=2.f*radius*std::cos(twoPi*freq/float(sr)),radius2=radius*radius;
        const float driveScale=std::sqrt(std::max(.0001f,1.f-radius2));
        const float yl=exciter.l*inputGain*driveScale*weight+gain*left[std::size_t(i)].one-radius2*left[std::size_t(i)].two;
        const float yr=exciter.r*inputGain*driveScale*weight+gain*right[std::size_t(i)].one-radius2*right[std::size_t(i)].two;
        const float oldL=left[std::size_t(i)].one,oldR=right[std::size_t(i)].one;
        left[std::size_t(i)].two=oldL;left[std::size_t(i)].one=yl;right[std::size_t(i)].two=oldR;right[std::size_t(i)].one=yr;
        wet.l+=yl*weight;wet.r+=yr*weight;normalise+=weight;
      }
      wet=wet*(0.55f/std::max(.1f,normalise));
      return finish(in,wet);
    }

    if(current.tone!=previousTone||current.damp!=previousDamp||current.size!=previousSize||current.feedback!=previousFeedback||trackedChanged)coefficientsDirty=mode>0;
    if(coefficientsDirty&&++coefficientTick>=16){configurePhysical();coefficientTick=0;coefficientsDirty=false;}
    const float inputGain=.05f+.95f*std::clamp(current.excite*.01f,0.f,1.f);
    Frame wet{};
    for(int i=0;i<activeModes;i++){
      const auto& modeCoefficients=physicalModes[std::size_t(i)];
      const auto index=std::size_t(i);
      const float yl=exciter.l*inputGain*modeCoefficients.driveScale+modeCoefficients.feedback*left[index].one-modeCoefficients.radius2*left[index].two;
      const float yr=exciter.r*inputGain*modeCoefficients.driveScale+modeCoefficients.feedback*right[index].one-modeCoefficients.radius2*right[index].two;
      const float oldL=left[index].one,oldR=right[index].one;
      left[index].two=oldL;left[index].one=yl;right[index].two=oldR;right[index].one=yr;
      wet.l+=yl*modeCoefficients.outputWeight;wet.r+=yr*modeCoefficients.outputWeight;
    }
    wet=wet*physicalGain;
    return finish(in,wet);
  }

 private:
  struct State {float one=0,two=0;};
  struct ModeCoefficients {float feedback=0,radius2=0,driveScale=0,outputWeight=0;};
  static constexpr int maxPhysicalModes=24;
  static constexpr float twoPi=6.2831853071795864769f;
  static constexpr float pi=3.1415926535897932385f;

  Frame finish(Frame in,Frame wet) const noexcept {
    const float peak=std::max(std::abs(wet.l),std::abs(wet.r));
    if(peak>.85f)wet=wet*(.85f/peak);
    return fx::mix(in,wet,std::clamp(current.mix*.01f,0.f,1.f));
  }

  void addPhysicalMode(float frequency,float t60,float driveWeight,float outputWeight,float frequencyScale,float& energy) noexcept {
    frequency*=frequencyScale;
    if(!std::isfinite(frequency)||frequency<20.f||frequency>float(sr*.24)||activeModes>=maxPhysicalModes)return;
    frequency=std::max(30.f,frequency);
    t60=std::clamp(t60,.035f,8.f);
    const float radius=std::exp(-6.90775527898f/(float(sr)*t60));
    const float radius2=radius*radius;
    auto& c=physicalModes[std::size_t(activeModes++)];
    c.feedback=2.f*radius*std::cos(twoPi*frequency/float(sr));
    c.radius2=radius2;
    c.driveScale=std::sqrt(std::max(1.0e-6f,1.f-radius2))*std::max(0.f,driveWeight);
    c.outputWeight=std::max(0.f,outputWeight);
    energy+=c.outputWeight*c.outputWeight;
  }

  void configurePhysical() noexcept {
    activeModes=0;physicalGain=.55f;float energy=0.f;
    const float size=std::clamp(current.size*.01f,0.f,1.f),tone=std::clamp(current.tone*.01f,0.f,1.f);
    const float damping=std::clamp(current.damp*.01f,0.f,1.f),resonance=std::clamp(current.feedback*.01f,0.f,1.f);
    const float contactHz=350.f+14500.f*std::pow(tone,1.4f);
    float firstFrequency=100.f;
    float frequencyScale=1.f;
    if(current.mode==1){
      constexpr int masses=24;
      const float q1=pi/float(2*masses+1);
      firstFrequency=80.f/(.65f+.875f*size);
      const float sqrtKOverM=(twoPi*firstFrequency)/(2.f*std::sin(q1*.5f));
      const int hit=20;
      const float shapeNorm=std::sqrt(2.f/(float(masses)+.5f));
      const float baseT60=.10f+(1.f-damping)*1.6f+resonance*1.4f;
      if(current.follow&&trackedHz>30.f){const float wanted=trackedHz*std::pow(2.f,(.5f-size)*2.f)*(.6f+.8f*tone);frequencyScale=wanted/firstFrequency;}
      for(int j=1;j<=masses;j++){
        const float q=float(2*j-1)*pi/float(2*masses+1);
        const float frequency=sqrtKOverM*std::sin(q*.5f)/pi;
        const float shape=std::abs(std::sin(q*float(hit)))*shapeNorm;
        const float contact=std::exp(-std::pow(frequency/contactHz,2.f));
        const float t60=baseT60/(1.f+std::pow(frequency/2400.f,1.2f));
        addPhysicalMode(frequency,t60,shape*contact,shape,frequencyScale,energy);
      }
    }else if(current.mode==2){
      // Simply-supported thin aluminum plate: flexural modes scale as k^2.
      constexpr float young=69.0e9f,density=2700.f,poisson=.33f,thickness=.0012f;
      constexpr float lengthX=.27f,lengthY=.20f,hitX=.22f,hitY=.31f;
      const float stiffnessSpeed=std::sqrt(young/(12.f*density*(1.f-poisson*poisson)));
      const float plateScale=.55f+.9f*size;
      const float coefficient=pi*thickness*stiffnessSpeed*.5f/(plateScale*plateScale);
      firstFrequency=coefficient*(1.f/(lengthX*lengthX)+1.f/(lengthY*lengthY));
      const float baseT60=.10f+(1.f-damping)*2.5f+resonance*1.8f;
      if(current.follow&&trackedHz>30.f){const float wanted=trackedHz*std::pow(2.f,(.5f-size)*2.f)*(.6f+.8f*tone);frequencyScale=wanted/firstFrequency;}
      for(int m=1;m<=4;m++)for(int n=1;n<=6;n++){
        const float frequency=coefficient*(float(m*m)/(lengthX*lengthX)+float(n*n)/(lengthY*lengthY));
        const float shape=std::abs(std::sin(pi*float(m)*hitX)*std::sin(pi*float(n)*hitY));
        const float contact=std::exp(-std::pow(frequency/contactHz,2.f));
        const float t60=baseT60/(1.f+std::pow(frequency/1900.f,1.25f));
        addPhysicalMode(frequency,t60,shape*contact,shape,frequencyScale,energy);
      }
    }else{
      // Thin glass bowl rim approximated as an ovalising circular ring.
      constexpr float young=70.0e9f,density=2500.f,thickness=.0015f,radius=.045f;
      const float sizeOfRing=.55f+.9f*size;
      const float ringRadius=radius*sizeOfRing;
      const float materialSpeed=std::sqrt(young/density);
      const float coefficient=materialSpeed*thickness/(4.f*pi*std::sqrt(3.f)*ringRadius*ringRadius);
      firstFrequency=coefficient*(2.f*(2.f*2.f-1.f)/std::sqrt(5.f));
      const float baseT60=.15f+(1.f-damping)*4.8f+resonance*2.5f;
      if(current.follow&&trackedHz>30.f){const float wanted=trackedHz*std::pow(2.f,(.5f-size)*2.f)*(.6f+.8f*tone);frequencyScale=wanted/firstFrequency;}
      for(int order=2;order<=12;order++){
        const float n=float(order);
        const float shapeFactor=n*(n*n-1.f)/std::sqrt(n*n+1.f);
        const float frequency=coefficient*shapeFactor;
        const float shape=std::sqrt(.5f*std::exp(-std::pow(frequency/8500.f,2.f))/(1.f+.025f*float(order-2)));
        const float contact=std::exp(-std::pow(frequency/contactHz,2.f));
        const float t60=baseT60/(1.f+std::pow(frequency/10500.f,1.5f));
        const float split=.0018f*float(order-1);
        addPhysicalMode(frequency*(1.f-split),t60,shape*contact,shape,frequencyScale,energy);
        addPhysicalMode(frequency*(1.f+split),t60,shape*contact,shape,frequencyScale,energy);
      }
    }
    physicalGain=.55f/std::sqrt(std::max(.01f,energy));
  }

  inline static constexpr float ratios[4][4]{
    {1.f,1.41f,2.17f,3.08f},
    {1.f,1.49f,2.47f,3.98f},
    {1.f,1.34f,2.11f,3.79f},
    {1.f,1.50f,2.50f,3.50f}
  };
  inline static constexpr float modeWeights[4][4]{
    {1.f,.78f,.58f,.38f},
    {1.f,.92f,.72f,.52f},
    {1.f,.88f,.82f,.70f},
    {1.f,.86f,.68f,.55f}
  };
  inline static constexpr float modeDecay[4]{.72f,1.18f,1.72f,2.08f};
  double sr=44100.;float smooth=.001f,inputLevel=0,pitchPole=.25f,pitchLow=0,pitchEnvelope=0,trackedHz=0,lastPitchSample=0,physicalGain=.55f;int pitchPeriod=0,activeModes=0,coefficientTick=0;bool coefficientsDirty=false;MaterialSettings current{},target{};
  std::array<State,maxPhysicalModes> left{},right{};
  std::array<ModeCoefficients,maxPhysicalModes> physicalModes{};
};

}
