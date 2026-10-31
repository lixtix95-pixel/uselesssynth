// Deterministic legacy FM output for an exact comparison with the old engine.
// Compile with -DOROGEN_LEGACY_FM_HEADER=... to substitute a saved old fm.h.
#ifdef OROGEN_LEGACY_FM_HEADER
#include OROGEN_LEGACY_FM_HEADER
#else
#include "../src/engines/fm.h"
#endif
#include <cstdio>
#include <cmath>
using namespace orogen;
int main()
{
    for (int sr : {22050, 44100, 48000, 96000})
        for (int note : {36, 48, 60, 72, 84})
            for (int scenario=0;scenario<30;++scenario)
            {
                FmEngine f; EngineParams p;
                p.detune=(scenario%3)*0.3f;
                p.level=.2f+(scenario%5)*.18f;
                p.p[0]=(scenario%7)/6.0f;
                p.p[1]=(scenario%9)/8.0f;
                p.p[2]=(scenario%6)/5.0f;
                p.p[3]=(scenario%4)/3.0f;
                f.setSampleRate (sr); f.start(note,.8f); f.prepare(p);
                for (int i=0;i<1024;++i)
                {
                    if(i==300) {p.p[0]=1.0f-p.p[0]; p.p[1]=.7f; p.p[2]=.4f; f.prepare(p);}
                    if(i==700) {p.p[3]=.1f; f.prepare(p);}
                    const float v=f.render(p);
                    std::fwrite(&v,sizeof v,1,stdout);
                }
            }
}
