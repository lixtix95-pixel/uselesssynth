// Инструмент: полифония + патч + цепочка эффектов. Это то, что в VST3 станет
// одним инстансом с 8 слотами A–H и мастер-шиной.
#pragma once

#include "voice.h"
#include "fx.h"
#include "arpeggiator.h"

#include <vector>
#include <deque>
#include <array>
#include <atomic>
#include <cstdint>

namespace orogen {

class Instrument
{
public:
    static constexpr int kMaxVoices = 16;

    Instrument() noexcept
    {
        for (auto& meter : publishedMeters) meter.store (0.0f, std::memory_order_relaxed);
    }

    void prepare (double sr, int voices = kMaxVoices)
    {
        sampleRate = sr;
        bank.build (sr);
        drainRetired();

        numVoices = clamp (voices, 1, kMaxVoices);
        this->voices.resize ((size_t) numVoices);
        for (auto& v : this->voices)
        {
            v.init (bank, sr, &retireBin);
            v.stageSlots (patch, bank);
            v.applyPendingEngines();
        }
        fx.prepare (sr);
        fx.set (fxSettings);
        arpeggiator.prepare (sr);
        lastNote = 60;
        sampleCounter = 0;
        resetStats();
        resetMetering (sr);
    }

    void setSampleRate (double sr)
    {
        sampleRate = sr;
        for (auto& v : voices) v.setSampleRate (sr);
        fx.prepare (sr);
        fx.set (fxSettings);
        arpeggiator.prepare (sr);
        resetMetering (sr);
    }

    // --- Патч: три режима, чтобы аудио-поток не аллоцировал -------------------
    //
    // stagePatch()  — message thread. Создаёт движки для изменившихся типов
    //                 слотов (аллокация здесь допустима) и публикует их
    //                 аудио-потоку. Работающие движки не трогаются.
    // applyPatch()  — аудио-поток. Копирует POD-структуру патча и подхватывает
    //                 подготовленные движки. Ни одной аллокации.
    // setPatch()    — оба шага сразу, для стендов и тестов (один поток).
    void stagePatch (const Patch& p)
    {
        patch = p;
        for (auto& v : voices) v.stageSlots (patch, bank);
    }

    void applyPatch (const Patch& p) noexcept
    {
        patch = p;
        for (auto& v : voices) v.applyPendingEngines();
    }

    void setPatch (const Patch& p)
    {
        stagePatch (p);
        for (auto& v : voices) v.applyPendingEngines();
    }

    // Заменённые движки живут в retire bin до drainRetired(). Если объект
    // уничтожают, не дождавшись уборки (выход из теста, закрытие плагина),
    // bin остался бы с указателями — подчищаем здесь.
    ~Instrument() { drainRetired(); }

    // Уборка заменённых движков — ТОЛЬКО message thread. Возвращает число
    // удалённых объектов (для тестов и диагностики).
    int drainRetired()
    {
        int total = 0;
        void* buf[64];
        for (int n; (n = retireBin.drain (buf, 64)) > 0; )
        {
            for (int i = 0; i < n; ++i)
            {
                delete static_cast<EngineBase*> (buf[i]);
                EngineStats::deleted.fetch_add (1, std::memory_order_relaxed);
            }
            total += n;
        }
        return total;
    }

    // Сколько движков ещё ждёт уборки (для тестов).
    int retiredPending() const { return retireBin.size(); }
    int pendingEngineSwaps() const
    {
        int n = 0;
        for (const auto& v : voices) n += v.pendingCount();
        return n;
    }

    const Patch& getPatch() const { return patch; }

    void setFx (const FxChain::Settings& s) { fxSettings = s; fx.set (s); }

    // Транспортная задержка цепочки эффектов в сэмплах. Обёртка VST3 обязана
    // отдать её хосту через setLatencySamples().
    int fxLatency() const { return fx.latency(); }

    // --- Арпеджиатор ---------------------------------------------------------
    // Включённый ARP перехватывает noteOn/noteOff и сам выдаёт события в render().
    // Инструмент остаётся прежним: арпеджиатор стоит до голосов.
    Arpeggiator& arp() { return arpeggiator; }
    const Arpeggiator& arp() const { return arpeggiator; }
    void setArp (const ArpSettings& s) { arpeggiator.setSettings (s); }
    void setTempo (double bpm) { arpeggiator.setBpm (bpm); }

