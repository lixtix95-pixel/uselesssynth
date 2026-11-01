// User Granular sample decoding and embedded state. Never call from render().
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "instrument/instrument.h"

namespace orogen {

struct GranularLoadResult
{
    std::shared_ptr<const UserSample> sample;
    juce::String error;
};

inline bool granularFileExtensionSupported (const juce::File& file)
{
    return file.hasFileExtension ("wav;aif;aiff;flac");
}

inline GranularLoadResult loadGranularSample (const juce::File& file)
{
    if (! granularFileExtensionSupported (file))
        return { {}, "Choose a WAV, AIFF or FLAC file." };
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (! reader || reader->numChannels == 0)
        return { {}, "Could not open or decode the audio file." };
    const double sr = reader->sampleRate;
    const auto frames = reader->lengthInSamples;
    if (! std::isfinite (sr) || sr < kUserSampleMinRate || sr > kUserSampleMaxRate)
        return { {}, "Supported sample rates are 8-192 kHz." };
    if (frames < (juce::int64) kUserSampleMinFrames)
        return { {}, "The sample is too short (minimum 512 frames)." };
    if (frames > (juce::int64) kUserSampleMaxFrames || (double) frames > sr * kUserSampleMaxSeconds + 0.5)
        return { {}, "The sample is too long: maximum 30 seconds and 1,440,000 frames (30 s at 48 kHz)." };
    try
    {
        // Granular currently renders mono: intentionally take the left/first
        // channel, avoiding phase cancellation from averaging stereo sources.
        juce::AudioBuffer<float> buffer (1, (int) frames);
        if (! reader->read (&buffer, 0, (int) frames, 0, true, false))
            return { {}, "Could not read the complete sample." };
        std::vector<float> mono (buffer.getReadPointer (0), buffer.getReadPointer (0) + (int) frames);
        auto sample = makeUserSample (std::move (mono), sr,
                                      file.getFileNameWithoutExtension().substring (0, 256).toStdString());
        if (! sample) return { {}, "Invalid audio values in the sample." };
        return { std::move (sample), {} };
    }
    catch (const std::bad_alloc&)
    {
        return { {}, "Not enough memory to prepare the sample." };
    }
}

inline juce::var userSamplesToVar (const Instrument::UserSamples& samples)
{
    juce::Array<juce::var> slots;
    for (const auto& sample : samples)
    {
        if (! sample) { slots.add (juce::var()); continue; }
        juce::MemoryOutputStream bytes;
        for (float s : sample->shot.levels[0]) bytes.writeFloat (s); // little-endian float32
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", juce::String::fromUTF8 (sample->shot.name.c_str()));
        entry->setProperty ("frames", (int) sample->shot.levels[0].size());
        entry->setProperty ("rate", sample->sourceRate);
        entry->setProperty ("format", "f32le-mono-v1");
        entry->setProperty ("data", bytes.getMemoryBlock().toBase64Encoding());
        slots.add (juce::var (entry));
    }
    return juce::var (slots);
}

// Missing property is a legacy preset/state and clears user samples. A present
// malformed payload rejects the entire restore before any parameters change.
inline bool userSamplesFromVar (const juce::var& value, Instrument::UserSamples& out)
{
    Instrument::UserSamples candidate {};
    if (value.isVoid() || value.isUndefined()) { out = std::move (candidate); return true; }
    const auto* slots = value.getArray();
    if (slots == nullptr || slots->size() != 8) return false;
    for (int i = 0; i < 8; ++i)
    {
        const auto& item = slots->getReference (i);
        if (item.isVoid() || item.isUndefined()) continue;
        const auto* entry = item.getDynamicObject();
        if (entry == nullptr || entry->getProperty ("format").toString() != "f32le-mono-v1") return false;
        const int frames = (int) entry->getProperty ("frames");
        const double rate = (double) entry->getProperty ("rate");
        if (frames < (int) kUserSampleMinFrames || frames > (int) kUserSampleMaxFrames
            || ! std::isfinite (rate) || rate < kUserSampleMinRate || rate > kUserSampleMaxRate
            || (double) frames > rate * kUserSampleMaxSeconds + 0.5) return false;
        const size_t expected = (size_t) frames * sizeof (float);
        const auto text = entry->getProperty ("data").toString();
        if ((size_t) text.length() > expected * 2 + 128) return false;
        juce::MemoryBlock block;
        if (! block.fromBase64Encoding (text) || block.getSize() != expected) return false;
        juce::MemoryInputStream input (block, false);
        std::vector<float> audio ((size_t) frames);
        for (auto& s : audio) s = input.readFloat();
        if (! validUserSample (audio, rate)) return false;
        const auto name = entry->getProperty ("name").toString().substring (0, 256).toStdString();
        candidate[(size_t) i] = makeUserSample (std::move (audio), rate, name);
        if (! candidate[(size_t) i]) return false;
    }
    out = std::move (candidate);
    return true;
}

} // namespace orogen
