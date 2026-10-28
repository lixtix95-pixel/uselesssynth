// Тесты движка Resynth: общий стенд + независимость времени/высоты.
#include "engine_suite.h"
#include "../src/engines/resynth.h"

#include <cstdio>
#include <cmath>
#include <vector>

using namespace orogen;

int main()
{
    std::printf ("OROGEN — движок Resynth\n=================================================\n\n");

    SampleBank samples;
    samples.buildFactory();
    ResynthBank bank;
    bank.build (samples);
    std::printf ("анализ: %zu источников, %.0f КБ кадров\n",
                 bank.sources.size(), (double) bank.bytes() / 1024.0);

    // Пробный стационарный источник для метрики алиасинга.
    // На музыкальных сэмплах партиалы имеют атаку и затухание, а ресинтез
    // обновляет их раз в 10 мс: каждая гармоника «размазывается» на ±50 Гц,
    // и метрика «энергия вне k·f0» считает это размытие алиасингом (даёт
    // -17 дБ у чистого синтеза). На стационарном пробнике амплитуды партиалов
    // постоянны, размытия нет — остаётся только настоящий алиасинг.
    SampleBank probeSamples;
    probeSamples.shots.push_back (buildProbeShot());
    ResynthBank probeBank;
    probeBank.build (probeSamples);

    suite::Config cfg;
    cfg.name = "Resynth (общий стенд)";
    cfg.make = [&probeBank] {
        auto e = std::make_unique<ResynthEngine>();
        e->setBank (&probeBank);
        return std::unique_ptr<EngineBase> (e.release());
    };
    cfg.params = [] (int) {
        EngineParams p;
        p.table = 0;        // Fm Tine (тональный источник — годится для метрики)
        p.p[0] = 0.25f;     // stretch ~1.2x
        p.p[1] = 0.5f;      // питч 0
        p.p[2] = 0.0f;      // без наклона
        p.p[3] = 0.0f;      // без drift
        p.detune = 0.0f;
        p.level = 1.0f;
        return p;
    };
    cfg.notes = { 45, 57, 69 };
    cfg.aliasWindow = [] (int) { return 8192; };
    cfg.aliasTargetDb = -55.0;
    cfg.levelTarget = 0.20;
    cfg.levelToleranceDb = 8.0;
    cfg.maxContentSpreadDb = 12.0;
    cfg.dcTarget = 2.0e-3;
    cfg.clickTolerance = 1.4f;

    const int before = suite::gFailures;
    int fails = suite::run (cfg);

    // ------------------------------------------------ stretch не меняет тембр
    std::printf ("\n--- Stretch: длина меняется, тембр остаётся ---\n");
    {
        // Измеряем на СТАЦИОНАРНОМ пробнике: у музыкального сэмпла тембр
        // меняется внутри самой записи (атака/хвост), и сравнение «разных
        // участков источника» ничего не говорит о движке.
        double centroids[3] = {};
        double lengths[3] = {};
        int idx = 0;
        for (float st : { 0.0f, 0.5f, 1.0f })
        {
            EngineParams p;
            p.table = 0; p.p[0] = st; p.p[1] = 0.5f; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f;

            ResynthEngine e;
            e.setBank (&probeBank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            e.prepare (p);

            // длина: сколько сэмплов до конца кадров
            int n = 0;
            while (n < 48000 * 20 && e.isActive()) { (void) e.render (p); ++n; }
            lengths[idx] = n / 48000.0;

            // спектр на одной и той же позиции (160-й кадр)
            ResynthEngine e2;
            e2.setBank (&probeBank);
            e2.setSampleRate (48000.0);
            e2.start (57, 1.0f);
            e2.prepare (p);
            int guard = 0;
            while (e2.framesElapsed() < 160.0f && guard++ < 48000 * 40) (void) e2.render (p);

            const int N = 8192;
            std::vector<std::complex<float>> spec ((size_t) N);
            for (int i = 0; i < N; ++i)
            {
                const double t = (double) i / (N - 1);
                const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                               + 0.14128 * std::cos (2 * kTwoPi * t)
                               - 0.01168 * std::cos (3 * kTwoPi * t);
                spec[(size_t) i] = { (float) (e2.render (p) * w), 0.0f };
            }
            Fft::forward (spec);
            double num = 0.0, den = 0.0;
            for (int b = 1; b < N / 2; ++b)
            {
                const double m = std::abs (spec[(size_t) b]);
                num += m * b * 48000.0 / N; den += m;
            }
            centroids[idx] = (den > 1e-9) ? num / den : 0.0;
            std::printf ("     stretch %.2f: длина %.2f c, центроид %.0f Гц\n",
                         0.25 + st * 3.75, lengths[idx], centroids[idx]);
            ++idx;
        }
        suite::check (lengths[2] > lengths[0] * 3.0, "stretch удлиняет звук",
                      suite::num (lengths[0], 2) + " -> " + suite::num (lengths[2], 2) + " c");
        const double cSpread = std::fabs (centroids[2] - centroids[0]) / std::max (1.0, centroids[0]);
        suite::check (cSpread < 0.06, "stretch не меняет тембр (центроид)",
                      suite::num (centroids[0], 0) + " -> " + suite::num (centroids[2], 0) + " Гц");
    }

    // --------------------------------------- питч не меняет длительность
    std::printf ("\n--- Pitch: высота меняется, длительность остаётся ---\n");
    {
        double lengths[3] = {};
        double peaks[3] = {};
        int idx = 0;
        for (float pt : { 0.35f, 0.5f, 0.65f })
        {
            ResynthEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            EngineParams p;
            p.table = 0; p.p[0] = 0.25f; p.p[1] = pt; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);

            int n = 0;
            while (n < 48000 * 12) { (void) e.render (p); if (! e.isActive()) break; ++n; }
            lengths[idx] = n / 48000.0;
            ++idx;
        }
        std::printf ("     длина при питче -7/0/+7 полутонов: %.2f / %.2f / %.2f c\n",
                     lengths[0], lengths[1], lengths[2]);
        suite::check (std::fabs (lengths[2] - lengths[0]) < 0.05, "питч не меняет длительность");
        (void) peaks;
    }

    // --------------------------------------- партиалы ограничены Найквистом
    std::printf ("\n--- Партиалы выше Найквиста отбрасываются ---\n");
    {
        for (int note : { 69, 81, 93 })
        {
            ResynthEngine e;
            e.setBank (&probeBank);
            e.setSampleRate (48000.0);
            e.start (note, 1.0f);
            EngineParams p;
            p.table = 0; p.p[0] = 0.0f; p.p[1] = 0.5f; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);

            const double db = suite::aliasDb (e, p, note, 48000.0, 8192);
            std::printf ("     нота %2d: энергия вне гармоник %.1f дБ\n", note, db);
            suite::check (db < -40.0, "нет алиасинга на ноте " + std::to_string (note));
        }
    }

    fails = suite::gFailures - before;
    std::printf ("\n=================================================\n");
    if (fails == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else            std::printf ("ЕСТЬ ПРОВАЛЫ (провалов: %d)\n", fails);
    return fails;
}