    void noteOn (int note, float velocity)
    {
        if (arpeggiator.getSettings().on) { arpeggiator.noteOn (note, velocity); return; }
        startVoice (note, velocity);
    }

    void noteOff (int note)
    {
        if (arpeggiator.getSettings().on) { arpeggiator.noteOff (note); return; }
        releaseVoice (note);
    }

private:
    // Запуск/снятие голоса без участия арпеджиатора: нужно и для прямых нот,
    // и для событий, которые арпеджиатор выдаёт внутри render().
    void startVoice (int note, float velocity)
    {
        Voice* slot = nullptr;

        // 1. свободный голос
        for (auto& v : voices) if (! v.isActive()) { slot = &v; break; }

        // 2. если все заняты — забираем самый «дешёвый для уха»: сначала уже
        //    отпущенные и самые старые, потом просто самый старый
        if (! slot)
        {
            ++stolenVoices;
            Voice* best = nullptr;
            double bestScore = -1.0e18;
            for (auto& v : voices)
            {
                const double score = (v.isReleased() ? 1.0e9 : 0.0) + v.age();
                if (score > bestScore) { bestScore = score; best = &v; }
            }
            slot = best;
            if (slot) slot->kill();
        }
        if (! slot) return;

        slot->start (patch, note, velocity, (float) lastNote, (uint32_t) (notesStarted + 1));
        lastNote = note;
        ++notesStarted;
    }

    void releaseVoice (int note)
    {
        for (auto& v : voices)
            if (v.isActive() && ! v.isReleased() && v.note() == note) v.noteOff();
    }

public:
    void allNotesOff()
    {
        for (auto& v : voices) if (v.isActive()) v.noteOff();
    }

    void panic()
    {
        for (auto& v : voices) v.kill();
    }

    // Один сэмпл стерео на выходе (после эффектов).
    inline void render (float& l, float& r) noexcept
    {
        if (arpeggiator.getSettings().on)
        {
            // события арпеджиатора на этот сэмпл: offset всегда 0
            ArpEvent ev[Arpeggiator::kMaxPending];
            const int n = arpeggiator.process (1, ev, Arpeggiator::kMaxPending);
            for (int i = 0; i < n; ++i)
            {
                if (ev[i].noteOn) startVoice (ev[i].note, ev[i].velocity);
                else              releaseVoice (ev[i].note);
            }
        }

        float vl = 0.0f, vr = 0.0f;
        int active = 0;
        // ВАЖНО: голоса складываются, а не перезаписывают друг друга.
        // Раньше render() писал в те же vl/vr, и каждый следующий голос затирал
        // предыдущий — в полифонии играл только последний активный голос
        // (на слух: «аккорд звучит как одна тихая нота»).
        for (auto& v : voices)
        {
            float gl = 0.0f, gr = 0.0f;
            if (v.render (patch, gl, gr)) { vl += gl; vr += gr; ++active; }
        }

        peakActiveVoices = std::max (peakActiveVoices, active);
        activeVoices = active;

        // компенсация плотности: восемь слотов × 16 голосов без неё уходят в лимитер
        const float density = 1.0f / std::sqrt (1.0f + 0.35f * (float) active);
        const float trim = density * masterGain * patch.gain;
        vl *= trim;
        vr *= trim;

        // Защита от «выбросов» движков: новый движок в первом сэмпле после
        // start()/prepare() может дать импульс, а компрессор стоит после него
        // и запомнит этот выброс на секунды (наблюдали просадку −120 дБ).
        lastPreFxPeak = std::max (lastPreFxPeak, std::max (std::fabs (vl), std::fabs (vr)));
        if (! std::isfinite (vl) || ! std::isfinite (vr)) { vl = vr = 0.0f; ++nonFiniteSamples; }
        vl = clamp (vl, -8.0f, 8.0f);
        vr = clamp (vr, -8.0f, 8.0f);

        fx.process (vl, vr);
        l = antiDenormal (vl);
        r = antiDenormal (vr);
        ++sampleCounter;
        if (++meterFrames >= meterWindowFrames) publishMeters();
    }

    int activeVoiceCount() const { return activeVoices; }

