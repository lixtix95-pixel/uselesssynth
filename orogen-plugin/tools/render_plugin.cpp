// Офлайн-рендер через сам плагин (тот же AudioProcessor, что грузится в DAW).
//
//   orogen_render <preset-index> <out.wav> [seconds]
//
// Сценарий намеренно совпадает с тем, что делает хост: prepareToPlay, затем
// блоки по 512 сэмплов с MIDI-событиями, затем хвост. Всё, что мы измеряем в
// песочнице, относится именно к коду плагина, а не к стенду движков.
#include "../src/plugin/PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <cmath>
#include <vector>

using namespace orogen;

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& buf, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> out (f.createOutputStream());
    if (out == nullptr) return;
    std::unique_ptr<juce::AudioFormatWriter> w (
        fmt.createWriterFor (out.get(), sr, (unsigned int) buf.getNumChannels(), 16, {}, 0));
    if (w == nullptr) return;
    out.release();                                  // владение переходит writer'у
    w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

int main (int argc, char** argv)
{
    // без X11: только менеджер сообщений (нужен AsyncUpdater для задержки)
    auto* mm = juce::MessageManager::getInstance();
    juce::ignoreUnused (mm);

    const int presetIndex = argc > 1 ? std::atoi (argv[1]) : 1;
    const juce::File outFile (argc > 2 ? argv[2] : "plugin_demo.wav");
    const double seconds = argc > 3 ? std::atof (argv[3]) : 12.0;

    const double sr = 48000.0;
    const int blockSize = 512;
    const int total = (int) (sr * seconds);

    OrogenProcessor proc;
    proc.setPlayConfigDetails (0, 2, sr, blockSize);
    proc.prepareToPlay (sr, blockSize);

    proc.setCurrentProgram (presetIndex);
    std::printf ("пресет %d «%s», параметров у хоста: %d\n",
                 presetIndex, proc.getProgramName (presetIndex).toRawUTF8(),
                 proc.getParameters().size());

    juce::AudioBuffer<float> full (2, total);
    full.clear();

    // Аккорд с басом; вторая половина — та же нота с другой артикуляцией.
    const int notes[4] = { 45, 52, 57, 60 };
    const int noteOnAt = (int) (sr * 0.15);
    const int noteOffAt = (int) (sr * (seconds - 1.0));
    juce::MidiBuffer midi;

    int pos = 0;
    double budget = 0.0;                            // суммарное время в processBlock
    while (pos < total)
    {
        const int n = juce::jmin (blockSize, total - pos);
        juce::AudioBuffer<float> block (2, n);
        block.clear();

        midi.clear();
        if (noteOnAt >= pos && noteOnAt < pos + n)
            for (int k = 0; k < 4; ++k)
            {
                auto m = juce::MidiMessage::noteOn (1, notes[k], 0.85f);
                m.setTimeStamp (noteOnAt - pos);
                midi.addEvent (m, noteOnAt - pos);
            }
        if (noteOffAt >= pos && noteOffAt < pos + n)
            for (int k = 0; k < 4; ++k)
            {
                auto m = juce::MidiMessage::noteOff (1, notes[k]);
                midi.addEvent (m, noteOffAt - pos);
            }

        proc.processBlock (block, midi);
        budget += proc.lastBlockSeconds();
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, n);
        pos += n;
    }

    // --- метрики ----------------------------------------------------------
    double sum = 0.0, peak = 0.0;
    long long nan = 0, over = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* d = full.getReadPointer (ch);
        for (int i = 0; i < total; ++i)
        {
            const float v = d[i];
            if (! std::isfinite (v)) ++nan;
            peak = std::max (peak, (double) std::fabs (v));
            if (std::fabs (v) > 0.999f) ++over;
            sum += (double) v * v;
        }
    }
    const double rms = std::sqrt (sum / (double) (total * 2));
    const double cpu = 100.0 * budget / ((double) total / sr);
    std::printf ("пик %.3f  RMS %.4f  NaN %lld  отсчётов>0.999: %lld  задержка %d сэмплов  "
                 "CPU %.1f %% ядра\n", peak, rms, nan, over, proc.reportedLatency(), cpu);

    // файл пишем после нормализации на 0.89, как в остальных демо
    const float gain = peak > 1.0e-6 ? (float) (0.89 / peak) : 1.0f;
    full.applyGain (gain);
    writeWav (outFile, full, sr);
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("записано: %s (%.1f с, усиление %.3f)\n",
                 outFile.getFullPathName().toRawUTF8(), seconds, gain);
    return nan > 0 ? 1 : 0;
}
