// Тесты движка Harmonic: общий стенд + спецтесты (рекуррентность, число партиалов, оба пути).
#include "../src/engines/harmonic.h"
#include "engine_suite.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <memory>

using namespace orogen;

// Считаем, сколько партиалов реально выше -60 дБ, и сравниваем с ожидаемым
// числом (ограничение по Найквисту).
static int countPartials (EngineParams p, int note)
{
    const double sr = 48000.0, f0 = midiToHz (note);
    HarmonicEngine eng;
    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);

    constexpr int N = 16384;
    std::vector<std::complex<float>> spec ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const double t = (double) i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                       + 0.14128 * std::cos (2 * kTwoPi * t)
                       - 0.01168 * std::cos (3 * kTwoPi * t);
        spec[(size_t) i] = { (float) (eng.render (p) * w), 0.0f };
    }
    Fft::forward (spec);
    double peak = 0.0;
    for (int b = 1; b < N / 2; ++b) peak = std::max (peak, (double) std::abs (spec[(size_t) b]));
    const double binHz = sr / N;
    int cnt = 0;
    for (int k = 1; k <= 200; ++k)
    {
        const int centre = (int) std::lround (k * f0 / binHz);
        if (centre + 5 >= N / 2) break;
        double lm = 0.0;
        for (int b = std::max (1, centre - 5); b <= centre + 5; ++b)
            lm = std::max (lm, (double) std::abs (spec[(size_t) b]));
        if (lm >= peak * 0.001) ++cnt;                        // -60 дБ
    }
    return cnt;
}

// доля энергии выше 1 кГц (нота 57, tilt 0.8): у правильного медленного пути
// это единицы-десятки дБ, у сломанного (все партиалы на f0) — минус 70 и ниже
static double highBandDb()
{
    EngineParams p; p.p[0] = 0.8f; p.p[1] = 1.0f; p.p[2] = 0.45f; p.p[3] = 0.0f; p.level = 1.0f;
    HarmonicEngine h; h.setSampleRate (48000.0); h.start (57, 1.0f); h.prepare (p);
    const int N = 16384;
    std::vector<double> x ((size_t) N);
    for (int i = 0; i < 4000; ++i) h.render (p);
    for (int i = 0; i < N; ++i) x[(size_t) i] = h.render (p) * (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (N - 1)));
    double lo = 0.0, hiB = 0.0;
    for (double f = 60.0; f < 20000.0; f += 23.4)
    {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < N; ++n) { const double a = 2.0 * M_PI * f * n / 48000.0; re += x[(size_t) n] * std::cos (a); im -= x[(size_t) n] * std::sin (a); }
        const double e = re * re + im * im;
        if (f < 1000.0) lo += e; else hiB += e;
    }
    return 10.0 * std::log10 ((hiB + 1e-30) / (lo + 1e-30));
}

