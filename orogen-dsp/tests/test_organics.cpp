// Тесты движка Organics: общий стенд + специфика слоёв.
#include "engine_suite.h"
#include "../src/engines/organics.h"

#include <cstdio>
#include <cmath>
#include <vector>

using namespace orogen;

// Алиасинг для Organics: слой воздуха — шум, но он ограничен ФНЧ (6 кГц),
// а источник — сэмпл с полосой до 4 кГц при питч-рейте 1. Значит всё выше
// 12 кГц (с учётом питч-рейта) — ошибка движка: образы интерполятора,
// завёрнутые партиалы или шум без ограничения полосы.
static double organicsAliasDb (EngineBase& eng, const EngineParams& p, int note, double sr)
{
    const double rate = midiToHz (note) / kSampleF0;
    const double limit = std::min (0.92 * sr / 2.0, 12000.0 * std::max (1.0, rate));
    const int N = 8192;

    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);
    for (int i = 0; i < 8000; ++i) (void) eng.render (p);

    const float hpCoef = (float) std::exp (-kTwoPi * 40.0 / sr);
    float hx1 = 0, hy1 = 0, hx2 = 0, hy2 = 0;
    std::vector<std::complex<float>> spec ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        float s = eng.render (p);
        float y = hpCoef * (hy1 + s - hx1); hx1 = s; hy1 = y; s = y;
        y = hpCoef * (hy2 + s - hx2); hx2 = s; hy2 = y; s = y;
        const double t = (double) i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                       + 0.14128 * std::cos (2 * kTwoPi * t)
                       - 0.01168 * std::cos (3 * kTwoPi * t);
        spec[(size_t) i] = { (float) (s * w), 0.0f };
    }
    Fft::forward (spec);

    const double binHz = sr / N;
    double total = 0.0, above = 0.0;
    for (int b = 1; b < N / 2; ++b)
    {
        const double hz = b * binHz;
        if (hz < 30.0) continue;
        const double m2 = std::norm (spec[(size_t) b]);
        total += m2;
        if (hz > limit) above += m2;
    }
    return 10.0 * std::log10 (above / std::max (1.0e-30, total));
}

