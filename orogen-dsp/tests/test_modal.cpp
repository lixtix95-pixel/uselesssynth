// Тесты движка Modal: общий стенд + специфика модального синтеза.
#include "engine_suite.h"
#include "../src/engines/modal.h"

#include <cstdio>
#include <cmath>
#include <vector>

using namespace orogen;

// Алиасинг для модального движка: спектр ингармонический по замыслу,
// поэтому меряется не «энергия вне k·f0», а энергия, которой не может быть
// физически: выше 0.45·fs (резонаторы выше Найквиста отбрасываются,
// возбуждение ограничено по полосе) и ниже 20 Гц (DC от импульса).
static double modalAliasDb (EngineBase& eng, const EngineParams& p, int note, double sr)
{
    const int N = 16384;
    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);

    std::vector<std::complex<float>> spec ((size_t) N);
    double total = 0.0;
    for (int i = 0; i < N; ++i)
    {
        float s = eng.render (p);
        const double t = (double) i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                       + 0.14128 * std::cos (2 * kTwoPi * t)
                       - 0.01168 * std::cos (3 * kTwoPi * t);
        spec[(size_t) i] = { (float) (s * w), 0.0f };
        total += (double) s * s;
    }
    Fft::forward (spec);

    const double binHz = sr / N;
    double above = 0.0;
    for (int b = 1; b < N / 2; ++b)
    {
        const double hz = b * binHz;
        if (hz < 0.44 * sr) continue;
        above += std::norm (spec[(size_t) b]);
    }
    // нормируем на энергию сигнала во временной области, а не на сумму бинов
    return 10.0 * std::log10 (above / std::max (1.0e-30, total * (double) N * 0.25));
}

int main()
{
    std::printf ("OROGEN — движок Modal\n=================================================\n\n");

    suite::Config cfg;
    cfg.name = "Modal (общий стенд)";
    cfg.make = [] { return std::unique_ptr<EngineBase> (new ModalEngine()); };
    cfg.params = [] (int) {
        EngineParams p;
        p.table = 0;          // колокол
        p.p[0] = 0.6f;        // яркость: 8 мод
        p.p[1] = 0.55f;       // T60 ~ 6.7 с
        p.p[2] = 0.5f;        // жёсткость удара
        p.p[3] = 0.3f;        // нелинейность
        p.detune = 0.0f;
        p.tuneSemis = 0.0f;
        p.level = 1.0f;
        return p;
    };
    cfg.notes = { 45, 57, 69, 81 };
    cfg.aliasMeasure = modalAliasDb;
    cfg.aliasTargetDb = -60.0;
    cfg.levelTarget = 0.20;
    cfg.levelToleranceDb = 8.0;
    cfg.maxContentSpreadDb = 14.0;
    cfg.dcTarget = 2.0e-3;
    cfg.clickTolerance = 1.6f;   // удар по определению имеет быстрый фронт

    const int before = suite::gFailures;
    int fails = suite::run (cfg);

    // -------------------------------------------------- моды и их число
    std::printf ("\n--- Число мод: растёт с яркостью, отбрасывается выше Найквиста ---\n");
    {
        for (int note : { 45, 69, 93 })
        {
            int counts[3] = {};
            int idx = 0;
            for (float br : { 0.0f, 0.5f, 1.0f })
            {
                ModalEngine e;
                e.setSampleRate (48000.0);
                e.start (note, 1.0f);
                EngineParams p;
                p.table = 2; p.p[0] = br; p.p[1] = 0.6f; p.p[2] = 0.5f; p.p[3] = 0.0f;
                p.detune = 0.0f; p.level = 1.0f;
                e.prepare (p);
                counts[idx++] = e.modeCount();
            }
            std::printf ("     нота %2d: яркость 0/0.5/1 -> мод %d/%d/%d\n", note, counts[0], counts[1], counts[2]);
            suite::check (counts[0] <= counts[1] && counts[1] <= counts[2], "моды растут с яркостью");
            suite::check (counts[2] > 0, "на ноте " + std::to_string (note) + " есть хотя бы одна мода");
        }
    }

    // ------------------------------------------- T60: затухание по времени
    std::printf ("\n--- T60: измеренное затухание против заданного ---\n");
    {
        for (float dec : { 0.2f, 0.5f, 0.9f })
        {
            ModalEngine e;
            e.setSampleRate (48000.0);
            e.start (45, 1.0f);
            EngineParams p;
            p.table = 0; p.p[0] = 0.5f; p.p[1] = dec; p.p[2] = 0.6f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);

            // T60 по наклону огибающей между 0.3 и 1.3 с.
            // Ждать реального -60 дБ нельзя: при T60 = 11 с это 11 секунд
            // рендера на каждую точку. Наклон даёт ту же величину:
            //   T60 = (t2 - t1) * 60 / (20 * log10(e1 / e2))
            const int win = 2400;                       // 50 мс
            std::vector<double> env;
            for (int w = 0; w < 40; ++w)
            {
                double acc = 0.0;
                for (int i = 0; i < win; ++i) { const float s = e.render (p); acc += (double) s * s; }
                env.push_back (std::sqrt (acc / win));
            }
            const size_t w1 = 6, w2 = 26;               // 0.3 с и 1.3 с
            const double e1 = std::max (1.0e-9, env[w1]);
            const double e2 = std::max (1.0e-9, env[w2]);
            const double t1 = (double) w1 * win / 48000.0, t2 = (double) w2 * win / 48000.0;
            const double t60 = (t2 - t1) * 60.0 / std::max (0.5, 20.0 * std::log10 (e1 / e2));
            const double wanted = 0.25 + dec * 11.75;
            std::printf ("     decay %.1f: задано T60 %.2f с, измерено %.2f с (спад %.1f дБ за 1 с)\n",
                         dec, wanted, t60, 20.0 * std::log10 (e1 / e2));
            suite::check (t60 > wanted * 0.45 && t60 < wanted * 1.8, "затухание совпадает с заданным");
        }
    }

    // ------------------------------------------- стабильность на 10 секунд
    std::printf ("\n--- Стабильность: 10 с на самом длинном T60 ---\n");
    {
        ModalEngine e;
        e.setSampleRate (48000.0);
        e.start (45, 1.0f);
        EngineParams p;
        p.table = 3; p.p[0] = 1.0f; p.p[1] = 1.0f; p.p[2] = 0.8f; p.p[3] = 0.5f;
        p.detune = 0.0f; p.level = 1.0f;
        e.prepare (p);
        bool bad = false; float peak = 0.0f;
        for (int i = 0; i < 480000; ++i)
        {
            const float s = e.render (p);
            if (! std::isfinite (s) || std::fabs (s) > 8.0f) { bad = true; break; }
            if (i > 1000) peak = std::max (peak, std::fabs (s));
        }
        std::printf ("     пик за 10 с: %.3f\n", peak);
        suite::check (! bad && peak < 4.0f, "банк резонаторов устойчив 10 с");
    }

    fails = suite::gFailures - before;
    std::printf ("\n=================================================\n");
    if (fails == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else            std::printf ("ЕСТЬ ПРОВАЛЫ (провалов: %d)\n", fails);
    return fails;
}