int main()
{
    std::printf ("OROGEN — движок Harmonic\n=================================================\n");

    suite::Config cfg;
    cfg.name = "Harmonic (общий стенд)";
    cfg.make = [] { return std::unique_ptr<EngineBase> (new HarmonicEngine()); };
    cfg.params = [] (int) {
        EngineParams p;
        p.p[0] = 0.62f;   // наклон: ярко
        p.p[1] = 0.45f;   // чётные партиалы
        p.p[2] = 0.00f;   // без растяжки -> быстрый путь
        p.p[3] = 0.00f;   // Motion = AM-сайдбенды -> в замере алиасинга это шум, не баг
        p.detune = 0.3f;
        p.level = 1.0f;
        return p;
    };
    cfg.aliasTargetDb = -70.0;      // аддитив обязан быть чище всех
    suite::run (cfg);

    std::printf ("\n--- Ограничение партиалов по Найквисту ---\n");
    std::printf ("     нота   f0, Гц   предсказано   измерено\n");
    bool limitOk = true;
    for (int note : { 24, 36, 48, 60, 72, 84, 96 })
    {
        EngineParams p;
        p.p[0] = 0.95f;          // максимально ярко: партиалы должны быть слышны
        p.p[1] = 1.0f;           // чётные тоже
        p.p[2] = 0.0f; p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;
        const double f0 = midiToHz (note);
        const int predicted = std::min (64, (int) std::floor (0.45 * 48000.0 / f0));
        const int measured = countPartials (p, note);
        std::printf ("      %3d  %7.1f      %3d          %3d\n", note, f0, predicted, measured);
        if (measured > predicted + 4) limitOk = false;
    }
    suite::check (limitOk, "партиалов не больше, чем влезает ниже Найквиста");

    std::printf ("\n--- Быстрый (Чебышёв) vs медленный (растяжка) путь ---\n");
    for (float spread : { 0.0f, 0.5f, 1.0f })
    {
        EngineParams p;
        p.p[0] = 0.6f; p.p[1] = 0.5f; p.p[2] = spread; p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;

        // для быстрого пути алиасинг измерим (спектр строго гармонический),
        // для медленного — нет: растянутые партиалы ингармоничны ПО ЗАМЫСЛУ,
        // как у настоящей фортепианной струны (f_k = k·f0·sqrt(1+B·k²)).
        HarmonicEngine h;
        h.setSampleRate (48000.0); h.start (57, 1.0f); h.prepare (p);
        double acc = 0.0; float peak = 0.0f;
        for (int i = 0; i < 24000; ++i) { const float s = h.render (p); acc += (double) s * s; peak = std::max (peak, std::fabs (s)); }
        const double rms = std::sqrt (acc / 24000.0);

        if (spread < 0.02f)
        {
            const double db = suite::aliasDb (*[] { return std::unique_ptr<EngineBase> (new HarmonicEngine()); }(),
                                              p, 57, 48000.0);
            std::printf ("   spread %.1f -> быстрый  , алиасинг %6.1f дБ, rms %.3f, crest %.1f\n",
                         spread, db, rms, peak / std::max (1e-9, rms));
            suite::check (db < -60.0, "быстрый путь (Чебышёв) чистый");
        }
        else
        {
            // Регрессия: однажды частота партиала считалась без номера k,
            // и все 64 партиала играли на f0 — спектр целиком уходил ниже 1 кГц,
            // при этом rms и crest выглядели нормально. Поэтому проверяем
            // долю энергии выше 1 кГц отдельно.
            const double hf = highBandDb ();
            std::printf ("   spread %.1f -> медленный, rms %.3f, crest %.1f, энергия >1 кГц %6.1f дБ\n",
                         spread, rms, peak / std::max (1e-9, rms), hf);
            suite::check (hf > -20.0, "медленный путь действительно даёт спектр выше f0", suite::num (hf, 1) + " дБ");
            suite::check (rms > 0.05 && peak < 1.2f, "медленный путь стабилен и без перегруза",
                          "crest " + suite::num (peak / std::max (1e-9, rms), 1));
        }
    }

    std::printf ("\n--- Стабильность рекуррентности на длинном рендере ---\n");
    {
        EngineParams p;
        p.p[0] = 1.0f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f; p.detune = 0.0f;
        HarmonicEngine h; h.setSampleRate (48000.0); h.start (48, 1.0f); h.prepare (p);
        float peak = 0.0f; bool bad = false;
        for (int i = 0; i < 48000 * 10; ++i)         // 10 секунд, 64 партиала
        {
            const float s = h.render (p);
            if (! std::isfinite (s)) bad = true;
            peak = std::max (peak, std::fabs (s));
        }
        suite::check (! bad && peak < 2.0f, "нет расходимости за 10 с (64 партиала)",
                      "peak " + suite::num (peak, 2));
    }

    std::printf ("\n=================================================\n%s (провалов: %d)\n",
                 suite::gFailures == 0 ? "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" : "ЕСТЬ ПРОВАЛЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
