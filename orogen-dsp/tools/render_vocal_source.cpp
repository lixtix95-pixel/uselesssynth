// Фабричный вокальный источник ДО любых движков/FX, для сравнения высоты.
// Сам генератор сохраняет штатную калибровку банка; после рендера нет
// нормировки, лимитера и коррекции усиления. Для исключения щелчка конца файла
// последняя часть только отрезается по переходу через ноль (сэмплы не меняются).
// make build/render_vocal_source
// ./build/render_vocal_source output.wav 2     # 2=Choir, 6=Vox
#include "../src/content/sample_bank.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace orogen;

int main (int argc, char** argv)
{
    if (argc != 3) { std::fprintf (stderr, "usage: %s output.wav <2=Choir|6=Vox>\n", argv[0]); return 2; }
    const int kind = std::atoi (argv[2]);
    if (kind != 2 && kind != 6) return 2;
    constexpr int sr = 48000;
    std::vector<float> x;
    renderOneShot ((OneShotKind) kind, x, sr, 2.0);
    for (int i = (int) x.size() - 1; i > (int) x.size() - 1000; --i)
        if ((x[(size_t) i] >= 0.0f) != (x[(size_t) (i - 1)] >= 0.0f))
        { x.resize ((size_t) i); break; }
    FILE* f = std::fopen (argv[1], "wb");
    if (f == nullptr) return 3;
    const uint32_t bytes = (uint32_t) (x.size() * sizeof (float));
    const auto u32 = [f] (uint32_t n) { std::fwrite (&n, 4, 1, f); };
    const auto u16 = [f] (uint16_t n) { std::fwrite (&n, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (48 + bytes);
    std::fwrite ("WAVEfmt ", 1, 8, f); u32 (16); u16 (3); u16 (1);
    u32 (sr); u32 (sr * 4); u16 (4); u16 (32);
    std::fwrite ("fact", 1, 4, f); u32 (4); u32 ((uint32_t) x.size());
    std::fwrite ("data", 1, 4, f); u32 (bytes);
    const bool ok = std::fwrite (x.data(), sizeof (float), x.size(), f) == x.size();
    const bool closed = std::fclose (f) == 0;
    double energy = 0.0; float peak = 0.0f; int bad = 0;
    for (float v : x)
    {
        if (! std::isfinite (v)) ++bad;
        peak = std::max (peak, std::fabs (v)); energy += (double) v * v;
    }
    std::printf ("%s source %d: %.6f s, peak %.6f, RMS %.6f, NaN/Inf %d\n",
                 argv[1], kind, (double) x.size() / sr, peak, std::sqrt (energy / x.size()), bad);
    return ok && closed && ! bad ? 0 : 3;
}
