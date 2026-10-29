// Тесты инструмента: 8 слотов, полифония, морф, сплит, эффекты, CPU.
// Проверяется то, что нельзя проверить по одному движку: суммирование голосов,
// калибровка патчей, voice stealing, хвосты и мастер-цепочка.
#include "engine_suite.h"
#include "../src/instrument/instrument.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <new>
#include <memory>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// Счётчик аллокаций по потокам. Доказывает, что смена типа движка не выделяет
// память в аудио-потоке: message-поток (stagePatch) считает отдельно, поток
// рендера — отдельно.
// ---------------------------------------------------------------------------
namespace alloc_count
{
    struct Counters { std::atomic<long long> news { 0 }, deletes { 0 }; };
    struct Registry
    {
        static constexpr int kMax = 8;
        Counters slots[kMax];
        std::atomic<int> n { 0 };
    };
    inline Registry& registry() { static Registry r; return r; }
    inline thread_local Counters* mine = nullptr;
    inline std::atomic<bool> enabled { false };

    inline void attach (int index)
    {
        mine = &registry().slots[index];
        if (index + 1 > registry().n.load()) registry().n.store (index + 1);
        enabled.store (true);
    }
    inline long long news()   { return mine ? mine->news.load() : 0; }
    inline long long dels()   { return mine ? mine->deletes.load() : 0; }
    inline void reset()       { if (mine) { mine->news.store (0); mine->deletes.store (0); } }
}

namespace
{
    inline void countNew()
    {
        if (alloc_count::mine != nullptr && alloc_count::enabled.load (std::memory_order_relaxed))
            alloc_count::mine->news.fetch_add (1, std::memory_order_relaxed);
    }
    inline void countDelete()
    {
        if (alloc_count::mine != nullptr && alloc_count::enabled.load (std::memory_order_relaxed))
            alloc_count::mine->deletes.fetch_add (1, std::memory_order_relaxed);
    }
}

void* operator new (std::size_t n) { countNew(); return std::malloc (n == 0 ? 1 : n); }
void operator delete (void* p) noexcept { countDelete(); std::free (p); }
void operator delete (void* p, std::size_t) noexcept { countDelete(); std::free (p); }
void* operator new[] (std::size_t n) { countNew(); return std::malloc (n == 0 ? 1 : n); }
void operator delete[] (void* p) noexcept { countDelete(); std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { countDelete(); std::free (p); }

using namespace orogen;
using suite::check;
using suite::num;

namespace {

struct RenderStats
{
    double rms = 0.0, rmsWindow = 0.0, dc = 0.0;
    float peak = 0.0f, maxDelta = 0.0f;
    int   nonFinite = 0;
    std::vector<float> l, r;
};

RenderStats renderPatch (Instrument& inst, const Patch& p, const std::vector<int>& notes,
                         double seconds, double windowFrom = -1.0, double windowTo = -1.0)
{
    inst.setPatch (p);
    for (int nt : notes) inst.noteOn (nt, 0.8f);

    const int n = (int) (48000.0 * seconds);
    RenderStats st;
    st.l.assign ((size_t) n, 0.0f);
    st.r.assign ((size_t) n, 0.0f);

    double acc = 0.0, accWin = 0.0, dcv = 0.0;
    int cntWin = 0;
    for (int i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        inst.render (l, r);
        st.l[(size_t) i] = l; st.r[(size_t) i] = r;
        if (! std::isfinite (l) || ! std::isfinite (r)) ++st.nonFinite;
        acc += (double) l * l; dcv += l;
        st.peak = std::max (st.peak, std::max (std::fabs (l), std::fabs (r)));
        const double t = (double) i / 48000.0;
        if ((windowFrom < 0.0 || t >= windowFrom) && (windowTo < 0.0 || t < windowTo))
        { accWin += (double) l * l; ++cntWin; }
        if (i > 0) st.maxDelta = std::max (st.maxDelta, std::fabs (l - st.l[(size_t) i - 1]));
    }
    st.rms = std::sqrt (acc / std::max (1, n));
    st.rmsWindow = cntWin > 0 ? std::sqrt (accWin / cntWin) : st.rms;
    st.dc = dcv / std::max (1, n);
    return st;
}

double db (double x) { return 20.0 * std::log10 (std::max (1e-12, x)); }

// центроид спектра в окне (Гц) — для проверки, что морф реально меняет тембр
double centroid (const std::vector<float>& x, int from, int len)
{
    // Fft — radix-2: длина обязана быть степенью двойки (иначе bit-reverse
    // уходит за границы буфера; поймано ASan на len = 6144)
    int p2 = 1;
    while (p2 * 2 <= len) p2 *= 2;
    len = p2;
    if (from < 0 || (size_t) (from + len) > x.size()) return 0.0;
    std::vector<std::complex<float>> spec ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / (len - 1));
        spec[(size_t) i] = { (float) (x[(size_t) (from + i)] * w), 0.0f };
    }
    Fft::forward (spec);
    double num = 0.0, den = 0.0;
    for (int k = 1; k < len / 2; ++k)
    {
        const double mag = std::abs (spec[(size_t) k]);
        num += mag * k; den += mag;
    }
    return den > 1.0e-9 ? (num / den) * 48000.0 / len : 0.0;
}

} // namespace

