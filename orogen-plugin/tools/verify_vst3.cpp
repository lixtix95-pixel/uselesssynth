// Проверка собранного .vst3 «снаружи»: загружаем бандл так, как это делает хост
// (через VST3PluginFormat), и проверяем то, что видит DAW:
//   — имя/производителя/категорию из moduleinfo;
//   — число параметров и уникальные имена (порядок ID проверяет test_plugin);
//   — размер редактора (интерфейс из макета 1280×800);
//   — что процессор выдаёт звук и не выдаёт NaN.
//
//   verify_vst3 <путь к OROGEN.vst3> [секунды]
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

using namespace juce;

static constexpr int kNumApvtsParams = 298;   // см. Params.h / test_plugin

static int failures = 0;
static void check (bool ok, const String& what, const String& detail = {})
{
    std::printf ("  [ %s ] %s%s%s\n", ok ? "OK" : "FAIL", what.toRawUTF8(),
                 detail.isEmpty() ? "" : "  —  ", detail.toRawUTF8());
    if (! ok) ++failures;
}

static void check (bool ok, const char* what, const String& detail = {})
{
    check (ok, String::fromUTF8 (what), detail);
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juceInit;

    const File bundle (argc > 1 ? argv[1] : "OROGEN.vst3");
    const double seconds = argc > 2 ? std::atof (argv[2]) : 1.0;

    if (! bundle.exists())
    {
        std::printf ("нет файла: %s\n", bundle.getFullPathName().toRawUTF8());
        return 2;
    }

    AudioPluginFormatManager fm;
    fm.addFormat (std::make_unique<VST3PluginFormat>());

    OwnedArray<PluginDescription> types;
    auto* format = fm.getFormat (0);
    if (format == nullptr) { std::printf ("формат VST3 недоступен\n"); return 2; }

    format->findAllTypesForFile (types, bundle.getFullPathName());
    check (types.size() >= 1, "хост видит плагин в бандле", String (types.size()) + String::fromUTF8 (" тип(ов)"));
    if (types.isEmpty()) return 2;

    auto& d = *types[0];
    std::printf ("  имя: %s | %s | категория: %s\n",
                 d.name.toRawUTF8(), d.manufacturerName.toRawUTF8(), d.category.toRawUTF8());
    check (d.isInstrument, "плагин помечен как инструмент (IS_SYNTH)");
    check (d.name == "OROGEN", "имя плагина OROGEN", d.name);

    String err;
    auto inst = format->createInstanceFromDescription (d, 48000.0, 512, err);
    check (inst != nullptr, "инстанс создаётся", err);
    if (inst == nullptr) return 2;

    // --- параметры ---
    const auto params = inst->getParameters();

    // Обёртка VST3 добавляет к нашим параметрам 16 × 130 параметров MIDI CC —
    // это её штатное поведение. Считаем «свои» как всё остальное и отдельно
    // печатаем служебные, которые добавил сам хост-слой.
    int midiCC = 0, ours = 0;
    std::set<String> names;
    StringArray extra;
    for (auto* p : params)
    {
        const auto n = p->getName (64);
        names.insert (n);
        if (n.containsIgnoreCase ("MIDI CC")) ++midiCC;
        else { ++ours; if (ours > kNumApvtsParams) extra.add (n); }
    }

    std::printf ("  параметров всего: %d = наших %d + MIDI CC обёртки %d\n",
                 params.size(), ours, midiCC);
    if (! extra.isEmpty())
    {
        std::printf ("  служебные сверх наших:");
        for (const auto& e : extra) std::printf (" [%s]", e.toRawUTF8());
        std::printf ("\n");
    }

    check (ours == kNumApvtsParams + 2 && names.count ("Bypass") == 1 && names.count ("Program") == 1,
           "все параметры APVTS и два служебных доходят до хоста",
           String (ours) + " = " + String (kNumApvtsParams) + " + 2");
    check (midiCC == 16 * 130, "MIDI CC обёртки на месте", String (midiCC));
    check (names.size() == (size_t) params.size(), "имена параметров не совпадают ни у кого",
           String ((int) names.size()));

    // по одному представителю каждой группы: имена как в Params.h
    for (const char* probe : { "sA_Level", "sA_Cutoff", "sA_Filter", "sA_Engine",
                               "Reverb Mix", "Comb Hz A", "Master", "mod1_Amount",
                               "Arp On", "lfo1_Rate", "Plate Pre Filter", "Plate Input Diff 1",
                               "Plate Input Diff 2", "Plate Output" })
        check (names.count (String (probe)) == 1, String::fromUTF8 ("виден хосту: ") + probe);

    // --- состояние: сохранить/загрузить через настоящий VST3-хост ---
    const char* plateNames[] = { "Plate Pre Filter", "Plate Input Diff 1", "Plate Input Diff 2", "Plate Output" };
    const float plateValues[] = { 0.23f, 0.41f, 0.73f, 0.635f }; // нормализованный диапазон хоста
    AudioProcessorParameter* plateParams[4] {};
    for (auto* parameter : params)
        for (int i = 0; i < 4; ++i)
            if (parameter->getName (64) == plateNames[i]) plateParams[i] = parameter;
    for (int i = 0; i < 4; ++i)
        if (plateParams[i] != nullptr) plateParams[i]->setValueNotifyingHost (plateValues[i]);
    MemoryBlock state;
    inst->getStateInformation (state);
    check (state.getSize() > 0, "состояние сериализуется", String (state.getSize()) + String::fromUTF8 (" байт"));
    for (auto* parameter : plateParams)
        if (parameter != nullptr) parameter->setValueNotifyingHost (0.05f);
    inst->setStateInformation (state.getData(), (int) state.getSize());
    float maxStateDiff = 0.0f;
    int foundPlate = 0;
    for (int i = 0; i < 4; ++i)
        if (plateParams[i] != nullptr)
        {
            ++foundPlate;
            maxStateDiff = std::max (maxStateDiff, std::fabs (plateParams[i]->getValue() - plateValues[i]));
        }
    check (foundPlate == 4 && maxStateDiff < 0.002f,
           "4 параметра Plate переживают VST3 host state", String (maxStateDiff, 6));

    // --- редактор ---
    check (inst->hasEditor(), "есть редактор");
    if (auto* ed = inst->createEditorIfNeeded())
    {
        check (ed->getWidth() == 1280 && ed->getHeight() == 800,
               "размер редактора по макету", String (ed->getWidth()) + "×" + String (ed->getHeight()));

        Image img (Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        Graphics g (img);
        ed->paintEntireComponent (g, true);
        check (img.getWidth() == 1280, "редактор отрисовывается в изображение");
        inst->editorBeingDeleted (ed);
    }

    // --- звук ---
    inst->prepareToPlay (48000.0, 512);

    MidiBuffer midi;
    midi.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);

    const int blockSize = 512;
    const int blocks = (int) std::ceil (seconds * 48000.0 / blockSize);
    AudioBuffer<float> buffer (2, blockSize);

    double sumSq = 0.0;
    float peak = 0.0f;
    int nans = 0, samples = 0;

    for (int b = 0; b < blocks; ++b)
    {
        buffer.clear();
        inst->processBlock (buffer, midi);
        midi.clear();

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* data = buffer.getReadPointer (ch);
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = data[i];
                if (! std::isfinite (v)) ++nans;
                sumSq += (double) v * (double) v;
                peak = std::max (peak, std::abs (v));
                ++samples;
            }
        }
    }

    const double rms = std::sqrt (sumSq / (double) jmax (1, samples));
    check (rms > 0.01, "после note-on есть звук", "RMS " + String (rms, 4));
    check (nans == 0, "нет NaN/Inf", String (nans));
    check (peak < 1.0f, "нет клиппинга", String::fromUTF8 ("пик ") + String (peak, 3));

    inst->releaseResources();

    std::printf ("\n=== %s (провалов: %d)\n", failures == 0 ? "ПЛАГИН ПРОЙДЕН" : "ЕСТЬ ПРОВАЛЫ", failures);
    return failures == 0 ? 0 : 1;
}
