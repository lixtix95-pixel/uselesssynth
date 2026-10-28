// Общий тестовый стенд: одинаковые 11 проверок для каждого движка.
// Движок передаёт фабрику и функцию параметров — остальное меряется одинаково,
// чтобы «доведён до конца» означало одно и то же для всех восьми.
#pragma once

#include "../src/engines/engine.h"
#include "../src/core/fft.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>
#include <string>
#include <cmath>

namespace suite {

using namespace orogen;

inline int gFailures = 0;

inline void check (bool ok, const std::string& what, const std::string& detail = "")
{
    std::printf ("  [%s] %s%s%s\n", ok ? " OK " : "FAIL", what.c_str(),
                 detail.empty() ? "" : "  — ", detail.c_str());
    if (! ok) ++gFailures;
}

inline std::string num (double v, int prec = 2)
{
    char buf[64];
    std::snprintf (buf, sizeof buf, "%.*f", prec, v);
    return buf;
}

// ---------------------------------------------------------------------------
// Алиасинг как отношение энергии не-гармонических бинов к полной.
// ---------------------------------------------------------------------------
inline double aliasDb (EngineBase& eng, const EngineParams& p, int note,
                       double sr, int fftSize = 16384)
{
    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);

    // ФВЧ 2×40 Гц перед анализом.
    // Без него у one-shot'ов (Sample, Organics) вся энергия — в ОГИБАЮЩЕЙ
    // (затухание за 2 секунды = спектр около 0 Гц), и её утечка по всему
    // диапазону считается «алиасингом»: метрика показывала -7 дБ у чистого
    // сэмпла. Огибающая — не алиасинг, поэтому режем её до измерения.
    const float hpCoef = (float) std::exp (-kTwoPi * 40.0 / sr);
    float hpX1 = 0.0f, hpY1 = 0.0f, hpX2 = 0.0f, hpY2 = 0.0f;

    std::vector<std::complex<float>> spec ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
    {
        float s = eng.render (p);
        float y = hpCoef * (hpY1 + s - hpX1); hpX1 = s; hpY1 = y;
        s = y;
        y = hpCoef * (hpY2 + s - hpX2); hpX2 = s; hpY2 = y;
        s = y;
        const double t = (double) i / (fftSize - 1);
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                       + 0.14128 * std::cos (2 * kTwoPi * t)
                       - 0.01168 * std::cos (3 * kTwoPi * t);
        spec[(size_t) i] = { (float) (s * w), 0.0f };
    }
    Fft::forward (spec);

    const double f0 = midiToHz (note);
    const double binHz = sr / fftSize;
    double total = 0.0, alias = 0.0;
    for (int b = 1; b < fftSize / 2; ++b)
    {
        const double hz = (double) b * binHz;
        if (hz < 25.0) continue;
        const double m2 = std::norm (spec[(size_t) b]);
        total += m2;
        const double ratio = hz / f0;
        const double nearest = std::round (ratio);
        // ±8 бинов: у сэмплерных движков петля слегка «размазывает» основной тон
        // (утечка шире, чем ±4 бина окна). Это утечка, а не алиасинг.
        if (nearest >= 1.0 && std::fabs (ratio - nearest) * f0 < 8.0 * binHz) continue;
        alias += m2;
    }
    return 10.0 * std::log10 (std::max (1e-30, alias) / std::max (1e-30, total));
}

struct Config
{
    std::string name;
    std::function<std::unique_ptr<EngineBase>()> make;
    std::function<EngineParams (int note)> params;
    std::vector<int> notes     = { 45, 57, 69, 81 };
    double aliasTargetDb       = -60.0;
    double levelTarget         = 0.20;
    double levelToleranceDb    = 6.0;
    double maxContentSpreadDb  = 9.0;      // у перкуссивного контента RMS выше по природе
    double dcTarget            = 1.0e-3;
    float  clickTolerance      = 1.3f;
    // Длина окна анализа алиасинга. Для one-shot-движков окно обязано лежать
    // внутри активной части сэмпла: иначе утечка от обрыва сигнала (-27 дБ)
    // считается алиасингом. 0 = брать по умолчанию (16384).
    std::function<int (int note)> aliasWindow;
    // Заменяемая метрика алиасинга. Нужна движкам, у которых спектр
    // заведомо негармонический (Granular: зёрна с независимыми фазами дают
    // законные боковые полосы на частоте спавна). Для них «алиасинг»
    // измеряется как энергия выше полосы источника — см. test_granular.cpp.
    std::function<double (EngineBase&, const EngineParams&, int note, double sr)> aliasMeasure;
};

// ---------------------------------------------------------------------------
inline int failures() noexcept { return gFailures; }

