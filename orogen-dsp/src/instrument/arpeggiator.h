// ARP — арпеджиатор (эффект 16/16 из Terrain). Стоит ДО голосов: он не
// обрабатывает звук, а превращает удержанные ноты в поток note on/off.
//
// Почему в DSP-ядре, а не в плагине: это чистая логика без JUCE, значит её
// можно проверить стендом (порядок событий, гейт, latch, свинг) без хоста и
// без аудио-устройства. Плагин передаёт только темп из playhead.
//
// Аудио-поток: события складываются в буфер вызывающего, ни одной аллокации
// (списки нот — массивы фиксированного размера).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace orogen {

enum class ArpMode : int { Up = 0, Down, UpDown, Random, AsPlayed, Chord, Count };

enum class ArpDiv : int { Quarter = 0, Eighth, Sixteenth, ThirtySecond,
                          EighthTriplet, SixteenthTriplet, Count };

inline const char* arpModeName (ArpMode m)
{
    switch (m)
    {
        case ArpMode::Up:       return "Up";
        case ArpMode::Down:     return "Down";
        case ArpMode::UpDown:   return "Up/Down";
        case ArpMode::Random:   return "Random";
        case ArpMode::AsPlayed: return "As Played";
        case ArpMode::Chord:    return "Chord";
        default:                return "?";
    }
}

inline const char* arpDivName (ArpDiv d)
{
    switch (d)
    {
        case ArpDiv::Quarter:          return "1/4";
        case ArpDiv::Eighth:           return "1/8";
        case ArpDiv::Sixteenth:        return "1/16";
        case ArpDiv::ThirtySecond:     return "1/32";
        case ArpDiv::EighthTriplet:    return "1/8T";
        case ArpDiv::SixteenthTriplet: return "1/16T";
        default:                       return "?";
    }
}

// Длина шага в биениях (1/4 = одно биение, триоль = 1/3 и т.д.)
inline double arpDivBeats (ArpDiv d)
{
    switch (d)
    {
        case ArpDiv::Quarter:          return 1.0;
        case ArpDiv::Eighth:           return 0.5;
        case ArpDiv::Sixteenth:        return 0.25;
        case ArpDiv::ThirtySecond:     return 0.125;
        case ArpDiv::EighthTriplet:    return 1.0 / 3.0;
        case ArpDiv::SixteenthTriplet: return 1.0 / 6.0;
        default:                       return 0.5;
    }
}

struct ArpSettings
{
    bool    on = false;
    ArpMode mode = ArpMode::Up;
    ArpDiv  div = ArpDiv::Eighth;
    int     octaves = 1;        // 1…4
    float   gate = 0.6f;        // 0.05…1 — доля шага, которую нота звучит
    bool    latch = false;      // аккорд держится до следующего нажатия
    float   swing = 0.0f;       // 0…0.6 — сдвиг нечётных шагов
};

struct ArpEvent
{
    int   sampleOffset = 0;
    bool  noteOn = false;
    int   note = 60;
    float velocity = 0.8f;
};

class Arpeggiator
{
public:
    static constexpr int kMaxHeld = 16;
    static constexpr int kMaxPending = 32;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        heldCount = 0;
        stepIndex = 0;
        samplesToStep = 0.0;
        gateLeft = 0.0;
        soundingCount = 0;
        current = -1;
        pendingCount = 0;
        rngState = 0x9E3779B9u;
        ons = 0;
        latchArmed = true;
    }

    void setSettings (const ArpSettings& s) noexcept
    {
        const bool wasOn = settings.on;
        settings = s;
        settings.octaves = std::max (1, std::min (4, settings.octaves));
        settings.gate = std::max (0.05f, std::min (1.0f, settings.gate));
        settings.swing = std::max (0.0f, std::min (0.6f, settings.swing));
        if (wasOn && ! settings.on)
        {
            // выключили — снимаем всё, что держали
            heldCount = 0;
            if (current >= 0) { pushPending (current, 0.0f, false); current = -1; }
            soundingCount = 0;
            gateLeft = 0.0;
        }
        if (settings.div != divCache) { divCache = settings.div; samplesToStep = 0.0; }
    }

    void setBpm (double bpm) noexcept { tempo = bpm > 1.0 ? bpm : 120.0; }

    const ArpSettings& getSettings() const noexcept { return settings; }
    int  heldNotes() const noexcept { return heldCount; }
    int  currentNote() const noexcept { return current; }
    long long noteOnCount() const noexcept { return ons; }

    void noteOn (int note, float velocity) noexcept
    {
        if (! settings.on) { pushPending (note, velocity, true); return; }

        if (settings.latch && latchArmed)
        {
            // начало нового аккорда: старые ноты забываем
            heldCount = 0;
            latchArmed = false;
        }
        if (heldCount < kMaxHeld) held[(size_t) heldCount++] = { note, velocity };
    }

    void noteOff (int note) noexcept
    {
        if (! settings.on) { pushPending (note, 0.0f, false); return; }
        if (settings.latch) { latchArmed = (heldCount > 0); return; }
        removeNote (note);
    }

    void allNotesOff() noexcept
    {
        heldCount = 0;
        latchArmed = true;
        if (current >= 0) { pushPending (current, 0.0f, false); current = -1; }
        soundingCount = 0;
        gateLeft = 0.0;
    }

    // Прогоняет numSamples и складывает события в out (не больше maxOut).
    // Возвращает число событий. Без аллокаций.
    int process (int numSamples, ArpEvent* out, int maxOut) noexcept
    {
        int n = 0;
        auto emit = [&] (int offset, bool on, int note, float vel)
        {
            if (n < maxOut) out[n++] = { offset, on, note, vel };
        };
        auto closeSounding = [&] (int offset)
        {
            for (int k = 0; k < soundingCount; ++k) emit (offset, false, sounding[k], 0.0f);
            soundingCount = 0;
            gateLeft = 0.0;
        };

        // выключенный арпеджиатор: отдаём накопленные note on/off как есть
        if (! settings.on)
        {
            for (int i = 0; i < pendingCount; ++i)
                emit (pending[(size_t) i].sampleOffset, pending[(size_t) i].noteOn,
                      pending[(size_t) i].note, pending[(size_t) i].velocity);
            pendingCount = 0;
            return n;
        }
        pendingCount = 0;

        const double stepSamples = stepLengthSamples();
        for (int i = 0; i < numSamples; ++i)
        {
            if (soundingCount > 0)
            {
                gateLeft -= 1.0;
                if (gateLeft <= 0.0) closeSounding (i);
            }

            samplesToStep -= 1.0;
            if (samplesToStep > 0.0) continue;

            const bool offBeat = (stepIndex & 1) != 0;
            samplesToStep = stepSamples * (offBeat
                              ? 1.0 + (double) settings.swing * 0.5
                              : 1.0 - (double) settings.swing * 0.5);
            ++stepIndex;

            if (heldCount == 0) continue;
            closeSounding (i);                   // gate = 1: нота ещё звучит, снимаем

            if (settings.mode == ArpMode::Chord)
            {
                for (int k = 0; k < heldCount; ++k)
                {
                    const int note = clampNote (held[(size_t) k].note);
                    emit (i, true, note, held[(size_t) k].velocity);
                    sounding[soundingCount++] = note;
                }
            }
            else
            {
                const int note = nextNote();
                if (note < 0) continue;
                emit (i, true, note, velocityFor (note));
                sounding[soundingCount++] = note;
            }
            gateLeft = std::max (1.0, (double) settings.gate * stepSamples);
            ++ons;
        }
        return n;
    }

