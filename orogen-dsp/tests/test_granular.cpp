// Тесты движка Granular: общий стенд + специфика гранулярного синтеза.
#include "engine_suite.h"
#include "../src/engines/granular.h"

#include <cstdio>
#include <cmath>
#include <vector>

using namespace orogen;

// Окно анализа. Гранулярный сигнал нестационарен по природе (зёрна рождаются
// и умирают), поэтому окно берём короче: 2048 = 43 мс, внутри него фаза
// планировщика почти не меняется, и утечка от нестационарности не маскирует
// алиасинг. Допуск гармоник в стенде — 8 бинов = 187 Гц при f0 = 880 Гц.
static constexpr int kAliasWindow = 2048;

// ---------------------------------------------------------------------------
// Метрика алиасинга для грануляра.
//
// Гармоническая метрика (энергия вне k·f0) к грануляру неприменима: зёрна
// стартуют в независимых фазах, и на частоте спавна возникает ЗАКОННАЯ
// амплитудная модуляция. Её боковые полосы (f0 ± density) метрика считает
// алиасингом — отсюда «-17 дБ» у чистого спектра.
//
// Здесь измеряется физическая ошибка: энергия ВЫШЕ полосы источника.
// Пробник (buildProbeShot) — стационарная полоса до 4.4 кГц, поэтому
// при питч-рейте R всё выше 4.4·R·1.15 — артефакт движка (алиасинг,
// образы интерполятора, зеркала MIP-уровней).
// ---------------------------------------------------------------------------
static double bandAliasDb (EngineBase& eng, const EngineParams& p, int note, double sr)
{
    const double rate = midiToHz (note) / kSampleF0;
    const double bandLimit = std::min (0.92 * sr / 2.0, 4400.0 * rate * 1.15);
    const int N = 8192;

    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);
    for (int i = 0; i < 16000; ++i) (void) eng.render (p);       // прогрев: зёрна заполнили буфер

    // ФВЧ 2×40 Гц: огибающая грануляра (её «дыхание») — не алиасинг
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
        if (hz < 30.0) continue;                     // ниже полосы пробника нет ничего
        const double m2 = std::norm (spec[(size_t) b]);
        total += m2;
        if (hz > bandLimit) above += m2;
    }
    return 10.0 * std::log10 (above / std::max (1.0e-30, total));
}