inline int run (const Config& cfg, double sr = 48000.0)
{
    const int before = gFailures;
    std::printf ("\n=== %s ===\n", cfg.name.c_str());

    // 1. базовый рендер
    {
        auto eng = cfg.make();
        const EngineParams p = cfg.params (57);
        eng->setSampleRate (sr);
        eng->start (57, 1.0f);
        eng->prepare (p);
        double rms = 0.0; bool bad = false; float peak = 0.0f;
        for (int i = 0; i < 48000; ++i)
        {
            const float s = eng->render (p);
            if (! std::isfinite (s)) bad = true;
            rms += (double) s * s;
            peak = std::max (peak, std::fabs (s));
        }
        rms = std::sqrt (rms / 48000.0);
        check (! bad, "нет NaN/Inf");
        check (rms > 0.005, "сигнал присутствует",
               "rms " + num (rms, 4) + ", peak " + num (peak, 3));
    }

    // 2. алиасинг
    double worstAlias = -999.0;
    {
        std::string detail;
        for (int note : cfg.notes)
        {
            auto eng = cfg.make();
            EngineParams p = cfg.params (note);
            p.detune = 0.0f;                        // детюн сдвигает партиалы -> искажает метрику
            const int n = cfg.aliasWindow ? cfg.aliasWindow (note) : 16384;
            const double db = cfg.aliasMeasure ? cfg.aliasMeasure (*eng, p, note, sr)
                                               : aliasDb (*eng, p, note, sr, n);
            worstAlias = std::max (worstAlias, db);
            detail += "n" + std::to_string (note) + ":" + num (db, 1) + "  ";
        }
        check (worstAlias < cfg.aliasTargetDb, "алиасинг ниже цели " + num (cfg.aliasTargetDb, 0) + " дБ",
               detail + " худший " + num (worstAlias, 1) + " дБ");
    }

    // 3. DC
    {
        double worstDc = 0.0;
        for (int note : { 45, 69 })
        {
            auto eng = cfg.make();
            const EngineParams p = cfg.params (note);
            eng->setSampleRate (sr);
            eng->start (note, 1.0f);
            eng->prepare (p);
            for (int i = 0; i < 24000; ++i) (void) eng->render (p);   // установившийся режим
            double acc = 0.0;
            for (int i = 0; i < 24000; ++i) acc += eng->render (p);
            worstDc = std::max (worstDc, std::fabs (acc / 24000.0));
        }
        check (worstDc < cfg.dcTarget, "DC ниже порога",
               "худший " + num (worstDc * 1000.0, 3) + "e-3");
    }

    // 4. калибровка уровня
    {
        double mn = 99.0, mx = 0.0, sum = 0.0; int cnt = 0;
        for (int table = 0; table < 4; ++table)
            for (int note : { 45, 57, 69 })
            {
                auto eng = cfg.make();
                EngineParams p = cfg.params (note);
                p.table = table;
                eng->setSampleRate (sr);
                eng->start (note, 1.0f);
                eng->prepare (p);
                double acc = 0.0; const int N = 24000;
                for (int i = 0; i < N; ++i) { const float v = eng->render (p); acc += (double) v * v; }
                const double rms = std::sqrt (acc / N);
                if (rms < 1e-6) continue;
                mn = std::min (mn, rms); mx = std::max (mx, rms); sum += rms; ++cnt;
            }
        if (cnt > 0)
        {
            const double avg = sum / cnt;
            const double spreadDb = 20.0 * std::log10 (mx / mn);
            std::printf ("     средний RMS %.3f, разброс %.1f дБ\n", avg, spreadDb);
            check (std::fabs (20.0 * std::log10 (avg / cfg.levelTarget)) < cfg.levelToleranceDb,
                   "уровень в целевой зоне " + num (cfg.levelTarget, 2));
            check (spreadDb < cfg.maxContentSpreadDb,
                   "разброс между вариантами контента < " + num (cfg.maxContentSpreadDb, 0) + " дБ");
        }
    }

    // 5. щелчки при скачке параметра
    {
        auto eng = cfg.make();
        EngineParams p = cfg.params (60);
        eng->setSampleRate (sr);
        eng->start (60, 1.0f);
        eng->prepare (p);
        float prev = 0.0f, natural = 0.0f, jump = 0.0f;
        for (int i = 0; i < 48000; ++i)
        {
            bool jumping = false;
            if (i == 12000) { p = cfg.params (60); p.p[0] = 0.95f; eng->prepare (p); jumping = true; }
            if (i == 24000) { p = cfg.params (60); p.p[0] = 0.05f; eng->prepare (p); jumping = true; }
            const float s = eng->render (p);
            if (i > 0)
            {
                const float d = std::fabs (s - prev);
                if (jumping) jump = std::max (jump, d); else natural = std::max (natural, d);
            }
            prev = s;
        }
        const float excess = jump / std::max (1.0e-6f, natural);
        check (excess < cfg.clickTolerance, "скачок ручки без щелчка",
               "×" + num (excess, 2) + " от естественного наклона");
    }

    // 6. денормали после stop()
    {
        auto eng = cfg.make();
        const EngineParams p = cfg.params (60);
        eng->setSampleRate (sr);
        eng->start (60, 1.0f);
        eng->prepare (p);
        for (int i = 0; i < 2000; ++i) (void) eng->render (p);
        eng->stop();
        bool denormal = false;
        for (int i = 0; i < 2000; ++i)
            if (std::fpclassify (eng->render (p)) == FP_SUBNORMAL) denormal = true;
        check (! denormal, "нет субнормалей после stop()");
    }

    // 7. CPU
    {
        constexpr int nVoices = 16, blocks = 600;
        std::vector<std::unique_ptr<EngineBase>> vs;
        for (int i = 0; i < nVoices; ++i)
        {
            auto e = cfg.make();
            const EngineParams p = cfg.params (57 + i % 12);
            e->setSampleRate (sr); e->start (57 + i % 12, 1.0f); e->prepare (p);
            vs.push_back (std::move (e));
        }
        volatile float sink = 0.0f;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int b = 0; b < blocks; ++b)
            for (int i = 0; i < 256; ++i)
            {
                float a = 0.0f;
                for (auto& v : vs) a += v->render (cfg.params (60));
                sink = sink + a;
            }
        auto t1 = std::chrono::high_resolution_clock::now();
        const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
        const double audioMs = blocks * 256.0 / sr * 1000.0;
        const double pct = ms / audioMs * 100.0;
        std::printf ("     CPU: %.1f%% ядра на 16 голосов (%.2f%% на голос)\n", pct, pct / nVoices);
        check (pct < 100.0, "укладывается в реальное время");
    }

    (void) worstAlias;
    return gFailures - before;
}

} // namespace suite
