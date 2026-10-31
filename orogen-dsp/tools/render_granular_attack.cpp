// Диагностика исправления огибающей: первые 0.3 с атаки фабричного Choir/Vox.
// Это исходные отсчёты Granular, не мастер-выход плагина: нет Voice/FX,
// лимитера, нормировки или изменения уровня. IEEE float может содержать |x|>1;
// файл старой версии способен клиппироваться обычным проигрывателем/ЦАП.
// make build/render_granular_attack
// ./build/render_granular_attack <файл.wav> [2=Choir, 6=Vox]
#include "../src/engines/granular.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace orogen;

static bool writeWav (const char* path, const std::vector<float>& samples, uint32_t sr)
{
    FILE* f = std::fopen (path, "wb");
    if (f == nullptr) return false;
    const uint32_t bytes = (uint32_t) (samples.size() * sizeof (float));
    const auto u32 = [f] (uint32_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    const auto u16 = [f] (uint16_t n) { std::fwrite (&n, sizeof (n), 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (48u + bytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (1); u32 (sr);
    u32 (sr * 4); u16 (4); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((uint32_t) samples.size());
    std::fwrite ("data", 1, 4, f); u32 (bytes);
    const bool ok = std::fwrite (samples.data(), sizeof (float), samples.size(), f) == samples.size();
    return std::fclose (f) == 0 && ok;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: %s <file.wav> [2|6]\n", argv[0]); return 2; }
    constexpr int sr = 48000, frames = sr * 3 / 10;
    const int source = argc > 2 ? std::atoi (argv[2]) : 2;
    if (source != 2 && source != 6) { std::fprintf (stderr, "source must be 2 or 6\n"); return 2; }
    SampleBank bank; bank.buildFactory (sr);
    GranularEngine e; e.setBank (&bank); e.setSampleRate (sr); e.start (45, 1.0f);
    EngineParams p; p.table = source; p.p[0] = 0.0f; p.p[1] = 0.2f;
    p.p[2] = 0.0f; p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f; e.prepare (p);
    std::vector<float> out; out.reserve (frames);
    int bad = 0;
    for (int i = 0; i < frames; ++i)
    {
        const float x = e.render (p);
        if (! std::isfinite (x)) ++bad;
        out.push_back (x);
    }
    // Обрезаем на последнем переходе через ноль, не меняя самих отсчётов.
    for (int i = frames - 1; i > frames - 1000; --i)
        if ((out[(size_t) i] >= 0.0f) != (out[(size_t) (i - 1)] >= 0.0f))
        { out.resize ((size_t) i); break; }
    double sumSq = 0.0; float peak = 0.0f; int over = 0;
    for (float x : out)
    {
        sumSq += (double) x * x; peak = std::max (peak, std::fabs (x));
        if (std::fabs (x) > 1.0f) ++over;
    }
    if (bad || ! writeWav (argv[1], out, sr)) return 1;
    std::printf ("%s: %s, %.6f с, 48 kHz mono float32, чистый Granular\n",
                 argv[1], bank.shots[(size_t) source].name.c_str(), (double) out.size() / sr);
    std::printf ("peak %.6f, RMS %.6f, |x|>1 %d/%zu, NaN/Inf %d\n",
                 peak, std::sqrt (sumSq / out.size()), over, out.size(), bad);
    return 0;
}
