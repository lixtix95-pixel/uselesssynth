// Emit deterministic static Wavetable samples for comparing two revisions.
// Usage: compile twice with -I<revision>/orogen-dsp/src and compare output files.
#include "engines/wavetable.h"
#include <cstdio>

using namespace orogen;

int main (int argc, char** argv)
{
    if (argc < 2) return 2;
    FILE* file = std::fopen (argv[1], "wb");
    if (file == nullptr) return 2;
    WavetableBank bank; bank.buildFactory();
    for (int table = 0; table < 8; ++table)
        for (int note : { 36, 48, 60, 72, 84 })
            for (float position : { .1f, .6f, 1.f })
                for (int mode = 0; mode < 5; ++mode)
                {
                    EngineParams p;
                    p.table = table; p.level = 1.0f; p.detune = 0.0f;
                    p.p[0] = position; p.p[3] = .3f;
                    p.p[1] = (mode == 1 || mode == 4) ? .5f : 0.0f;
                    p.p[2] = mode == 2 ? .3f : mode == 3 ? .7f : mode == 4 ? 1.0f : 0.0f;
                    WavetableEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
                    e.start (note, 1.0f); e.prepare (p);
                    for (int i = 0; i < 1024; ++i)
                    {
                        const float value = e.render (p);
                        if (std::fwrite (&value, sizeof (value), 1, file) != 1)
                        { std::fclose (file); return 1; }
                    }
                }
    return std::fclose (file) == 0 ? 0 : 1;
}
