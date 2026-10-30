// Автоматизация Detune/Tune/Level/Position на коротких источниках.
// Отдельный стенд для ASan/UBSan без нереалистичного CPU-бенчмарка.
#include "../src/engines/granular.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
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
    std::printf("ASan/UBSan: %d cases, %d grains, %d unsafe reads, %d NaN, raw peak %.6f\n",cases,grains,unsafe,bad,peak);
    return (cases==240 && grains>100 && unsafe==0 && bad==0) ? 0 : 1;
}