int main()
{
    std::printf ("OROGEN — движок Granular\n=================================================\n\n");

    SampleBank bank;
    bank.buildFactory();
    std::printf ("контент: %zu источников, %.1f МБ\n", bank.shots.size(),
                 (double) 0 * 0.0);

    // ------------------------------------------------------------------ стенд
    suite::Config cfg;
    cfg.name = "Granular (общий стенд)";
    cfg.make = [&bank] {
        auto e = std::make_unique<GranularEngine>();
        e->setBank (&bank);
        return std::unique_ptr<EngineBase> (e.release());
    };
    cfg.params = [] (int) {
        EngineParams p;
        p.table = 0;                 // Fm Tine
        p.p[0] = 0.35f;              // позиция: тело записи
        p.p[1] = 0.45f;              // размер зерна ~ 100 мс
        p.p[2] = 0.55f;              // плотность ~ 50 зёрен/с
        p.p[3] = 0.0f;               // без спреда: для метрики нужен чистый тон
        p.detune = 0.0f;
        p.tuneSemis = 0.0f;
        p.level = 1.0f;
        return p;
    };
    cfg.notes = { 45, 57, 69, 81 };
    cfg.aliasTargetDb = -55.0;
    cfg.aliasWindow = [] (int) { return kAliasWindow; };

    // Стенд гоняет движок на пробнике: только на стационарной полосе имеет
    // смысл вопрос «сколько энергии вылезло выше источника».
    SampleBank probeBank;
    probeBank.shots.push_back (buildProbeShot());
    cfg.make = [&probeBank] {
        auto e = std::make_unique<GranularEngine>();
        e->setBank (&probeBank);
        return std::unique_ptr<EngineBase> (e.release());
    };
    cfg.aliasMeasure = bandAliasDb;
    cfg.levelTarget = 0.20;
    cfg.levelToleranceDb = 8.0;
    cfg.maxContentSpreadDb = 14.0;
    cfg.dcTarget = 2.0e-3;
    cfg.clickTolerance = 1.3f;

    const int before = suite::gFailures;
    int fails = suite::run (cfg);

    // ------------------------------------------------------------- плотность
    std::printf ("\n--- Уровень не зависит от плотности (нормировка на перекрытие) ---\n");
    {
        std::vector<double> rmsVals;
        for (float dens : { 0.15f, 0.4f, 0.65f, 0.9f })
        {
            GranularEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            EngineParams p;
            p.table = 1; p.p[0] = 0.3f; p.p[1] = 0.5f; p.p[2] = dens; p.p[3] = 0.2f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);
            for (int i = 0; i < 4800; ++i) (void) e.render (p);         // прогрев
            double acc = 0.0;
            for (int i = 0; i < 48000; ++i) { const float s = e.render (p); acc += (double) s * s; }
            const double rms = std::sqrt (acc / 48000.0);
            rmsVals.push_back (rms);
            std::printf ("     плотность %.2f: RMS %.4f\n", dens, rms);
        }
        const double lo = *std::min_element (rmsVals.begin(), rmsVals.end());
        const double hi = *std::max_element (rmsVals.begin(), rmsVals.end());
        const double spreadDb = 20.0 * std::log10 (hi / lo);
        suite::check (spreadDb < 6.0, "разброс уровня при смене плотности < 6 дБ",
                      suite::num (spreadDb, 1) + " дБ");
    }

    // -------------------------------------------------------------- размер
    std::printf ("\n--- Размер зерна: 8 мс .. 220 мс ---\n");
    {
        for (float sz : { 0.0f, 0.5f, 1.0f })
        {
            GranularEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            EngineParams p;
            p.table = 0; p.p[0] = 0.35f; p.p[1] = sz; p.p[2] = 0.5f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);
            for (int i = 0; i < 4800; ++i) (void) e.render (p);
            double acc = 0.0; float peak = 0.0f; bool bad = false;
            for (int i = 0; i < 24000; ++i)
            {
                const float s = e.render (p);
                if (! std::isfinite (s)) bad = true;
                acc += (double) s * s; peak = std::max (peak, std::fabs (s));
            }
            std::printf ("     size %.2f: RMS %.4f, пик %.3f%s\n",
                         sz, std::sqrt (acc / 24000.0), peak, bad ? "  <-- NaN" : "");
            suite::check (! bad, "конечный сигнал на size " + suite::num (sz, 2));
        }
    }

    // ------------------------------------------------------------ спред
    std::printf ("\n--- Спред питча: ширина спектра растёт ---\n");
    {
        // Спектральная ширина = число бинов в полосе f0/2 .. 2·f0, где уровень
        // выше пик-20 дБ. При спреде 0 все зёрна на одной высоте -> узкий пик;
        // при спреде 1 высоты разбросаны на ±7 полутонов -> широкая «шапка».
        double width[2] = {};
        int idx = 0;
        for (float sp : { 0.0f, 1.0f })
        {
            GranularEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (57, 1.0f);
            EngineParams p;
            p.table = 0; p.p[0] = 0.35f; p.p[1] = 0.6f; p.p[2] = 0.6f; p.p[3] = sp;
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
            const double f0 = midiToHz (57), binHz = 48000.0 / N;
            double peak = 0.0;
            for (int b = 1; b < N / 2; ++b) peak = std::max (peak, (double) std::norm (spec[(size_t) b]));
            int count = 0;
            for (int b = 1; b < N / 2; ++b)
            {
                const double hz = b * binHz;
                if (hz < f0 * 0.5 || hz > f0 * 2.0) continue;
                if (std::norm (spec[(size_t) b]) > peak * 1.0e-2) ++count;    // выше пик-20 дБ
            }
            width[idx++] = (double) count;
            std::printf ("     спред %.1f: бинов выше пик-20 дБ в полосе f0/2..2f0 — %d\n", sp, count);
        }
        suite::check (width[1] > width[0] * 2.0, "спред расширяет спектр",
                      suite::num (width[0], 0) + " -> " + suite::num (width[1], 0) + " бинов");
    }

    // ------------------------------------------------------------ детерминизм
    std::printf ("\n--- Детерминизм при фиксированном seed ---\n");
    {
        auto renderAll = [&] (std::vector<float>& out) {
            GranularEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (60, 1.0f);
            EngineParams p;
            p.table = 3; p.p[0] = 0.4f; p.p[1] = 0.5f; p.p[2] = 0.5f; p.p[3] = 0.7f;
            p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);
            out.resize (24000);
            for (auto& v : out) v = e.render (p);
        };
        std::vector<float> a, b;
        renderAll (a);
        renderAll (b);
        bool same = true;
        for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { same = false; break; }
        suite::check (same, "два прогона бит-в-бит совпадают");
    }

    // ------------------------------------- заворот зерна не уходит за запись
    //
    // Раньше зерно читалось «сквозь» конец записи, и MipReader заворачивал
    // чтение на атаку прямо внутри окна: на коротких источниках это давало
    // щелчки, а у источников с коротким телом — уровень −57 дБ относительно
    // остальных (Wood Knock: RMS 0.0002 против 0.137 у Karplus).
    std::printf ("\n--- Зерно не заворачивается за конец записи ---\n");
    {
        double lo = 1.0e9, hi = 0.0;
        for (size_t t = 0; t < bank.shots.size(); ++t)
        {
            GranularEngine e;
            e.setBank (&bank);
            e.setSampleRate (48000.0);
            e.start (60, 1.0f);
            EngineParams p;
            p.table = (int) t; p.p[0] = 0.3f; p.p[1] = 0.45f; p.p[2] = 0.55f; p.p[3] = 0.25f;
            p.detune = 0.0f; p.level = 0.8f;
            e.prepare (p);
            double acc = 0.0;
            int n = 48000 * 2;
            for (int i = 0; i < n; ++i) { const float v = e.render (p); acc += (double) v * v; }
            const double rms = std::sqrt (acc / n);
            lo = std::min (lo, rms);
            hi = std::max (hi, rms);
        }
        const double spreadDb = 20.0 * std::log10 (hi / std::max (1.0e-9, lo));
        std::printf ("   RMS по 8 источникам: от %.4f до %.4f — разброс %.1f дБ\n", lo, hi, spreadDb);
        suite::check (spreadDb < 12.0, "уровень не зависит от источника", suite::num (spreadDb, 1) + " дБ");
    }

    fails = suite::gFailures - before;
    std::printf ("\n=================================================\n");
    if (fails == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else            std::printf ("ЕСТЬ ПРОВАЛЫ (провалов: %d)\n", fails);
    return fails;
}
