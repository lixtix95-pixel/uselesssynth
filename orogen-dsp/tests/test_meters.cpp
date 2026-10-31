// Метры A–H: свежий пик, спад в тишине, reset и параллельное чтение GUI.
// Метры не участвуют в аудиотракте и публикуются отдельно от голосов.
#include "engine_suite.h"
#include "../src/instrument/instrument.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace orogen;

static Patch meterPatch()
{
    Patch p = patchInit();
    p.slots[0].type = EngineType::Granular;
    p.slots[0].params.p[2] = 1.0f; p.slots[0].params.detune = 0.0f;
    p.slots[0].attack = 0.005f; p.slots[0].decay = 0.1f;
    p.slots[0].sustain = 1.0f; p.slots[0].release = 0.05f;
    p.slots[0].tail = 0.0f;
    return p;
}

static void renderFrames (Instrument& inst, int frames)
{
    for (int i = 0; i < frames; ++i) { float l = 0.0f, r = 0.0f; inst.render (l, r); }
}

int main()
{
    std::printf ("OROGEN — уровни слотов: звук, спад и публикация в GUI\n=================================================\n");
    Instrument inst;
    float initial = 0.0f, minimumPlay = 120.0f, maximumEnd = -120.0f;
    int badSilentSlots = 0, badMonotone = 0, nonfinite = 0;
    const bool raceOnly = std::getenv ("OROGEN_METER_RACE_ONLY") != nullptr;
    if (! raceOnly) for (const int sr : { 44100, 48000, 96000 })
    {
        inst.prepare ((double) sr, 2);
        inst.setPatch (meterPatch());
        float d[8]; inst.slotLevelsDb (d);
        initial = std::max (initial, std::fabs (d[0] + 120.0f));
        inst.noteOn (60, 1.0f);
        renderFrames (inst, sr / 4); inst.slotLevelsDb (d);
        const float playing = d[0]; minimumPlay = std::min (minimumPlay, playing);
        for (int i = 1; i < 8; ++i) if (d[i] > -110.0f) ++badSilentSlots;
        inst.noteOff (60);
        renderFrames (inst, sr / 2); inst.slotLevelsDb (d); const float mid = d[0];
        renderFrames (inst, sr * 2); inst.slotLevelsDb (d); const float end = d[0];
        if (end > mid + 0.01f) ++badMonotone;
        if (! std::isfinite (playing) || ! std::isfinite (mid) || ! std::isfinite (end)) ++nonfinite;
        maximumEnd = std::max (maximumEnd, end);
        std::printf ("   %d Гц: на ноте %.2f дБ, через 0.5 с %.2f, через 2.5 с %.2f\n", sr, playing, mid, end);
    }
    if (! raceOnly)
    {
        suite::check (initial < 0.01f, "prepare сбрасывает показания всех слотов");
        suite::check (minimumPlay > -60.0f && badSilentSlots == 0,
                      "звучащий слот виден, выключенные остаются тихими");
        suite::check (maximumEnd < -90.0f && badMonotone == 0 && nonfinite == 0,
                      "после note-off и тишины индикатор затухает на всех трёх частотах",
                      suite::num (maximumEnd, 2) + " dB худший");

        // Короткое событие внутри одного окна публикации не должно исчезнуть.
        inst.prepare (48000.0, 4);
        auto shortPatch = meterPatch();
        shortPatch.slots[0].type = EngineType::Wavetable;
        shortPatch.slots[0].attack = 0.001f; shortPatch.slots[0].release = 0.001f;
        inst.setPatch (shortPatch);
        const auto beforeWindow = inst.meterRevision();
        inst.noteOn (60, 1.0f); renderFrames (inst, 96); inst.noteOff (60);
        renderFrames (inst, 383);
        const auto beforeBoundary = inst.meterRevision();
        renderFrames (inst, 1); // 2 мс нота, всего 10 мс
        const auto atBoundary = inst.meterRevision();
        float d[8]; inst.slotLevelsDb (d); const float transient = d[0];
        suite::check (transient > -70.0f, "2-мс транзиент попадает в 10-мс окно метра",
                      suite::num (transient, 2) + " dB");
        suite::check (beforeWindow == beforeBoundary && atBoundary == beforeWindow + 1,
                      "ревизия метра меняется ровно на границе окна 480 отсчётов");

        // Две зоны, две одновременно звучащих ноты; отпускание одной не
        // должно затронуть показания другой, а первая должна затухнуть.
        inst.prepare (48000.0, 4);
        auto split = shortPatch;
        split.slots[0].loNote = 0; split.slots[0].hiNote = 59;
        split.slots[1] = split.slots[0]; split.slots[1].loNote = 60; split.slots[1].hiNote = 127;
        split.slots[0].release = split.slots[1].release = 0.05f;
        inst.setPatch (split);
        inst.noteOn (48, 1.0f); inst.noteOn (72, 1.0f);
        renderFrames (inst, 24000); inst.slotLevelsDb (d);
        const float aPlaying = d[0], bPlaying = d[1];
        inst.noteOff (48); renderFrames (inst, 72000); inst.slotLevelsDb (d);
        const float aReleased = d[0], bHeld = d[1];
        std::printf ("   полифония A/B: %.2f / %.2f dB, после отпускания A %.2f / %.2f dB\n",
                     aPlaying, bPlaying, aReleased, bHeld);
        suite::check (aPlaying > -60.0f && bPlaying > -60.0f && aReleased < -90.0f && bHeld > -60.0f,
                      "пики нескольких голосов и слотов публикуются независимо");
        inst.setSampleRate (96000.0); inst.slotLevelsDb (d);
        bool reset = true; for (float v : d) reset = reset && std::fabs (v + 120.0f) < 0.01f;
        suite::check (reset, "смена частоты дискретизации сбрасывает все показания");
        std::atomic<float> lockFreeProbe { 0.0f };
        const bool lockFree = lockFreeProbe.is_lock_free();
        std::printf ("   атомарный float: %s на этой машине\n", lockFree ? "lock-free" : "использует блокировку");
        suite::check (lockFree, "публикация уровня lock-free на тестируемой платформе");
    }

    // Одна нить только рендерит, другая только читает опубликованные метры.
    // Этот же стенд можно собрать с ThreadSanitizer, если он доступен.
    inst.prepare (48000.0, 2); inst.setPatch (meterPatch());
    std::atomic<bool> done { false };
    std::atomic<int> reads { 0 }, errors { 0 };
    std::thread reader ([&] {
        float d[8];
        while (! done.load (std::memory_order_acquire))
        {
            (void) inst.meterRevision();
            inst.slotLevelsDb (d);
            for (float v : d) if (! std::isfinite (v) || v < -120.01f || v > 80.0f) ++errors;
            ++reads;
        }
    });
    for (int round = 0; round < 6; ++round)
    {
        inst.noteOn (60, 1.0f); renderFrames (inst, 12000);
        inst.noteOff (60); renderFrames (inst, 12000);
    }
    done.store (true, std::memory_order_release); reader.join();
    std::printf ("   параллельное чтение: %d снимков, %d NaN/невалидных значений\n", reads.load(), errors.load());
    suite::check (reads.load() > 100 && errors.load() == 0, "параллельное чтение выдаёт конечные значения");
    std::printf ("\n%s (провалов: %d)\n", suite::gFailures ? "ЕСТЬ ПРОВАЛЫ" : "ВСЕ ТЕСТЫ ПРОЙДЕНЫ", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
