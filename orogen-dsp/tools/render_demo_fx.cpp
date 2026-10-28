// Демо новых эффектов (перенесены из MonoJuice 0.9.5): 32 секунды, 8 секций.
//   audio/fx_demo.wav
//   1. Drive: 8 моделей подряд на одной ноте (смена каждые 0.5 с)
//   2. Drive: 4 остальные модели + Wavefolder/Decimator
//   3. Material: 4 профиля (latex / spring / metal / glass)
//   4. Spectre: окна 1024 → 16384, сдвиг по частоте
//   5. Glitch: статтер на барабане и на аккорде
//   6. Всё вместе: drive → material → spectre → glitch
#include "../src/instrument/fx.h"
#include "../src/instrument/fx_drive.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

using namespace orogen;

static void writeWav16 (const std::string& path, const std::vector<float>& l,
                        const std::vector<float>& r, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return;
    const uint32_t n = (uint32_t) l.size(), db = n * 4;
    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (36 + db); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (1); u16 (2); u32 ((uint32_t) sr);
    u32 ((uint32_t) sr * 4); u16 (4); u16 (16);
    std::fwrite ("data", 1, 4, f); u32 (db);
    for (uint32_t i = 0; i < n; ++i)
    {
        auto q = [] (float x) { return (int16_t) std::lrint (clamp (x, -1.0f, 1.0f) * 32767.0f); };
        const int16_t a = q (l[i]), b = q (r[i]);
        std::fwrite (&a, 2, 1, f); std::fwrite (&b, 2, 1, f);
    }
    std::fclose (f);
}

