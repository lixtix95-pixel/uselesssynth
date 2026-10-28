// Тесты движка Wavetable: алиасинг, стабильность, щелчки, CPU, сериализация.
// Запуск: ./build/test_wavetable
#include "../src/engines/wavetable.h"
#include "../src/core/fft.h"
#include "../src/core/svf.h"
#include "../src/core/adsr.h"

#include <cstdio>
#include <chrono>
#include <vector>
#include <cmath>
#include <string>

using namespace orogen;

static int gFailures = 0;

static void check (bool ok, const char* what, const std::string& detail = "")
{
    std::printf ("  [%s] %s%s%s\n", ok ? " OK " : "FAIL", what,
                 detail.empty() ? "" : "  — ", detail.c_str());
    if (! ok) ++gFailures;
}

// ---------------------------------------------------------------------------
// Измерение алиасинга: рендерим ноту, считаем спектр, ищем максимум среди
// НЕгармонических бинов относительно основного тона.
// ---------------------------------------------------------------------------
struct AliasResult { double aliasDb; double rms; };

// Алиасинг как ОТНОШЕНИЕ ЭНЕРГИЙ: сумма энергии не-гармонических бинов / полная.
// Так метрика не зависит от того, насколько тих основной тон (у формантных
// таблиц он на -70 дБ тише формант, и «относительно фундаментала» шум выглядел
// бы катастрофой при том же абсолютном уровне).
static AliasResult measureAliasing (WavetableEngine& eng, const EngineParams& p,
                                    int note, double sr, int fftSize = 16384)
{
    eng.setSampleRate (sr);
    eng.start (note, 1.0f);
    eng.prepare (p);

    std::vector<float> buf ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i) buf[(size_t) i] = eng.render (p);

    std::vector<std::complex<float>> spec ((size_t) fftSize);
    double rms = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        const double t = (double) i / (fftSize - 1);
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * t)
                       + 0.14128 * std::cos (2 * kTwoPi * t)
                       - 0.01168 * std::cos (3 * kTwoPi * t);
        spec[(size_t) i] = { (float) (buf[(size_t) i] * w), 0.0f };
        rms += (double) buf[(size_t) i] * buf[(size_t) i];
    }
    rms = std::sqrt (rms / fftSize);
    Fft::forward (spec);

    const double f0 = midiToHz (note);
    const double binHz = sr / fftSize;

    double totalE = 0.0, aliasE = 0.0;
    for (int b = 1; b < fftSize / 2; ++b)
    {
        const double hz = (double) b * binHz;
        if (hz < 25.0) continue;
        const double m2 = std::norm (spec[(size_t) b]);
        totalE += m2;

        const double ratio = hz / f0;
        const double nearest = std::round (ratio);
        // допуск только в бинах: окно Блэкмана-Харриса «размазывает» партиал
        // на ±4 бина, независимо от его частоты
        const double tol = 5.0 * binHz;
        if (nearest >= 1.0 && std::fabs (ratio - nearest) * f0 < tol) continue;
        aliasE += m2;
    }
    const double db = 10.0 * std::log10 (std::max (1e-30, aliasE) / std::max (1e-30, totalE));
    return { db, rms };
}

