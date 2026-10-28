// Тесты движка CHOP: общий стенд + спецтесты слайсера (детектор атак, раскладка
// по клавишам, транспонирование, повторы, реверс, обрыв куска без щелчка).
#include "../src/engines/chop.h"
#include "engine_suite.h"
#include <cstdio>

using namespace orogen;

// Окно анализа алиасинга внутри слайса: короткий удар не покрывает 16384
// сэмплов, утечка от обрыва считалась бы алиасингом.
static int fftForSlice (int note, double sr = 48000.0)
{
    const double rate = std::pow (2.0, (note - 45) / 12.0);
    const double active = 96000.0 / rate;          // ~2 с лупа, ускоренного в rate
    int n = 16384;
    while (n > 512 && (double) n > 0.35 * active) n >>= 1;
    return n;
}

int main()
{
    std::printf ("OROGEN — движок CHOP (слайсер)\n=================================================\n");

    LoopBank bank;
    auto t0 = std::chrono::high_resolution_clock::now();
    bank.buildFactory();
    auto t1 = std::chrono::high_resolution_clock::now();
    size_t bytes = 0;
    for (auto& l : bank.loops) for (auto& v : l.shot.levels) bytes += v.size() * 4;
    std::printf ("\nконтент: %zu лупа, %.1f МБ, построено за %.0f мс\n",
                 bank.loops.size(), bytes / 1e6,
                 std::chrono::duration<double, std::milli> (t1 - t0).count());

    // ---------------------------------------------------------------- 1
    std::printf ("\n--- 1. Детектор атак: найденные границы против истины ---\n");
    int totalHits = 0, matched = 0, spurious = 0;
    double worstOffsetMs = 0.0;
    for (auto& l : bank.loops)
    {
        const int tol = (int) (0.025 * 48000.0);        // 25 мс
        int hit = 0;
        for (int s : l.truth)
        {
            int best = 1 << 30;
            for (int f : l.slices) best = std::min (best, std::abs (f - s));
            if (best <= tol) { ++hit; worstOffsetMs = std::max (worstOffsetMs, best / 48.0); }
        }
        // служебный слайс в 0 (если удар не в начале) не считается лишним
        int extra = 0;
        for (int f : l.slices)
        {
            if (f == 0) continue;
            bool hasTruth = false;
            for (int s : l.truth) if (std::abs (f - s) <= tol) hasTruth = true;
            if (! hasTruth) ++extra;
        }
        std::printf ("   %-10s слайсов %2zu, ударов %2zu, совпало %2d, лишних %d\n",
                     l.shot.name.c_str(), l.slices.size(), l.truth.size(), hit, extra);
        totalHits += (int) l.truth.size();
        matched += hit;
        spurious += extra;
    }
    suite::check (matched == totalHits, "все удары найдены детектором",
                  suite::num (matched, 0) + " из " + suite::num (totalHits, 0));
    suite::check (worstOffsetMs < 25.0, "границы не дальше 25 мс от истинного удара",
                  suite::num (worstOffsetMs, 1) + " мс худшее");
    suite::check (spurious == 0, "ложных слайсов нет", suite::num (spurious, 0));

    // чувствительность: выше порог — меньше слайсов (детектор реально управляем)
    {
        const auto strict = detectSlices (bank.loops[0].shot.levels[0], 48000.0, 0.05f);
        const auto loose  = detectSlices (bank.loops[0].shot.levels[0], 48000.0, 1.0f);
        std::printf ("   порог sens 0.05 -> слайсов %zu, sens 1.0 -> %zu\n",
                     strict.starts.size(), loose.starts.size());
        // при sens = 0.05 порог высокий — остаются только сильные удары (5 из 5),
        // при sens = 1.0 в брейке начинают резаться и хэты (это и есть «больше срезов»,
        // а не ошибка: значение по умолчанию 0.5)
        const size_t truthN = bank.loops[0].truth.size();
        suite::check (strict.starts.size() == truthN && loose.starts.size() > truthN,
                      "чувствительность управляет порогом: 0.05 — только сильные удары, 1.0 — и тихие",
                      suite::num ((double) strict.starts.size(), 0) + " / "
                      + suite::num ((double) loose.starts.size(), 0) + " при "
                      + suite::num ((double) truthN, 0) + " ударах");
    }

    // ---------------------------------------------------------------- 2
    std::printf ("\n--- 2. Раскладка по клавишам: нота = слайс ---\n");
    {
        EngineParams p;
        p.table = 0; p.p[0] = 0.0f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f;
        p.level = 1.0f; p.detune = 0.0f;
        const int n = (int) bank.loops[0].slices.size();
        bool ok = true;
        std::printf ("     нота  слайс  старт(мс)  rate\n");
        for (int i = 0; i < n; ++i)
        {
            ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
            e.start (45 + i, 1.0f); e.prepare (p);
            const int startMs = (int) (1000.0 * e.sliceStartSample() / 48000.0);
            std::printf ("      %3d    %3d      %5d   %.3f\n", 45 + i, e.currentSlice(), startMs, e.playbackRate());
            if (e.currentSlice() != i) ok = false;
            if (std::fabs (e.playbackRate() - 1.0f) > 1.0e-4f) ok = false;
        }
        suite::check (ok, "ноты подряд играют слайсы по порядку, высота не меняется");
        // выше последнего слайса — заворот по кругу
        ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
        e.start (45 + n, 1.0f); e.prepare (p);
        suite::check (e.currentSlice() == 0, "нота за последним слайсом заворачивает на первый",
                      "слайс " + suite::num (e.currentSlice(), 0));
    }

    // ---------------------------------------------------------------- 3
    std::printf ("\n--- 3. Транспонирование (режим Chromatic) ---\n");
    {
        EngineParams p;
        p.table = 0; p.p[0] = 0.10f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f; p.level = 1.0f;
        bool ok = true;
        std::printf ("     нота  слайс  rate   длина выхода, сэмплов\n");
        for (int note : { 45, 57, 69, 45 })
        {
            ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
            e.start (note, 1.0f); e.prepare (p);
            int len = 0;
            for (int i = 0; i < 240000; ++i) { (void) e.render (p); if (e.playing()) len = i + 1; }
            const double expect = e.sliceLength() / e.playbackRate();
            std::printf ("      %3d    %3d   %.2f   %6d (ожидание %.0f)\n",
                         note, e.currentSlice(), e.playbackRate(), len, expect);
            if (std::fabs (e.playbackRate() - std::pow (2.0, (note - 45) / 12.0)) > 2.0e-3) ok = false;
            if (std::fabs ((double) len - expect) > 0.05 * expect + 64.0) ok = false;
        }
        suite::check (ok, "нота транспонирует слайс: rate 2^(n/12), длина падает обратно пропорционально");
    }

    // ---------------------------------------------------------------- 4
    std::printf ("\n--- 4. Повторы и реверс ---\n");
    {
        EngineParams p;
        p.table = 0; p.p[0] = 0.10f; p.p[1] = 0.35f; p.p[2] = 0.0f; p.p[3] = 0.0f; p.level = 1.0f;
        auto measure = [&] (float repeatP, float reverseP)
        {
            EngineParams q = p; q.p[2] = repeatP; q.p[3] = reverseP;
            ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
            e.start (45, 1.0f); e.prepare (q);
            int last = 0, bursts = 0; float prev = 0.0f;
            for (int i = 0; i < 240000; ++i)
            {
                const float s = e.render (q);
                if (std::fabs (s) > 1.0e-6f) last = i;
                if (std::fabs (s) > 0.02f && std::fabs (prev) <= 0.02f) ++bursts;
                prev = s;
            }
            return std::pair<int, int> (last, bursts);
        };
        const auto one = measure (0.0f, 0.0f);
        const auto four = measure (1.0f, 0.0f);
        std::printf ("     повторов 1: %d сэмплов, всплесков %d\n", one.first, one.second);
        std::printf ("     повторов 4: %d сэмплов, всплесков %d\n", four.first, four.second);
        suite::check (four.first > one.first * 3, "p2 = 1 растягивает кусок вчетверо",
                      suite::num (one.first, 0) + " -> " + suite::num (four.first, 0));
        suite::check (four.second > one.second + 1, "повторы слышны как отдельные всплески",
                      suite::num (four.second, 0) + " всплесков");

        // реверс: у прямого слайса атака в начале, у реверсивного — в конце
        auto firstHalfRms = [&] (float reverseP)
        {
            EngineParams q = p; q.p[2] = 0.0f; q.p[3] = reverseP;
            ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
            e.start (45, 1.0f); e.prepare (q);
            double a = 0.0, b = 0.0;
            const int len = (int) e.sliceLength();
            for (int i = 0; i < len; ++i)
            {
                const float s = e.render (q);
                (i < len / 2 ? a : b) += (double) s * s;
            }
            return a / (len / 2) - b / (len - len / 2);
        };
        const double fwd = firstHalfRms (0.0f), rev = firstHalfRms (1.0f);
        std::printf ("     энергия начало-минус-конец: прямой %+.5f, реверс %+.5f\n", fwd, rev);
        suite::check (fwd > 0.0 && rev < 0.0, "реверс меняет направление куска");
    }

    // ---------------------------------------------------------------- 5
    std::printf ("\n--- 5. Обрыв куска: щелчков нет ---\n");
    {
        EngineParams p;
        p.table = 0; p.p[0] = 0.10f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f; p.level = 1.0f;
        bool ok = true;
        for (int note : { 45, 52, 57 })
        {
            ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
            e.start (note, 1.0f); e.prepare (p);
            float prev = 0.0f, worstDelta = 0.0f, natural = 0.0f;
            int atEnd = 0;
            const int len = (int) (e.sliceLength() / e.playbackRate());
            for (int i = 0; i < len + 2000; ++i)
            {
                const float s = e.render (p);
                if (i > 0)
                {
                    const float d = std::fabs (s - prev);
                    natural = std::max (natural, d * 0.999f);
                    if (d > natural * 4.0f) { worstDelta = std::max (worstDelta, d); ++atEnd; }
                }
                prev = s;
            }
            std::printf ("     нота %d: длина %d, разрывов %d, худший скачок %.4f\n",
                         note, len, atEnd, worstDelta);
            if (atEnd > 0) ok = false;
        }
        suite::check (ok, "в конце слайса нет разрыва (окно 5 мс в шкале исходника)");
    }

    // ---------------------------------------------------------------- 6
    std::printf ("\n--- 6. Постоянная составляющая в теле слайса ---\n");
    {
        EngineParams p;
        p.table = 0; p.p[0] = 0.0f; p.p[1] = 1.0f; p.p[2] = 0.0f; p.p[3] = 0.0f; p.level = 1.0f;
        double worst = 0.0;
        for (int note : { 45, 47 })
        {
            ChopEngine e; e.setBank (&bank); e.setSampleRate (48000.0);
            e.start (note, 1.0f); e.prepare (p);
            const int len = (int) e.sliceLength();
            double acc = 0.0; int n = 0;
            for (int i = 0; i < len - 480; ++i)        // без последних 10 мс (фейд)
            {
                acc += e.render (p); ++n;
            }
            worst = std::max (worst, std::fabs (acc / std::max (1, n)));
        }
        std::printf ("     среднее в теле слайса: %.2e\n", worst);
        suite::check (worst < 1.0e-4, "в теле слайса постоянной составляющей нет",
                      suite::num (worst, 6));
    }

    // ---------------------------------------------------------------- 7
    // Общий стенд гоняем на ПРОБНОМ лупе: стационарная полоса 40 гармоник,
    // нарезанная на 4 равных слайса. Только на таком контенте метрика
    // «энергия вне гармоник» = алиасинг; у перкуссии спектр широкополосный
    // по природе, и любая такая метрика даёт ~0 дБ (проверено: −0.2 дБ).
    LoopBank probeBank;
    {
        Loop probe;
        probe.shot = buildProbeShot (48000.0, 4.0, 4400.0);
        for (int i = 0; i < 4; ++i) probe.slices.push_back (i * 48000);
        probe.shot.rootNote = 45;
        probeBank.loops.push_back (std::move (probe));
    }

    suite::Config cfg;
    cfg.name = "CHOP (общий стенд)";
    cfg.make = [&probeBank] {
        auto e = new ChopEngine();
        e->setBank (&probeBank);
        return std::unique_ptr<EngineBase> (e);
    };
    cfg.params = [] (int) {
        EngineParams p;
        p.p[0] = 0.10f;    // слайс выбирается параметром, нота транспонирует:
                           // только в этом режиме высота тона = midiToHz(note),
                           // и метрика «энергия вне гармоник» вообще применима
        p.p[1] = 1.0f;     // весь слайс
        p.p[2] = 0.0f;     // без повторов
        p.p[3] = 0.0f;     // не реверс
        p.level = 1.0f;
        p.detune = 0.0f;
        return p;
    };
    cfg.notes = { 45, 52, 57, 64 };    // rate 1.0 / 1.5 / 2.0 / 3.0
    cfg.aliasWindow = [] (int note) { return fftForSlice (note); };
    cfg.aliasTargetDb = -55.0;
    // DC: общий стенд меряет среднее по окну 0.5 с, а кусок в это окно
    // ПОПАДАЕТ концом и его 5-мс фейдом — дефицит энергии фейда читается как
    // 1.5·10⁻³. Настоящая постоянная составляющая тела слайса проверяется
    // отдельным тестом ниже (< 1·10⁻⁴).
    cfg.dcTarget      = 3.0e-3;
    cfg.levelTarget   = 0.25;
    cfg.maxContentSpreadDb = 9.0;
    suite::run (cfg);

    std::printf ("\n=================================================\n%s (провалов: %d)\n",
                 suite::gFailures == 0 ? "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" : "ЕСТЬ ПРОВАЛЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