int main()
{
    const double sr = 48000.0;
    const double secs = 34.0;
    const int n = (int) (sr * secs);
    std::vector<float> outL ((size_t) n, 0.0f), outR ((size_t) n, 0.0f);

    // простая «нота»: пила + синус, чтобы драйву было что красить
    auto voice = [] (double t, double hz)
    {
        const double ph = hz * t;
        const double saw = 2.0 * (ph - std::floor (ph)) - 1.0;
        return (float) (0.45 * saw + 0.35 * std::sin (kTwoPi * ph));
    };

    // --- 1-2. Drive: модели по очереди, по 0.5 с
    {
        const DriveModel models[] = {
            DriveModel::SoftClip, DriveModel::HardClip, DriveModel::Tanh, DriveModel::Tube,
            DriveModel::Tape, DriveModel::Transformer, DriveModel::Fuzz, DriveModel::Diode,
            DriveModel::Fold, DriveModel::Sine, DriveModel::BitCrush, DriveModel::Rectifier,
            DriveModel::PurestDrive, DriveModel::Density, DriveModel::AirTape, DriveModel::AirTube,
            DriveModel::Wavefolder, DriveModel::Decimator, DriveModel::DiodeWdf
        };
        DriveFx d; d.prepare (sr); d.setMix (1.0f); d.setAmount (0.6f);
        const int seg = (int) (sr * 0.5);
        int idx = 0;
        DriveModel cur = models[0];
        d.setModel (cur);
        for (int i = 0; i < seg * 19 && i < n; ++i)
        {
            const int m = i / seg;
            if (m != idx) { idx = m; cur = models[m]; d.setModel (cur); }
            float l = voice (i / sr, 220.0), r = l;
            d.process (l, r);
            outL[(size_t) i] = l * 0.55f;
            outR[(size_t) i] = r * 0.55f;
        }
        std::printf ("  1-2. Drive: 19 моделей, по 0.5 с\n");
    }

    // --- 3. Material: четыре профиля по 1.5 с
    {
        const int seg = (int) (sr * 1.5);
        const int start = (int) (sr * 9.5);
        for (int mode = 0; mode < 4; ++mode)
        {
            MaterialFx m; m.prepare (sr);
            m.setMode (mode); m.setMix (1.0f); m.setExcite (0.6f);
            m.setTone (0.5f); m.setDamp (0.5f); m.setSize (0.55f); m.setFeedback (0.55f);
            for (int i = 0; i < seg; ++i)
            {
                const int t = start + mode * seg + i;
                if (t >= n) break;
                // короткий щипок в начале сегмента
                float l = (i < 240) ? 0.85f * (float) std::sin (kTwoPi * 330.0 * i / sr) * (1.0f - i / 240.0f) : 0.0f;
                float r = l;
                m.process (l, r);
                outL[(size_t) t] += l * 0.6f;
                outR[(size_t) t] += r * 0.6f;
            }
        }
        std::printf ("  3. Material: 4 профиля\n");
    }

    // --- 4. Spectre: окна 1024 / 4096 / 16384 и сдвиг
    {
        const int start = (int) (sr * 15.5);
        const int seg = (int) (sr * 1.4);
        const int windows[3] = { 1, 3, 5 };
        for (int k = 0; k < 3; ++k)
        {
            SpectreFx sp; sp.prepare (sr);
            sp.setWindow (windows[k]);
            sp.setMix (1.0f);
            sp.setProminence (4.0f);
            sp.setShift (k == 2 ? 7.0f : 0.0f);      // в третьем сегменте — сдвиг на квинту
            for (int i = 0; i < seg; ++i)
            {
                const int t = start + k * seg + i;
                if (t >= n) break;
                float l = voice (i / sr, 440.0), r = l;
                sp.process (l, r);
                outL[(size_t) t] += l * 0.5f;
                outR[(size_t) t] += r * 0.5f;
            }
        }
        std::printf ("  4. Spectre: окна 1024/4096/16384 + сдвиг\n");
    }

    // --- 5. Glitch: статтер
    {
        const int start = (int) (sr * 20.0);
        const int seg = (int) (sr * 3.0);
        GlitchFx g; g.prepare (sr);
        g.setChance (0.55f); g.setLengthMs (85.0f); g.setRepeats (3.0f);
        g.setReverse (0.25f); g.setMix (1.0f);
        for (int i = 0; i < seg; ++i)
        {
            const int t = start + i;
            if (t >= n) break;
            // пульсирующий аккорд, чтобы статтеру было что захватывать
            const bool on = (i % 12000) < 7000;
            float l = on ? (voice (i / sr, 261.63) + voice (i / sr, 392.0)) * 0.6f : 0.0f;
            float r = l;
            g.process (l, r);
            outL[(size_t) t] += l * 0.55f;
            outR[(size_t) t] += r * 0.55f;
        }
        std::printf ("  5. Glitch: статтер на аккорде\n");
    }

    // --- 6. Всё вместе
    {
        const int start = (int) (sr * 23.5);
        FxChain fx; fx.prepare (sr);
        FxChain::Settings s;
        s.drive = 0.45f; s.driveModel = (int) DriveModel::Tube;
        s.materialMix = 0.45f; s.materialMode = 2;
        s.spectreMix = 0.35f; s.spectreWindow = 3; s.spectreProminence = 6.0f;
        s.glitchMix = 0.25f; s.glitchChance = 0.4f;
        s.chorusMix = 0.2f; s.delayMix = 0.18f; s.plateMix = 0.35f;
        fx.set (s);
        for (int i = 0; i < (int) (sr * 10.0); ++i)
        {
            const int t = start + i;
            if (t >= n) break;
            const bool on = (i % 16000) < 11000;
            float l = on ? voice (i / sr, 196.0) * 0.8f : 0.0f;
            float r = on ? voice (i / sr, 196.0 * 1.005) * 0.8f : 0.0f;
            fx.process (l, r);
            outL[(size_t) t] += l * 0.7f;
            outR[(size_t) t] += r * 0.7f;
        }
        std::printf ("  6. Всё вместе: drive + material + spectre + glitch + chorus + delay + plate\n");
    }

    // нормализация и запись
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max (peak, std::max (std::fabs (outL[(size_t) i]), std::fabs (outR[(size_t) i])));
    const float g = (peak > 1.0e-6f) ? 0.89f / peak : 1.0f;
    for (int i = 0; i < n; ++i) { outL[(size_t) i] *= g; outR[(size_t) i] *= g; }

    writeWav16 ("audio/fx_demo.wav", outL, outR, (int) sr);

    double acc = 0.0; float pk = 0.0f;
    for (int i = 0; i < n; ++i) { acc += (double) outL[(size_t) i] * outL[(size_t) i]; pk = std::max (pk, std::fabs (outL[(size_t) i])); }
    std::printf ("  записано audio/fx_demo.wav: %.0f с, пик %.3f, RMS %.4f (до нормировки пик %.3f)\n",
                 secs, pk, std::sqrt (acc / n), peak);
    for (int k = 0; k < 8; ++k)
    {
        const int a = (int) (sr * 4.0 * k), b = std::min (n, a + (int) (sr * 4.0));
        double e = 0.0;
        for (int i = a; i < b; ++i) e += (double) outL[(size_t) i] * outL[(size_t) i];
        std::printf ("   %2d–%2d c: RMS %.4f\n", 4 * k, 4 * (k + 1), std::sqrt (e / std::max (1, b - a)));
    }
    return 0;
}
