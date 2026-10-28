// Generated statically from original BIN; see research/translate_chorus.py.
#include "ChorusCore.h"
#include "ChorusTables.h"
namespace nova {
void ChorusCore::setParameters(const std::array<int,8>& p) noexcept {
 for(unsigned i=0;i<8;++i) { int v=std::clamp(p[i],0,127); y[0x52c+i]=Word(v*65536)&0xffffffu; }
}
void ChorusCore::setParametersF(const std::array<float,8>& p) noexcept {
 for(unsigned i=0;i<8;++i) { const float v=p[i]<0.f?0.f:(p[i]>127.f?127.f:p[i]); y[0x52c+i]=Word(v*65536.0f)&0xffffffu; }
}
void ChorusCore::reset(const std::array<int,8>& p) noexcept {
 x.fill(0);y.fill(0);delay.fill(0);setParameters(p);initialiseProcedure();
}
ChorusCore::Word ChorusCore::readX(Word a) const noexcept {
 if(a<x.size())return x[a];
 if(a>=0x114000 && a<0x115000)return delay[a-0x114000];
 if(a>=0x14a000 && a<0x14b000)return widthTable[a-0x14a000];
 if(a>=0x144ac7 && a<0x144bc9)return lowpassTable[a-0x144ac7];
 assert(false && "Unexpected CHORUS X address");return 0;
}
ChorusCore::Word ChorusCore::readY(Word a) const noexcept {
 if(a<y.size())return y[a];
 return readX(a);
}
void ChorusCore::writeX(Word a,Word v) noexcept {
 v&=0xffffffu;if(a<x.size()){x[a]=v;return;}
 if(a>=0x114000 && a<0x115000){delay[a-0x114000]=v;return;}
 assert(false && "Unexpected CHORUS X write");
}
void ChorusCore::writeY(Word a,Word v) noexcept {
 if(a<y.size()){y[a]=v&0xffffffu;return;}writeX(a,v);
}
void ChorusCore::process16(const std::int32_t* in,std::int32_t* out) noexcept {
 for(unsigned i=0;i<32;++i)x[0x220+i]=Word(in[i])&0xffffffu;
 processProcedure();
 for(unsigned i=0;i<32;++i)out[i]=signed24(y[0x100+i]);
}

void ChorusCore::initialiseProcedure() noexcept {
 std::int64_t a=0;
 Word x0=0,x1=0;
std::array<Word,8> r{{0x220,0,0,0,0,0,0x528,0x100}},n{};
std::array<Word,8> m; m.fill(0xffffffu);
bool saturation=false;
{ const auto t0=0x114000u;a=withLow(a,t0); } // P:147661 move    #>$114000,a0
{ const auto t0=readY(0x124);x1=t0; } // P:147663 move    y:>$124,x1
{ a=mac(a,0x2000u,x1,false,saturation,false); } // P:147665 maci    #>$2000,x1,a
{ const auto t0=low(a);x0=t0; } // P:147667 move    a0,x0
{ const auto t0=x0;writeY((r[6]+0x10),t0); } // P:147668 move    x0,y:(r6+$10)
{ const auto t0=0x800000u;x0=t0; } // P:147669 move    #$80,x0
{ const auto t0=x0;writeX((r[6]+0x11),t0); } // P:14766a move    x0,x:(r6+$11)
{ const auto t0=0x0u;x0=t0; } // P:14766b move    #$0,x0
{ const auto t0=x0;writeX((r[6]+0x12),t0); } // P:14766c move    x0,x:(r6+$12)
{ const auto t0=readY((r[6]+0x4));x0=t0; } // P:14766d move    y:(r6+$4),x0
{ a=multiply(0xfffc20u,x0,false); } // P:14766e mpyi    #>$fffc20,x0,a
{ const auto t0=word(a);writeX((r[6]+0x1d),t0); } // P:147670 move    a,x:(r6+$1d)
{ const auto t0=0x14a000u;x0=t0; } // P:147671 move    #>$14a000,x0
{ const auto t0=x0;writeY((r[6]+0x1b),t0); } // P:147673 move    x0,y:(r6+$1b)
{ const auto t0=0x0u;x0=t0; } // P:147674 move    #$0,x0
{ const auto t0=x0;writeY((r[6]+0x21),t0); } // P:147675 move    x0,y:(r6+$21)
{ const auto t0=x0;writeY((r[6]+0x20),t0); } // P:147676 move    x0,y:(r6+$20)
{ const auto t0=x0;writeY((r[6]+0x1d),t0); } // P:147677 move    x0,y:(r6+$1d)
{ const auto t0=x0;writeX((r[6]+0x22),t0); } // P:147678 move    x0,x:(r6+$22)
{ const auto t0=x0;writeY((r[6]+0x22),t0); } // P:147679 move    x0,y:(r6+$22)
{ return; } // P:14767a rts
}
void ChorusCore::processProcedure() noexcept {
std::int64_t a=0,b=0;
Word x0=0,x1=0,y0=0,y1=0;
std::array<Word,8> r{{0x220,0,0,0,0,0,0x528,0x100}},n{};
std::array<Word,8> m; m.fill(0xffffffu);
bool saturation=false;
{ const auto t0=0x0u;x0=t0; } // P:14767b move    #$0,x0
{ const auto t0=x0;writeX((r[6]-0x19),t0); } // P:14767c move    x0,x:(r6-$19)
{ const auto t0=0x7fffffu;b=q(t0); } // P:14767d move    #>$7fffff,b
{ const auto t0=readY((r[6]+0x21));a=q(t0); } // P:14767f move    y:(r6+$21),a
{  } // P:147680 cmp     #>$80,a
{ if(a>=q(0x80))goto P147687; } // P:147682 bge     func_147687
{ a=wrap56(a+q(0x1u)); } // P:147684 add     #>$1,a
{ b=0; } // P:147686 clr     b
P147687:
{ const auto t0=word(a);writeY((r[6]+0x21),t0); } // P:147687 move    a,y:(r6+$21)
{ const auto t0=word(b);writeY((r[6]+0x20),t0); } // P:147688 move    b,y:(r6+$20)
{ const auto t0=0x0u;r[4]=t0; } // P:147689 move    #>$0,r4
{ const auto t0=readY((r[6]+0xb));x0=t0; } // P:14768b move    y:(r6+$b),x0
{ a=multiply(x0,x0,false); } // P:14768c mpy     x0,x0,a
{ const auto t0=readX(post(r[0],1,m[0]));x1=t0; } // P:14768d move    x:(r0)+,x1
{ const auto t0=word(a);x0=t0; } // P:14768e move    a,x0
{ const auto t0=readX(post(r[0],1,m[0]));a=multiply(x1,x0,false);x1=t0; } // P:14768f mpy     x1,x0,a         x:(r0)+,x1
{ a=asl(a,2); } // P:147690 asl     #$2,a,a
{ const auto t0=readX(post(r[0],1,m[0]));b=multiply(x1,x0,false);x1=t0; } // P:147691 mpy     x1,x0,b         x:(r0)+,x1
{ b=asl(b,2); } // P:147692 asl     #$2,b,b
for(int loop147693=0;loop147693<16;++loop147693) { // P:147693 do      #<$10,>$147699
{ const auto t0=readX(post(r[0],1,m[0]));const auto t1=word(a);a=multiply(x1,x0,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:147695 mpy     x1,x0,a         x:(r0)+,x1      a,y:(r4)+
{ a=asl(a,2); } // P:147696 asl     #$2,a,a
{ const auto t0=readX(post(r[0],1,m[0]));const auto t1=word(b);b=multiply(x1,x0,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:147697 mpy     x1,x0,b         x:(r0)+,x1      b,y:(r4)+
{ b=asl(b,2); } // P:147698 asl     #$2,b,b
}
{ const auto t0=readY((r[6]+0x6));x0=t0; } // P:147699 move    y:(r6+$6),x0
{ a=multiply(x0,x0,false); } // P:14769a mpy     x0,x0,a
{ const auto t0=word(a);x0=t0; } // P:14769b move    a,x0
{ a=multiply(0x956u,x0,false); } // P:14769c mpyi    #>$956,x0,a
{ const auto t0=0x20u;r[0]=t0; } // P:14769e move    #>$20,r0
{ const auto t0=r[0];r[4]=t0; } // P:1476a0 move    r0,r4
{ const auto t0=word(a);x0=t0; } // P:1476a1 move    a,x0
{ const auto t0=readX((r[6]+0x11));a=q(t0); } // P:1476a2 move    x:(r6+$11),a
{ const auto t0=readX((r[6]+0x12));b=q(t0); } // P:1476a3 move    x:(r6+$12),b
{ const auto t0=word(a);x1=t0; } // P:1476a4 move    a,x1
{ saturation=true; } // P:1476a5 bset    #$14,sr
for(int loop1476a6=0;loop1476a6<16;++loop1476a6) { // P:1476a6 do      #<$10,>$1476aa
{ const auto t0=word(b);const auto t1=word(b);b=mac(b,x1,x0,true,saturation,false);writeX(post(r[0],1,m[0]),t0);y0=t1; } // P:1476a8 mac     -x1,x0,b        b,x:(r0)+       b,y0
{ const auto t0=word(a);const auto t1=word(a);a=mac(a,y0,x0,false,saturation,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:1476a9 mac     y0,x0,a         a,x1            a,y:(r4)+
}
{ saturation=false; } // P:1476aa bclr    #$14,sr
{ const auto t0=word(a);writeX((r[6]+0x11),t0); } // P:1476ab move    a,x:(r6+$11)
{ const auto t0=word(b);writeX((r[6]+0x12),t0); } // P:1476ac move    b,x:(r6+$12)
{ const auto t0=readY((r[6]+0x24));a=q(t0); } // P:1476ad move    y:(r6+$24),a
{ const auto t0=readY((r[6]+0x9));y0=t0; } // P:1476ae move    y:(r6+$9),y0
{ const auto t0=word(a);y1=t0; } // P:1476af move    a,y1
{ a=mac(a,0x28f5cu,y0,false,saturation,false); } // P:1476b0 maci    #>$28f5c,y0,a
{ a=mac(a,0xfd70a4u,y1,false,saturation,false); } // P:1476b2 maci    #>$fd70a4,y1,a
{ const auto t0=word(a);writeY((r[6]+0x24),t0); } // P:1476b4 move    a,y:(r6+$24)
{ const auto t0=word(a);y0=t0; } // P:1476b5 move    a,y0
{ a=multiply(0x800u,y0,false); } // P:1476b6 mpyi    #>$800,y0,a
{ const auto t0=word(a);r[0]=t0; } // P:1476b8 move    a,r0
{ const auto t0=readX((r[0]+0x14a000));a=q(t0); } // P:1476b9 move    x:(r0+$14a000),a
{ const auto t0=readX((r[0]+0x14a800));b=q(t0); } // P:1476bb move    x:(r0+$14a800),b
{ const auto t0=readY((r[6]+0x25));x0=t0; } // P:1476bd move    y:(r6+$25),x0
{ const auto t0=readY((r[6]+0x26));x1=t0; } // P:1476be move    y:(r6+$26),x1
{ const auto t0=word(b);writeY((r[6]+0x25),t0); } // P:1476bf move    b,y:(r6+$25)
{ const auto t0=word(a);writeY((r[6]+0x26),t0); } // P:1476c0 move    a,y:(r6+$26)
{ a=wrap56(a-q(x1)); } // P:1476c1 sub     x1,a
{ b=wrap56(b-q(x0)); } // P:1476c2 sub     x0,b
{ const auto t0=word(a);a=q(x1);y0=t0; } // P:1476c3 tfr     x1,a            a,y0
{ const auto t0=word(b);b=q(x0);y1=t0; } // P:1476c4 tfr     x0,b            b,y1
{ const auto t0=0x80000u;x0=t0; } // P:1476c5 move    #$8,x0
{ const auto t0=0x80u;r[0]=t0; } // P:1476c6 move    #>$80,r0
for(int loop1476c8=0;loop1476c8<16;++loop1476c8) { // P:1476c8 do      #<$10,>$1476cc
{ const auto t0=word(a);a=mac(a,y0,x0,false,saturation,false);writeY(r[0],t0); } // P:1476ca mac     y0,x0,a         a,y:(r0)
{ const auto t0=word(b);b=mac(b,x0,y1,false,saturation,false);writeX(post(r[0],1,m[0]),t0); } // P:1476cb mac     x0,y1,b         b,x:(r0)+
}
{ const auto t0=0x80u;r[5]=t0; } // P:1476cc move    #>$80,r5
{ const auto t0=0x20u;r[0]=t0; } // P:1476ce move    #>$20,r0
{ const auto t0=r[0];r[4]=t0; } // P:1476d0 move    r0,r4
{ const auto t0=0x50u;r[1]=t0; } // P:1476d1 move    #>$50,r1
{ const auto t0=r[1];r[2]=t0; } // P:1476d3 move    r1,r2
{ const auto t0=readX(r[5]);y0=t0; } // P:1476d4 move    x:(r5),y0
{ const auto t0=readX(post(r[0],1,m[0]));const auto t1=readY(post(r[5],1,m[5]));x0=t0;y1=t1; } // P:1476d5 move    x:(r0)+,x0      y:(r5)+,y1
for(int loop1476d6=0;loop1476d6<16;++loop1476d6) { // P:1476d6 do      #<$10,>$1476de
{ const auto t0=readY(post(r[4],1,m[4]));a=multiply(y0,x0,false);x1=t0; } // P:1476d8 mpy     y0,x0,a         y:(r4)+,x1
{ a=mac(a,y1,x1,false,saturation,false); } // P:1476d9 mac     y1,x1,a
{ const auto t0=readX(r[5]);b=multiply(x1,y0,false);y0=t0; } // P:1476da mpy     x1,y0,b         x:(r5),y0
{ const auto t0=readX(post(r[0],1,m[0]));const auto t1=readY(post(r[5],1,m[5]));b=mac(b,x0,y1,true,saturation,false);x0=t0;y1=t1; } // P:1476db mac     -x0,y1,b        x:(r0)+,x0      y:(r5)+,y1
{ const auto t0=word(a);writeX(post(r[1],1,m[1]),t0); } // P:1476dc move    a,x:(r1)+
{ const auto t0=word(b);writeY(post(r[2],1,m[2]),t0); } // P:1476dd move    b,y:(r2)+
}
{ const auto t0=0x20u;r[1]=t0; } // P:1476de move    #>$20,r1
{ const auto t0=0x50u;r[0]=t0; } // P:1476e0 move    #>$50,r0
{ const auto t0=0x30u;r[3]=t0; } // P:1476e2 move    #>$30,r3
{ const auto t0=0x60u;r[2]=t0; } // P:1476e4 move    #>$60,r2
{ const auto t0=0xc00000u;y0=t0; } // P:1476e6 move    #$c0,y0
{ const auto t0=0x6ed9ecu;y1=t0; } // P:1476e7 move    #>$6ed9ec,y1
{ const auto t0=readX(r[1]);x0=t0; } // P:1476e9 move    x:(r1),x0
for(int loop1476ea=0;loop1476ea<16;++loop1476ea) { // P:1476ea do      #<$10,>$1476f2
{ const auto t0=readY(post(r[1],1,m[1]));a=multiply(y0,x0,false);x1=t0; } // P:1476ec mpy     y0,x0,a         y:(r1)+,x1
{ const auto t0=readX(r[0]);a=mac(a,y1,x1,false,saturation,false);x0=t0; } // P:1476ed mac     y1,x1,a         x:(r0),x0
{ const auto t0=readY(post(r[0],1,m[0]));b=multiply(y0,x0,false);x1=t0; } // P:1476ee mpy     y0,x0,b         y:(r0)+,x1
{ const auto t0=readX(r[1]);b=mac(b,y1,x1,false,saturation,false);x0=t0; } // P:1476ef mac     y1,x1,b         x:(r1),x0
{ const auto t0=word(a);writeX(post(r[3],1,m[3]),t0); } // P:1476f0 move    a,x:(r3)+
{ const auto t0=word(b);writeX(post(r[2],1,m[2]),t0); } // P:1476f1 move    b,x:(r2)+
}
{ const auto t0=0xc00000u;y0=t0; } // P:1476f2 move    #$c0,y0
{ const auto t0=0x912614u;y1=t0; } // P:1476f3 move    #>$912614,y1
{ const auto t0=0x20u;r[1]=t0; } // P:1476f5 move    #>$20,r1
{ const auto t0=0x50u;r[0]=t0; } // P:1476f7 move    #>$50,r0
{ const auto t0=readX(r[1]);x0=t0; } // P:1476f9 move    x:(r1),x0
for(int loop1476fa=0;loop1476fa<16;++loop1476fa) { // P:1476fa do      #<$10,>$147702
{ const auto t0=readY(post(r[1],1,m[1]));a=multiply(y0,x0,false);x1=t0; } // P:1476fc mpy     y0,x0,a         y:(r1)+,x1
{ const auto t0=readX(r[0]);a=mac(a,y1,x1,false,saturation,false);x0=t0; } // P:1476fd mac     y1,x1,a         x:(r0),x0
{ const auto t0=readY(post(r[0],1,m[0]));b=multiply(y0,x0,false);x1=t0; } // P:1476fe mpy     y0,x0,b         y:(r0)+,x1
{ const auto t0=readX(r[1]);b=mac(b,y1,x1,false,saturation,false);x0=t0; } // P:1476ff mac     y1,x1,b         x:(r1),x0
{ const auto t0=word(a);writeX(post(r[3],1,m[3]),t0); } // P:147700 move    a,x:(r3)+
{ const auto t0=word(b);writeX(post(r[2],1,m[2]),t0); } // P:147701 move    b,x:(r2)+
}
{ const auto t0=readY((r[6]+0x4));x0=t0; } // P:147702 move    y:(r6+$4),x0
{ a=multiply(0xfffc20u,x0,false); } // P:147703 mpyi    #>$fffc20,x0,a
{ a=wrap56(a+q(0xfffff0u)); } // P:147705 add     #>$fffff0,a
{ const auto t0=readX((r[6]+0x1d));b=q(t0); } // P:147707 move    x:(r6+$1d),b
{ const auto t0=readY((r[6]+0x1d));b=withLow(b,t0); } // P:147708 move    y:(r6+$1d),b0
{ const auto t0=b;writeLong(0xff,t0); } // P:147709 move    b,l:?:>$ff
{ const auto t0=word(a);x0=t0; } // P:14770b move    a,x0
{ const auto t0=word(b);x1=t0; } // P:14770c move    b,x1
{ b=mac(b,0x28f5cu,x0,false,saturation,false); } // P:14770d maci    #>$28f5c,x0,b
{ b=mac(b,0xfd70a4u,x1,false,saturation,false); } // P:14770f maci    #>$fd70a4,x1,b
{ const auto t0=high(b);writeX((r[6]+0x1d),t0); } // P:147711 move    b1,x:(r6+$1d)
{ const auto t0=low(b);writeY((r[6]+0x1d),t0); } // P:147712 move    b0,y:(r6+$1d)
{ const auto t0=readX((r[6]+0x22));a=q(t0); } // P:147713 move    x:(r6+$22),a
{ const auto t0=readY((r[6]+0x22));a=withLow(a,t0); } // P:147714 move    y:(r6+$22),a0
{ const auto t0=readY((r[6]+0x5));x0=t0; } // P:147715 move    y:(r6+$5),x0
{ const auto t0=word(a);x1=t0; } // P:147716 move    a,x1
{ a=mac(a,0xfd70a4u,x0,false,saturation,false); } // P:147717 maci    #>$fd70a4,x0,a
{ a=mac(a,0xfd70a4u,x1,false,saturation,false); } // P:147719 maci    #>$fd70a4,x1,a
{ const auto t0=high(a);writeX((r[6]+0x22),t0); } // P:14771b move    a1,x:(r6+$22)
{ const auto t0=low(a);writeY((r[6]+0x22),t0); } // P:14771c move    a0,y:(r6+$22)
{ b=wrap56(b-q(0xfffff0u)); } // P:14771d sub     #>$fffff0,b
{ b=asl(b,8); } // P:14771f asl     #$8,b,b
{ const auto t0=word(a);x1=t0; } // P:147720 move    a,x1
{ const auto t0=word(b);x0=t0; } // P:147721 move    b,x0
{ a=multiply(x1,x0,false); } // P:147722 mpy     x1,x0,a
{ const auto t0=readY((r[6]+0x23));x0=t0; } // P:147723 move    y:(r6+$23),x0
{ const auto t0=word(a);writeY((r[6]+0x23),t0); } // P:147724 move    a,y:(r6+$23)
{ const auto t0=0x80000u;a=wrap56(a-q(x0));y1=t0; } // P:147725 sub     x0,a            #>$80000,y1
{ const auto t0=word(a);a=q(x0);y0=t0; } // P:147727 tfr     x0,a            a,y0
{ const auto t0=0x80u;r[3]=t0; } // P:147728 move    #>$80,r3
{ const auto t0=0xfu;m[3]=t0; } // P:14772a move    #$f,m3
for(int loop14772b=0;loop14772b<16;++loop14772b) { // P:14772b do      #<$10,>$14772e
{ const auto t0=word(a);a=mac(a,y1,y0,false,saturation,false);writeX(post(r[3],1,m[3]),t0); } // P:14772d mac     y1,y0,a         a,x:(r3)+
}
{ const auto t0=0x20u;r[1]=t0; } // P:14772e move    #>$20,r1
{ const auto t0=0x1fu;r[2]=t0; } // P:147730 move    #>$1f,r2
{ const auto t0=readX((r[6]+0x1d));b=q(t0); } // P:147732 move    x:(r6+$1d),b
{ const auto t0=readY((r[6]+0x1d));b=withLow(b,t0); } // P:147733 move    y:(r6+$1d),b0
{ const auto t0=readLong(0xff);y1=high(t0);y0=low(t0); } // P:147734 move    l:?:>$ff,y
{ b=wrap56(b-(std::int64_t(signed24(y1))*one+y0)); } // P:147736 sub     y,b
{ b=wrap56(b+q(0x10u)); } // P:147737 add     #>$10,b
{ b=asr(b,4); } // P:147739 asr     #$4,b,b
{ const auto t0=high(b);y1=t0; } // P:14773a move    b1,y1
{ const auto t0=low(b);y0=t0; } // P:14773b move    b0,y0
{ const auto t0=readLong(r[2]);a=t0; } // P:14773c move    l:(r2),a
{ const auto t0=readX(post(r[1],1,m[1]));x1=t0; } // P:14773d move    x:(r1)+,x1
{ const auto t0=readX(post(r[3],1,m[3]));x0=t0; } // P:14773e move    x:(r3)+,x0
for(int loop14773f=0;loop14773f<6;++loop14773f) { // P:14773f do      #<$6,>$14774a
{ const auto t0=readLong(0xff);b=t0; } // P:147741 move    l:?:>$ff,b
for(int loop147743=0;loop147743<16;++loop147743) { // P:147743 do      #<$10,>$147749
{ const auto t0=a;a=multiply(x1,x0,false);writeLong(post(r[2],1,m[2]),t0); } // P:147745 mpy     x1,x0,a         a,l:(r2)+
{ a=asr(a,8); } // P:147746 asr     #$8,a,a
{ const auto t0=readX(post(r[1],1,m[1]));a=wrap56(a+b);x1=t0; } // P:147747 add     b,a             x:(r1)+,x1
{ const auto t0=readX(post(r[3],1,m[3]));b=wrap56(b+(std::int64_t(signed24(y1))*one+y0));x0=t0; } // P:147748 add     y,b             x:(r3)+,x0
}
{  } // P:147749 nop
}
{ const auto t0=a;writeLong(post(r[2],1,m[2]),t0); } // P:14774a move    a,l:(r2)+
{ const auto t0=0xffffffu;m[3]=t0; } // P:14774b move    #>$ffffff,m3
{ const auto t0=readY((r[6]+0x10));r[0]=t0; } // P:14774d move    y:(r6+$10),r0
{ const auto t0=0x7ffu;m[0]=t0; } // P:14774f move    #>$7ff,m0
{ const auto t0=m[0];m[4]=t0; } // P:147751 move    m0,m4
{ const auto t0=r[0];a=q(t0); } // P:147752 move    r0,a
{ a=wrap56(a+q(0x800u)); } // P:147753 add     #>$800,a
{ const auto t0=word(a);r[1]=t0; } // P:147755 move    a,r1
{ const auto t0=0x20u;r[2]=t0; } // P:147756 move    #>$20,r2
{ const auto t0=0x80u;r[3]=t0; } // P:147758 move    #>$80,r3
for(int loop14775a=0;loop14775a<2;++loop14775a) { // P:14775a do      #<$2,>$14776a
{ const auto t0=readX(r[2]);n[0]=t0; } // P:14775c move    x:(r2),n0
{ r[4]=offset(r[0],n[0],m[0]); } // P:14775d lua     (r0)+n0,r4
for(int loop14775e=0;loop14775e<48;++loop14775e) { // P:14775e do      #<$30,>$147769
{ const auto t0=readY(post(r[2],1,m[2]));y1=t0; } // P:147760 move    y:(r2)+,y1
{ const auto t0=readY(post(r[4],1,m[4]));x0=t0; } // P:147761 move    y:(r4)+,x0
{ const auto t0=readY(post(r[4],1,m[4]));x1=t0; } // P:147762 move    y:(r4)+,x1
{ a=multiply(x1,y1,true); } // P:147763 mpysu   x1,y1,a
{ a=mac(a,x0,y1,true,saturation,true); } // P:147764 macsu   -x0,y1,a
{ const auto t0=readX(r[2]);a=asr(a,1);n[0]=t0; } // P:147765 asr     a               x:(r2),n0
{ a=wrap56(a+q(x0)); } // P:147766 add     x0,a
{ r[4]=offset(r[0],n[0],m[0]); } // P:147767 lua     (r0)+n0,r4
{ const auto t0=word(a);writeX(post(r[3],1,m[3]),t0); } // P:147768 move    a,x:(r3)+
}
{ const auto t0=r[1];r[0]=t0; } // P:147769 move    r1,r0
}
{ const auto t0=0xffffffu;m[4]=t0; } // P:14776a move    #>$ffffff,m4
{ const auto t0=0xffffffu;m[0]=t0; } // P:14776c move    #>$ffffff,m0
{ const auto t0=0x3eu;r[4]=t0; } // P:14776e move    #>$3e,r4
{ const auto t0=0x80u;r[0]=t0; } // P:147770 move    #>$80,r0
{ const auto t0=0x5fu;m[0]=t0; } // P:147772 move    #$5f,m0
{ const auto t0=0x10u;n[0]=t0; } // P:147773 move    #$10,n0
{ const auto t0=0x2aaaabu;y1=t0; } // P:147774 move    #>$2aaaab,y1
{ const auto t0=readY((r[6]+0x20));x0=t0; } // P:147776 move    y:(r6+$20),x0
{ a=multiply(x0,y1,false); } // P:147777 mpy     x0,y1,a
{ const auto t0=readX(post(r[0],n[0],m[0]));x1=t0; } // P:147778 move    x:(r0)+n0,x1
{ const auto t0=word(a);y1=t0; } // P:147779 move    a,y1
for(int loop14777a=0;loop14777a<16;++loop14777a) { // P:14777a do      #<$10,>$147783
{ const auto t0=readX(post(r[0],n[0],m[0]));const auto t1=word(a);a=multiply(y1,x1,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:14777c mpy     y1,x1,a         x:(r0)+n0,x1    a,y:(r4)+
{ const auto t0=readX(post(r[0],n[0],m[0]));a=mac(a,y1,x1,false,saturation,false);x1=t0; } // P:14777d mac     y1,x1,a         x:(r0)+n0,x1
{ const auto t0=readX(post(r[0],n[0],m[0]));a=mac(a,y1,x1,false,saturation,false);x1=t0; } // P:14777e mac     y1,x1,a         x:(r0)+n0,x1
{ const auto t0=readX(post(r[0],n[0],m[0]));const auto t1=word(b);b=multiply(y1,x1,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:14777f mpy     y1,x1,b         x:(r0)+n0,x1    b,y:(r4)+
{ const auto t0=readX(post(r[0],n[0],m[0]));b=mac(b,y1,x1,false,saturation,false);x1=t0; } // P:147780 mac     y1,x1,b         x:(r0)+n0,x1
{ b=mac(b,y1,x1,false,saturation,false);(void)post(r[0],1,m[0]); } // P:147781 mac     y1,x1,b         (r0)+
{ const auto t0=readX(post(r[0],n[0],m[0]));x1=t0; } // P:147782 move    x:(r0)+n0,x1
}
{ const auto t0=word(a);writeY(post(r[4],1,m[4]),t0); } // P:147783 move    a,y:(r4)+
{ const auto t0=word(b);writeY(post(r[4],1,m[4]),t0); } // P:147784 move    b,y:(r4)+
{ const auto t0=0xffffffu;m[0]=t0; } // P:147785 move    #>$ffffff,m0
{ const auto t0=0x144ac7u;r[2]=t0; } // P:147787 move    #>$144ac7,r2
{ const auto t0=readY((r[6]+0xa));b=q(t0); } // P:147789 move    y:(r6+$a),b
{ b=asr(b,16); } // P:14778a asr     #$10,b,b
{ const auto t0=0x40u;r[5]=t0; } // P:14778b move    #>$40,r5
{ const auto t0=0x60u;r[4]=t0; } // P:14778d move    #>$60,r4
{ const auto t0=word(b);n[2]=t0; } // P:14778f move    b,n2
{ const auto t0=readY((r[6]+0x1e));a=q(t0); } // P:147790 move    y:(r6+$1e),a
{ const auto t0=readY((r[6]+0x1f));b=q(t0); } // P:147791 move    y:(r6+$1f),b
{ const auto t0=0x0u;r[0]=t0; } // P:147792 move    #>$0,r0
{ const auto t0=readX(offset(r[2],n[2],m[2]));x0=t0; } // P:147794 move    x:(r2+n2),x0
{ const auto t0=readY(post(r[5],1,m[5]));y1=t0; } // P:147795 move    y:(r5)+,y1
for(int loop147796=0;loop147796<16;++loop147796) { // P:147796 do      #<$10,>$14779c
{ const auto t0=word(a);const auto t1=word(a);a=mac(a,x0,y1,false,saturation,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:147798 mac     x0,y1,a         a,x1            a,y:(r4)+
{ const auto t0=readY(post(r[5],1,m[5]));a=mac(a,x1,x0,true,saturation,false);y1=t0; } // P:147799 mac     -x1,x0,a        y:(r5)+,y1
{ const auto t0=word(b);const auto t1=word(b);b=mac(b,x0,y1,false,saturation,false);x1=t0;writeY(post(r[4],1,m[4]),t1); } // P:14779a mac     x0,y1,b         b,x1            b,y:(r4)+
{ const auto t0=readY(post(r[5],1,m[5]));b=mac(b,x1,x0,true,saturation,false);y1=t0; } // P:14779b mac     -x1,x0,b        y:(r5)+,y1
}
{ const auto t0=word(a);writeY((r[6]+0x1e),t0); } // P:14779c move    a,y:(r6+$1e)
{ const auto t0=word(b);writeY((r[6]+0x1f),t0); } // P:14779d move    b,y:(r6+$1f)
{ const auto t0=0x0u;r[4]=t0; } // P:14779e move    #>$0,r4
{ const auto t0=0x60u;r[5]=t0; } // P:1477a0 move    #>$60,r5
{ const auto t0=0x2u;n[4]=t0; } // P:1477a2 move    #$2,n4
{ const auto t0=n[4];n[5]=t0; } // P:1477a3 move    n4,n5
{ const auto t0=readY((r[6]+0x10));a=q(t0); } // P:1477a4 move    y:(r6+$10),a
{ const auto t0=word(a);r[0]=t0; } // P:1477a5 move    a,r0
{ a=wrap56(a+q(0x800u)); } // P:1477a6 add     #>$800,a
{ const auto t0=0x7ffu;m[0]=t0; } // P:1477a8 move    #>$7ff,m0
{ const auto t0=m[0];m[1]=t0; } // P:1477aa move    m0,m1
{ const auto t0=word(a);r[1]=t0; } // P:1477ab move    a,r1
{ const auto t0=readY((r[6]+0x8));a=q(t0); } // P:1477ac move    y:(r6+$8),a
{ a=wrap56(-a); } // P:1477ad neg     a
{ a=wrap56(a-q(0xfd71u)); } // P:1477ae sub     #>$fd71,a
{ const auto t0=0x7fffffu;b=q(t0); } // P:1477b0 move    #>$7fffff,b
{ b=wrap56(b-a); } // P:1477b2 sub     a,b
{ b=asr(b,1); } // P:1477b3 asr     b
{ const auto t0=word(a);x1=t0; } // P:1477b4 move    a,x1
{ const auto t0=word(b);x0=t0; } // P:1477b5 move    b,x0
{ const auto t0=readY(post(r[4],n[4],m[4]));y0=t0; } // P:1477b6 move    y:(r4)+n4,y0
for(int loop1477b7=0;loop1477b7<16;++loop1477b7) { // P:1477b7 do      #<$10,>$1477bc
{ const auto t0=readY(post(r[5],n[5],m[5]));a=multiply(y0,x0,false);y0=t0; } // P:1477b9 mpy     y0,x0,a         y:(r5)+n5,y0
{ const auto t0=readY(post(r[4],n[4],m[4]));a=mac(a,x1,y0,false,saturation,false);y0=t0; } // P:1477ba mac     x1,y0,a         y:(r4)+n4,y0
{ const auto t0=word(a);writeX(post(r[0],1,m[0]),t0); } // P:1477bb move    a,x:(r0)+
}
{ const auto t0=0x1u;r[4]=t0; } // P:1477bc move    #>$1,r4
{ const auto t0=0x61u;r[5]=t0; } // P:1477be move    #>$61,r5
{ const auto t0=r[0];writeY((r[6]+0x10),t0); } // P:1477c0 move    r0,y:(r6+$10)
{ const auto t0=readY(post(r[4],n[4],m[4]));y0=t0; } // P:1477c2 move    y:(r4)+n4,y0
for(int loop1477c3=0;loop1477c3<16;++loop1477c3) { // P:1477c3 do      #<$10,>$1477c8
{ const auto t0=readY(post(r[5],n[5],m[5]));a=multiply(y0,x0,false);y0=t0; } // P:1477c5 mpy     y0,x0,a         y:(r5)+n5,y0
{ const auto t0=readY(post(r[4],n[4],m[4]));a=mac(a,x1,y0,false,saturation,false);y0=t0; } // P:1477c6 mac     x1,y0,a         y:(r4)+n4,y0
{ const auto t0=word(a);writeX(post(r[1],1,m[1]),t0); } // P:1477c7 move    a,x:(r1)+
}
{ const auto t0=0xffffffu;m[0]=t0; } // P:1477c8 move    #>$ffffff,m0
{ const auto t0=m[0];m[1]=t0; } // P:1477ca move    m0,m1
{ const auto t0=0x0u;r[4]=t0; } // P:1477cb move    #>$0,r4
{ const auto t0=0x40u;r[5]=t0; } // P:1477cd move    #>$40,r5
{ const auto t0=readY((r[6]+0x7));x0=t0; } // P:1477cf move    y:(r6+$7),x0
{ const auto t0=0x7fffffu;a=q(t0); } // P:1477d0 move    #>$7fffff,a
{ a=wrap56(a-q(x0)); } // P:1477d2 sub     x0,a
{ const auto t0=readY(post(r[4],1,m[4]));y0=t0; } // P:1477d3 move    y:(r4)+,y0
{ const auto t0=word(a);x1=t0; } // P:1477d4 move    a,x1
for(int loop1477d5=0;loop1477d5<16;++loop1477d5) { // P:1477d5 do      #<$10,>$1477dd
{ const auto t0=readY(post(r[5],1,m[5]));a=multiply(x1,y0,false);y0=t0; } // P:1477d7 mpy     x1,y0,a         y:(r5)+,y0
{ const auto t0=readY(post(r[4],1,m[4]));a=mac(a,y0,x0,false,saturation,false);y0=t0; } // P:1477d8 mac     y0,x0,a         y:(r4)+,y0
{ const auto t0=readY(post(r[5],1,m[5]));b=multiply(x1,y0,false);y0=t0; } // P:1477d9 mpy     x1,y0,b         y:(r5)+,y0
{ const auto t0=readY(post(r[4],1,m[4]));b=mac(b,y0,x0,false,saturation,false);y0=t0; } // P:1477da mac     y0,x0,b         y:(r4)+,y0
{ const auto t0=word(a);writeY(post(r[7],1,m[7]),t0); } // P:1477db move    a,y:(r7)+
{ const auto t0=word(b);writeY(post(r[7],1,m[7]),t0); } // P:1477dc move    b,y:(r7)+
}
{ return; } // P:1477dd rts
}
}
