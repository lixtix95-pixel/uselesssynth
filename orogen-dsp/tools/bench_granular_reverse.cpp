// 16 голосов, 3 повтора, измерять ОТДЕЛЬНО от компиляции.
// make build/bench_granular_reverse && ./build/bench_granular_reverse
#include "../src/engines/granular.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>
using namespace orogen;
int main() {
    constexpr double sr=48000; constexpr int voices=16, frames=256*600;
    SampleBank bank; bank.shots.push_back(buildProbeShot());
    volatile float sink=0;
    for(float chance: {0.0f,0.5f,1.0f}) {
        double run[3]{};
        for(int r=0;r<3;++r) {
            std::vector<std::unique_ptr<GranularEngine>> es;
            EngineParams p; p.p[0]=.35f; p.p[1]=.45f; p.p[2]=.55f; p.p[3]=0; p.detune=0; p.grainReverse=chance;
            for(int k=0;k<voices;++k) {
                auto e=std::make_unique<GranularEngine>(); e->setBank(&bank);e->setSampleRate(sr);
                e->start(57+k%12,1); e->prepare(p); es.push_back(std::move(e));
            }
            auto t0=std::chrono::steady_clock::now();
            for(int i=0;i<frames;++i) { float s=0;for(auto& e:es)s+=e->render(p);sink=sink+s; }
            auto t1=std::chrono::steady_clock::now();
            double seconds=std::chrono::duration<double>(t1-t0).count();run[r]=100*seconds/(frames/sr);
        }
        std::sort(run,run+3);
        std::printf("reverse %.0f%%: %.2f / %.2f / %.2f%% core, median %.2f%%; voices %d, sr %.0f, audio %.1f s\n",chance*100,run[0],run[1],run[2],run[1],voices,sr,frames/sr);
    }
    return !std::isfinite(sink);
}
