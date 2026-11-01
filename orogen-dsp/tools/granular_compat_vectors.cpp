// Deterministic factory Granular vector. Compile against either source tree:
// c++ -O2 -std=c++17 -Isrc tools/granular_compat_vectors.cpp -o /tmp/grain-vectors
// /tmp/grain-vectors > factory.bin
#include "engines/granular.h"
#include <cstdio>
using namespace orogen;
int main()
{
    for (double sr : { 44100., 48000., 96000. })
    {
        SampleBank bank; bank.buildFactory (sr);
        for (int source = 0; source < 8; ++source)
            for (int note : { 36, 48, 60, 84 })
                for (float spread : { 0.0f, 0.6f })
                {
                    GranularEngine engine; engine.setBank (&bank); engine.setSampleRate (sr);
                    engine.setNoteRandom (0.2f); engine.setRandomStreamSeed (noteSlotStreamSeed (19, source));
                    EngineParams params; params.table = source; params.p[0] = .4f; params.p[1] = .4f;
                    params.p[2] = .7f; params.p[3] = spread; params.grainReverse = .4f;
                    engine.start (note, .8f); engine.prepare (params);
                    for (int i = 0; i < 4096; ++i)
                    {
                        const float sample = engine.render (params);
                        if (std::fwrite (&sample, sizeof (sample), 1, stdout) != 1) return 1;
                    }
                }
    }
}
