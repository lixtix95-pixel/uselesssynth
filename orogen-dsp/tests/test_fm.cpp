// Тесты движка FM: общий стенд + спецтесты (правило Карсона, пресеты).
#include "../src/engines/fm.h"
#include "engine_suite.h"

#include <cstdio>

using namespace orogen;

// ---------------------------------------------------------------------------
// Правило Карсона: число значимых боковых полос ≈ index + 1.
// Считаем спектр, находим бины выше -X дБ от максимума и проверяем,
// что их количество растёт линейно с индексом (наклон ≈ 1).
// ---------------------------------------------------------------------------
static int countSidebands (float idxAmount, int note = 45)
{
    const double sr = 48000.0, f0 = midiToHz (note);
    FmEngine eng;
    EngineParams p;
    // ratio РОВНО 1.0: боковые полосы попадают на k·f0, иначе спектр ингармонический
    // по замыслу и «число гармоник» измерить нельзя
    p.p[0] = (1.0f - 0.5f) / 15.5f;
    p.p[1] = idxAmount;
    p.p[2] = 0.0f;
    p.p[3] = 0.0f;
    p.detune = 0.0f;
    p.level = 1.0f;

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
    // Считаем ГАРМОНИКИ, а не бины: у каждой гармоники окно размазывает
    // главный лепесток на ±4 бина, и «считать бины» даёт завышение в 3-5 раз.
    const double binHz = sr / N;
    int count = 0;
    for (int k = 1; k <= 400; ++k)
    {
        const int   centre = (int) std::lround (k * f0 / binHz);
        if (centre + 5 >= N / 2) break;
        double localMax = 0.0;
        for (int b = std::max (1, centre - 5); b <= centre + 5; ++b)
            localMax = std::max (localMax, (double) std::abs (spec[(size_t) b]));
        if (localMax >= peak * 0.01) ++count;                         // -40 дБ
    }
    return count;
}

int main()
{
    std::printf ("OROGEN — движок FM\n=================================================\n");

    suite::Config cfg;
    cfg.name = "FM (общий стенд)";
    cfg.make = [] { return std::unique_ptr<EngineBase> (new FmEngine()); };
    cfg.params = [] (int) {
        EngineParams p;
        // ДЛЯ ЗАМЕРА АЛИАСИНГА — ЦЕЛОЕ ratio: у нецелого спектр ингармонический
        // по замыслу, и энергетическая метрика сочла бы его алиасингом.
        // Нецелые ratio и их «законную» ингармоничность проверяем отдельно ниже.
        p.p[0] = (3.0f - 0.5f) / 15.5f;   // ratio = 3.0
        p.p[1] = 0.45f;    // index ≈ 3.6 рад
        // второй модулятор (ratio 1.5x) и feedback дают законную ингармонику —
        // для замера алиасинга их отключаем, они проверяются отдельно
        p.p[2] = 0.0f;
        p.p[3] = 0.0f;
        p.detune = 0.3f;
        p.level = 1.0f;
        return p;
    };
    cfg.aliasTargetDb = -60.0;
    suite::run (cfg);

    // ---------------------------------------------------------------- Карсон
    std::printf ("\n--- Правило Карсона: число значимых полос ~ index + 1 ---\n");
    std::printf ("   заявленный индекс   полос (измерено)   ожидание\n");
    struct R { float idx; int bands; };
    std::vector<R> res;
    for (float a : { 0.05f, 0.15f, 0.30f, 0.50f, 0.70f, 0.90f })
    {
        const int bands = countSidebands (a);
        res.push_back ({ a * 8.0f, bands });
        std::printf ("        %5.1f рад            %2d              ~%d\n", a * 8.0f, bands, (int) (a * 8.0f + 1));
    }
    // проверяем монотонный рост и масштаб
    bool mono = true;
    for (size_t i = 1; i < res.size(); ++i) if (res[i].bands < res[i - 1].bands) mono = false;
    suite::check (mono, "ширина спектра растёт с индексом");
    // Карсон: полос ~ index+1 по модулю знака -> всего ~2·index+1
    // при ratio = 1 боковые полосы лежат на ±k·f0, значит значимых гармоник
    // примерно index+1 (Карсон: BW = 2(Δf+fm) = 2·fm·(index+1))
    const double predicted = res.back().idx + 1.0;
    const double actual    = (double) res.back().bands;
    suite::check (actual > predicted * 0.6 && actual < predicted * 2.2,
                  "ширина совпадает с правилом Карсона (index+1 гармоник)",
                  "измерено " + std::to_string ((int) actual) + ", предсказано ~"
                  + std::to_string ((int) predicted));

    // ---------------------------------------------------------------- ratio
    std::printf ("\n--- Целое vs нецелое ratio (гармонический vs ингармонический) ---\n");
    for (float ratioN : { 1.0f, 2.0f, 3.0f, 2.5f, 3.7f })
    {
        FmEngine eng;
        EngineParams p;
        p.p[0] = (ratioN - 0.5f) / 15.5f;      // обратное преобразование в p[0]
        p.p[1] = 0.5f; p.p[2] = 0.0f; p.p[3] = 0.0f;
        p.detune = 0.0f;
        const double db = suite::aliasDb (eng, p, 45, 48000.0);
        std::printf ("   ratio %4.1f -> не-гармоническая энергия %7.1f дБ %s\n",
                     ratioN, db, ratioN == std::floor (ratioN) ? "(ожидаемо чисто)" : "(ингармоника — так и должно быть)");
    }

    // ---------------------------------------------------------------- пресеты
    std::printf ("\n--- Пресеты FM ---\n");
    double mn = 99, mx = 0;
    for (int i = 0; i < 6; ++i)
    {
        const auto& pr = fmPreset (i);
        FmEngine eng;
        EngineParams p;
        p.p[0] = pr.p[0]; p.p[1] = pr.p[1]; p.p[2] = pr.p[2]; p.p[3] = pr.p[3];
        p.detune = 0.3f; p.level = 1.0f;
        eng.setSampleRate (48000.0);
        eng.start (57, 1.0f);
        eng.prepare (p);
        double acc = 0.0; float peak = 0.0f;
        for (int k = 0; k < 24000; ++k)
        {
            const float s = eng.render (p);
            acc += (double) s * s;
            peak = std::max (peak, std::fabs (s));
        }
        const double rms = std::sqrt (acc / 24000.0);
        mn = std::min (mn, rms); mx = std::max (mx, rms);
        std::printf ("   %-8s rms %.3f  peak %.2f  ratio ~%.1f\n", pr.name, rms, peak,
                     0.5f + pr.p[0] * 15.5f);
    }
    const double spread = 20.0 * std::log10 (mx / mn);
    std::printf ("   разброс между пресетами: %.1f дБ\n", spread);
    suite::check (spread < 10.0, "пресеты не «скачут» по громкости");

    std::printf ("\n=================================================\n%s (провалов: %d)\n",
                 suite::gFailures == 0 ? "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" : "ЕСТЬ ПРОВАЛЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