private:
    struct Held { int note = 60; float velocity = 0.8f; };

    double stepLengthSamples() const noexcept
    {
        return std::max (32.0, arpDivBeats (settings.div) * 60.0 / tempo * sr);
    }

    static int clampNote (int n) noexcept { return n < 0 ? 0 : (n > 127 ? 127 : n); }

    int withOctave (int note, int octave) const noexcept
    {
        return clampNote (note + 12 * octave);
    }

    // нота номер idx в возрастающем порядке (n ≤ 16 — сортировка вставками дешевле всего)
    int sorted (int idx) const noexcept
    {
        int tmp[kMaxHeld];
        const int n = heldCount;
        for (int i = 0; i < n; ++i) tmp[i] = held[(size_t) i].note;
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && tmp[j - 1] > tmp[j]; --j) std::swap (tmp[j - 1], tmp[j]);
        return tmp[std::max (0, std::min (n - 1, idx))];
    }

    int nextNote() noexcept
    {
        if (heldCount == 0) return -1;
        const int span = heldCount * settings.octaves;

        switch (settings.mode)
        {
            case ArpMode::AsPlayed:
            {
                const int pos = (stepIndex - 1) % span;
                return withOctave (held[(size_t) (pos % heldCount)].note, pos / heldCount);
            }
            case ArpMode::Random:
            {
                rngState = rngState * 1664525u + 1013904223u;
                const int idx = (int) ((rngState >> 8) % (uint32_t) heldCount);
                const int pos = (stepIndex - 1) % span;
                return withOctave (sorted (idx), pos / heldCount);
            }
            case ArpMode::Down:
            {
                const int pos = (stepIndex - 1) % span;
                return withOctave (sorted (heldCount - 1 - (pos % heldCount)),
                                   settings.octaves - 1 - pos / heldCount);
            }
            case ArpMode::UpDown:
            {
                const int cycle = span > 1 ? span * 2 - 2 : 1;
                int pos = (stepIndex - 1) % cycle;
                if (pos >= span) pos = cycle - pos;
                return withOctave (sorted (pos % heldCount), pos / heldCount);
            }
            case ArpMode::Up:
            default:
            {
                const int pos = (stepIndex - 1) % span;
                return withOctave (sorted (pos % heldCount), pos / heldCount);
            }
        }
    }

    float velocityFor (int note) const noexcept
    {
        for (int i = 0; i < heldCount; ++i)
            if (held[(size_t) i].note == note) return held[(size_t) i].velocity;
        // нота могла быть транспонирована на октаву — ищем по остатку
        for (int i = 0; i < heldCount; ++i)
            if ((held[(size_t) i].note % 12) == (note % 12)) return held[(size_t) i].velocity;
        return 0.8f;
    }

    void removeNote (int note) noexcept
    {
        for (int i = 0; i < heldCount; ++i)
            if (held[(size_t) i].note == note)
            {
                for (int j = i; j + 1 < heldCount; ++j) held[(size_t) j] = held[(size_t) (j + 1)];
                --heldCount;
                return;
            }
    }

    void pushPending (int note, float vel, bool on) noexcept
    {
        if (pendingCount >= kMaxPending) return;
        pending[(size_t) pendingCount++] = { 0, on, note, vel };
    }

    ArpSettings settings {};
    ArpDiv divCache = ArpDiv::Eighth;
    Held held[kMaxHeld] {};
    ArpEvent pending[kMaxPending] {};
    int heldCount = 0, pendingCount = 0, stepIndex = 0, current = -1;
    int sounding[kMaxHeld] {};
    int soundingCount = 0;
    double samplesToStep = 0.0, gateLeft = 0.0;
    double sr = 48000.0, tempo = 120.0;
    bool latchArmed = true;
    uint32_t rngState = 0x9E3779B9u;
    long long ons = 0;
};

} // namespace orogen