int main()
{
    std::printf ("=== ИНСТРУМЕНТ: 8 слотов, полифония, эффекты ===\n\n");

    int patchCount = 0;
    const Patch* patches = factoryPatches (patchCount);

    // ---------------------------------------------------------------- 1
    std::printf ("--- 1. Калибровка громкости патчей ---\n");
    double loDb = 1e9, hiDb = -1e9;
    for (int i = 0; i < patchCount; ++i)
    {
        Instrument inst; inst.prepare (48000.0, 16);
        // окно 0…1 с: у перкуссивных патчей (Glass Keys) установившегося уровня нет,
        // поэтому громкость меряем по первой секунде аккорда
        const RenderStats st = renderPatch (inst, patches[i], { 57, 60, 63, 66 }, 2.0, 0.0, 1.0);
        const double rmsDb = db (st.rmsWindow);
        loDb = std::min (loDb, rmsDb); hiDb = std::max (hiDb, rmsDb);
        std::printf ("   %-12s rms %.4f (%6.1f дБ), пик %.3f\n", patches[i].name, st.rmsWindow, rmsDb, st.peak);
    }
    check (hiDb - loDb < 3.5, "разброс громкости между патчами < 3.5 дБ", num (hiDb - loDb, 1) + " дБ");

    // ---------------------------------------------------------------- 2
    std::printf ("\n--- 2. Пик и целостность ---\n");
    {
        Instrument inst; inst.prepare (48000.0, 16);
        float worst = 0.0f; int bad = 0;
        for (int i = 0; i < patchCount; ++i)
        {
            const RenderStats st = renderPatch (inst, patches[i], { 57, 60, 63, 66 }, 3.0);
            worst = std::max (worst, st.peak); bad += st.nonFinite;
        }
        check (worst < 0.999f, "ни один патч не клипует", "пик " + num (worst, 3));
        check (bad == 0, "нет NaN/Inf на выходе", std::to_string (bad));
    }

    // ---------------------------------------------------------------- 3
    std::printf ("\n--- 3. Полифония действительно суммирует голоса ---\n");
    {
        Instrument a; a.prepare (48000.0, 16);
        const RenderStats one = renderPatch (a, patches[0], { 60 }, 2.0, 1.0, 2.0);
        Instrument b; b.prepare (48000.0, 16);
        const RenderStats four = renderPatch (b, patches[0], { 57, 60, 63, 66 }, 2.0, 1.0, 2.0);
        const double ratio = four.rmsWindow / std::max (1e-9, one.rmsWindow);
        std::printf ("   одна нота rms %.4f, четыре ноты rms %.4f, отношение %.2f\n",
                     one.rmsWindow, four.rmsWindow, ratio);
        // Регрессия: однажды голоса перезаписывали друг друга, и «аккорд» звучал
        // как одна нота (отношение ≈ 1.0). Порог 1.3: четыре некогерентных голоса
        // дают ×2, но компенсация плотности 1/sqrt(1+0.35·N) забирает часть.
        check (ratio > 1.3, "четыре ноты громче одной (голоса складываются)", num (ratio, 2));
    }

    // ---------------------------------------------------------------- 4
    std::printf ("\n--- 4. Voice stealing ---\n");
    {
        Instrument inst; inst.prepare (48000.0, 16);
        inst.setPatch (patches[2]);
        for (int i = 0; i < 24; ++i) inst.noteOn (48 + i * 2, 0.7f);
        RenderStats st;
        float peak = 0.0f; int nonFinite = 0;
        for (int i = 0; i < 48000; ++i)
        {
            float l = 0.0f, r = 0.0f; inst.render (l, r);
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
            if (! std::isfinite (l)) ++nonFinite;
        }
        std::printf ("   24 ноты на 16 голосах: потеряно %d, максимум активных %d, пик %.3f\n",
                     inst.stolenCount(), inst.maxActiveVoiceCount(), peak);
        check (inst.maxActiveVoiceCount() <= 16, "активных голосов не больше 16",
               std::to_string (inst.maxActiveVoiceCount()));
        check (inst.stolenCount() >= 8, "лишние ноты воруют голоса", std::to_string (inst.stolenCount()));
        check (peak < 1.0f && nonFinite == 0, "при воровстве нет клиппинга и NaN", num (peak, 3));
    }

    // ---------------------------------------------------------------- 5
    std::printf ("\n--- 5. DC на выходе ---\n");
    {
        double worst = 0.0;
        for (int i = 0; i < patchCount; ++i)
        {
            Instrument inst; inst.prepare (48000.0, 16);
            const RenderStats st = renderPatch (inst, patches[i], { 57, 60, 63, 66 }, 3.0);
            worst = std::max (worst, std::fabs (st.dc));
        }
        check (worst < 2.0e-3, "постоянная составляющая ниже 2·10⁻³", num (worst, 5));
    }

    // ---------------------------------------------------------------- 6
    std::printf ("\n--- 6. Морф восьми слотов ---\n");
    {
        Instrument inst; inst.prepare (48000.0, 16);
        Patch p = patches[4];
        inst.setPatch (p);
        inst.noteOn (60, 0.8f);
        std::vector<float> out;
        std::vector<float> cents;
        const int n = (int) (48000.0 * 8.0);
        std::vector<float> buf ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            if (i % 4800 == 0) { p.morph = clamp ((float) i / (float) n, 0.0f, 1.0f); inst.setPatch (p); }
            float l = 0.0f, r = 0.0f; inst.render (l, r);
            buf[(size_t) i] = l;
        }
        for (int seg = 0; seg < 4; ++seg)
            cents.push_back (centroid (buf, seg * 48000 * 2 + 24000, 16384));
        std::printf ("   центроид по морфу 0→1:");
        for (double c : cents) std::printf (" %.0f", c);
        std::printf (" Гц\n");
        const bool moves = std::fabs (cents.back() - cents.front()) > 100.0;
        check (moves, "морф меняет спектр (центроид сдвигается > 100 Гц)",
               num (cents.back() - cents.front(), 0) + " Гц");

        // Щелчок ищем как выброс: максимум шага против 99.9-го перцентиля шагов.
        // Сравнивать с «наклоном синуса 1 кГц» нельзя — у яркого FM-слоя
        // естественный шаг в разы больше (та же ошибка, что была в тестах движков).
        std::vector<float> deltas;
        deltas.reserve ((size_t) n);
        for (int i = 1; i < n; ++i) deltas.push_back (std::fabs (buf[(size_t) i] - buf[(size_t) i - 1]));
        std::vector<float> sorted = deltas;
        std::nth_element (sorted.begin(), sorted.begin() + (long) (sorted.size() * 0.999), sorted.end());
        const float p999 = sorted[(size_t) (sorted.size() * 0.999)];
        const float maxDelta = *std::max_element (deltas.begin(), deltas.end());
        std::printf ("   шаг: максимум %.4f, 99.9-й перцентиль %.4f, отношение %.2f\n",
                     maxDelta, p999, maxDelta / std::max (1e-9f, p999));
        check (maxDelta < p999 * 3.0f, "кроссфейд морфа без щелчков (нет выброса шага)",
               num (maxDelta / std::max (1e-9f, p999), 2) + "× от перцентиля");
    }

    // ---------------------------------------------------------------- 7
    std::printf ("\n--- 7. Сплит: зоны нот ---\n");
    {
        Patch split = patchSplitBassLead();
        Instrument lo; lo.prepare (48000.0, 8);
        lo.setPatch (split);
        lo.noteOn (40, 0.8f);
        for (int i = 0; i < 4800; ++i) { float l, r; lo.render (l, r); }
        float lvLow[8]; lo.slotLevelsDb (lvLow);

        Instrument hi; hi.prepare (48000.0, 8);
        hi.setPatch (split);
        hi.noteOn (72, 0.8f);
        for (int i = 0; i < 4800; ++i) { float l, r; hi.render (l, r); }
        float lvHigh[8]; hi.slotLevelsDb (lvHigh);

        std::printf ("   нота 40: слот A %.0f дБ, слот B %.0f дБ\n", lvLow[0], lvLow[1]);
        std::printf ("   нота 72: слот A %.0f дБ, слот B %.0f дБ\n", lvHigh[0], lvHigh[1]);
        check (lvLow[0] > lvLow[1] + 6.0f, "нижняя нота играет только слот A");
        check (lvHigh[1] > lvHigh[0] + 6.0f, "верхняя нота играет только слот B");
    }

    // ---------------------------------------------------------------- 8
    std::printf ("\n--- 8. Отпускание ноты и хвосты ---\n");
    {
        for (int idx : { 1, 2, 3 })
        {
            Instrument inst; inst.prepare (48000.0, 16);
            inst.setPatch (patches[idx]);
            for (int i = 0; i < 4; ++i) inst.noteOn (57 + i * 3, 0.8f);
            for (int i = 0; i < 48000; ++i) { float l, r; inst.render (l, r); }
            inst.allNotesOff();
            for (int i = 0; i < (int) (48000 * 8.0); ++i) { float l, r; inst.render (l, r); }
            double acc = 0.0; float pk = 0.0f;
            for (int i = 0; i < 4800; ++i) { float l, r; inst.render (l, r); acc += (double) l * l; pk = std::max (pk, std::fabs (l)); }
            std::printf ("   %-12s через 8 с после отпускания: rms %.2e, активных %d\n",
                         patches[idx].name, std::sqrt (acc / 4800), inst.activeVoiceCount());
            check (std::sqrt (acc / 4800) < 1.0e-4 && inst.activeVoiceCount() == 0,
                   std::string ("хвост ") + patches[idx].name + " затихает и освобождает голоса");
        }
    }

    // ---------------------------------------------------------------- 9
    std::printf ("\n--- 9. Эффекты: реверб и дилей ---\n");
    {
        Instrument dry; dry.prepare (48000.0, 8);
        dry.setPatch (patches[0]);
        dry.noteOn (60, 0.8f);
        for (int i = 0; i < 24000; ++i) { float l, r; dry.render (l, r); }
        dry.noteOff (60);
        for (int i = 0; i < 72000; ++i) { float l, r; dry.render (l, r); }   // 1.5 с: сухой хвост кончился
        double accDry = 0.0;
        for (int i = 0; i < 24000; ++i) { float l, r; dry.render (l, r); accDry += (double) l * l; }

        Instrument wet; wet.prepare (48000.0, 8);
        FxChain::Settings fx;
        fx.reverbMix = 0.55f; fx.reverbSize = 0.85f; fx.delayMix = 0.35f; fx.delayTime = 0.28f; fx.delayFb = 0.5f;
        wet.setFx (fx);
        wet.setPatch (patches[0]);
        wet.noteOn (60, 0.8f);
        for (int i = 0; i < 24000; ++i) { float l, r; wet.render (l, r); }
        wet.noteOff (60);
        for (int i = 0; i < 72000; ++i) { float l, r; wet.render (l, r); }
        double accWet = 0.0; float pk = 0.0f;
        for (int i = 0; i < 24000; ++i) { float l, r; wet.render (l, r); accWet += (double) l * l; pk = std::max (pk, std::fabs (l)); }
        const double gain = db (std::sqrt (accWet / 24000)) - db (std::sqrt (accDry / 24000));
        std::printf ("   хвост через 1.5 с после отпускания: сухо %.2e, с эффектами %.2e (+%.1f дБ)\n",
                     std::sqrt (accDry / 24000), std::sqrt (accWet / 24000), gain);
        check (gain > 20.0, "реверб+дилей продлевают хвост", "+" + num (gain, 1) + " дБ");
        check (pk < 1.0f, "мокрый сигнал не клипует", num (pk, 3));
    }

    // ---------------------------------------------------------------- 10
    std::printf ("\n--- 10. Лимитер ---\n");
    {
        FxChain fx; fx.prepare (48000.0);
        FxChain::Settings s; s.drive = 0.6f; s.compThreshold = -20.0f; s.compRatio = 8.0f; s.compMakeup = 6.0f;
        fx.set (s);
        float peak = 0.0f; bool finite = true;
        for (int i = 0; i < 48000; ++i)
        {
            float l = 8.0f * fastSin ((float) i / 48000.0f * 220.0f);
            float r = l;
            fx.process (l, r);
            if (! std::isfinite (l)) finite = false;
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
        }
        std::printf ("   вход 8·FS: выход пик %.3f\n", peak);
        check (peak <= 0.985f && finite, "лимитер держит пик ≤ 0.985", num (peak, 3));
    }

    // ---------------------------------------------------------------- 11
    std::printf ("\n--- 11. CPU: реальное время ---\n");
    {
        Instrument inst; inst.prepare (48000.0, 16);
        inst.setPatch (patches[4]);                    // самый тяжёлый: 8 движков
        for (int i = 0; i < 16; ++i) inst.noteOn (40 + i * 3, 0.8f);
        const int n = 48000 * 2;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) { float l, r; inst.render (l, r); }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const double audio = (double) n / 48000.0;
        std::printf ("   16 голосов × 8 слотов: %.2f с на %.1f с аудио = %.0f %% ядра\n",
                     sec, audio, 100.0 * sec / audio);
        // регрессия: пока голос запускал все 8 движков независимо от веса слота,
        // здесь было 114 % ядра; ленивый старт слота дал 26 %
        check (sec < audio * 0.6, "16 голосов × 8 слотов укладываются в 60 % ядра",
               num (100.0 * sec / audio, 0) + " % ядра");

        // реалистичный случай: 3 слота (Terra Pad), 8 голосов
        Instrument light; light.prepare (48000.0, 8);
        light.setPatch (patches[1]);
        for (int i = 0; i < 8; ++i) light.noteOn (48 + i * 4, 0.8f);
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) { float l, r; light.render (l, r); }
        const double sec2 = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
        std::printf ("   8 голосов × 3 слота: %.2f с на %.1f с аудио = %.0f %% ядра\n",
                     sec2, audio, 100.0 * sec2 / audio);
        check (sec2 < audio * 0.6, "реалистичный патч (8 голосов × 3 слота) в 60 % ядра",
               num (100.0 * sec2 / audio, 0) + " %");
    }

    // ---------------------------------------------------------------- смена движков
    std::printf ("\n--- Смена типа движка: без аллокаций в аудио-потоке ---\n");
    {
        EngineStats::reset();
        Instrument inst;
        inst.prepare (48000.0, 16);          // создаёт 16 × 8 = 128 движков
        inst.setPatch (patches[1]);
        for (int i = 0; i < 6; ++i) inst.noteOn (45 + i * 3, 0.8f);

        // message-поток: крутит типы слотов и публикует их
        std::atomic<bool> stop { false };
        std::atomic<int> staged { 0 };
        std::thread ui ([&]
        {
            alloc_count::attach (1);                 // отдельный счётчик потока
            int k = 0;
            while (! stop.load (std::memory_order_relaxed))
            {
                Patch p = patches[1];
                p.slots[0].type = (EngineType) (k % (int) EngineType::Count);
                p.slots[2].type = (EngineType) ((k + 3) % (int) EngineType::Count);
                inst.stagePatch (p);                 // аллокации — здесь, и это норма
                inst.drainRetired();                 // уборка — здесь же
                staged.fetch_add (2, std::memory_order_relaxed);
                ++k;
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
        });

        // аудио-поток: рендер 2 с, считаем аллокации
        alloc_count::attach (0);
        alloc_count::reset();
        alloc_count::enabled.store (true);
        Patch live = patches[1];
        const int total = 48000 * 2;
        double sum = 0.0; long long nan = 0; int swaps = 0;
        for (int i = 0; i < total; ++i)
        {
            if ((i & 511) == 0)
            {
                live.slots[0].type = (EngineType) ((i / 512) % (int) EngineType::Count);
                swaps += inst.pendingEngineSwaps();   // сколько ждёт подхвата
                inst.applyPatch (live);               // подхват подготовленного
            }
            float l = 0.0f, r = 0.0f;
            inst.render (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) ++nan;
            sum += (double) l * l + (double) r * r;
        }
        alloc_count::enabled.store (false);
        const long long audioNews = alloc_count::news();
        const long long uiNews = alloc_count::registry().slots[1].news.load();

        stop.store (true);
        ui.join();
        // добираем то, что осталось подготовленным к моменту остановки
        inst.applyPatch (live);
        int drained = inst.drainRetired();
        inst.applyPatch (live);
        drained += inst.drainRetired();
        const long long live2 = EngineStats::live();

        std::printf ("   message-поток: %d смен, %lld аллокаций (это норма)\n",
                     staged.load(), uiNews);
        std::printf ("   аудио-поток: %lld аллокаций, подхвачено слотов %d, "
                     "снято с очереди %d, живых движков %lld\n",
                     audioNews, swaps, drained, live2);
        std::printf ("   RMS %.4f, NaN %lld\n", std::sqrt (sum / (2.0 * total)), nan);

        // порог низкий намеренно: тест ограничен 2 с, и на медленной машине
        // message-поток успевает сделать меньше смен (наблюдалось 100…142)
        check (staged.load() > 20, "смены типов действительно происходили",
               num (staged.load(), 0) + " шт.");
        check (audioNews == 0, "аудио-поток не аллоцирует при смене движка",
               num ((double) audioNews, 0) + " аллокаций");
        check (nan == 0, "нет NaN во время смены движков", num ((double) nan, 0));
        check (swaps > 0, "подмена движков доходила до аудио-потока", num (swaps, 0));
        // 16 голосов × 8 слотов = 128 движков; после уборки лишних быть не должно
        check (live2 == 128, "все заменённые движки удалены", num ((double) live2, 0) + " живых");
    }

    // Отдельно от теста аллокаций: обновлённый движок должен начать звук
    // ДЕЙСТВУЮЩЕЙ ноты, а не ждать следующего Note On. Тестируем все девять
    // типов (включая возврат на Wavetable) и разные фазы control tick.
    std::printf ("\n--- Смена каждого движка при удержанной ноте ---\n");
    {
        EngineBank bank;
        bank.build (48000.0);
        int sounding = 0, different = 0, nonFinite = 0;
        for (int target = 0; target < (int) EngineType::Count; ++target)
        {
            Patch original = patchInit();
            if (target == 0) original.slots[0].type = EngineType::FM;
            original.slots[0].tail = 0.0f;
            original.slots[0].sustain = 0.9f;
            Voice changed, unchanged;
            changed.init (bank, 48000.0); unchanged.init (bank, 48000.0);
            changed.stageSlots (original, bank); unchanged.stageSlots (original, bank);
            changed.applyPendingEngines(); unchanged.applyPendingEngines();
            changed.start (original, 60, 0.8f, 60.0f);
            unchanged.start (original, 60, 0.8f, 60.0f);
            float l = 0.0f, r = 0.0f, lr = 0.0f, rr = 0.0f;
            const int warm = 12001 + 17 * target;  // не совпадает с 128 отсчётами
            for (int i = 0; i < warm; ++i)
            {
                changed.render (original, l, r); unchanged.render (original, lr, rr);
            }
            Patch swapped = original;
            swapped.slots[0].type = (EngineType) target;
            changed.stageSlots (swapped, bank);
            changed.applyPendingEngines();
            double energy = 0.0, delta = 0.0;
            for (int i = 0; i < 24000; ++i)
            {
                changed.render (swapped, l, r); unchanged.render (original, lr, rr);
                if (! std::isfinite (l) || ! std::isfinite (r)) ++nonFinite;
                energy += (double) l * l;
                const double d = (double) l - lr;
                delta += d * d;
            }
            const double rms = std::sqrt (energy / 24000.0);
            const double diff = std::sqrt (delta / 24000.0);
            if (rms > 1.0e-4) ++sounding;
            if (diff > 1.0e-3) ++different;
            std::printf ("   %-10s: RMS %.5f, ΔRMS %.5f\n",
                         engineTypeName ((EngineType) target), rms, diff);
        }
        check (sounding == 9 && different == 9 && nonFinite == 0,
               "9 типов запускаются и звучат иначе без повторного Note On",
               std::to_string (sounding) + "/9, NaN " + std::to_string (nonFinite));
    }

    // ---------------------------------------------------------------- арпеджиатор
    std::printf ("\n--- Арпеджиатор (эффект 16/16): порядок нот, гейт, latch, режимы ---\n");
    {
        const double sr = 48000.0;
        auto makeArp = [&] (ArpSettings as, double bpm)
        {
            auto a = std::make_unique<Arpeggiator>();
            a->prepare (sr);
            a->setBpm (bpm);
            a->setSettings (as);
            return a;
        };
        auto collect = [&] (Arpeggiator& a, int samples, std::vector<int>& ons, std::vector<int>& offs)
        {
            ArpEvent ev[Arpeggiator::kMaxPending];
            int done = 0;
            while (done < samples)
            {
                const int n = std::min (1024, samples - done);
                const int cnt = a.process (n, ev, Arpeggiator::kMaxPending);
                for (int i = 0; i < cnt; ++i)
                    (ev[i].noteOn ? ons : offs).push_back (ev[i].note);
                done += n;
            }
        };

        ArpSettings as;
        as.on = true; as.mode = ArpMode::Up; as.div = ArpDiv::Eighth;
        as.octaves = 1; as.gate = 0.5f;

        // 1. порядок Up: 60, 64, 67 (нажаты в обратном порядке — важен порядок по высоте)
        {
            auto a = makeArp (as, 120.0);          // 1/8 при 120 BPM = 0.25 с = 12000 сэмплов
            a->noteOn (67, 0.8f); a->noteOn (60, 0.8f); a->noteOn (64, 0.8f);
            std::vector<int> ons, offs;
            collect (*a, 12000 * 3, ons, offs);
            std::printf ("   Up:    ноты %d, %d, %d\n", ons[0], ons[1], ons[2]);
            check (ons.size() == 3 && ons[0] == 60 && ons[1] == 64 && ons[2] == 67,
                   "Up играет по возрастанию высоты");
            check (offs.size() >= 2 && offs[0] == 60, "note off приходит раньше следующего шага");
        }

        // 2. Down и Up/Down
        {
            as.mode = ArpMode::Down;
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f); a->noteOn (67, 0.8f); a->noteOn (64, 0.8f);
            std::vector<int> ons, offs;
            collect (*a, 12000 * 3, ons, offs);
            std::printf ("   Down:  ноты %d, %d, %d\n", ons[0], ons[1], ons[2]);
            check (ons[0] == 67 && ons[1] == 64 && ons[2] == 60, "Down играет по убыванию");

            as.mode = ArpMode::UpDown;
            auto b = makeArp (as, 120.0);
            b->noteOn (60, 0.8f); b->noteOn (64, 0.8f); b->noteOn (67, 0.8f);
            std::vector<int> o2, f2;
            collect (*b, 12000 * 4, o2, f2);
            std::printf ("   UpDn:  ноты %d, %d, %d, %d\n", o2[0], o2[1], o2[2], o2[3]);
            check (o2[0] == 60 && o2[1] == 64 && o2[2] == 67 && o2[3] == 64,
                   "Up/Down разворачивается без повтора крайних нот");
        }

        // 3. октавы: 2 октавы удваивают набор
        {
            as.mode = ArpMode::Up; as.octaves = 2;
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f); a->noteOn (64, 0.8f);
            std::vector<int> ons, offs;
            collect (*a, 12000 * 4, ons, offs);
            std::printf ("   2 октавы: %d, %d, %d, %d\n", ons[0], ons[1], ons[2], ons[3]);
            check (ons[0] == 60 && ons[1] == 64 && ons[2] == 72 && ons[3] == 76,
                   "верхняя октава добавляется сверху");
        }

        // 4. темп: вдвое быстрее BPM = вдвое больше шагов
        {
            as.octaves = 1; as.mode = ArpMode::Up;
            auto slow = makeArp (as, 120.0);
            auto fast = makeArp (as, 240.0);
            slow->noteOn (60, 0.8f); fast->noteOn (60, 0.8f);
            std::vector<int> s1, s2, f1, f2;
            collect (*slow, 48000, s1, f1);
            collect (*fast, 48000, s2, f2);
            std::printf ("   1 с при 120 BPM: %d шагов, при 240 BPM: %d шагов\n",
                         (int) s1.size(), (int) s2.size());
            check (s2.size() == s1.size() * 2, "шаги масштабируются темпом");
        }

        // 5. гейт: 50 % шага — нота звучит примерно половину
        {
            as.mode = ArpMode::Up; as.gate = 0.5f;
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f);
            ArpEvent ev[Arpeggiator::kMaxPending];
            const int cnt = a->process (12000, ev, Arpeggiator::kMaxPending);
            int onAt = -1, offAt = -1;
            for (int i = 0; i < cnt; ++i)
            {
                if (ev[i].noteOn && onAt < 0) onAt = ev[i].sampleOffset;
                if (! ev[i].noteOn && onAt >= 0) { offAt = ev[i].sampleOffset; break; }
            }
            const double lenPct = 100.0 * (offAt - onAt) / 12000.0;
            std::printf ("   гейт 0.5: нота звучит %.1f %% шага\n", lenPct);
            check (lenPct > 45.0 && lenPct < 55.0, "гейт 0.5 ≈ половина шага", num (lenPct, 1) + " %");
        }

        // 6. latch: после note off нота продолжает играть, новый аккорд сменяет
        {
            as.latch = true; as.gate = 0.5f;
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f);
            std::vector<int> ons, offs;
            collect (*a, 12000 * 2, ons, offs);
            a->noteOff (60);
            std::vector<int> ons2, offs2;
            collect (*a, 12000 * 2, ons2, offs2);
            std::printf ("   latch: до отпускания %d нот, после — %d\n", (int) ons.size(), (int) ons2.size());
            check (ons2.size() >= 1, "latch держит ноту после note off", num ((double) ons2.size(), 0));
            a->noteOn (67, 0.8f);
            std::vector<int> ons3, offs3;
            collect (*a, 12000 * 2, ons3, offs3);
            check (! ons3.empty() && ons3[0] == 67, "новый аккорд заменяет старый (latch)");
            a->setSettings (ArpSettings {});        // выключили — снимает всё
            check (a->heldNotes() == 0, "выключение арпеджиатора сбрасывает удержанные ноты");
        }

        // 7. Random устойчив: ноты не выходят за набор, распределение не вырождено
        {
            as = ArpSettings {}; as.on = true; as.mode = ArpMode::Random; as.div = ArpDiv::Sixteenth;
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f); a->noteOn (64, 0.8f); a->noteOn (67, 0.8f);
            std::vector<int> ons, offs;
            collect (*a, 48000, ons, offs);
            int lo = 999, hi = -999, distinct = 0;
            bool inSet[128] = {};
            for (int n : ons) { lo = std::min (lo, n); hi = std::max (hi, n); if (! inSet[n]) { inSet[n] = true; ++distinct; } }
            std::printf ("   Random: %d нот, диапазон %d…%d, разных высот %d\n",
                         (int) ons.size(), lo, hi, distinct);
            check (lo >= 60 && hi <= 67 && distinct >= 2, "Random не выходит за набор и не залипает");
        }

        // 8. Chord: все ноты сразу, без арпеджио
        {
            as = ArpSettings {}; as.on = true; as.mode = ArpMode::Chord;
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f); a->noteOn (64, 0.8f); a->noteOn (67, 0.8f);
            std::vector<int> ons, offs;
            collect (*a, 12000 * 2, ons, offs);
            std::printf ("   Chord: %d нот за 2 шага\n", (int) ons.size());
            check (ons.size() == 6 && ons[0] == 60 && ons[1] == 64 && ons[2] == 67,
                   "Chord играет весь аккорд каждым шагом");
        }

        // 9. без аллокаций в аудио-потоке
        {
            auto a = makeArp (as, 120.0);
            a->noteOn (60, 0.8f); a->noteOn (64, 0.8f);
            ArpEvent ev[Arpeggiator::kMaxPending];
            a->process (64, ev, Arpeggiator::kMaxPending);      // прогрев
            alloc_count::attach (2);
            alloc_count::reset();
            alloc_count::enabled.store (true);
            long long notes = 0;
            for (int i = 0; i < 48000 * 4; ++i) notes += a->process (1, ev, Arpeggiator::kMaxPending);
            alloc_count::enabled.store (false);
            const long long news = alloc_count::news();
            std::printf ("   процесс: %lld событий, %lld аллокаций\n", notes, news);
            check (news == 0, "арпеджиатор не аллоцирует в аудио-потоке", num ((double) news, 0));
            check (notes > 50, "события действительно генерируются", num ((double) notes, 0));
        }

        // 10. в инструменте: включённый арпеджиатор играет ноты сам
        {
            Instrument i2;
            i2.prepare (48000.0, 16);
            i2.setPatch (patches[0]);
            ArpSettings on; on.on = true; on.mode = ArpMode::Up; on.div = ArpDiv::Sixteenth; on.gate = 0.5f;
            i2.setArp (on);
            i2.setTempo (120.0);
            i2.noteOn (60, 0.8f); i2.noteOn (64, 0.8f);
            double sum = 0.0; float peak = 0.0f; int nan = 0;
            for (int i = 0; i < 48000 * 2; ++i)
            {
                float l = 0.0f, r = 0.0f;
                i2.render (l, r);
                if (! std::isfinite (l)) ++nan;
                sum += (double) l * l + (double) r * r;
                peak = std::max (peak, std::fabs (l));
            }
            const double level = std::sqrt (sum / (2.0 * 48000 * 2));
            std::printf ("   через Instrument: RMS %.4f, пик %.3f, NaN %d, нот запущено %lld\n",
                         level, peak, nan, (long long) i2.startedCount());
            check (level > 0.005 && nan == 0 && peak < 0.999f,
                   "ARP внутри инструмента звучит и не клипует", num (level, 4));
            check (i2.startedCount() > 4, "ноты запускает арпеджиатор, а не напрямую",
                   num ((double) i2.startedCount(), 0) + " нот");
        }
    }

    // ---------------------------------------------------------------- мод-матрица
    std::printf ("\n--- Мод-матрица: 8 маршрутов, 8 источников, 8 назначений ---\n");
    {
        const double sr = 48000.0;
        auto renderPatch = [&] (Patch p, double seconds, std::vector<float>* out = nullptr)
        {
            Instrument i2;
            i2.prepare (sr, 8);
            i2.setPatch (p);
            i2.noteOn (60, 0.9f);
            const int total = (int) (sr * seconds);
            std::vector<float> buf;
            buf.reserve ((size_t) total);
            float peak = 0.0f;
            int nan = 0;
            for (int i = 0; i < total; ++i)
            {
                float l = 0.0f, r = 0.0f;
                i2.render (l, r);
                if (! std::isfinite (l)) ++nan;
                peak = std::max (peak, std::fabs (l));
                buf.push_back (l);
            }
            if (out) *out = buf;
            return std::make_pair (peak, nan);
        };

        Patch base;
        base.setName ("ModBase");
        // ВАЖНО: у SlotPatch по умолчанию on = true, то есть «пустой» патч играет
        // все восемь слотов. Если этого не учесть, тест меряет сумму восьми
        // движков, и модуляция одного слота в ней теряется (на этом я и попался
        // в первой версии блока: центроид не двигался вообще).
        for (int s2 = 1; s2 < 8; ++s2) base.slots[s2].on = false;
        base.slots[0].on = true;
        base.slots[0].type = EngineType::Wavetable;
        base.slots[0].cutoff = 0.5f;
        base.slots[0].attack = 0.01f; base.slots[0].decay = 0.4f;
        base.slots[0].sustain = 0.7f; base.slots[0].release = 0.3f;

        // 1. нулевые маршруты ничего не меняют — побитовое совпадение
        {
            std::vector<float> a, b;
            Patch p = base;
            renderPatch (p, 0.5, &a);

            Patch q = base;
            for (int r = 0; r < kModRoutes; ++r)
            {
                q.routes[r].source = ModSource::Lfo1;
                q.routes[r].dest = ModDest::Cutoff;
                q.routes[r].amount = 0.0f;          // выключен
            }
            renderPatch (q, 0.5, &b);
            double d = 0.0;
            for (size_t i = 0; i < a.size(); ++i) d = std::max (d, (double) std::fabs (a[i] - b[i]));
            std::printf ("   amount = 0 на всех 8 маршрутах: макс. разница %.3e\n", d);
            check (d == 0.0, "выключенная матрица не меняет звук", num (d, 9));
        }

        // 2. Env → Cutoff: при sustain = 1 конверт держит фильтр открытым
        {
            auto centroidOf = [&] (float amount)
            {
                Patch p = base;
                p.slots[0].cutoff = 0.15f;
                p.slots[0].sustain = 1.0f;          // конверт стоит на максимуме
                p.routes[0] = { ModSource::Env, ModDest::Cutoff, amount };
                std::vector<float> out;
                renderPatch (p, 0.5, &out);
                return centroid (out, 9600, 8192);
            };
            const double bright = centroidOf (0.8f);
            const double plain  = centroidOf (0.0f);
            std::printf ("   Env → Cutoff: центроид %.0f Гц с маршрутом, %.0f Гц без\n", bright, plain);
            check (bright > plain * 1.15, "маршрут Env открывает фильтр",
                   num (bright, 0) + " против " + num (plain, 0) + " Гц");

            // а теперь то же с медленно падающим конвертом (sustain 0.15, спад 3 с):
            // яркость должна убывать вместе с конвертом, пока звук ещё слышен
            Patch p = base;
            p.slots[0].cutoff = 0.10f;
            p.slots[0].attack = 0.005f; p.slots[0].decay = 3.0f; p.slots[0].sustain = 0.15f;
            p.routes[0] = { ModSource::Env, ModDest::Cutoff, 0.9f };
            std::vector<float> out;
            renderPatch (p, 2.0, &out);
            const double early = centroid (out, 2400, 8192);
            const double late  = centroid (out, 80000, 8192);
            std::printf ("   падающий конверт: центроид %.0f Гц в начале, %.0f Гц в конце\n", early, late);
            check (early > late * 1.3, "яркость следует за конвертом",
                   num (early, 0) + " → " + num (late, 0) + " Гц");
        }

        // 3. Pitch: LFO 1 → Pitch с amount 0.5 = ±6 полутонов, проверяем по частоте
        {
            auto detect = [] (const std::vector<float>& x, int from, int len)
            {
                double best = 0.0, bestHz = 0.0;
                for (double hz = 100.0; hz < 1200.0; hz += 0.5)
                {
                    double si = 0.0, co = 0.0;
                    for (int i = 0; i < len; ++i)
                    {
                        const double ph = kTwoPi * hz * (double) i / 48000.0;
                        si += x[(size_t) (from + i)] * std::sin (ph);
                        co += x[(size_t) (from + i)] * std::cos (ph);
                    }
                    const double m = si * si + co * co;
                    if (m > best) { best = m; bestHz = hz; }
                }
                return bestHz;
            };
            Patch p = base;
            p.slots[0].cutoff = 1.0f;
            p.lfo[0].rate = 0.5f;                     // 0.5 Гц: полный период 2 с
            p.lfo[0].amount = 0.0f;                   // простой путь выключен
            p.routes[0] = { ModSource::Lfo1, ModDest::Pitch, 0.5f };   // ±6 полутонов
            std::vector<float> out;
            renderPatch (p, 2.0, &out);
            const double f1 = detect (out, 24000, 4096);    // четверть периода — вверх
            const double f2 = detect (out, 60000, 4096);    // три четверти — вниз
            std::printf ("   LFO → Pitch ±6: %.1f Гц вверху, %.1f Гц внизу (нота 60 ≈ 261.6)\n", f1, f2);
            check (f1 > 300.0 && f2 < 230.0, "маршрут Pitch качает высоту на ±6 полутонов");

            Patch q = base;
            q.slots[0].cutoff = 1.0f;
            q.lfo[0].rate = 0.5f;
            std::vector<float> flat;
            renderPatch (q, 2.0, &flat);
            const double f0 = detect (flat, 24000, 4096);
            std::printf ("   без маршрута: %.1f Гц\n", f0);
            check (std::fabs (f0 - 261.6) < 4.0, "без маршрута высота ровная", num (f0, 1) + " Гц");
        }

        // 4. Mod Wheel → Level: колесо управляет громкостью
        {
            auto level = [&] (float wheel)
            {
                Patch p = base;
                p.modWheel = wheel;
                p.slots[0].params.level = 0.18f;        // не заходим в tanh голоса
                p.routes[0] = { ModSource::ModWheel, ModDest::Level, -0.9f };
                std::vector<float> out;
                renderPatch (p, 0.4, &out);
                double sum = 0.0;
                for (size_t i = 9600; i < out.size(); ++i) sum += (double) out[i] * out[i];
                return std::sqrt (sum / (double) std::max<size_t> (1, out.size() - 9600));
            };
            const double l0 = level (0.0f), l1 = level (1.0f);
            std::printf ("   Mod Wheel → Level(−0.9): RMS %.4f при 0, %.4f при 1\n", l0, l1);
            check (l0 > l1 * 1.4, "колесо модуляции меняет уровень", num (l0, 3) + " → " + num (l1, 3));
        }

        // 5. Pan: маршрут уводит слот в сторону; сумма L/R сохраняется
        {
            auto renderLR = [&] (float panAmount)
            {
                Patch p = base;
                p.slots[0].cutoff = 1.0f;
                p.routes[0] = { ModSource::ModWheel, ModDest::Pan, panAmount };
                p.modWheel = 1.0f;
                Instrument i2; i2.prepare (sr, 8); i2.setPatch (p);
                i2.noteOn (60, 0.9f);
                double sumL = 0.0, sumR = 0.0;
                for (int i = 0; i < 24000; ++i)
                {
                    float l = 0.0f, r = 0.0f;
                    i2.render (l, r);
                    if (i > 4800) { sumL += (double) l * l; sumR += (double) r * r; }
                }
                return sumL / std::max (1e-12, sumR);
            };
            const double bal0 = renderLR (0.0f);
            const double bal1 = renderLR (0.9f);
            std::printf ("   Pan: L/R = %.2f без маршрута, %.2f с маршрутом +0.9\n", bal0, bal1);
            check (bal1 < bal0 * 0.5, "маршрут Pan смещает слот в правый канал",
                   num (bal0, 2) + " → " + num (bal1, 2));
        }

        // 6. Random: у каждой ноты своё значение источника (высота чуть разная)
        {
            Patch p = base;
            p.slots[0].cutoff = 1.0f;
            p.routes[0] = { ModSource::Random, ModDest::Pitch, 0.05f };   // ±0.6 полутона
            Instrument i2; i2.prepare (sr, 8); i2.setPatch (p);

            std::vector<double> f;
            for (int n = 0; n < 3; ++n)
            {
                i2.noteOn (60, 0.9f);                    // та же нота, но новый запуск
                std::vector<float> out;
                for (int i = 0; i < 12000; ++i) { float l = 0, r = 0; i2.render (l, r); out.push_back (l); }
                i2.noteOff (60);

                double best = 0.0, hz = 0.0;
                for (double t = 250.0; t < 275.0; t += 0.1)
                {
                    double si = 0.0, co = 0.0;
                    for (int i = 4800; i < 12000; ++i)
                    {
                        const double ph = kTwoPi * t * (double) (i - 4800) / 48000.0;
                        si += out[(size_t) i] * std::sin (ph);
                        co += out[(size_t) i] * std::cos (ph);
                    }
                    const double m = si * si + co * co;
                    if (m > best) { best = m; hz = t; }
                }
                f.push_back (hz);
            }
            std::printf ("   Random → Pitch ±0.6: три запуска дали %.1f / %.1f / %.1f Гц\n", f[0], f[1], f[2]);
            const int distinct = (f[0] != f[1] ? 1 : 0) + (f[1] != f[2] ? 1 : 0) + (f[0] != f[2] ? 1 : 0);
            check (distinct >= 1, "Random даёт разное значение на каждую ноту",
                   num ((double) distinct, 0) + " разных пар из 3");
        }

        // 7. Восемь маршрутов одновременно: без NaN и без клиппинга
        {
            Patch p = base;
            p.lfo[0].rate = 3.0f; p.lfo[1].rate = 0.7f;
            const ModSource srcs[kModRoutes] = { ModSource::Lfo1, ModSource::Lfo2, ModSource::Env,
                                                 ModSource::Velocity, ModSource::KeyTrack,
                                                 ModSource::ModWheel, ModSource::Random, ModSource::Bend };
            const ModDest dsts[kModRoutes] = { ModDest::Cutoff, ModDest::Pitch, ModDest::Morph,
                                               ModDest::Level, ModDest::Pan, ModDest::Resonance,
                                               ModDest::Warp, ModDest::Detune };
            for (int r = 0; r < kModRoutes; ++r)
                p.routes[r] = { srcs[r], dsts[r], 0.35f };
            p.bend = 0.5f;
            p.modWheel = 0.7f;
            const auto res = renderPatch (p, 1.0);
            std::printf ("   все 8 маршрутов сразу: пик %.3f, NaN %d\n", res.first, res.second);
            check (res.second == 0 && res.first < 0.999f, "8 маршрутов не ломают выход",
                   num (res.first, 3) + " пик");
        }

        // 8. Мод-матрица не аллоцирует в аудио-потоке
        {
            Instrument i2;
            i2.prepare (sr, 16);
            Patch p = base;
            p.lfo[0].rate = 4.0f; p.lfo[1].rate = 1.3f;
            for (int r = 0; r < kModRoutes; ++r)
                p.routes[r] = { (ModSource) (r % (int) ModSource::Count),
                                (ModDest) (r % (int) ModDest::Count), 0.3f };
            i2.setPatch (p);
            for (int n = 0; n < 6; ++n) i2.noteOn (48 + n * 4, 0.85f);
            for (int i = 0; i < 4096; ++i) { float l = 0, r = 0; i2.render (l, r); }   // прогрев
            alloc_count::attach (3);
            alloc_count::reset();
            alloc_count::enabled.store (true);
            int nan = 0;
            for (int i = 0; i < 48000; ++i)
            {
                float l = 0.0f, r = 0.0f;
                i2.render (l, r);
                if (! std::isfinite (l)) ++nan;
            }
            alloc_count::enabled.store (false);
            const long long news = alloc_count::news();
            std::printf ("   аудио-поток с 8 маршрутами: %lld аллокаций, NaN %d\n", news, nan);
            check (news == 0, "мод-матрица не аллоцирует в аудио-потоке", num ((double) news, 0));
            check (nan == 0, "нет NaN при модуляции");
        }
    }


    // ---------------------------------------------------------------
    // 12. Фильтры: 16 моделей — наклоны АЧХ, резонанс, устойчивость
    // ---------------------------------------------------------------
    {
        std::printf ("\n--- 12. Фильтры: 16 моделей (наклон, резонанс, устойчивость) ---\n");
        std::printf ("   id  модель   спад дБ/окт (октава в полосе задержания)  пик на fc (res 0.9)\n");

        // АЧХ одиночного фильтра: синус на частоте hz, RMS после установления,
        // поделённый на RMS входа (нормировка на 1.0 = передача без изменений).
        auto magAt = [] (FilterModel m, double fc, double hz, float res)
        {
            SlotFilter f;
            f.prepare (48000.0);
            f.setParams ((float) fc, res);
            f.reset();
            const int n = 24000, settle = 9000;
            const double amp = 0.1;
            double acc = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const float x = (float) (amp * std::sin (kTwoPi * hz * i / 48000.0));
                const float y = f.process (x, m);
                if (i >= settle) acc += (double) y * (double) y;
            }
            return std::sqrt (acc / (n - settle)) / (amp * 0.7071067811865476);
        };

        const double fc = 1000.0;
        // { модель, номинальный наклон дБ/окт }
        const std::pair<FilterModel, double> slopes[7] = {
            { FilterModel::LP6, 6.0 },  { FilterModel::LP12, 12.0 }, { FilterModel::LP24, 24.0 },
            { FilterModel::LP36, 36.0 }, { FilterModel::HP6, 6.0 },  { FilterModel::HP12, 12.0 },
            { FilterModel::HP24, 24.0 },
        };
        int slopeBad = 0;
        for (auto& e : slopes)
        {
            const auto m = e.first;
            const double nominal = e.second;
            // LP меряем выше среза, HP — ниже; обе точки в полосе задержания,
            // но не у самого нуля (там уже шум вычислений)
            const bool lp = e.first == FilterModel::LP6 || e.first == FilterModel::LP12
                         || e.first == FilterModel::LP24 || e.first == FilterModel::LP36;
            const double f1 = lp ? fc * 2.0 : fc / 2.0;
            const double f2 = lp ? fc * 4.0 : fc / 4.0;
            const double a = magAt (m, fc, f1, 0.0f);
            const double b = magAt (m, fc, f2, 0.0f);
            const double measured = 20.0 * std::log10 (std::max (1e-12, a) / std::max (1e-12, b));
            std::printf ("   %2d  %-7s %+7.1f (норма %+.0f)\n", (int) m, filterModelName (m),
                         measured, nominal);
            if (std::fabs (measured - nominal) > 3.5) ++slopeBad;
        }
        check (slopeBad == 0, "наклоны всех LP/HP совпадают с названием (±3.5 дБ/окт)",
               std::to_string (slopeBad) + " из 7 вне допуска");

        // Резонанс: у резонансных моделей пик на fc при res 0.9 заметно выше 1.
        const FilterModel resonant[7] = { FilterModel::LP12, FilterModel::LP24, FilterModel::BP12,
                                          FilterModel::HP12, FilterModel::HP24, FilterModel::BP24,
                                          FilterModel::Peak };
        double worstPeak = 1e9;
        for (auto m : resonant)
        {
            const double p = magAt (m, fc, fc, 0.9f);
            std::printf ("   %2d  %-7s пик %.2f\n", (int) m, filterModelName (m), p);
            worstPeak = std::min (worstPeak, p);
        }
        check (worstPeak > 3.0, "резонанс 0.9 даёт пик > 3× на частоте среза у всех резонансных моделей",
               num (worstPeak, 2) + "× минимум");

        // Регресс на старую ошибку: id 3 обязан быть настоящим 24 дБ/окт.
        const double hp24Slope = 20.0 * std::log10 (magAt (FilterModel::HP24, fc, fc / 2.0, 0.0f)
                                                    / magAt (FilterModel::HP24, fc, fc / 4.0, 0.0f));
        check (hp24Slope > 20.0, "HP24 (id 3) — реальные 24 дБ/окт, а не 12",
               num (hp24Slope, 1) + " дБ/окт");

        // Совместимость: старые id 0–4 сохраняют смысл, всё вне диапазона — clamp.
        const char* names5[5] = { "LP12", "LP24", "BP12", "HP24", "Notch12" };
        int nameBad = 0;
        for (int i = 0; i < 5; ++i)
            if (std::string (filterModelName (filterModelFromInt (i))) != names5[i]) ++nameBad;
        check (nameBad == 0, "старые id 0–4 не изменили смысла", std::to_string (nameBad) + " расхождений");
        check (filterModelFromInt (-3) == FilterModel::LP12
               && filterModelFromInt (999) == FilterModel::Vowel,
               "недопустимый id зажимается в диапазон 0…15",
               std::string ("-3 → ") + filterModelName (filterModelFromInt (-3))
               + ", 999 → " + filterModelName (filterModelFromInt (999)));

        // Устойчивость: все 16 моделей на трёх срезах при res 1.0, вход — шум 0.5.
        int unstable = 0, nanCnt = 0;
        double worstOut = 0.0;
        for (int mi = 0; mi < (int) FilterModel::Count; ++mi)
        {
            const auto m = (FilterModel) mi;
            for (double fcv : { 60.0, 1000.0, 12000.0 })
            {
                SlotFilter f;
                f.prepare (48000.0);
                f.setParams ((float) fcv, 1.0f);
                f.reset();
                Rng rng ((uint32_t) (0x9e3779b9u + 7919u * (uint32_t) mi));
                double peak = 0.0;
                for (int i = 0; i < 48000; ++i)
                {
                    const float x = 0.5f * (rng.nextFloat() * 2.0f - 1.0f);
                    const float y = f.process (x, m);
                    if (! std::isfinite (y)) { ++nanCnt; break; }
                    peak = std::max (peak, (double) std::fabs (y));
                }
                worstOut = std::max (worstOut, peak);
                if (! (peak < 24.0)) ++unstable;
            }
        }
        check (nanCnt == 0 && unstable == 0,
               "все 16 моделей устойчивы при res 1.0 на 60/1000/12000 Гц",
               "макс. пик " + num (worstOut, 2) + ", срывов " + std::to_string (unstable));

        // Встраивание в голос: срез 400 Гц против 8 кГц меняет центроид.
        auto centroidForCutoff = [] (float cut)
        {
            Instrument vi;
            vi.prepare (48000.0, 4);
            Patch p;
            p.slots[0].type = EngineType::Wavetable;
            p.slots[0].on = true;
            p.slots[0].filterType = (int) FilterModel::LP24;
            p.slots[0].res = 0.2f;
            p.slots[0].cutoff = cut;
            for (int i = 1; i < 8; ++i) p.slots[i].on = false;
            vi.setPatch (p);
            vi.drainRetired();
            vi.noteOn (57, 0.9f);                 // A3 ≈ 220 Гц
            std::vector<float> buf;
            for (int i = 0; i < 36000; ++i)
            {
                float l = 0.0f, r = 0.0f;
                vi.render (l, r);
                if (i >= 12000) buf.push_back (l);
            }
            return centroid (buf, 0, (int) buf.size());
        };
        const double cLow = centroidForCutoff (0.15f);
        const double cHigh = centroidForCutoff (0.95f);
        std::printf ("   голос: центроид %.0f Гц при закрытом срезе, %.0f Гц при открытом\n", cLow, cHigh);
        check (cHigh > cLow * 1.5, "фильтр в голосе реально меняет тембр по параметру Cutoff",
               num (cLow, 0) + " → " + num (cHigh, 0) + " Гц");
    }

    std::printf ("\n=================================================\n");
    if (suite::gFailures == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else std::printf ("ПРОВАЛОВ: %d\n", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
