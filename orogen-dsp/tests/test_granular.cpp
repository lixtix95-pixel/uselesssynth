// Тесты движка Granular: общий стенд + специфика гранулярного синтеза.
#include "engine_suite.h"
#include "../src/engines/granular.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <array>
#include <utility>

using namespace orogen;

namespace orogen {
// Наблюдаем настоящие MIP-ридеры и исходные координаты, а не уровень смеси:
// соседние зёрна могут замаскировать заворот на начало записи.
struct GranularTestProbe
{
    static float rate (const GranularEngine& e, int i)
    {
        return e.grains[i].active ? e.grains[i].rate : 0.0f;
    }

    // Начало, полный проход и усиление первого активного зерна.
    static std::array<float, 3> firstGrain (const GranularEngine& e)
    {
        for (const auto& g : e.grains)
            if (g.active) return { (float) (g.pos - g.rate), g.dur * g.rate, g.amp };
        return {};
    }

    // Фактическая координата и заданный шаг, без округления пробника в float.
    static std::array<double, 2> firstCursor (const GranularEngine& e)
    {
        for (const auto& g : e.grains)
            if (g.active) return { (double) g.pos, (double) g.rate };
        return {};
    }

    struct Stats
    {
        int spawned = 0, unsafe = 0, wrappedReads = 0, kernelWrapReads = 0;
        double worstOverrun = 0.0, startSum = 0.0, durationSum = 0.0;
    };

    static void observe (const GranularEngine& e, std::array<bool, GranularEngine::kMaxGrains>& previous,
                         Stats& stats)
    {
        for (int i = 0; i < GranularEngine::kMaxGrains; ++i)
        {
            const auto& g = e.grains[i];
            if (g.active && ! previous[(size_t) i])
            {
                ++stats.spawned;
                // observe() вызывается сразу после первого render(): позиция уже
                // сдвинута на один rate, но phase ещё в начале Hann-окна.
                const double start = (double) g.pos - (double) g.rate;
                const double end = start + (double) g.dur * (double) g.rate;
                stats.startSum += start;
                stats.durationSum += (double) g.dur;
                const double safeEnd = (double) g.mip.lenOrig - 128.0;
                const double over = end - safeEnd;
                if (over > 0.25) { ++stats.unsafe; stats.worstOverrun = std::max (stats.worstOverrun, over); }
            }
            // Проверяем и последний отсчёт зерна: после render() active уже
            // может быть false, хотя MipReader только что выполнил чтение.
            if (g.active || previous[(size_t) i])
            {
                const double wrapA = ((double) g.mip.lenA - 8.0) / (double) g.mip.invScaleA;
                const double wrapB = ((double) g.mip.lenB - 8.0) / (double) g.mip.invScaleB;
                const double justRead = (double) g.pos - (double) g.rate;
                if (justRead >= wrapA || (g.mip.wB > 0.001f && justRead >= wrapB)) ++stats.wrappedReads;
                // 32/64 отвода sinc могут пересечь край MIP-уровня, даже
                // если центральный индекс ещё лежит внутри буфера.
                const auto kernelWrap = [justRead, &g] (int len, float invScale) {
                    const int centre = (int) std::floor (justRead * (double) invScale);
                    const int half = g.mip.grainHalf();
                    return centre - (half - 1) < 0 || centre + half >= len;
                };
                if (kernelWrap (g.mip.lenA, g.mip.invScaleA)
                    || (g.mip.wB > 0.001f && kernelWrap (g.mip.lenB, g.mip.invScaleB)))
                    ++stats.kernelWrapReads;
            }
            previous[(size_t) i] = g.active;
        }
    }
};
}

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

// Стационарный синус нужен для измерений плотности/позиции: динамика и хвост
// фабричных записей иначе смешивают изменение алгоритма с изменением материала.
static OneShot makeStationaryShot (double sr, double seconds, double overtoneHz = 0.0,
                                   double fundamentalHz = 110.0)
{
    OneShot s;
    s.name = "Granular stationary test";
    s.rootNote = 45;
    s.levels.resize (1);
    const int n = (int) (seconds * sr);
    s.levels[0].resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        s.levels[0][(size_t) i] = (float) (0.30 * std::sin (kTwoPi * fundamentalHz * t)
            + (overtoneHz > 0.0 ? 0.30 * std::sin (kTwoPi * overtoneHz * t) : 0.0));
    }
    buildMipLevels (s, sr);
    return s;
}

