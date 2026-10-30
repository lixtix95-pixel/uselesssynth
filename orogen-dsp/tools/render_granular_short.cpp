// Granular: граница 10-мс огибающей, один отсчёт разницы в источнике.
// Диагностический mono float32 WAV без FX, лимитера или нормировки файла.
#include "../src/engines/granular.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>
using namespace orogen;

static void put16 (std::ofstream& out, uint16_t n)
{
    const char b[] = { (char) (n & 255), (char) ((n >> 8) & 255) };
    out.write (b, 2);
}
static void put32 (std::ofstream& out, uint32_t n)
{
    const char b[] = { (char) (n & 255), (char) ((n >> 8) & 255),
                       (char) ((n >> 16) & 255), (char) ((n >> 24) & 255) };
    out.write (b, 4);
}

int main (int argc, char** argv)
{
    const char* filename = argc > 1 ? argv[1] : "granular_short.wav";
    constexpr int sr = 48000, step = 480, interval = 12000, pulses = 16;
    SampleBank bank;
    for (int length : { step - 1, step })
    {
        OneShot s; s.name = "short sine"; s.rootNote = 45;
        s.levels.resize (1); s.levels[0].resize ((size_t) length);
        for (int i = 0; i < length; ++i)
            s.levels[0][(size_t) i] = (float) (0.3 * std::sin (kTwoPi * 1000.0 * i / sr));
        buildMipLevels (s, sr);
        bank.shots.push_back (std::move (s));
    }

    GranularEngine e; e.setBank (&bank); e.setSampleRate (sr);
    EngineParams p; p.p[0] = 0.35f; p.p[1] = 1.0f;
    p.p[2] = 0.0f; p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;
    std::vector<float> audio ((size_t) interval * pulses);
    double energies[2] = {}, totalEnergy = 0.0;
    float peak = 0.0f;
    int bad = 0;
    for (int pulse = 0; pulse < pulses; ++pulse)
    {
        p.table = pulse % 2;
        e.start (45, 1.0f); e.prepare (p);
        for (int i = 0; i < interval; ++i)
        {
            const float x = e.render (p);
            if (! std::isfinite (x)) ++bad;
            audio[(size_t) pulse * interval + i] = x;
            peak = std::max (peak, std::fabs (x));
            totalEnergy += (double) x * x;
            if (i < step) energies[pulse % 2] += (double) x * x;
        }
    }
    const double rmsA = std::sqrt (energies[0] / (step * (pulses / 2)));
    const double rmsB = std::sqrt (energies[1] / (step * (pulses / 2)));
    const double diffDb = 20.0 * std::log10 (std::max (1.0e-20, rmsB) / std::max (1.0e-20, rmsA));
    std::printf ("%s: 4 s, 48 kHz mono float32; no FX/limiter/file normalisation\n", filename);
    std::printf ("alternating source lengths 479/480 samples, 1 kHz sine, amp 0.3\n");
    std::printf ("first 10-ms RMS %.8f/%.8f, difference %+.5f dB; overall RMS %.8f, peak %.8f, NaN/Inf %d\n",
                 rmsA, rmsB, diffDb, std::sqrt (totalEnergy / audio.size()), peak, bad);
    if (bad) return 1;
    std::ofstream out (filename, std::ios::binary);
    if (! out) { std::perror (filename); return 1; }
    const uint32_t dataSize = (uint32_t) (audio.size() * sizeof (float));
    out.write ("RIFF", 4); put32 (out, 36 + dataSize); out.write ("WAVEfmt ", 8);
    put32 (out, 16); put16 (out, 3); put16 (out, 1); put32 (out, sr);
    put32 (out, sr * sizeof (float)); put16 (out, sizeof (float)); put16 (out, 32);
    out.write ("data", 4); put32 (out, dataSize);
    out.write (reinterpret_cast<const char*> (audio.data()), dataSize);
    if (! out) { std::fprintf (stderr, "WAV write failed\n"); return 1; }
    return 0;
}