    // Уровни A–H в дБ (−120 = тишина): GUI читает только атомарный снимок.
    // Голоса и их изменяемые float-метры остаются в аудиопотоке.
    void slotLevelsDb (float* out) const noexcept
    {
        for (int i = 0; i < Voice::kSlots; ++i)
        {
            const float pk = publishedMeters[(size_t) i].load (std::memory_order_relaxed);
            out[i] = 20.0f * std::log10 (pk + 1.0e-6f);
        }
    }
    // GUI обнаруживает приостановленный processBlock без обращения к часам
    // или блокировкам из аудиопотока. Счётчик меняется раз в окно и при reset.
    std::uint32_t meterRevision() const noexcept { return publishedMeterRevision.load (std::memory_order_acquire); }

    int maxActiveVoiceCount() const { return peakActiveVoices; }
    int stolenCount() const { return stolenVoices; }
    float preFxPeak() const { return lastPreFxPeak; }
    int nonFiniteCount() const { return nonFiniteSamples; }
    int startedCount() const { return notesStarted; }
    double seconds() const { return (double) sampleCounter / sampleRate; }
    const EngineBank& content() const { return bank; }
    const std::deque<Voice>& voiceList() const { return voices; }
    void setMasterGain (float g) { masterGain = clamp (g, 0.0f, 2.0f); }

    void resetStats() { peakActiveVoices = 0; stolenVoices = 0; notesStarted = 0; lastPreFxPeak = 0.0f; nonFiniteSamples = 0; }

private:
    // Подготовка вызывается только вне render(). GUI может читать атомарный
    // снимок и во время перестройки, но не обращается к контейнеру voices.
    void resetMetering (double sr) noexcept
    {
        const double rate = std::max (1.0, sr);
        meterWindowFrames = std::max (1, (int) std::lround (rate * 0.01)); // 10 мс
        meterReleaseCoef = (float) std::exp (-(double) meterWindowFrames / (rate * 0.12));
        meterFrames = 0;
        meterEnvelope.fill (0.0f);
        for (auto& v : voices) v.clearMeters();
        for (auto& meter : publishedMeters) meter.store (0.0f, std::memory_order_relaxed);
        publishedMeterRevision.fetch_add (1, std::memory_order_release);
    }

    // Вызывается только аудиопотоком раз в 10 мс: пики за окно сбрасываются,
    // а короткий экспоненциальный спад удерживает и краткие транзиенты для UI.
    void publishMeters() noexcept
    {
        meterFrames = 0;
        std::array<float, Voice::kSlots> peaks {};
        for (auto& v : voices)
        {
            const auto& voicePeaks = v.slotMeters();
            for (int i = 0; i < Voice::kSlots; ++i)
                peaks[(size_t) i] = std::max (peaks[(size_t) i], voicePeaks[(size_t) i]);
            v.clearMeters();
        }
        for (int i = 0; i < Voice::kSlots; ++i)
        {
            const float peak = std::isfinite (peaks[(size_t) i]) ? peaks[(size_t) i] : 0.0f;
            float& level = meterEnvelope[(size_t) i];
            level = std::max (peak, level * meterReleaseCoef);
            if (level < 1.0e-8f) level = 0.0f; // не допускать denormal в длительной тишине
            publishedMeters[(size_t) i].store (level, std::memory_order_relaxed);
        }
        publishedMeterRevision.fetch_add (1, std::memory_order_release);
    }

    EngineBank bank;
    EngineRetireBin retireBin;
    Arpeggiator arpeggiator;
    // deque, а не vector: Voice неперемещаемый (атомарный флаг подмены движков),
    // а deque никогда не переносит элементы — адреса стабильны.
    std::deque<Voice> voices;
    Patch patch = patchInit();
    FxChain fx;
    FxChain::Settings fxSettings;

    // Только publishedMeters разделяется с GUI; остальное — локально аудиопотоку.
    std::array<std::atomic<float>, Voice::kSlots> publishedMeters {};
    std::atomic<std::uint32_t> publishedMeterRevision { 0 };
    std::array<float, Voice::kSlots> meterEnvelope {};
    int meterFrames = 0, meterWindowFrames = 480;
    float meterReleaseCoef = 0.9200444f;

    double sampleRate = 48000.0;
    long long sampleCounter = 0;
    int numVoices = kMaxVoices;
    int lastNote = 60;
    float masterGain = 0.8f;

    int activeVoices = 0, peakActiveVoices = 0, stolenVoices = 0, notesStarted = 0;
    int nonFiniteSamples = 0;
    float lastPreFxPeak = 0.0f;
};

} // namespace orogen
