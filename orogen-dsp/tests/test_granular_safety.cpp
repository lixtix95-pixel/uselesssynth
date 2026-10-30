// Автоматизация Detune/Tune/Level/Position на коротких источниках.
// Отдельный стенд для ASan/UBSan без нереалистичного CPU-бенчмарка.
#include "../src/engines/granular.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
using namespace orogen;
namespace orogen {
struct GranularTestProbe {
    static void seed(GranularEngine& e, uint32_t value) { e.rng=Rng{value}; }
    static float firstPhase(const GranularEngine& e) { return e.grains[0].phase; }
    static double firstUpper(const GranularEngine& e) {
        for (const auto& g : e.grains) if (g.active)
            return g.pos-(double)g.rate+(double)g.dur*(double)g.rate;
        return -1.0;
    }
    static double firstLower(const GranularEngine& e) {
        for (const auto& g : e.grains) if (g.active) {
            const double start=g.pos-(double)g.rate;
            return g.rate < 0.0f ? start+(double)g.dur*(double)g.rate : start;
        }
        return -1.0;
    }
    static void inspect(const GranularEngine& e, std::array<bool, GranularEngine::kMaxGrains>& prev,
                        int& spawned, int& unsafe, int* reversed=nullptr) {
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i) {
            const auto& g = e.grains[i];
            if (g.active && !prev[(size_t)i]) {
                ++spawned;
                if (reversed && g.rate < 0.0f) ++*reversed;
            }
            if (g.active || prev[(size_t)i]) {
                const double x = (double) g.pos - g.rate;
                auto edge = [&](int len, float scale) {
                    const int centre = (int)std::floor(x * scale), half = g.mip.grainHalf();
                    return centre - (half - 1) < 0 || centre + half >= len;
                };
                if (edge(g.mip.lenA, g.mip.invScaleA) ||
                    (g.mip.wB > 0.001f && edge(g.mip.lenB, g.mip.invScaleB))) ++unsafe;
            }
            prev[(size_t)i] = g.active;
        }
    }
};
}
int main() {
    int edgeCases=0, edgeGrains=0, edgeUnsafe=0, edgeBad=0, edgeNotGuarded=0;
    double edgeMaxOffset=0.0;
    float edgeMinPhase=1.0f;
    int grains=0, unsafe=0, bad=0, cases=0; float peak=0.0f;
    for (double sr : {44100.0,48000.0,96000.0})
      for (double seconds : {0.005,0.05,0.5,2.0}) {
        SampleBank bank;
        bank.shots.push_back(buildOneShot(OneShotKind::FmTine, sr, seconds));
        for (int note : {45,69,93,117,127})
          for (float spread : {0.0f,1.0f})
            for (float random : {-1.0f,1.0f}) {
                GranularEngine e; e.setBank(&bank); e.setSampleRate(sr);
                e.setNoteRandom(random); e.start(note,1.0f);
                EngineParams p; p.p[0]=0.97f; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=spread;
                p.detune=0.0f; p.tuneSemis=-24.0f; p.level=1.0f; e.prepare(p);
                std::array<bool,GranularEngine::kMaxGrains> previous{};
                for (int i=0; i<(int)(sr*0.20); ++i) {
                    if ((i%128)==0) {
                        const int k = (i/128)%3;
                        p.detune = (k==0 ? 0.0f : 1.0f);
                        p.tuneSemis = (k==0 ? -24.0f : (k==1 ? 0.0f : 24.0f));
                        p.level = (k==0 ? 0.0f : 1.0f);
                        p.p[0] = (k==0 ? 0.0f : 1.0f);
                        p.grainReverse = (k==0 ? 0.0f : (k==1 ? 0.5f : 1.0f));
                        e.prepare(p);
                    }
                    const float v=e.render(p);
                    if (!std::isfinite(v)) ++bad;
                    peak=std::max(peak,std::fabs(v));
                    GranularTestProbe::inspect(e,previous,grains,unsafe);
                }
                ++cases;
            }
      }
    // Явно покрываем 0/50/100% реверса на разных длительностях, нотах и
    // крайних позициях, включая источники короче одного окна огибающей.
    int reverseCases=0, reverseGrains=0, reverseCount=0, reverseUnsafe=0, reverseBad=0, signErrors=0;
    for (double sr : {44100.0,48000.0,96000.0})
      for (double seconds : {0.005,0.05,0.5,2.0}) {
        SampleBank bank; bank.shots.push_back(buildOneShot(OneShotKind::FmTine,sr,seconds));
        for (int note : {36,60,93,127})
          for (float pos : {0.0f,0.5f,1.0f})
            for (float chance : {0.0f,0.5f,1.0f}) {
                GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(note,1.0f);
                EngineParams p; p.p[0]=pos; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=0.5f;
                p.detune=0.0f; p.grainReverse=chance; e.prepare(p);
                std::array<bool,GranularEngine::kMaxGrains> previous{};
                const int before=reverseGrains, backwardsBefore=reverseCount;
                for (int i=0; i<(int)(sr*0.10); ++i) {
                    if (!std::isfinite(e.render(p))) ++reverseBad;
                    GranularTestProbe::inspect(e,previous,reverseGrains,reverseUnsafe,&reverseCount);
                }
                const int spawned=reverseGrains-before, backwards=reverseCount-backwardsBefore;
                if ((chance==0.0f && backwards!=0) || (chance==1.0f && backwards!=spawned)) ++signErrors;
                ++reverseCases;
            }
      }
    // Длинная запись включает ветку префиксной интеграции огибающей:
    // 220-мс зерно на ноте 115 проходит > 1.28 с исходных отсчётов.
    int longGrains=0, longUnsafe=0, longBad=0;
    {
        constexpr int sr=48000;
        OneShot shot; shot.name="long safety"; shot.rootNote=45;
        shot.levels.resize(1); shot.levels[0].resize((size_t)sr*26);
        for (int i=0; i<sr*26; ++i)
            shot.levels[0][(size_t)i]=(float)(0.03*std::sin(kTwoPi*100.0*i/sr));
        buildMipLevels(shot,sr);
        SampleBank bank; bank.shots.push_back(std::move(shot));
        for (float chance : {0.0f,1.0f}) {
            GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(115,1.0f);
            EngineParams p; p.p[0]=0.5f; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=0.0f;
            p.detune=0.0f; p.level=1.0f; p.grainReverse=chance; e.prepare(p);
            std::array<bool,GranularEngine::kMaxGrains> previous{};
            for (int i=0; i<sr/4; ++i) {
                if (!std::isfinite(e.render(p))) ++longBad;
                GranularTestProbe::inspect(e,previous,longGrains,longUnsafe);
            }
        }
        const auto& s=bank.shots[0];
        for (double a : {-480.0,0.0,480.0,600000.0,1248000.0})
            for (double length : {1.0,480.0,2400.0,600000.0})
                if (!std::isfinite(s.meanEnvEnergy(a,a+length))) ++longBad;
    }
    // Отдельно — дробный rate при большой координате, включая края записи.
    // Это проверяет изменённый double-путь курсора и 32-отводный sinc.
    int slowGrains=0, slowUnsafe=0, slowBad=0;
    {
        constexpr int sr=96000;
        OneShot shot; shot.name="long slow safety"; shot.rootNote=45;
        shot.levels.resize(1); shot.levels[0].resize((size_t)sr*26);
        for (int i=0; i<sr*26; ++i)
            shot.levels[0][(size_t)i]=(float)(0.03*std::sin(kTwoPi*8000.0*i/sr));
        buildMipLevels(shot,sr);
        SampleBank bank; bank.shots.push_back(std::move(shot));
        for (float pos : {0.0f,0.5f,0.95f,1.0f})
          for (float chance : {0.0f,1.0f}) {
            GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(36,1.0f);
            EngineParams p; p.p[0]=pos; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=0.0f;
            p.detune=0.0f; p.level=1.0f; p.grainReverse=chance; e.prepare(p);
            std::array<bool,GranularEngine::kMaxGrains> previous{};
            for (int i=0; i<sr/8; ++i) {
                if (!std::isfinite(e.render(p))) ++slowBad;
                GranularTestProbe::inspect(e,previous,slowGrains,slowUnsafe);
            }
        }
        // Полная жизнь длинного зерна при 96 кГц: float-фаза может
        // закончиться на несколько отсчётов позже dur. Прижимаем рассчитанный
        // интервал к краю с помощью джиттера первого зерна и проверяем оба
        // направления для 32-tap (rate 32) и 64-tap (rate 48) sinc.
        for (float ratio : {1.0f,1.5f})
          for (bool reverse : {false,true}) {
            const float tune=12.0f*std::log2(ratio);
            const float rate=std::pow(2.0f,(105.0f-45.0f+tune)/12.0f);
            MipReader mip;
            if (!mip.setupGranular(bank.shots[0],rate)) { ++edgeBad; continue; }
            if (mip.grainHalf() != (ratio==1.0f ? SincKernel::kHalf : GrainSincKernel::kHalf)) ++edgeBad;
            const float begin=std::max(64.0f,(float)(mip.grainHalf()+1)/mip.invScaleA);
            const float end=std::min((float)bank.shots[0].lens[0]-128.0f,
                                     (float)(mip.lenA-mip.grainHalf()-2)/mip.invScaleA);
            const float usable=end-begin;
            // 20649 отсчётов дают заметную задержку float-фазы на верхнем
            // краю; 21120 (220 мс) — на нижнем.
            const float sizeT=reverse ? 1.0f : (20649.0f/(float)sr-0.008f)/0.212f;
            const float dur=std::min((0.008f+sizeT*0.212f)*(float)sr,0.5f*usable/rate-2.0f);
            const float range=usable-(dur+2.0f)*rate;
            const uint32_t seed=reverse ? 0x51F3A7u : 42u; // джиттер: отрицательный/положительный
            Rng offsetRng{seed}; (void)offsetRng.nextBipolar();
            const float jitter=offsetRng.nextBipolar()*0.5f*dur*rate;
            EngineParams edge;
            edge.p[0]=std::clamp((reverse ? -jitter : range-jitter)/range,0.0f,1.0f);
            edge.p[1]=sizeT; edge.p[2]=0.0f; edge.p[3]=0.0f;
            edge.detune=0.0f; edge.tuneSemis=tune; edge.grainReverse=reverse ? 1.0f : 0.0f;
            GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(105,1.0f);
            GranularTestProbe::seed(e,seed); e.prepare(edge);
            std::array<bool,GranularEngine::kMaxGrains> previous{};
            for (int i=0; i<(int)(sr*0.24); ++i) {
                if (!std::isfinite(e.render(edge))) ++edgeBad;
                if (i==0) {
                    const double actual=reverse ? GranularTestProbe::firstLower(e) : GranularTestProbe::firstUpper(e);
                    const double expected=reverse ? (double)begin : (double)end-2.0*(double)rate;
                    edgeMaxOffset=std::max(edgeMaxOffset,std::fabs(actual-expected));
                }
                GranularTestProbe::inspect(e,previous,edgeGrains,edgeUnsafe);
            }
            const float stopped=GranularTestProbe::firstPhase(e);
            edgeMinPhase=std::min(edgeMinPhase,stopped);
            if (stopped>=1.0f || previous[0]) ++edgeNotGuarded;
            ++edgeCases;
          }
    }
    // Граница пустой/непустой огибающей: исходник на один отсчёт короче
    // 10 мс, ровно 10 мс и 5 мс, включая настоящую тишину.
    int shortCases=0, shortGrains=0, shortUnsafe=0, shortBad=0;
    for (int sr : {44100,48000,96000}) {
        const int step=(int)(sr*0.01);
        for (int n : {step/2,step-1,step})
          for (float amp : {0.0f,0.03f,0.30f,0.80f}) {
            OneShot shot; shot.name="short boundary safety"; shot.rootNote=45;
            shot.levels.resize(1); shot.levels[0].resize((size_t)n);
            for (int i=0; i<n; ++i)
                shot.levels[0][(size_t)i]=(float)(amp*std::sin(kTwoPi*1000.0*i/sr));
            buildMipLevels(shot,sr);
            const int expectedCells=n<step ? 0 : 1;
            if ((int)shot.env.size()!=expectedCells) ++shortBad;
            SampleBank bank; bank.shots.push_back(std::move(shot));
            for (float pos : {0.0f,0.5f,1.0f})
              for (float chance : {0.0f,1.0f}) {
                GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(36,1.0f);
                EngineParams p; p.p[0]=pos; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=0.0f;
                p.detune=0.0f; p.level=1.0f; p.grainReverse=chance; e.prepare(p);
                std::array<bool,GranularEngine::kMaxGrains> previous{};
                for (int i=0; i<sr/10; ++i) {
                    const float x=e.render(p);
                    if (!std::isfinite(x) || (amp==0.0f && x!=0.0f)) ++shortBad;
                    GranularTestProbe::inspect(e,previous,shortGrains,shortUnsafe);
                }
                ++shortCases;
            }
        }
    }
    std::printf("ASan/UBSan: %d cases, %d grains, %d unsafe reads, %d NaN, raw peak %.6f\n",cases,grains,unsafe,bad,peak);
    std::printf("reverse 0/50/100%%: %d cases, %d grains (%d reverse), %d unsafe reads, %d NaN/Inf, %d sign errors\n",
                reverseCases,reverseGrains,reverseCount,reverseUnsafe,reverseBad,signErrors);
    std::printf("long fast source: %d grains, %d unsafe reads, %d NaN/Inf\n",longGrains,longUnsafe,longBad);
    std::printf("short envelope boundary: %d cases, %d grains, %d unsafe reads, %d invalid/silence errors\n",shortCases,shortGrains,shortUnsafe,shortBad);
    std::printf("long slow source: %d grains, %d unsafe reads, %d NaN/Inf\n",slowGrains,slowUnsafe,slowBad);
    std::printf("exact edges 96k (32/64 tap, forward/reverse): %d cases, %d grains, max offset %.6f, min stop phase %.8f, %d unsafe reads, %d NaN/Inf, %d not guarded\n",
                edgeCases,edgeGrains,edgeMaxOffset,edgeMinPhase,edgeUnsafe,edgeBad,edgeNotGuarded);
    return (cases==240 && grains>100 && unsafe==0 && bad==0
            && reverseCases==432 && reverseGrains>100 && reverseCount>100
            && reverseUnsafe==0 && reverseBad==0 && signErrors==0
            && longGrains>5 && longUnsafe==0 && longBad==0
            && slowGrains>5 && slowUnsafe==0 && slowBad==0
            && edgeCases==4 && edgeGrains==4 && edgeMaxOffset<2.0 && edgeMinPhase>0.99f
            && edgeUnsafe==0 && edgeBad==0 && edgeNotGuarded==0
            && shortCases==216 && shortGrains>100 && shortUnsafe==0 && shortBad==0) ? 0 : 1;
}