int main()
{
    std::printf ("OROGEN — движок Organics\n=================================================\n\n");

    SampleBank bank;
    bank.buildFactory();
    std::printf ("контент: %zu источников\n", bank.shots.size());

    suite::Config cfg;
    cfg.name = "Organics (общий стенд)";
    cfg.make = [&bank] {
        auto e = std::make_unique<OrganicsEngine>();
        e->setBank (&bank);
        return std::unique_ptr<EngineBase> (e.release());
    };
    cfg.params = [] (int) {
        EngineParams p;
        p.table = 2;        // Choir Ah
        p.p[0] = 0.45f;     // тело
        p.p[1] = 0.30f;     // воздух
        p.p[2] = 0.25f;     // дыхание
        p.p[3] = 0.20f;     // bloom
        p.detune = 0.0f;
        p.level = 1.0f;
        return p;
    };
    cfg.notes = { 45, 57, 69, 81 };
    cfg.aliasMeasure = organicsAliasDb;
    cfg.aliasTargetDb = -55.0;
    cfg.levelTarget = 0.20;
    cfg.levelToleranceDb = 8.0;
    cfg.maxContentSpreadDb = 30.0;   // удар (Wood Knock) и пэд (Tape Pad) дают разный RMS по природе — см. отчёт
    cfg.dcTarget = 2.0e-3;
    cfg.clickTolerance = 1.5f;

    const int before = suite::gFailures;
    int fails = suite::run (cfg);

    // -------------------------------------------------- вклад слоёв
    std::printf ("\n--- Вклад слоёв: тело и воздух слышны ---\n");
    {
        double spectra[3][3] = {};        // [вариант][полоса: 0=низ,1=середина,2=верх]
        int idx = 0;
        const char* names[3] = { "только источник", "тело 0.9", "воздух 0.9" };
        for (int variant = 0; variant < 3; ++variant)
        {
            OrganicsEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            EngineParams p;
            p.table = 2;
            p.p[0] = (variant == 1) ? 0.9f : 0.0f;
            p.p[1] = (variant == 2) ? 0.9f : 0.0f;
            p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);
            for (int i = 0; i < 12000; ++i) (void) e.render (p);

            const int N = 8192;
            std::vector<std::complex<float>> spec ((size_t) N);
            for (int i = 0; i < N; ++i)
            {
                const double t = (double) i / (N - 1);
                const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                               + 0.14128 * std::cos (2 * kTwoPi * t)
                               - 0.01168 * std::cos (3 * kTwoPi * t);
                spec[(size_t) i] = { (float) (e.render (p) * w), 0.0f };
            }
            Fft::forward (spec);
            double band[3] = { 0.0, 0.0, 0.0 };
            for (int b = 1; b < N / 2; ++b)
            {
                const double hz = b * 48000.0 / N;
                const double m2 = std::norm (spec[(size_t) b]);
                if (hz < 500) band[0] += m2;
                else if (hz < 3000) band[1] += m2;
                else band[2] += m2;
            }
            for (int k = 0; k < 3; ++k) spectra[idx][k] = 10.0 * std::log10 (band[k] + 1e-30);
            std::printf ("     %-16s низ %.1f дБ, середина %.1f дБ, верх %.1f дБ\n",
                         names[variant], spectra[idx][0], spectra[idx][1], spectra[idx][2]);
            ++idx;
        }
        suite::check (spectra[1][1] > spectra[0][1] + 1.0, "тело усиливает середину",
                      suite::num (spectra[1][1] - spectra[0][1], 1) + " дБ");
        suite::check (spectra[2][2] > spectra[0][2] + 1.0, "воздух добавляет верх",
                      suite::num (spectra[2][2] - spectra[0][2], 1) + " дБ");
    }

    // --------------------------------------------- bloom: плавная атака
    std::printf ("\n--- Bloom: атака растягивается ---\n");
    {
        for (float bl : { 0.0f, 0.8f })
        {
            OrganicsEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            EngineParams p;
            p.table = 2; p.p[0] = 0.3f; p.p[1] = 0.1f; p.p[2] = 0.0f; p.p[3] = bl;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);

            // время до 90 % от максимума огибающей
            double peak = 0.0, t90 = 0.0;
            std::vector<double> env;
            for (int w = 0; w < 100; ++w)
            {
                double acc = 0.0;
                for (int i = 0; i < 120; ++i) { const float s = e.render (p); acc += (double) s * s; }
                env.push_back (std::sqrt (acc / 120.0));
                peak = std::max (peak, env.back());
            }
            for (size_t w = 0; w < env.size(); ++w)
                if (env[w] > peak * 0.9) { t90 = (double) w * 120.0 / 48000.0; break; }
            std::printf ("     bloom %.1f: время до 90 %% амплитуды %.1f мс\n", bl, t90 * 1000.0);
            if (bl > 0.5f) suite::check (t90 > 0.08, "bloom замедляет атаку", suite::num (t90 * 1000.0, 1) + " мс");
        }
    }

    // ---------------------------------- устойчивая часть не «пульсирует»
    //
    // Петля по затухающей записи раньше возвращала уровень скачком: у Wood
    // Knock и Fm Tine это давало 100–340 % разброса RMS в устойчивой части
    // (слышно как «пулемёт» и как дыхание не в такт). Теперь петля идёт по
    // всему файлу с равномощным кроссфейдом и выровнена по периоду.
    std::printf ("\n--- Устойчивая часть ровная (нет пульсации петли) ---\n");
    {
        for (int table : { 2, 5 })
        {
            OrganicsEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (60, 1.0f);
            EngineParams p;
            p.table = table; p.p[0] = 0.6f; p.p[1] = 0.45f; p.p[2] = 0.1f; p.p[3] = 0.2f;
            p.detune = 0.0f; p.level = 0.8f;
            e.prepare (p);

            const int hop = 480, n = 48000 * 3;
            std::vector<double> env;
            for (int k = 0; k + hop <= n; k += hop)
            {
                double acc = 0.0;
                for (int j = 0; j < hop; ++j) { const float v = e.render (p); acc += (double) v * v; }
                env.push_back (std::sqrt (acc / hop));
            }
            // вторая половина записи: атака и первая петля не в счёт
            double mean = 0.0, var = 0.0;
            const size_t from = env.size() / 3;
            for (size_t i = from; i < env.size(); ++i) mean += env[i];
            mean /= (double) (env.size() - from);
            for (size_t i = from; i < env.size(); ++i) var += (env[i] - mean) * (env[i] - mean);
            var /= (double) (env.size() - from);
            const double ripple = mean > 0.0 ? std::sqrt (var) / mean : 0.0;
            std::printf ("   %-16s RMS %.4f, разброс в устойчивой части %.1f %%\n",
                         bank.shots[(size_t) table].name.c_str(), mean, ripple * 100.0);
            suite::check (ripple < 0.65, "петля не пульсирует", suite::num (ripple * 100.0, 1) + " %");
        }
    }

    fails = suite::gFailures - before;
    std::printf ("\n=================================================\n");
    if (fails == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else            std::printf ("ЕСТЬ ПРОВАЛЫ (провалов: %d)\n", fails);
    return fails;
}