int main()
{
    const double sr = 48000.0;
    std::printf ("OROGEN — тесты движка Wavetable\n");
    std::printf ("=================================================\n");

    // ---------------------------------------------------------------- 1. контент
    std::printf ("\n1. Построение банка (8 таблиц × 6 кадров × %d MIP-уровней, шаг 1/3 октавы)\n", kWtLevels);
    auto t0 = std::chrono::high_resolution_clock::now();
    WavetableBank bank;
    bank.buildFactory (6);
    auto t1 = std::chrono::high_resolution_clock::now();
    const double buildMs = std::chrono::duration<double, std::milli> (t1 - t0).count();

    size_t samples = 0;
    for (const auto& t : bank.tables)
        for (size_t l = 0; l < t.levels.size(); ++l)
            samples += t.levels[l].size();

    std::printf ("  время построения: %.1f мс, сэмплов: %zu (%.2f МБ float32)\n",
                 buildMs, samples, samples * 4.0 / 1e6);

    bool allLevelsOk = true;
    for (const auto& t : bank.tables)
        for (size_t l = 0; l < t.levels.size(); ++l)
        {
            if ((int) t.levels[l].size() != t.frames * t.lens[l]) allLevelsOk = false;
            if (t.lens[l] < 2 * t.harms[l]) allLevelsOk = false;    // band-limited по построению
        }
    check (allLevelsOk, "MIP-уровни: размеры согласованы, длина >= 2·партиалов");
    check (bank.tables.size() == 8, "8 таблиц в банке");

    // пик каждого кадра должен быть в допуске (нормализация)
    double worstPeak = 0.0;
    for (const auto& t : bank.tables)
        for (size_t l = 0; l < t.levels.size(); ++l)
        {
            const int len = t.lens[l];
            for (int f = 0; f < t.frames; ++f)
            {
                const float* d = t.levels[l].data() + (size_t) f * len;
                float pk = 0.0f;
                for (int i = 0; i < len; ++i) pk = std::max (pk, std::fabs (d[i]));
                worstPeak = std::max (worstPeak, (double) pk);
            }
        }
    check (worstPeak > 0.9 && worstPeak < 1.001, "кадры нормализованы (peak ≈ 0.98)",
           "max peak = " + std::to_string (worstPeak).substr (0, 5));

    // ---------------------------------------------------------------- 2. сериализация
    std::printf ("\n2. Сериализация .orowt\n");
    const std::string path = "build/test_bank.orowt";
    check (bank.save (path), "save()");

    WavetableBank bank2;
    const bool loaded = bank2.load (path);
    check (loaded, "load()");
    bool identical = (bank2.tables.size() == bank.tables.size());
    if (identical)
        for (size_t i = 0; i < bank.tables.size() && identical; ++i)
            for (size_t l = 0; l < bank.tables[i].levels.size() && identical; ++l)
                identical = (bank2.tables[i].levels[l] == bank.tables[i].levels[l]);
    check (identical, "данные после round-trip бит-в-бит совпадают");

    // ---------------------------------------------------------------- 3. звук
    std::printf ("\n3. Базовый рендер\n");
    WavetableEngine eng;
    eng.setBank (&bank);

    EngineParams p;
    p.table = 0; p.p[0] = 0.3f; p.p[1] = 0.0f; p.p[2] = 0.0f; p.p[3] = 0.0f; p.level = 1.0f;
    p.detune = 0.0f;   // детюн голоса сдвигает ВСЕ партиалы -> искажает измерение алиасинга

    {
        eng.setSampleRate (sr);
        eng.start (57, 1.0f);
        eng.prepare (p);
        double sum = 0.0; bool bad = false; float peak = 0.0f;
        const int n = 48000;
        for (int i = 0; i < n; ++i)
        {
            const float s = eng.render (p);
            if (! std::isfinite (s)) bad = true;
            sum += (double) s * s;
            peak = std::max (peak, std::fabs (s));
        }
        check (! bad, "нет NaN/Inf");
        check (std::sqrt (sum / n) > 0.01, "сигнал присутствует",
               "rms = " + std::to_string (std::sqrt (sum / n)).substr (0, 5)
               + ", peak = " + std::to_string (peak).substr (0, 5));
    }

    // ---------------------------------------------------------------- 4. алиасинг
    std::printf ("\n4. Алиасинг по клавиатуре (чистый путь: без warp/fold)\n");
    std::printf ("     нота   частота    худший не-партиал   rms\n");
    double worstAlias = -999.0;
    for (int note = 24; note <= 96; note += 6)
    {
        EngineParams q = p;
        q.p[0] = 0.3f;
        WavetableEngine e2; e2.setBank (&bank);
        const auto r = measureAliasing (e2, q, note, sr);
        worstAlias = std::max (worstAlias, r.aliasDb);
        std::printf ("     %3d  %8.1f Гц        %8.1f дБ      %.4f\n", note, midiToHz (note), r.aliasDb, r.rms);
    }
    check (worstAlias < -60.0, "алиасинг < -60 дБ на всём диапазоне",
           "худший = " + std::to_string (worstAlias).substr (0, 6) + " дБ");

    std::printf ("\n5. Алиасинг по таблицам (нота A4 = 440 Гц)\n");
    double worstTable = -999.0;
    for (int tb = 0; tb < (int) bank.tables.size(); ++tb)
    {
        EngineParams q = p; q.table = tb; q.p[0] = 0.45f;
        WavetableEngine e2; e2.setBank (&bank);
        const auto r = measureAliasing (e2, q, 69, sr);
        worstTable = std::max (worstTable, r.aliasDb);
        std::printf ("     %-22s %8.1f дБ\n", bank.tables[(size_t) tb].name.c_str(), r.aliasDb);
    }
    check (worstTable < -60.0, "все таблицы чистые", "худший = "
           + std::to_string (worstTable).substr (0, 6) + " дБ");

    // ---------------------------------------------------------------- 6. нелинейности
    std::printf ("\n6. Нелинейности (warp / fold) с оверсэмплингом 4×/8×\n");
    struct Case { const char* name; float warp, fold; };
    const Case cases[] = {
        { "warp 0.5",          0.5f, 0.0f },
        { "warp 1.0",          1.0f, 0.0f },
        { "fold 0.5",          0.0f, 0.5f },
        { "fold 1.0",          0.0f, 1.0f },
        { "warp 1.0 + fold 1", 1.0f, 1.0f },
    };
    double worstDist = -999.0;
    for (const auto& c : cases)
    {
        EngineParams q = p; q.p[1] = c.warp; q.p[2] = c.fold;
        WavetableEngine e2; e2.setBank (&bank);
        double worst = -999.0, rmsSum = 0.0;
        for (int note : { 45, 57, 69, 81 })
        {
            const auto r = measureAliasing (e2, q, note, sr);
            worst = std::max (worst, r.aliasDb);
            rmsSum += r.rms;
        }
        worstDist = std::max (worstDist, worst);
        std::printf ("     %-20s худший %7.1f дБ   rms %.4f\n", c.name, worst, rmsSum / 4.0);
    }
    check (worstDist < -40.0, "нелинейности не разваливают спектр",
           "худший = " + std::to_string (worstDist).substr (0, 6) + " дБ");

    // ---------------------------------------------------------------- 7. щелчки
    std::printf ("\n7. Стабильность при резком движении ручки WT Pos\n");
    {
        EngineParams q = p;
        eng.setSampleRate (sr);
        eng.start (60, 1.0f);
        eng.prepare (q);
        // Естественный наклон сигнала (без движения ручки) vs наклон при скачке:
        // щелчок — это разница между ними, а не абсолютная величина.
        float prev = 0.0f, natural = 0.0f, withJump = 0.0f;
        for (int i = 0; i < 48000; ++i)
        {
            const bool jumping = (i >= 12000 && i < 12000 + 4800) || (i >= 24000 && i < 24000 + 4800);
            if (i == 12000) { q.p[0] = 0.95f; eng.prepare (q); }
            if (i == 24000) { q.p[0] = 0.05f; eng.prepare (q); }
            const float s = eng.render (q);
            if (i > 0)
            {
                const float d = std::fabs (s - prev);
                if (jumping) withJump = std::max (withJump, d);
                else         natural  = std::max (natural, d);
            }
            prev = s;
        }
        const float excess = withJump / std::max (1.0e-6f, natural);
        check (excess < 1.3f, "скачок ручки не даёт щелчка (наклон в пределах естественного)",
               "max |Δ| = " + std::to_string (withJump).substr (0, 5)
               + " при естественном " + std::to_string (natural).substr (0, 5)
               + " (×" + std::to_string (excess).substr (0, 4) + ")");
    }

    // ---------------------------------------------------------------- 8. денормали
    std::printf ("\n8. Денормали и тишина\n");
    {
        eng.setSampleRate (sr);
        eng.start (60, 1.0f);
        eng.prepare (p);
        for (int i = 0; i < 1000; ++i) (void) eng.render (p);
        eng.stop();
        bool denormal = false;
        for (int i = 0; i < 1000; ++i)
        {
            const float s = eng.render (p);
            if (std::fpclassify (s) == FP_SUBNORMAL) denormal = true;
        }
        check (! denormal, "на выходе нет субнормалей после stop()");
    }

    // ---------------------------------------------------------------- 8a. DC
    std::printf ("\n8a. Постоянная составляющая (нелинейности дают DC)\n");
    {
        double worstDc = 0.0;
        for (float fold : { 0.0f, 0.5f, 1.0f })
            for (int note : { 45, 69 })
            {
                EngineParams q = p; q.p[1] = 0.5f; q.p[2] = fold;
                WavetableEngine e2; e2.setBank (&bank);
                e2.setSampleRate (sr); e2.start (note, 1.0f); e2.prepare (q);
                // мерим в установившемся режиме: DC-блокер имеет время ~40 мс
                for (int i = 0; i < 24000; ++i) (void) e2.render (q);
                double acc = 0.0; const int N = 24000;
                for (int i = 0; i < N; ++i) acc += e2.render (q);
                worstDc = std::max (worstDc, std::fabs (acc / N));
            }
        check (worstDc < 1.0e-3, "DC ниже -60 дБ от полной шкалы",
               "худший " + std::to_string (worstDc).substr (0, 8));
    }

    // ---------------------------------------------------------------- 8b. калибровка уровня
    std::printf ("\n8b. Калибровка уровня (RMS по 8 таблицам × 3 нотам, level = 1)\n");
    {
        double mn = 99.0, mx = 0.0, sum = 0.0; int cnt = 0;
        for (int tb = 0; tb < (int) bank.tables.size(); ++tb)
            for (int note : { 45, 57, 69 })
            {
                EngineParams q = p; q.table = tb; q.p[0] = 0.45f;
                WavetableEngine e2; e2.setBank (&bank);
                e2.setSampleRate (sr); e2.start (note, 1.0f); e2.prepare (q);
                double acc = 0.0; const int N = 24000;
                for (int i = 0; i < N; ++i) { const float v = e2.render (q); acc += (double) v * v; }
                const double rms = std::sqrt (acc / N);
                mn = std::min (mn, rms); mx = std::max (mx, rms); sum += rms; ++cnt;
            }
        const double spreadDb = 20.0 * std::log10 (mx / mn);
        std::printf ("     средний RMS %.3f, разброс %.3f..%.3f (%.1f дБ)\n", sum / cnt, mn, mx, spreadDb);
        check (sum / cnt > 0.15 && sum / cnt < 0.26, "средний уровень в целевой зоне ~0.2");
        check (spreadDb < 6.0, "разброс между таблицами < 6 дБ");
    }

    // ---------------------------------------------------------------- 9. CPU
    std::printf ("\n9. Производительность по путям (16 голосов)\n");
    {
        auto bench = [&] (const char* label, EngineParams q)
        {
            constexpr int nVoices = 16, blocks = 1200;
            std::vector<WavetableEngine> vs ((size_t) nVoices);
            for (auto& v : vs) { v.setBank (&bank); v.setSampleRate (sr); v.start (60, 1.0f); v.prepare (q); }
            volatile float sink = 0.0f;
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int b = 0; b < blocks; ++b)
                for (int i = 0; i < 512; ++i)
                {
                    float a = 0.0f;
                    for (auto& v : vs) a += v.render (q);
                    sink = sink + a;
                }
            auto t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
            const double audioMs = blocks * 512.0 / sr * 1000.0;
            std::printf ("     %-22s %5.1f%% ядра  (%.2f%% на голос)\n",
                         label, ms / audioMs * 100.0, ms / audioMs * 100.0 / nVoices);
            return ms / audioMs;
        };
        EngineParams q = p; q.detune = 0.0f;
        q.p[1] = 0.0f; q.p[2] = 0.0f;  bench ("чистый путь", q);
        q.p[1] = 0.8f; q.p[2] = 0.0f;  bench ("warp (4x)", q);
        q.p[1] = 0.0f; q.p[2] = 1.0f;  bench ("fold сильный (8x)", q);
        const double worst = bench ("warp + fold (8x)", EngineParams { { 0.3f, 0.8f, 1.0f, 0.3f } });
        check (worst < 1.0, "худший путь укладывается в реальное время");
    }
    if (false)
    {
        const int nVoices = 16, seconds = 10;
        std::vector<WavetableEngine> voices ((size_t) nVoices);
        EngineParams q = p; q.p[1] = 0.3f; q.p[2] = 0.2f;   // с нелинейностями, худший случай
        for (auto& v : voices) { v.setBank (&bank); v.setSampleRate (sr); v.start (60, 1.0f); v.prepare (q); }

        const long total = (long) sr * seconds;
        std::vector<float> out ((size_t) total);
        auto s0 = std::chrono::high_resolution_clock::now();
        for (long i = 0; i < total; ++i)
        {
            float acc = 0.0f;
            for (auto& v : voices) acc += v.render (q);
            out[(size_t) i] = acc;
        }
        auto s1 = std::chrono::high_resolution_clock::now();
        const double ms = std::chrono::duration<double, std::milli> (s1 - s0).count();
        const double audioMs = seconds * 1000.0;
        std::printf ("     %.0f мс на %.0f мс аудио -> %.1f%% одного ядра (16 голосов)\n",
                     ms, audioMs, ms / audioMs * 100.0);
        std::printf ("     ~%.2f%% на голос\n", ms / audioMs * 100.0 / nVoices);
        check (ms / audioMs < 1.0, "16 голосов укладываются в реальное время",
               std::to_string (ms / audioMs * 100.0).substr (0, 5) + "%");
    }

    std::printf ("\n=================================================\n");
    std::printf ("%s (провалов: %d)\n", gFailures == 0 ? "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" : "ЕСТЬ ПРОВАЛЫ", gFailures);
    return gFailures == 0 ? 0 : 1;
}
