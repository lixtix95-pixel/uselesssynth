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
    static void inspect(const GranularEngine& e, std::array<bool, GranularEngine::kMaxGrains>& prev,
                        int& spawned, int& unsafe) {
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i) {
            const auto& g = e.grains[i];
            if (g.active && !prev[(size_t)i]) ++spawned;
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
        GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(115,1.0f);
        EngineParams p; p.p[0]=0.5f; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=0.0f;
        p.detune=0.0f; p.level=1.0f; e.prepare(p);
        std::array<bool,GranularEngine::kMaxGrains> previous{};
        for (int i=0; i<sr/4; ++i) {
            if (!std::isfinite(e.render(p))) ++longBad;
            GranularTestProbe::inspect(e,previous,longGrains,longUnsafe);
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
        for (float pos : {0.0f,0.5f,0.95f,1.0f}) {
            GranularEngine e; e.setBank(&bank); e.setSampleRate(sr); e.start(36,1.0f);
            EngineParams p; p.p[0]=pos; p.p[1]=1.0f; p.p[2]=1.0f; p.p[3]=0.0f;
            p.detune=0.0f; p.level=1.0f; e.prepare(p);
            std::array<bool,GranularEngine::kMaxGrains> previous{};
            for (int i=0; i<sr/8; ++i) {
                if (!std::isfinite(e.render(p))) ++slowBad;
                GranularTestProbe::inspect(e,previous,slowGrains,slowUnsafe);
            }
        }
    }
    std::printf("ASan/UBSan: %d cases, %d grains, %d unsafe reads, %d NaN, raw peak %.6f\n",cases,grains,unsafe,bad,peak);
    std::printf("long fast source: %d grains, %d unsafe reads, %d NaN/Inf\n",longGrains,longUnsafe,longBad);
    std::printf("long slow source: %d grains, %d unsafe reads, %d NaN/Inf\n",slowGrains,slowUnsafe,slowBad);
    return (cases==240 && grains>100 && unsafe==0 && bad==0
            && longGrains>5 && longUnsafe==0 && longBad==0
            && slowGrains>5 && slowUnsafe==0 && slowBad==0) ? 0 : 1;
}