// Мощность двух независимых тонов в рендере настоящего двигателя. Источник
// содержит равные амплитуды; 24 окна усредняют случайные фазы зёрен. Верхний
// тон задаётся в частотах выхода, а измеряемая частота может быть его alias.
static std::pair<double, double> twoToneGranularDb (double rate, double inputToneAtOutputHz,
                                                    double measuredToneHz, float reverse = 0.0f)
{
    constexpr double sr = 48000.0, fundamentalHz = 30.0;
    constexpr int N = 2048, windows = 24;
    SampleBank b;
    b.shots.push_back (makeStationaryShot (sr, 4.0, inputToneAtOutputHz / rate, fundamentalHz));
    GranularEngine e;
    e.setBank (&b); e.setSampleRate (sr); e.start (93, 1.0f); // базовый rate=16
    EngineParams p;
    p.p[0] = 0.4f; p.p[1] = 0.45f; p.p[2] = 1.0f; p.p[3] = 0.0f;
    p.tuneSemis = (float) (12.0 * std::log2 (rate / 16.0)); p.level = 1.0f;
    p.detune = 0.0f;  // частоты анализа заданы ТОЧНО для этого rate
    p.grainReverse = reverse;
    e.prepare (p);
    for (int i = 0; i < 24000; ++i) (void) e.render (p);
    double fundamentalPower = 0.0, tonePower = 0.0;
    for (int w = 0; w < windows; ++w)
    {
        double ls = 0.0, lc = 0.0, hs = 0.0, hc = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double x = e.render (p);
            const double window = 0.5 - 0.5 * std::cos (kTwoPi * i / (N - 1));
            const double lo = kTwoPi * fundamentalHz * rate * i / sr;
            const double hi = kTwoPi * measuredToneHz * i / sr;
            ls += window * x * std::sin (lo); lc += window * x * std::cos (lo);
            hs += window * x * std::sin (hi); hc += window * x * std::cos (hi);
        }
        fundamentalPower += ls * ls + lc * lc;
        tonePower += hs * hs + hc * hc;
    }
    return { 10.0 * std::log10 (std::max (1.0e-30, tonePower)
                                 / std::max (1.0e-30, fundamentalPower)), fundamentalPower };
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

    // Detune — общий параметр слота, а не синоним макроса Spread. Сравниваем
    // два идентичных рендера с Spread = 0, чтобы обнаружить мёртвую ручку.
    std::printf ("\n--- Detune и автоматизация уровня Granular ---\n");
    {
        GranularEngine a, b;
        a.setBank (&probeBank); b.setBank (&probeBank);
        a.setSampleRate (48000.0); b.setSampleRate (48000.0);
        a.start (60, 1.0f); b.start (60, 1.0f);
        EngineParams p, q;
        p.p[0] = q.p[0] = 0.35f; p.p[1] = q.p[1] = 0.5f;
        p.p[2] = q.p[2] = 0.8f; p.p[3] = q.p[3] = 0.0f;
        p.detune = 0.0f; q.detune = 1.0f;
        a.prepare (p); b.prepare (q);
        double dif = 0.0, ref = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            const float x = a.render (p), y = b.render (q);
            const double d = (double) x - y;
            dif += d * d; ref += (double) x * x;
        }
        const double relative = std::sqrt (dif / std::max (1.0e-20, ref));
        std::printf ("   Detune 0 -> 1 при Spread 0: относительная ΔRMS %.6f\n", relative);
        suite::check (relative > 0.01, "Detune действительно изменяет сигнал Granular",
                      suite::num (relative, 6));

        auto grainRate = [&probeBank] (float noteRandom, float detune) {
            GranularEngine e;
            e.setBank (&probeBank); e.setSampleRate (48000.0);
            e.setNoteRandom (noteRandom); e.start (60, 1.0f);
            EngineParams ep;
            ep.detune = detune; ep.p[3] = 0.0f;
            e.prepare (ep); (void) e.render (ep);
            return GranularTestProbe::rate (e, 0);
        };
        const double centre = grainRate (1.0f, 0.0f);
        const double plus = grainRate (1.0f, 1.0f);
        const double minus = grainRate (-1.0f, 1.0f);
        const double centsPlus = 1200.0 * std::log2 (plus / centre);
        const double centsMinus = 1200.0 * std::log2 (minus / centre);
        std::printf ("   две ноты: %.3f / %.3f цента относительно Detune 0\n", centsMinus, centsPlus);
        suite::check (std::fabs (centsPlus - 10.0) < 0.02 && std::fabs (centsMinus + 10.0) < 0.02,
                      "Detune задаёт независимое отклонение ±10 центов на голос");

        GranularEngine live;
        live.setBank (&probeBank); live.setSampleRate (48000.0);
        live.setNoteRandom (1.0f); live.start (60, 1.0f);
        EngineParams liveP;
        liveP.detune = 0.0f; liveP.p[1] = 1.0f; liveP.p[2] = 1.0f; liveP.p[3] = 0.0f;
        live.prepare (liveP);
        for (int i = 0; i < 100; ++i) (void) live.render (liveP);
        const float oldRate = GranularTestProbe::rate (live, 0);
        liveP.detune = 1.0f;
        live.prepare (liveP);
        float newRate = 0.0f;
        int response = -1;
        for (int i = 0; i < 2400 && newRate == 0.0f; ++i)
        {
            (void) live.render (liveP);
            newRate = GranularTestProbe::rate (live, 1);
            if (newRate > 0.0f) response = i + 1;
        }
        const float heldRate = GranularTestProbe::rate (live, 0);
        const double liveCents = newRate > 0.0f ? 1200.0 * std::log2 (newRate / oldRate) : 0.0;
        std::printf ("   автоматизация: новое зерно через %d отсч., %.3f цента; старое %.8f -> %.8f\n",
                     response, liveCents, oldRate, heldRate);
        suite::check (response > 0 && response <= 600 && oldRate > 0.0f
                      && heldRate == oldRate && std::fabs (liveCents - 10.0) < 0.02,
                      "удержанное зерно не перескакивает, следующее получает новый Detune");

        GranularEngine control, automated;
        control.setBank (&probeBank); automated.setBank (&probeBank);
        control.setSampleRate (48000.0); automated.setSampleRate (48000.0);
        control.start (60, 1.0f); automated.start (60, 1.0f);
        p.detune = q.detune = 0.0f; p.level = q.level = 1.0f;
        control.prepare (p); automated.prepare (q);
        float prev = 0.0f;
        for (int i = 0; i < 12000; ++i)
        {
            prev = control.render (p);
            (void) automated.render (q);
        }
        for (int i = 0; i < 48000 && std::fabs (prev) < 0.12f; ++i)
        {
            prev = control.render (p);
            (void) automated.render (q);
        }
        q.level = 0.0f;
        automated.prepare (q);
        const float x = control.render (p), y = automated.render (q);
        const double firstRatio = std::fabs ((double) x - y) / std::max (1.0e-8, std::fabs ((double) x));
        double ec = 0.0, ea = 0.0;
        for (int i = 0; i < 24000; ++i)
        {
            const float c = control.render (p), z = automated.render (q);
            if (i >= 12000) { ec += (double) c * c; ea += (double) z * z; }
        }
        const double settled = std::sqrt (ea / std::max (1.0e-20, ec));
        std::printf ("   Level 1 -> 0: первый относительный скачок %.6f, итоговое отношение RMS %.6f\n",
                     firstRatio, settled);
        suite::check (firstRatio < 0.02 && settled > 0.39 && settled < 0.41,
                      "уровень обновляется без мгновенного скачка и приходит к исходной калибровке",
                      suite::num (firstRatio, 6) + " / " + suite::num (settled, 6));
    }

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

    // Проверяем КАЖДОЕ зерно и каждое фактическое чтение, включая повторное
    // использование ячеек: раньше span считался по g.rate от предыдущего зерна.
    // Высокая нота и длинное зерно делают заворот воспроизводимым.
    std::printf ("\n--- Границы каждого зерна: скорости, повторное использование, MIP ---\n");
    {
        GranularTestProbe::Stats stats;
        int badSamples = 0;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            SampleBank shortBank;
            shortBank.shots.push_back (buildOneShot (OneShotKind::FmTine, sr, 0.50));
            for (int note : { 45, 69, 93, 117, 127 })
                for (float spread : { 0.0f, 1.0f })
                  for (float tune : { -24.0f, 0.0f, 24.0f })
                {
                    GranularEngine e;
                    e.setBank (&shortBank);
                    e.setSampleRate (sr);
                    e.start (note, 1.0f);
                    EngineParams p;
                    p.table = 0; p.p[0] = 0.97f; p.p[1] = 1.0f;
                    p.p[2] = 1.0f; p.p[3] = spread;
                    p.detune = 0.0f; p.tuneSemis = tune; p.level = 1.0f;
                    e.prepare (p);
                    std::array<bool, GranularEngine::kMaxGrains> previous {};
                    for (int i = 0; i < (int) (sr * 0.5); ++i)
                    {
                        if (! std::isfinite (e.render (p))) ++badSamples;
                        GranularTestProbe::observe (e, previous, stats);
                    }
                }
        }
        std::printf ("   зёрен %d, небезопасных %d, чтений с заворотом %d, макс. выход %.1f отсч., NaN %d\n",
                     stats.spawned, stats.unsafe, stats.wrappedReads, stats.worstOverrun, badSamples);
        suite::check (stats.spawned > 700 && stats.unsafe == 0,
                      "каждое зерно целиком помещается в источник",
                      suite::num (stats.unsafe, 0) + " / " + suite::num (stats.spawned, 0));
        suite::check (stats.wrappedReads == 0 && badSamples == 0,
                      "MIP не заворачивает чтение внутри окна зерна",
                      suite::num (stats.wrappedReads, 0) + " чтений, " + suite::num (badSamples, 0) + " NaN");
    }

    // Первая гранула не должна ждать целый интервал планировщика: при 4/с
    // это 250 мс тишины до атаки инструмента.
    std::printf ("\n--- Задержка первой гранулы при разных Density и sample rate ---\n");
    {
        double worstMs = 0.0;
        int missing = 0;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            SampleBank b;
            b.shots.push_back (makeStationaryShot (sr, 2.0));
            for (float density : { 0.0f, 0.5f, 1.0f })
            {
                GranularEngine e;
                e.setBank (&b); e.setSampleRate (sr); e.start (57, 1.0f);
                EngineParams p;
                p.p[0] = 0.3f; p.p[1] = 0.5f; p.p[2] = density; p.p[3] = 0.0f; p.level = 1.0f;
                e.prepare (p);
                int first = -1;
                for (int i = 0; i < (int) (sr * 0.35); ++i)
                    if (std::fabs (e.render (p)) > 1.0e-4f) { first = i; break; }
                if (first < 0) ++missing;
                else worstMs = std::max (worstMs, 1000.0 * first / sr);
                std::printf ("   %.1f кГц, density %.1f: %d отсч. / %.3f мс\n",
                             sr / 1000.0, density, first, first < 0 ? -1.0 : 1000.0 * first / sr);
            }
        }
        suite::check (missing == 0 && worstMs < 6.0,
                      "первая гранула начинается без ожидания периода Density",
                      suite::num (worstMs, 3) + " мс, пропусков " + suite::num (missing, 0));
    }

    // Сравниваем с неизменённым двойником НА ТОМ ЖЕ отсчёте. Так естественный
    // наклон волны не принимается за щелчок автоматизации.
    std::printf ("\n--- Ступень Density без скачка уже звучащих зёрен ---\n");
    {
        SampleBank b;
        b.shots.push_back (makeStationaryShot (48000.0, 2.0));
        double worstRel = 0.0, worstJump = 0.0, worstNatural = 0.0;
        int missing = 0;
        for (float from : { 0.0f, 1.0f })
        {
            GranularEngine changed, reference;
            changed.setBank (&b); reference.setBank (&b);
            changed.setSampleRate (48000.0); reference.setSampleRate (48000.0);
            changed.start (57, 1.0f); reference.start (57, 1.0f);
            EngineParams p;
            p.p[0] = 0.35f; p.p[1] = 1.0f; p.p[2] = from; p.p[3] = 0.2f; p.level = 1.0f;
            changed.prepare (p); reference.prepare (p);
            float previous = 0.0f;
            bool found = false;
            for (int i = 0; i < 48000 * 4; ++i)
            {
                previous = changed.render (p);
                (void) reference.render (p);
                // Совпадает с моментом старого пересчёта нормировки каждые
                // 16 отсчётов, когда регрессия воспроизводилась стабильно.
                if (i > 48000 && i % 16 == 15 && std::fabs (previous) > 0.05f)
                { found = true; break; }
            }
            if (! found) { ++missing; continue; }
            EngineParams q = p;
            q.p[2] = 1.0f - from;
            changed.prepare (q);
            const float a = changed.render (q);
            const float b0 = reference.render (p);
            const double rel = std::fabs ((double) a - b0) / std::max (1.0e-9, std::fabs ((double) b0));
            worstRel = std::max (worstRel, rel);
            worstJump = std::max (worstJump, std::fabs ((double) a - previous));
            worstNatural = std::max (worstNatural, std::fabs ((double) b0 - previous));
            std::printf ("   %.0f → %.0f: до %.6f, после %.6f, без смены %.6f, Δ %.2f %%\n",
                         from, q.p[2], previous, a, b0, 100.0 * rel);
        }
        suite::check (missing == 0 && worstRel < 0.005,
                      "Density не делает ступень в текущих зёрнах",
                      suite::num (worstRel * 100.0, 3) + " %, скачок " + suite::num (worstJump, 6)
                      + ", естественный " + suite::num (worstNatural, 6));
    }

    // Зерно начинается в тихом предударе, а окно накрывает громкую атаку.
    // Нормировка только по первой 10-мс ячейке ошибочно усиливает ВСЁ зерно
    // как тихое, хотя большая часть его энергии лежит дальше в источнике.
    std::printf ("\n--- Нормировка зерна на переходе тихо → громко ---\n");
    {
        constexpr int sr = 48000;
        OneShot shot;
        shot.name = "rising source"; shot.rootNote = 45;
        shot.levels.resize (1); shot.levels[0].resize (sr);
        for (int i = 0; i < sr; ++i)
        {
            const double t = (double) i / sr;
            const double a = t < 0.20 ? 0.06 : (t < 0.23 ? 0.06 + 0.34 * (t - 0.20) / 0.03 : 0.40);
            shot.levels[0][(size_t) i] = (float) (a * std::sin (kTwoPi * 220.0 * t));
        }
        buildMipLevels (shot, sr);
        SampleBank b; b.shots.push_back (std::move (shot));
        GranularEngine e; e.setBank (&b); e.setSampleRate (sr); e.start (45, 1.0f);
        EngineParams p;
        p.table = 0; p.p[0] = 0.22f; p.p[1] = 0.45f; p.p[2] = 0.0f; p.p[3] = 0.0f;
        p.detune = 0.0f; p.level = 1.0f; e.prepare (p);
        (void) e.render (p);
        const auto grain = GranularTestProbe::firstGrain (e);
        const auto& source = b.shots[0];
        double eSum = 0.0, wSum = 0.0;
        for (int i = 0; i < 512; ++i)
        {
            const double phase = (i + 0.5) / 512.0;
            const double w = 0.5 - 0.5 * std::cos (kTwoPi * phase);
            const double local = source.envAt (grain[0] + grain[1] * (float) phase);
            eSum += w * w * local * local; wSum += w * w;
        }
        const double spanRms = std::sqrt (eSum / wSum);
        const double local = source.envAt (grain[0]);
        const double body = source.bodyRms;
        const double reference = clamp ((float) (body / std::max (local, spanRms)), 0.25f, 5.0f)
                               * clamp ((float) (GranularEngine::kBodyTarget / body), 0.05f, 6.0f);
        const double gainRatio = grain[2] / std::max (1.0e-9, reference);
        double peak = 0.0, power = 0.0;
        for (int i = 0; i < sr / 5; ++i)
        {
            const float v = e.render (p);
            peak = std::max (peak, std::fabs ((double) v)); power += (double) v * v;
        }
        std::printf ("   старт %.1f мс, локальный RMS %.6f, RMS участка %.6f, усиление %.6f / эталон %.6f\n",
                     grain[0] * 1000.0 / sr, local, spanRms, grain[2], reference);
        std::printf ("   пик %.6f, RMS %.6f, отношение усиления к эталону %.3f\n",
                     peak, std::sqrt (power / (sr / 5)), gainRatio);
        suite::check (grain[1] > 0.0f && spanRms > 2.0 * local && std::fabs (gainRatio - 1.0) < 0.08,
                      "нормировка учитывает реальную энергию участка зерна", suite::num (gainRatio, 3));
        suite::check (peak > 0.10 && peak < 0.90,
                      "атака не усиливается ошибочно в несколько раз", suite::num (peak, 4));
    }

    // На длинном источнике 256 точечных проб могут оказаться по обе стороны
    // от единственной громкой 10-мс ячейки. Строим настоящую запись 30 с,
    // помещаем всплеск между соседними пробами и сравниваем gain зерна с
    // независимым интегралом Hann² по всей огибающей.
    std::printf ("\n--- Длинный источник: короткая атака между пробами ---\n");
    {
        constexpr int sr = 48000;
        OneShot shot; shot.name = "long transient"; shot.rootNote = 45;
        shot.levels.resize (1); shot.levels[0].resize (sr * 30);
        for (int i = 0; i < sr * 30; ++i)
            shot.levels[0][(size_t) i] = (float) (0.03 * std::sin (kTwoPi * 100.0 * i / sr));
        buildMipLevels (shot, sr);
        SampleBank b; b.shots.push_back (std::move (shot));
        GranularEngine e; e.setBank (&b); e.setSampleRate (sr); e.start (115, 1.0f);
        EngineParams p;
        p.table = 0; p.p[0] = 0.5f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f;
        p.detune = 0.0f; p.level = 1.0f; e.prepare (p); (void) e.render (p);
        const auto baseline = GranularTestProbe::firstGrain (e);
        const double a = baseline[0] + baseline[1] * 127.5 / 256.0;
        const double z = baseline[0] + baseline[1] * 128.5 / 256.0;
        const int cell = (int) std::floor (0.5 * (a + z) / 480.0);
        const bool between = cell * 480 > a && (cell + 1) * 480 < z;
        auto& source = b.shots[0];
        if (cell >= 0 && (cell + 1) * 480 < (int) source.levels[0].size())
            for (int i = cell * 480; i < (cell + 1) * 480; ++i)
                source.levels[0][(size_t) i] = (float) (0.80 * std::sin (kTwoPi * 100.0 * i / sr));
        buildMipLevels (source, sr);
        e.start (115, 1.0f); e.prepare (p); (void) e.render (p);
        const auto grain = GranularTestProbe::firstGrain (e);
        double sum = 0.0, weight = 0.0;
        for (int i = 0; i < 262144; ++i)
        {
            const double phase = (i + 0.5) / 262144.0;
            const double w = 0.5 - 0.5 * std::cos (kTwoPi * phase);
            const double env = source.envAt (grain[0] + grain[1] * (float) phase);
            sum += w * w * env * env; weight += w * w;
        }
        const double rms = std::sqrt (sum / weight), local = source.envAt (grain[0]);
        const double body = source.bodyRms;
        const double ideal = clamp ((float) (body / std::max (local, rms)), 0.25f, 5.0f)
                           * clamp ((float) (GranularEngine::kBodyTarget / body), 0.05f, 6.0f);
        const double ratio = grain[2] / std::max (1.0e-9, ideal);
        const double cellEnergy = source.meanEnvEnergy ((double) cell * 480, (double) (cell + 1) * 480);
        const double expectedCell = (double) source.env[(size_t) cell] * source.env[(size_t) cell];
        const double lastEnergy = source.meanEnvEnergy ((double) (source.env.size() + 1) * 480,
                                                        (double) (source.env.size() + 2) * 480);
        const double expectedLast = (double) source.env.back() * source.env.back();
        std::printf ("   проход %.3f с, ячейка между пробами %s, RMS %.6f / старт %.6f\n",
                     grain[1] / sr, between ? "да" : "нет", rms, local);
        std::printf ("   фактическое усиление %.6f / эталон %.6f = %.5f\n", grain[2], ideal, ratio);
        suite::check (between && grain[1] > 480.0f * 256.0f && rms > local * 1.25
                      && std::fabs (ratio - 1.0) < 0.015,
                      "короткая атака не теряется в длинном проходе", suite::num (ratio, 5));
        suite::check (source.envEnergyPrefix.size() == source.env.size() + 1
                      && std::fabs (cellEnergy - expectedCell) < 1.0e-9
                      && std::fabs (lastEnergy - expectedLast) < 1.0e-9,
                      "префикс энергии совпадает с ячейкой и продолжением края");
    }

    // Одна и та же стационарная запись, но 0.5 с и 4 с: высокая нота вынуждает
    // движок укоротить зерно в коротком источнике. Обе ожидаемые плотности
    // перекрытия > 1, поэтому RMS должен оставаться близким без лимитера.
    std::printf ("\n--- Нормировка фактической, а не запрошенной длины зерна ---\n");
    {
        double rms[2] = {};
        int index = 0;
        for (double seconds : { 0.5, 4.0 })
        {
            SampleBank b;
            b.shots.push_back (makeStationaryShot (48000.0, seconds));
            GranularEngine e;
            e.setBank (&b); e.setSampleRate (48000.0); e.start (93, 1.0f);
            EngineParams p;
            p.p[0] = 0.4f; p.p[1] = 0.45f; p.p[2] = 1.0f; p.p[3] = 0.0f; p.level = 1.0f;
            e.prepare (p);
            double energy = 0.0;
            for (int i = 0; i < 48000 * 5; ++i)
            {
                const float v = e.render (p);
                if (i >= 48000) energy += (double) v * v;
            }
            rms[index++] = std::sqrt (energy / (48000.0 * 4.0));
            std::printf ("   источник %.1f с: RMS %.6f\n", seconds, rms[index - 1]);
        }
        const double diffDb = std::fabs (20.0 * std::log10 (std::max (1.0e-12, rms[0])
                                                          / std::max (1.0e-12, rms[1])));
        suite::check (rms[0] > 0.01 && rms[1] > 0.01 && diffDb < 6.0,
                      "укороченные зёрна не теряют уровень дважды", suite::num (diffDb, 2) + " дБ");
    }

    // Позиция контролирует возможный диапазон старта, даже если длина зерна
    // урезана. Проверяем реальные позиции, не сравнение картинок и не RMS.
    std::printf ("\n--- Position остаётся направленным на высокой ноте ---\n");
    {
        SampleBank b;
        b.shots.push_back (makeStationaryShot (48000.0, 2.0));
        double means[5] = {};
        int index = 0, grains = 0;
        for (float position : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            GranularEngine e;
            e.setBank (&b); e.setSampleRate (48000.0); e.start (93, 1.0f);
            EngineParams p;
            p.p[0] = position; p.p[1] = 0.45f; p.p[2] = 1.0f; p.p[3] = 0.0f; p.level = 1.0f;
            e.prepare (p);
            std::array<bool, GranularEngine::kMaxGrains> previous {};
            GranularTestProbe::Stats st;
            for (int i = 0; i < 48000 * 5; ++i)
            {
                (void) e.render (p);
                GranularTestProbe::observe (e, previous, st);
            }
            grains += st.spawned;
            means[index++] = st.spawned > 0 ? st.startSum / st.spawned / 96000.0 : -1.0;
            std::printf ("   position %.2f: средний старт %.2f %% записи (%d зёрен)\n",
                         position, means[index - 1] * 100.0, st.spawned);
        }
        bool increasing = grains > 1000;
        for (int i = 1; i < 5; ++i) increasing = increasing && means[i] - means[i - 1] > 0.025;
        suite::check (increasing && means[4] - means[0] > 0.15,
                      "Position сдвигает распределение стартов вперёд",
                      suite::num ((means[4] - means[0]) * 100.0, 1) + " % длины источника");
    }

    // Намеренно очень короткий источник и крайние скорости. На таких данных
    // минимум 16 выходных отсчётов может физически не помещаться — допустимо
    // не запускать зерно, но недопустимо читать за край или заворачивать sinc.
    std::printf ("\n--- Очень короткий источник: реальные границы sinc ---\n");
    {
        SampleBank b;
        b.shots.push_back (makeStationaryShot (48000.0, 0.05));
        GranularTestProbe::Stats st;
        int bad = 0;
        for (int note : { 69, 93, 117, 127 })
            for (float tune : { 0.0f, 24.0f })
            {
                GranularEngine e;
                e.setBank (&b); e.setSampleRate (48000.0); e.start (note, 1.0f);
                EngineParams p;
                p.p[0] = 0.97f; p.p[1] = 1.0f; p.p[2] = 1.0f; p.p[3] = 1.0f;
                p.tuneSemis = tune; p.level = 1.0f;
                e.prepare (p);
                std::array<bool, GranularEngine::kMaxGrains> previous {};
                for (int i = 0; i < 48000; ++i)
                {
                    if (! std::isfinite (e.render (p))) ++bad;
                    GranularTestProbe::observe (e, previous, st);
                }
            }
        std::printf ("   зёрен %d, выходов %d, fmod %d, заворотов sinc %d, NaN %d\n",
                     st.spawned, st.unsafe, st.wrappedReads, st.kernelWrapReads, bad);
        suite::check (st.spawned > 50 && st.unsafe == 0 && st.wrappedReads == 0
                      && st.kernelWrapReads == 0 && bad == 0,
                      "даже крайние скорости не заворачивают чтение sinc");
    }

    std::printf ("\n--- Источник короче окна огибающей (5 мс) ---\n");
    {
        SampleBank b;
        b.shots.push_back (makeStationaryShot (48000.0, 0.005));
        const bool validBody = b.shots[0].env.empty() && std::isfinite (b.shots[0].bodyRms)
                            && b.shots[0].bodyRms > 1.0e-4f;
        GranularEngine e;
        e.setBank (&b); e.setSampleRate (48000.0); e.start (45, 1.0f);
        EngineParams p;
        p.p[0] = 0.5f; p.p[1] = 0.0f; p.p[2] = 1.0f; p.p[3] = 0.0f; p.level = 1.0f;
        e.prepare (p);
        GranularTestProbe::Stats st;
        std::array<bool, GranularEngine::kMaxGrains> previous {};
        int bad = 0;
        for (int i = 0; i < 48000; ++i)
        {
            if (! std::isfinite (e.render (p))) ++bad;
            GranularTestProbe::observe (e, previous, st);
        }
        std::printf ("   bodyRMS %.6f, уровней %zu, зёрен %d, sinc-заворотов %d, NaN %d\n",
                     b.shots[0].bodyRms, b.shots[0].lens.size(), st.spawned, st.kernelWrapReads, bad);
        suite::check (validBody && st.spawned > 10 && st.unsafe == 0 && st.wrappedReads == 0
                      && st.kernelWrapReads == 0 && bad == 0,
                      "короткая запись безопасна при построении банка и рендере");
    }

    // MIP-соседи обязаны описывать одну и ту же позицию исходника. Иначе их
    // кроссфейд гасит тон; простое отношение длин буферов не исправляет задержку
    // КИХ-дециматора. Замеряем фазу и амплитуду на стационарном синусе.
    // Независимый контроль границы 10-мс RMS-огибающей. При отсутствии
    // ячеек envAt() возвращает 1.0, что не является RMS короткого источника:
    // до исправления изменение длины на один отсчёт давало скачок до 12 дБ.
    std::printf ("\n--- Нормировка короткого источника на границе 10 мс ---\n");
    {
        struct ShortResult { double rms = 0.0, gain = 0.0; int envCells = -1; bool finite = true; };
        auto renderShort = [] (double sr, int sourceSamples, double sourceAmplitude, int note)
        {
            OneShot shot; shot.name = "short sine"; shot.rootNote = 45;
            shot.levels.resize (1); shot.levels[0].resize ((size_t) sourceSamples);
            for (int i = 0; i < sourceSamples; ++i)
                shot.levels[0][(size_t) i] = (float) (sourceAmplitude * std::sin (kTwoPi * 1000.0 * i / sr));
            buildMipLevels (shot, sr);
            SampleBank b; b.shots.push_back (std::move (shot));
            GranularEngine e; e.setBank (&b); e.setSampleRate (sr); e.start (note, 1.0f);
            EngineParams p; p.table = 0; p.p[0] = 0.35f; p.p[1] = 1.0f;
            p.p[2] = 0.0f; p.p[3] = 0.0f; p.detune = 0.0f; p.level = 1.0f;
            e.prepare (p);
            ShortResult out; out.envCells = (int) b.shots[0].env.size();
            double energy = 0.0;
            const int n = (int) (sr * 0.01); // первое зерно; следующее не раньше 250 мс
            for (int i = 0; i < n; ++i)
            {
                const float v = e.render (p);
                out.finite = out.finite && std::isfinite (v);
                if (i == 0) out.gain = GranularTestProbe::firstGrain (e)[2];
                energy += (double) v * v;
            }
            out.rms = std::sqrt (energy / std::max (1, n));
            return out;
        };

        double worstBoundaryDb = 0.0, worstGainDb = 0.0, worstScaleDb = 0.0;
        int boundaryCases = 0, scaleCases = 0;
        bool valid = true, silenceValid = true;
        for (int sr : { 44100, 48000, 96000 })
        {
            const int step = (int) (sr * 0.01);
            for (double amplitude : { 0.03, 0.30, 0.80 })
            {
                const auto below = renderShort (sr, step - 1, amplitude, 45);
                const auto at = renderShort (sr, step, amplitude, 45);
                valid = valid && below.finite && at.finite && below.envCells == 0 && at.envCells == 1
                      && below.rms > 1.0e-7 && below.gain > 0.0 && at.gain > 0.0;
                const double rmsDb = 20.0 * std::log10 (std::max (1.0e-20, at.rms)
                                                         / std::max (1.0e-20, below.rms));
                const double gainDb = 20.0 * std::log10 (std::max (1.0e-20, at.gain)
                                                          / std::max (1.0e-20, below.gain));
                worstBoundaryDb = std::max (worstBoundaryDb, std::fabs (rmsDb));
                worstGainDb = std::max (worstGainDb, std::fabs (gainDb));
                std::printf ("   %d Гц, ампл. %.2f: длина %d/%d, gain %.6f/%.6f, RMS %.6f/%.6f, Δ %+.3f дБ\n",
                             sr, amplitude, step - 1, step, below.gain, at.gain, below.rms, at.rms, rmsDb);
                ++boundaryCases;
            }
            // Одинаковый 5-мс тон с амплитудами 0.3 и 0.8: нота ниже,
            // чтобы 16 безопасных отсчётов зерна помещались на 44.1 кГц.
            const auto quiet = renderShort (sr, step / 2, 0.30, 36);
            const auto loud  = renderShort (sr, step / 2, 0.80, 36);
            const double db = 20.0 * std::log10 (std::max (1.0e-20, loud.rms)
                                                  / std::max (1.0e-20, quiet.rms));
            valid = valid && quiet.finite && loud.finite && quiet.envCells == 0 && loud.envCells == 0
                  && quiet.gain > 0.0 && loud.gain > 0.0 && quiet.rms > 1.0e-7;
            worstScaleDb = std::max (worstScaleDb, std::fabs (db));
            std::printf ("   %d Гц, 5 мс, амплитуда 0.30 -> 0.80: RMS %.6f/%.6f, Δ %+.3f дБ\n",
                         sr, quiet.rms, loud.rms, db);
            ++scaleCases;
            const auto silent = renderShort (sr, step - 1, 0.0, 45);
            silenceValid = silenceValid && silent.finite && silent.rms == 0.0;
        }
        suite::check (valid && boundaryCases == 9 && worstBoundaryDb < 0.20 && worstGainDb < 0.10,
                      "один отсчёт у границы 10 мс не меняет уровень зерна скачком",
                      suite::num (worstBoundaryDb, 3) + " дБ RMS, " + suite::num (worstGainDb, 3) + " дБ gain");
        suite::check (valid && scaleCases == 3 && worstScaleDb < 0.05,
                      "калибровка 5-мс источника не зависит от масштаба записи",
                      suite::num (worstScaleDb, 3) + " дБ");
        suite::check (silenceValid, "короткая тишина не создаёт сигнал и NaN/Inf");
    }

    std::printf ("\n--- Фазовое совпадение соседних MIP-уровней ---\n");
    {
        const OneShot s = makeStationaryShot (48000.0, 2.0);
        double worstPhase = 0.0, worstAmpError = 0.0;
        int crossfades = 0;
        for (float rate : { 1.04427378f, 2.08854756f, 4.17709512f, 8.35419024f,
                            1.41421356f, 2.82842712f, 5.65685425f, 11.3137085f })
        {
            MipReader r;
            r.setup (s, rate);
            if (r.wA > 0.49f && r.wA < 0.51f && r.wB > 0.49f && r.wB < 0.51f) ++crossfades;
            double sinDot = 0.0, cosDot = 0.0;
            for (int i = 24000; i < 72000; ++i)  // ровно 110 периодов
            {
                const double ph = kTwoPi * 110.0 * i / 48000.0;
                const double v = r.read ((float) i);
                sinDot += v * std::sin (ph);
                cosDot += v * std::cos (ph);
            }
            const double amp = std::hypot (sinDot, cosDot) / (0.30 * 24000.0);
            const double phaseDeg = std::atan2 (cosDot, sinDot) * 180.0 / kPi;
            worstAmpError = std::max (worstAmpError, std::fabs (1.0 - amp));
            worstPhase = std::max (worstPhase, std::fabs (phaseDeg));
            std::printf ("   rate %.3f: амплитуда %.4f, фаза %.2f°\n", rate, amp, phaseDeg);
        }
        suite::check (crossfades == 4 && worstAmpError < 0.03 && worstPhase < 2.0,
                      "соседние MIP-уровни не гасят общий тон",
                      "ошибка амплитуды " + suite::num (worstAmpError * 100.0, 2)
                      + " %, фазы " + suite::num (worstPhase, 2) + "°, переходов "
                      + suite::num (crossfades, 0));
    }

    // Нечётная длина уровня — результат отбрасывания последнего отсчёта,
    // а не сдвиг sample rate. Проверяем масштаб и реконструкцию на уровне 8.
    std::printf ("\n--- Нечётная длина источника не расстраивает MIP ---\n");
    {
        const OneShot s = makeStationaryShot (48000.0, 48001.0 / 48000.0, 0.0, 50.0);
        MipReader r;
        r.setup (s, 256.0f);
        const double expected = 1.0 / 256.0;
        double error = 0.0, energy = 0.0;
        for (int i = 5000; i < 40000; ++i)
        {
            const double ideal = 0.30 * std::sin (kTwoPi * 50.0 * i / 48000.0);
            const double d = r.read ((float) i) - ideal;
            error += d * d; energy += ideal * ideal;
        }
        const double rel = std::sqrt (error / std::max (1.0e-20, energy));
        std::printf ("   len0 %d, len8 %d, scale %.9f (ожидалось %.9f), ΔRMS %.5f\n",
                     s.lens[0], r.lenA, r.invScaleA, expected, rel);
        suite::check (s.lens.size() > 8 && r.lenA == 187
                      && std::fabs ((double) r.invScaleA - expected) < 1.0e-9 && rel < 0.02,
                      "шкала позиции задаётся прореживанием 2^L, а не длиной буфера");
    }

    // Стоп-полоса важна, но нельзя оплачивать её исчезновением слышимой
    // полосы: проверяем одновременно тон 14.4 кГц и alias тона, который на
    // 1% выше Найквиста (24.24 → 23.76 кГц).
    // Координата внутри зерна много раз увеличивается на дробный rate.
    // При float-коплении при высоком абсолютном pos каждый шаг округляется
    // к сетке 1/128, 1/64 и т. д.; это не случайная ошибка фазы, а неверный
    // фактический pitch. Тестируем и фабричные 2 с, и длинный источник.
    std::printf ("\n--- Точность курса зерна при позднем Position ---\n");
    {
        auto measureCents = [] (SampleBank& b, double sr, int note, bool reverse = false) {
            GranularEngine e; e.setBank (&b); e.setSampleRate (sr); e.start (note, 1.0f);
            EngineParams p;
            p.p[0] = 0.95f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f;
            p.detune = 0.0f; p.level = 1.0f; p.grainReverse = reverse ? 1.0f : 0.0f;
            e.prepare (p); (void) e.render (p);
            const auto first = GranularTestProbe::firstCursor (e);
            for (int i = 0; i < 2000; ++i) (void) e.render (p);
            const auto last = GranularTestProbe::firstCursor (e);
            if ((reverse && (first[1] >= 0.0 || last[1] >= 0.0))
                || (! reverse && (first[1] <= 0.0 || last[1] <= 0.0))) return 1.0e6;
            const double actual = (last[0] - first[0]) / 2000.0;
            return actual * first[1] > 0.0 ? 1200.0 * std::log2 (actual / first[1]) : 1.0e6;
        };
        double worstFactory = 0.0, worstReverseFactory = 0.0;
        int factoryCases = 0;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            SampleBank b; b.shots.push_back (makeStationaryShot (sr, 2.0));
            for (int note : { 37, 38, 39, 46, 60 })
            {
                worstFactory = std::max (worstFactory, std::fabs (measureCents (b, sr, note)));
                worstReverseFactory = std::max (worstReverseFactory, std::fabs (measureCents (b, sr, note, true)));
                ++factoryCases;
            }
        }
        std::printf ("   2 с / 44.1–96 кГц: %d случаев, худшая ошибка %.6f цента\n", factoryCases, worstFactory);
        suite::check (factoryCases == 15 && worstFactory < 0.02,
                      "фактическая скорость соответствует pitch в фабричном источнике",
                      suite::num (worstFactory, 6) + " цента");
        suite::check (factoryCases == 15 && worstReverseFactory < 0.02,
                      "обратный курс сохраняет pitch на 44.1–96 кГц",
                      suite::num (worstReverseFactory, 6) + " цента");

        constexpr double sr = 96000.0, toneHz = 8000.0;
        SampleBank longBank;
        longBank.shots.push_back (makeStationaryShot (sr, 26.0, 0.0, toneHz));
        double worstLong = 0.0, worstReverseLong = 0.0;
        for (int note : { 36, 37, 38, 46 })
        {
            worstLong = std::max (worstLong, std::fabs (measureCents (longBank, sr, note)));
            worstReverseLong = std::max (worstReverseLong, std::fabs (measureCents (longBank, sr, note, true)));
        }
        std::printf ("   26 с / 96 кГц: худшая ошибка %.6f / %.6f цента (прямо / реверс)\n",
                     worstLong, worstReverseLong);
        suite::check (worstLong < 0.02, "длинная запись не расстраивает зерно",
                      suite::num (worstLong, 6) + " цента");
        suite::check (worstReverseLong < 0.02, "длинная запись не расстраивает обратное зерно",
                      suite::num (worstReverseLong, 6) + " цента");

        // Независимая проверка интерполятора на заданных double-координатах:
        // идеальный тон известен аналитически. Низкая ошибка короткого начала
        // не должна ухудшаться из-за большой целой части позиции.
        const auto& shot = longBank.shots[0];
        MipReader rd; const float rate = (float) std::pow (2.0, -9.0 / 12.0);
        const bool ready = rd.setupGranular (shot, rate);
        double rel[2] = {}; int which = 0;
        for (double seconds : { 0.05, 24.5 })
        {
            double squared = 0.0, reference = 0.0;
            for (int i = 0; i < 8192; ++i)
            {
                const double x = seconds * sr + 0.123 + i * (double) rate;
                const double ideal = 0.30 * std::sin (kTwoPi * toneHz * x / sr);
                const double d = (double) rd.read (x) - ideal;
                squared += d * d; reference += ideal * ideal;
            }
            rel[which++] = std::sqrt (squared / reference);
        }
        std::printf ("   синус 8 кГц: ошибка интерполяции RMS %.8f / %.8f (начало / 24.5 с)\n", rel[0], rel[1]);
        suite::check (ready && rel[0] < 0.0002 && rel[1] < 0.0002,
                      "дробная позиция не теряет точность на длинном источнике",
                      suite::num (rel[0], 8) + " / " + suite::num (rel[1], 8));

        // Полный выход движка: частота измеряется по пересечениям нуля, не по
        // внутреннему счётчику. Один Hann-зерно, без FX и смены параметров.
        GranularEngine e; e.setBank (&longBank); e.setSampleRate (sr); e.start (36, 1.0f);
        EngineParams p;
        p.p[0] = 0.95f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f;
        p.detune = 0.0f; p.level = 1.0f; e.prepare (p);
        double firstCross = 0.0, lastCross = 0.0; int crosses = 0; float previous = 0.0f;
        for (int i = 0; i < (int) (0.18 * sr); ++i)
        {
            const float value = e.render (p);
            if (i > (int) (0.02 * sr) && previous < 0.0f && value >= 0.0f)
            {
                const double cross = i - 1.0 + (double) (-previous) / (double) (value - previous);
                if (crosses++ == 0) firstCross = cross;
                lastCross = cross;
            }
            previous = value;
        }
        const double measured = crosses > 1 ? (crosses - 1) * sr / (lastCross - firstCross) : 0.0;
        const double expected = toneHz * (double) rate;
        const double pitchError = measured > 0.0 ? 1200.0 * std::log2 (measured / expected) : 1.0e6;
        std::printf ("   выход движка: %.6f Гц / эталон %.6f Гц, ошибка %+.6f цента, переходов %d\n",
                     measured, expected, pitchError, crosses);
        suite::check (crosses > 100 && std::fabs (pitchError) < 0.05,
                      "измеренная частота полного зерна не зависит от позиции",
                      suite::num (pitchError, 6) + " цента");

        GranularEngine reverseEngine;
        reverseEngine.setBank (&longBank); reverseEngine.setSampleRate (sr); reverseEngine.start (36, 1.0f);
        p.grainReverse = 1.0f; reverseEngine.prepare (p);
        firstCross = lastCross = 0.0; crosses = 0; previous = 0.0f;
        for (int i = 0; i < (int) (0.18 * sr); ++i)
        {
            const float value = reverseEngine.render (p);
            if (i > (int) (0.02 * sr) && previous < 0.0f && value >= 0.0f)
            {
                const double cross = i - 1.0 + (double) (-previous) / (double) (value - previous);
                if (crosses++ == 0) firstCross = cross;
                lastCross = cross;
            }
            previous = value;
        }
        const double reverseHz = crosses > 1 ? (crosses - 1) * sr / (lastCross - firstCross) : 0.0;
        const double reverseError = reverseHz > 0.0 ? 1200.0 * std::log2 (reverseHz / expected) : 1.0e6;
        std::printf ("   выход reverse: %.6f Гц / эталон %.6f Гц, ошибка %+.6f цента\n",
                     reverseHz, expected, reverseError);
        suite::check (crosses > 100 && std::fabs (reverseError) < 0.05,
                      "измеренная частота обратного зерна точна", suite::num (reverseError, 6) + " цента");
    }

    std::printf ("\n--- Дробный pitch: полоса 14.4 кГц и alias у Найквиста ---\n");
    {
        double worstBandError = 0.0, worstAlias = -200.0, minReference = 1.0e30;
        for (double rate : { 17.6, 20.0, 24.0, 48.0, 150.0 })
        {
            const auto band = twoToneGranularDb (rate, 14400.0, 14400.0);
            const auto alias = twoToneGranularDb (rate, 24240.0, 23760.0);
            worstBandError = std::max (worstBandError, std::fabs (band.first));
            worstAlias = std::max (worstAlias, alias.first);
            minReference = std::min (minReference, std::min (band.second, alias.second));
            std::printf ("   rate %.1f: полоса %.2f дБ, alias %.2f дБ\n",
                         rate, band.first, alias.first);
        }
        suite::check (worstBandError < 3.5 && minReference > 100.0,
                      "частота 14.4 кГц не пропадает между уровнями MIP",
                      "макс. |Δ| " + suite::num (worstBandError, 2) + " дБ");
        suite::check (worstAlias < -60.0 && minReference > 100.0,
                      "alias у Найквиста остаётся подавлен",
                      "худший " + suite::num (worstAlias, 2) + " дБ");
    }
    std::printf ("\n--- Reverse: полоса 14.4 кГц и alias у Найквиста ---\n");
    {
        double worstBandError = 0.0, worstAlias = -200.0, minReference = 1.0e30;
        for (double rate : { 17.6, 20.0, 24.0, 48.0, 150.0 })
        {
            const auto band = twoToneGranularDb (rate, 14400.0, 14400.0, 1.0f);
            const auto alias = twoToneGranularDb (rate, 24240.0, 23760.0, 1.0f);
            worstBandError = std::max (worstBandError, std::fabs (band.first));
            worstAlias = std::max (worstAlias, alias.first);
            minReference = std::min (minReference, std::min (band.second, alias.second));
            std::printf ("   reverse |rate| %.1f: полоса %.2f дБ, alias %.2f дБ\n", rate, band.first, alias.first);
        }
        suite::check (worstBandError < 3.5 && minReference > 100.0,
                      "14.4 кГц сохраняется и при реверсе", "макс. |Δ| " + suite::num (worstBandError, 2) + " дБ");
        suite::check (worstAlias < -60.0 && minReference > 100.0,
                      "alias у Найквиста подавлен и при реверсе", "худший " + suite::num (worstAlias, 2) + " дБ");
    }

    // Тон выше НИКВИСТА после изменения питча заворачивается внутрь слышимой
    // полосы; прежняя метрика энергии «выше полосы источника» его не замечала.
    // В источнике две одинаковые по уровню частоты: 110 Гц и 0.55*SR/rate.
    // Вторая заворачивается на 0.45*SR. Сравниваем энергии непосредственно в
    // сигнале Granular, а не только в отдельном ридере.
    std::printf ("\n--- Завёрнутый тон на высоких и дробных pitch-rate ---\n");
    {
        constexpr double sr = 48000.0;
        constexpr int N = 2048, windows = 24;
        double worstDb = -300.0;
        bool finite = true;
        for (double rate : { 16.0, 24.0, 32.0, 48.0, 64.0 })
        {
            SampleBank b;
            b.shots.push_back (makeStationaryShot (sr, 4.0, 0.55 * sr / rate));
            GranularEngine e;
            e.setBank (&b); e.setSampleRate (sr); e.start (93, 1.0f);
            EngineParams p;
            p.p[0] = 0.4f; p.p[1] = 0.45f; p.p[2] = 1.0f; p.p[3] = 0.0f;
            p.tuneSemis = (float) (12.0 * std::log2 (rate / 16.0)); p.level = 1.0f;
            p.detune = 0.0f;  // иначе alias сдвинется относительно измерительного бина
            e.prepare (p);
            for (int i = 0; i < 24000; ++i) (void) e.render (p);
            double lowEnergy = 0.0, aliasEnergy = 0.0;
            for (int win = 0; win < windows; ++win)
            {
                double ls = 0.0, lc = 0.0, hs = 0.0, hc = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    const float v = e.render (p);
                    finite = finite && std::isfinite (v);
                    const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / (N - 1));
                    const double loPh = kTwoPi * 110.0 * rate * i / sr;
                    const double hiPh = kTwoPi * 0.45 * i;
                    ls += w * v * std::sin (loPh); lc += w * v * std::cos (loPh);
                    hs += w * v * std::sin (hiPh); hc += w * v * std::cos (hiPh);
                }
                lowEnergy += ls * ls + lc * lc;
                aliasEnergy += hs * hs + hc * hc;
            }
            const double db = 10.0 * std::log10 (std::max (1.0e-30, aliasEnergy)
                                                  / std::max (1.0e-30, lowEnergy));
            finite = finite && lowEnergy > 1.0e-8 && std::isfinite (db);
            worstDb = std::max (worstDb, db);
            std::printf ("   rate %.0f: завёрнутый тон относительно основы %.1f дБ\n", rate, db);
        }
        suite::check (finite && worstDb < -35.0,
                      "завёрнутый тон подавлен при целом и дробном pitch-rate",
                      suite::num (worstDb, 1) + " дБ (худший случай)");
    }

    fails = suite::gFailures - before;
    std::printf ("\n=================================================\n");
    if (fails == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else            std::printf ("ЕСТЬ ПРОВАЛЫ (провалов: %d)\n", fails);
    return fails;
}
