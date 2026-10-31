// WAV I/O and serialization for user wavetables. Run file reading, FFT and
// serialization away from processBlock; playback only uses immutable assets.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "instrument/instrument.h"

namespace orogen {

struct WavetableLoadResult
{
    std::shared_ptr<const UserWavetable> table;
    juce::String error;
};

inline WavetableLoadResult loadWavetableWav (const juce::File& file, bool resynth, int cycleLength)
{
    juce::WavAudioFormat wav;
    auto stream = file.createInputStream();
    if (stream == nullptr) return { {}, "Could not open the WAV file." };
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (stream.release(), true));
    if (reader == nullptr) return { {}, "Unsupported or invalid WAV file." };
    if (reader->numChannels == 0 || reader->lengthInSamples < 32 || reader->sampleRate < 8000.0
        || reader->sampleRate > 384000.0)
        return { {}, "The WAV has an unsupported sample rate or is too short." };
    const auto n = reader->lengthInSamples;
    if (resynth && n > (juce::int64) std::llround (reader->sampleRate * 120.0))
        return { {}, "Resynthesis accepts WAV files up to 120 seconds; nothing was imported." };
    if (! resynth && (cycleLength < 32 || n > (juce::int64) cycleLength * kUserWtMaxFrames
                      || n % cycleLength != 0))
        return { {}, "Packed WAV must contain 1–256 complete frames of the selected length. Use RESYNTH for ordinary recordings." };
    if (n > (juce::int64) std::numeric_limits<int>::max()) return { {}, "WAV is too large." };
    // Deliberately use the left channel, as common wavetable formats do;
    // averaging stereo can cancel an out-of-phase source.
    juce::AudioBuffer<float> buffer (1, (int) n);
    if (! reader->read (&buffer, 0, (int) n, 0, true, false))
        return { {}, "Could not read the WAV samples." };
    std::vector<float> mono (buffer.getReadPointer (0), buffer.getReadPointer (0) + (int) n);
    std::vector<float> canonical;
    const bool ok = resynth ? resynthesizeWavetable (mono, reader->sampleRate, canonical)
                            : unpackWavetableFrames (mono, cycleLength, canonical);
    if (! ok) return { {}, "Invalid WAV samples or unsupported frame layout." };
    const int frames = (int) canonical.size() / kUserWtFrameSamples;
    auto table = makeUserWavetable (canonical, frames, file.getFileNameWithoutExtension().toStdString());
    if (! table) return { {}, "Could not prepare the wavetable." };
    return { std::move (table), {} };
}

inline juce::var userWavetablesToVar (const Instrument::UserTables& tables)
{
    juce::Array<juce::var> slots;
    for (const auto& table : tables)
    {
        if (! table) { slots.add (juce::var()); continue; }
        juce::MemoryOutputStream bytes;
        for (float sample : table->samples) bytes.writeFloat (sample); // little-endian float32
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", juce::String::fromUTF8 (table->name.c_str()));
        entry->setProperty ("frames", table->frames);
        entry->setProperty ("format", "f32le-2048-v1");
        entry->setProperty ("data", bytes.getMemoryBlock().toBase64Encoding());
        slots.add (juce::var (entry));
    }
    return juce::var (slots);
}

// Missing property means an older preset/state and clears custom assets.
// A malformed present payload is rejected as a whole, leaving state untouched.
inline bool userWavetablesFromVar (const juce::var& value, Instrument::UserTables& out)
{
    Instrument::UserTables candidate {};
    if (value.isVoid() || value.isUndefined()) { out = std::move (candidate); return true; }
    const auto* slots = value.getArray();
    if (slots == nullptr || slots->size() != 8) return false;
    for (int i = 0; i < 8; ++i)
    {
        const auto& item = slots->getReference (i);
        if (item.isVoid() || item.isUndefined()) continue;
        const auto* entry = item.getDynamicObject();
        if (entry == nullptr || entry->getProperty ("format").toString() != "f32le-2048-v1") return false;
        const int frames = (int) entry->getProperty ("frames");
        if (frames < 1 || frames > kUserWtMaxFrames) return false;
        const size_t expected = (size_t) frames * kUserWtFrameSamples * sizeof (float);
        const auto text = entry->getProperty ("data").toString();
        if ((size_t) text.length() > expected * 2 + 128) return false;
        juce::MemoryBlock block;
        if (! block.fromBase64Encoding (text) || block.getSize() != expected) return false;
        juce::MemoryInputStream input (block, false);
        std::vector<float> samples ((size_t) frames * kUserWtFrameSamples);
        for (auto& sample : samples) sample = input.readFloat();
        if (! validUserFrames (samples, frames)) return false;
        const auto name = entry->getProperty ("name").toString().substring (0, 256).toStdString();
        candidate[(size_t) i] = makeUserWavetable (samples, frames, name);
        if (! candidate[(size_t) i]) return false;
    }
    out = std::move (candidate);
    return true;
}

} // namespace orogen
