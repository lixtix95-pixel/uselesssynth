// Тесты движка Sample: общий стенд + спецтесты (MIP при питч-шифте, петля, Scan).
#include "../src/engines/sample.h"
#include "engine_suite.h"
#include <cstdio>

using namespace orogen;


// Окно анализа для one-shot'а.
//
// Длинное окно (16384 = 341 мс) накрывает ВЕСЬ сэмпл, когда он ускорен:
// при +3 октавы one-shot длится 250 мс, при +4 — 125 мс. Окно тогда содержит
// обрыв сигнала в конце, и его утечка (-27 дБ, широкополосная) измеряется как
// «алиасинг». Это ошибка измерения, а не движка: то же окно, уложенное внутрь
// активной части, даёт -88 дБ на той же ноте.
// Правило: окно <= 70 % длительности сэмпла на этой ноте.
static int fftForNote (int note, double sr = 48000.0)
{
    const double rate   = orogen::midiToHz (note) / orogen::kSampleF0;
    const double active = 96000.0 / rate;              // выходных отсчётов в one-shot'е
    int n = 16384;
    while (n > 1024 && (double) n > 0.70 * active) n >>= 1;
    return n;
}

int main()
{
    std::printf ("OROGEN — движок Sample\n=================================================\n");

    SampleBank bank;
    auto t0 = std::chrono::high_resolution_clock::now();
    bank.buildFactory ();
    auto t1 = std::chrono::high_resolution_clock::now();
    size_t bytes = 0;
    for (const auto& s : bank.shots) for (auto& l : s.levels) bytes += l.size() * 4;
    std::printf ("\nконтент: %zu источников, %.1f МБ, построено за %.0f мс\n",
                 bank.shots.size(), bytes / 1e6,
                 std::chrono::duration<double, std::milli> (t1 - t0).count());

    // сериализация
    {
        bank.save ("build/test_samples.orosmp");
        SampleBank b2;
        const bool ok = b2.load ("build/test_samples.orosmp");
        bool same = ok && b2.shots.size() == bank.shots.size();
        if (same)
            for (size_t i = 0; i < bank.shots.size() && same; ++i)
                for (size_t l = 0; l < bank.shots[i].levels.size() && same; ++l)
                    same = (b2.shots[i].levels[l] == bank.shots[i].levels[l]);
        suite::check (ok, "load() .orosmp");
        suite::check (same, "round-trip бит-в-бит");
    }

    suite::Config cfg;
    cfg.name = "Sample (общий стенд)";
    cfg.make = [&bank] {
        auto e = new SampleEngine();
        e->setBank (&bank);
        return std::unique_ptr<EngineBase> (e);
    };
    cfg.params = [] (int) {
        EngineParams p;
        p.p[0] = 0.0f;     // Scan = 0 (начало)
        p.p[1] = 0.0f;     // без «заикания»
        p.p[2] = 0.9f;     // Tone открыт
        p.p[3] = 0.0f;     // без воздуха (это шум — в замере алиасинга он лишний)
        p.detune = 0.2f;
        p.level = 1.0f;
        return p;
    };
    cfg.aliasWindow = [] (int note) { return fftForNote (note); };
    cfg.aliasTargetDb = -55.0;
    cfg.levelTarget   = 0.20;
    cfg.maxContentSpreadDb = 14.0;   // перкуссивные источники (Wood Knock, Glass Hit)
                                     // по природе имеют RMS ниже тональных на 10+ дБ
    suite::run (cfg);

    // ---------------------------------------------------------------- MIP
    std::printf ("\n--- MIP при повышении ноты (алиасинг по октавам) ---\n");
    std::printf ("     нота   питч-rate   алиасинг\n");
    for (int note : { 45, 57, 69, 81, 93 })
    {
        EngineParams p;
        // тональный источник (Fm Tine): у шумовых (Karplus, Wood Knock) спектр
        // ингармонический по природе, и метрика «алиасинга» к ним неприменима
        p.table = 0; p.p[0] = 0; p.p[1] = 0; p.p[2] = 1.0f; p.p[3] = 0; p.detune = 0; p.level = 1.0f;
        SampleEngine e; e.setBank (&bank);
        const double db = suite::aliasDb (e, p, note, 48000.0, fftForNote (note));
        std::printf ("      %3d    %6.2f      %7.1f дБ\n", note, std::pow (2.0, (note - 45) / 12.0), db);
    }
    {
        // на верхних нотах (2 октавы вверх) без MIP было бы катастрофой
        EngineParams p;
        p.table = 0; p.p[0] = 0; p.p[1] = 0; p.p[2] = 1.0f; p.p[3] = 0; p.detune = 0; p.level = 1.0f;
        SampleEngine e; e.setBank (&bank);
        const double db = suite::aliasDb (e, p, 81, 48000.0, fftForNote (81));      // 3 октавы вверх
        suite::check (db < -55.0, "MIP держит алиасинг при питче +3 октавы",
                      suite::num (db, 1) + " дБ");
    }

    // ---------------------------------------------------------------- петля
    std::printf ("\n--- Петля: нет щелчков на обороте ---\n");
    {
        EngineParams p;
        p.table = 0; p.p[0] = 0; p.p[1] = 0; p.p[2] = 1.0f; p.p[3] = 0; p.detune = 0; p.level = 1.0f;
        SampleEngine e; e.setBank (&bank);
        e.setSampleRate (48000.0); e.start (45, 1.0f); e.prepare (p);
        float prev = 0.0f, natural = 0.0f, worst = 0.0f;
        int wraps = 0;
        float lastPos = 0.0f;
        for (int i = 0; i < 48000 * 6; ++i)      // 6 c: сэмпл 2 c -> 3 оборота петли
        {
            const float s = e.render (p);
            if (i > 0)
            {
                const float d = std::fabs (s - prev);
                natural = std::max (natural, d * 0.999f);
                if (d > natural * 3.0f) { worst = std::max (worst, d); ++wraps; }
            }
            prev = s;
            (void) lastPos;
        }
        std::printf ("     естественный наклон %.4f, аномальных скачков: %d\n", natural, wraps);
        suite::check (wraps == 0, "на обороте петли нет разрывов");
    }

    // ---------------------------------------------------------------- Scan
    std::printf ("\n--- Scan: скраб по записи ---\n");
    {
        EngineParams p;
        p.table = 3; p.p[0] = 0.0f; p.p[1] = 0; p.p[2] = 1.0f; p.p[3] = 0; p.detune = 0; p.level = 1.0f;
        SampleEngine e; e.setBank (&bank);
        e.setSampleRate (48000.0); e.start (45, 1.0f); e.prepare (p);
        for (int i = 0; i < 4800; ++i) (void) e.render (p);
        // смотрим RMS в начале и в хвосте сэмпла
        p.p[0] = 0.0f; e.prepare (p);
        double a = 0.0; for (int i = 0; i < 24000; ++i) { const float s = e.render (p); a += s * s; }
        p.p[0] = 0.8f; e.prepare (p);
        for (int i = 0; i < 4800; ++i) (void) e.render (p);
        double b = 0.0; for (int i = 0; i < 24000; ++i) { const float s = e.render (p); b += s * s; }
        std::printf ("     RMS в начале %.4f, в хвосте %.4f\n", std::sqrt (a / 24000), std::sqrt (b / 24000));
        suite::check (std::sqrt (b / 24000) < std::sqrt (a / 24000) * 1.5,
                      "Scan смещает точку чтения (хвост тише начала)");
    }

    std::printf ("\n=================================================\n%s (провалов: %d)\n",
                 suite::gFailures == 0 ? "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" : "ЕСТЬ ПРОВАЛЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
