#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <cassert>

namespace nova {
// Native, statically translated CHORUS procedure from OS 1.32B.
// No instruction decoder, program counter, JIT, firmware loader or emulator.
// Original 16-frame schedule and 24/48/56-bit arithmetic are retained.
class ChorusCore {
public:
 using Word=std::uint32_t;
  static constexpr std::array<int,8> defaults{{0,0,0,127,0,0,127,127}};
 ChorusCore() { reset(defaults); }
 void reset(const std::array<int,8>& parameters) noexcept;
 void setParameters(const std::array<int,8>& parameters) noexcept;
 // Float path for per-sample LFO modulation only. Bit-identical to the
 // integer path for integral inputs; fractional values address intermediate
 // DSP words (sub-step modulation). The translated procedure is untouched.
 void setParametersF(const std::array<float,8>& parameters) noexcept;
 void process16(const std::int32_t* interleavedInput, std::int32_t* interleavedOutput) noexcept;
 static std::int32_t signed24(Word v) noexcept { v&=0xffffffu; return v&0x800000u ? std::int32_t(v)-0x1000000 : std::int32_t(v); }
 // Test access uses the original data addresses, not executable P-memory.
 Word readX(Word address) const noexcept;
 Word readY(Word address) const noexcept;
 void writeX(Word address, Word value) noexcept;
 void writeY(Word address, Word value) noexcept;
private:
 std::array<Word,0x580> x{};
 std::array<Word,0x580> y{};
 std::array<Word,4096> delay{};
 void initialiseProcedure() noexcept;
 void processProcedure() noexcept;
 static constexpr std::int64_t one=0x1000000ll;
 static std::int64_t q(Word v) noexcept { return std::int64_t(signed24(v))*one; }
 static std::int64_t asr(std::int64_t v,unsigned n) noexcept {
  const auto divisor=std::int64_t(1)<<n;
  return v>=0?v/divisor:-((-v+divisor-1)/divisor);
 }
 static std::int64_t wrap56(std::int64_t v) noexcept {
  const auto u=std::uint64_t(v)&0xffffffffffffffull;
  return u&0x80000000000000ull ? std::int64_t(u)-0x100000000000000ll : std::int64_t(u);
 }
 static std::int64_t asl(std::int64_t v,unsigned n) noexcept { return wrap56(std::int64_t((std::uint64_t(v)<<n)&0xffffffffffffffull)); }
 static std::int64_t clip48(std::int64_t v) noexcept { return std::clamp<std::int64_t>(v,-0x800000000000ll,0x7fffffffffffll); }
 static Word word(std::int64_t v) noexcept { return Word(asr(clip48(v),24))&0xffffffu; }
 static Word high(std::int64_t v) noexcept { return Word(std::uint64_t(v)>>24)&0xffffffu; }
 static Word low(std::int64_t v) noexcept { return Word(v)&0xffffffu; }
 static std::int64_t withLow(std::int64_t a,Word v) noexcept { return wrap56(std::int64_t((std::uint64_t(a)&0xffffffff000000ull)|(v&0xffffffu))); }
 static std::int64_t multiply(Word a,Word b,bool unsignedB=false) noexcept { return std::int64_t(signed24(a))*(unsignedB?std::int64_t(b&0xffffffu):std::int64_t(signed24(b)))*2; }
 static std::int64_t mac(std::int64_t a,Word b,Word c,bool negative,bool saturate,bool unsignedC=false) noexcept {
  const auto v=a+(negative?-1:1)*multiply(b,c,unsignedC);
  return saturate?clip48(v):wrap56(v);
 }
 static Word offset(Word address,Word displacement,Word modulo) noexcept {
  const auto d=signed24(displacement);
  if(modulo==0xffffffu) return Word(std::int64_t(address)+d)&0xffffffu;
  Word mask=1;while(mask<=modulo)mask<<=1;--mask;
  const auto base=address&~mask;
  auto pos=std::int64_t(address&mask)+d;
  const auto size=std::int64_t(modulo)+1;
  pos%=size;if(pos<0)pos+=size;
  return base+Word(pos);
 }
 static Word post(Word& address,Word displacement,Word modulo) noexcept { auto old=address;address=offset(address,displacement,modulo);return old; }
 std::int64_t readLong(Word address) const noexcept { return std::int64_t(signed24(readX(address)))*one+readY(address); }
 void writeLong(Word address,std::int64_t value) noexcept { value=clip48(value);writeX(address,high(value));writeY(address,low(value)); }
};
}
