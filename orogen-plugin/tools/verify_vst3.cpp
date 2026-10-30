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
#include <array>
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
        // editorBeingDeleted снимает указатель у процессора, но не удаляет
        // сам объект. Иначе таймер и PreviewWorker продолжают обращаться к
        // уже уничтоженному VST3-инстансу при следующем проходе message loop.
        inst->editorBeingDeleted (ed);
        delete ed;
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
    inst.reset();

    // Отдельный инстанс VST3: Granular с минимальной плотностью через реальные
    // параметры хоста и MIDI. ROBIN даёт 1024 отсчёта задержки даже при mix=0,
    // поэтому измеряем общую задержку и остаток после неё раздельно.
    std::printf ("\n--- Granular через VST3-хост ---\n");
    String granularError;
    auto granular = format->createInstanceFromDescription (d, 48000.0, 128, granularError);
    check (granular != nullptr, "VST3-инстанс Granular создаётся", granularError);
    if (granular != nullptr)
    {
        granular->prepareToPlay (48000.0, 128);
        const auto gp = granular->getParameters();
        int missing = 0;
        const auto setNamed = [&] (const String& name, float value) {
            for (auto* p : gp)
                if (p->getName (64) == name) { p->setValueNotifyingHost (value); return; }
            ++missing;
            std::printf ("   не найден параметр: %s\n", name.toRawUTF8());
        };
        setNamed ("sA_Engine", 0.5f);          // 4 / 8 = Granular из девяти
        setNamed ("sA_On", 1.0f);
        for (const char* n : { "sB_On", "sC_On", "sD_On", "sE_On", "sF_On", "sG_On", "sH_On" }) setNamed (n, 0.0f);
        setNamed ("Patch Mode", 0.0f); setNamed ("Arp On", 0.0f);
        setNamed ("Stereo Width", 1.0f);  // полная панорама голоса
        setNamed ("Width", 2.0f / 3.0f); // FX width = 1.0 при диапазоне 0…1.5
        setNamed ("sA_P1", 0.35f); setNamed ("sA_P2", 0.45f);
        setNamed ("sA_P3", 0.0f); setNamed ("sA_P4", 0.0f);
        setNamed ("sA_Table", 0.0f); setNamed ("sA_Attack", 0.0f);
        setNamed ("sA_Sustain", 1.0f); setNamed ("sA_Tail", 0.0f);
        for (const char* n : { "Drive", "Chorus Mix", "Delay Mix", "Reverb Mix", "Plate Mix",
                               "Material Mix", "Spectre Mix", "Glitch Mix", "Robin Mix", "Comb Mix" })
            setNamed (n, 0.0f);
        setNamed ("Comp Ratio", 0.0f); setNamed ("Comp Makeup", 0.0f);
        MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (missing == 0, "параметры Granular и FX доступны VST3-хосту", String (missing) + String::fromUTF8 (" пропусков"));

        const int reported = granular->getLatencySamples();
        MidiBuffer event, none;
        event.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
        AudioBuffer<float> block (2, 128);
        int first = -1, nonFinite = 0;
        for (int b = 0; b < 120; ++b)
        {
            block.clear();
            granular->processBlock (block, b == 0 ? event : none);
            for (int i = 0; i < 128; ++i)
            {
                const float v = block.getSample (0, i);
                if (! std::isfinite (v)) ++nonFinite;
                if (first < 0 && std::fabs (v) > 1.0e-5f) first = b * 128 + i;
            }
        }
        std::printf ("   первый отсчёт %d, latency %d, остаток %.3f мс, NaN %d\n",
                     first, reported, first >= reported ? (first - reported) / 48.0 : -1.0, nonFinite);
        check (reported == 1024 && first >= reported && first - reported < 960 && nonFinite == 0,
               "Granular начинает звучать без 250-мс паузы",
               "latency " + String (reported) + ", onset " + String (first));

        // Панорама и Level меняются на той же ноте, через нормализованные
        // параметры настоящего VST3-хоста. Последняя половина окна заведомо
        // выходит за latency 1024 и время сглаживания.
        auto heldWindow = [&] {
            double left = 0.0, right = 0.0;
            int bad = 0;
            constexpr int blocks = 160;
            for (int b = 0; b < blocks; ++b)
            {
                block.clear();
                granular->processBlock (block, none);
                for (int i = 0; i < 128; ++i)
                {
                    const double l = block.getSample (0, i), r = block.getSample (1, i);
                    if (! std::isfinite (l) || ! std::isfinite (r)) ++bad;
                    if (b >= blocks / 2) { left += l * l; right += r * r; }
                }
            }
            return std::array<double, 3> { std::sqrt (left / (80.0 * 128.0)),
                                           std::sqrt (right / (80.0 * 128.0)), (double) bad };
        };
        setNamed ("sA_Pan", 0.0f);             // −1
        const auto l = heldWindow();
        setNamed ("sA_Pan", 1.0f);             // +1
        const auto r = heldWindow();
        std::printf ("   Pan -1 -> +1: L/R %.6f/%.6f -> %.6f/%.6f\n", l[0], l[1], r[0], r[1]);
        check (l[0] > 0.001 && l[1] < l[0] * 0.01 && r[1] > 0.001 && r[0] < r[1] * 0.01
               && l[2] == 0.0 && r[2] == 0.0,
               "VST3-хост автоматизирует Pan на удержанной ноте");

        setNamed ("sA_Level", 0.0f);
        const auto quiet = heldWindow();
        setNamed ("sA_Level", 1.0f);
        const auto restored = heldWindow();
        std::printf ("   Level 0 -> 1, tail 0: R RMS %.8f -> %.6f\n", quiet[1], restored[1]);
        check (quiet[1] < 1.0e-5 && restored[1] > 0.001 && quiet[2] == 0.0 && restored[2] == 0.0,
               "VST3 Level возвращает звук без нового note-on");
        check (missing == 0, "Pan, Level и параметры хвоста доступны хосту", String (missing) + " missing");
        granular->releaseResources();
    }
    granular.reset();

    // Фабричный Choir через реальный VST3. Это отдельный замер выхода,
    // а не обещание headroom для произвольных патчей и не замена сырому WAV.
    // Проверяем два заданных случая: штатную калибровку по центру и gain 1,
    // master 1 с полной левой панорамой. Все миксы FX = 0, compressor 1:1.
    std::printf ("\n--- Choir Granular через VST3-хост: два ограниченных сценария ---\n");
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        String error;
        auto choir = format->createInstanceFromDescription (d, 48000.0, 128, error);
        check (choir != nullptr, "инстанс Choir создаётся", String (scenario) + ": " + error);
        if (choir == nullptr) continue;
        choir->setCurrentProgram (0); choir->prepareToPlay (48000.0, 128);
        int missing = 0;
        const auto set = [&] (const String& name, float value) {
            for (auto* p : choir->getParameters())
                if (p->getName (64) == name) { p->setValueNotifyingHost (value); return; }
            ++missing; std::printf ("   не найден параметр: %s\n", name.toRawUTF8());
        };
        set ("sA_Engine", 0.5f); set ("sA_On", 1.0f);
        for (const char* n : { "sB_On", "sC_On", "sD_On", "sE_On", "sF_On", "sG_On", "sH_On" }) set (n, 0.0f);
        set ("Patch Mode", 0.0f); set ("Arp On", 0.0f);
        set ("sA_Table", 2.0f / 7.0f); set ("sA_P1", 0.35f); set ("sA_P2", 0.45f);
        set ("sA_P3", 0.0f); set ("sA_P4", 0.20f); set ("sA_Detune", 0.0f); set ("sA_Tune", 0.5f);
        set ("sA_Level", 1.0f); set ("sA_Pan", scenario == 0 ? 0.5f : 0.0f);
        set ("sA_Attack", 0.0f); set ("sA_Sustain", 1.0f); set ("sA_Tail", 0.0f);
        set ("sA_Cutoff", 1.0f); set ("sA_Reso", 0.0f);
        set ("Patch Gain", ((scenario == 0 ? 0.67f : 1.0f) - 0.05f) / 3.95f);
        set ("Master", scenario == 0 ? 0.4f : 0.5f); // физически 0.8 / 1.0
        set ("Stereo Width", 1.0f); set ("Width", 2.0f / 3.0f);
        set ("lfo1_Amount", 0.0f); set ("lfo2_Amount", 0.0f);
        for (int i = 1; i <= 8; ++i) set ("mod" + String (i) + "_Amount", 0.5f);
        for (const char* n : { "Drive", "Chorus Mix", "Delay Mix", "Reverb Mix", "Plate Mix",
                               "Material Mix", "Spectre Mix", "Glitch Mix", "Robin Mix", "Comb Mix" }) set (n, 0.0f);
        set ("Comp Ratio", 0.0f); set ("Comp Makeup", 0.0f);
        MessageManager::getInstance()->runDispatchLoopUntil (250);
        const int latency = choir->getLatencySamples();
        MidiBuffer on, none; on.addEvent (MidiMessage::noteOn (1, 60, 1.0f), 0);
        AudioBuffer<float> block (2, 128);
        int bad = 0, hits = 0, frames = 0;
        float peak = 0.0f; double energy = 0.0;
        for (int b = 0; b * 128 < 96000 + latency; ++b)
        {
            block.clear(); none.clear();
            choir->processBlock (block, b == 0 ? on : none);
            for (int i = 0; i < 128; ++i)
            {
                const int at = b * 128 + i;
                if (at < latency || at >= latency + 96000) continue;
                const float l = block.getSample (0, i), r = block.getSample (1, i);
                if (! std::isfinite (l) || ! std::isfinite (r)) ++bad;
                const float framePeak = std::max (std::fabs (l), std::fabs (r));
                peak = std::max (peak, framePeak);
                if (framePeak >= 0.979999f) ++hits;
                energy += (double) l * l + (double) r * r; ++frames;
            }
        }
        const double choirRms = std::sqrt (energy / std::max (1, frames * 2));
        std::printf ("   %s: 2 с, пик %.6f, stereo RMS %.6f, >=0.98: %d/%d, latency %d, NaN %d\n",
                     scenario == 0 ? "gain .67/master .8/pan centre" : "gain 1/master 1/pan left",
                     peak, choirRms, hits, frames, latency, bad);
        check (missing == 0 && latency == 1024 && frames == 96000 && bad == 0
               && choirRms > 0.001 && peak < 0.979999f && hits == 0,
               "данный сценарий Choir звучит и не достигает порога 0.98 на выходе",
               "peak " + String (peak, 6));
        choir->releaseResources();
    }

    // Настоящий VST3, 96 кГц, заводской Sub Bass на поздней позиции.
    // Источник имеет стабильную основную частоту 110 Гц за пределами атаки;
    // частота на выходе измеряется пересечениями нуля после ROBIN latency.
    std::printf ("\n--- Высота Granular на 96 кГц через VST3-хост ---\n");
    String pitchError;
    auto pitch = format->createInstanceFromDescription (d, 96000.0, 128, pitchError);
    check (pitch != nullptr, "VST3-инстанс для pitch 96 кГц создаётся", pitchError);
    if (pitch != nullptr)
    {
        pitch->setCurrentProgram (0);
        pitch->prepareToPlay (96000.0, 128);
        int missing = 0;
        const auto set = [&] (const String& name, float value) {
            for (auto* p : pitch->getParameters())
                if (p->getName (64) == name) { p->setValueNotifyingHost (value); return; }
            ++missing; std::printf ("   не найден параметр: %s\n", name.toRawUTF8());
        };
        set ("sA_Engine", 0.5f); set ("sA_On", 1.0f);
        for (const char* n : { "sB_On", "sC_On", "sD_On", "sE_On", "sF_On", "sG_On", "sH_On" }) set (n, 0.0f);
        set ("sA_Table", 1.0f); set ("sA_P1", 0.95f); set ("sA_P2", 1.0f);
        set ("sA_P3", 0.0f); set ("sA_P4", 0.0f); set ("sA_Tune", 0.5f); set ("sA_Detune", 0.0f);
        set ("sA_Level", 0.8f); set ("sA_Attack", 0.0f); set ("sA_Sustain", 1.0f); set ("sA_Tail", 0.0f);
        set ("sA_Cutoff", 1.0f); set ("sA_Reso", 0.0f); set ("Patch Mode", 0.0f); set ("Arp On", 0.0f);
        set ("lfo1_Amount", 0.0f); set ("lfo2_Amount", 0.0f);
        for (int i = 1; i <= 8; ++i) set ("mod" + String (i) + "_Amount", 0.5f);
        for (const char* n : { "Drive", "Chorus Mix", "Delay Mix", "Reverb Mix", "Plate Mix",
                               "Material Mix", "Spectre Mix", "Glitch Mix", "Robin Mix", "Comb Mix" }) set (n, 0.0f);
        set ("Comp Ratio", 0.0f); set ("Comp Makeup", 0.0f);
        MessageManager::getInstance()->runDispatchLoopUntil (250);

        const int latency = pitch->getLatencySamples();
        MidiBuffer note, empty; note.addEvent (MidiMessage::noteOn (1, 37, 0.8f), 0);
        AudioBuffer<float> block (2, 128);
        double firstCross = 0.0, lastCross = 0.0, energy = 0.0;
        float prev = 0.0f; int count = 0, bad = 0, crossings = 0;
        for (int b = 0; b < 180; ++b)
        {
            empty.clear(); block.clear();
            pitch->processBlock (block, b == 0 ? note : empty);
            for (int i = 0; i < 128; ++i)
            {
                const int at = b * 128 + i;
                const float x = block.getSample (0, i);
                if (! std::isfinite (x)) ++bad;
                if (at >= latency + 3840 && at < latency + 19200)
                {
                    energy += (double) x * x; ++count;
                    if (prev < 0.0f && x >= 0.0f)
                    {
                        const double crossing = at - 1.0 + (double) (-prev) / (double) (x - prev);
                        if (crossings++ == 0) firstCross = crossing;
                        lastCross = crossing;
                    }
                }
                prev = x;
            }
        }
        const double measured = crossings > 1 ? (crossings - 1) * 96000.0 / (lastCross - firstCross) : 0.0;
        const double target = 110.0 * std::pow (2.0, -8.0 / 12.0);
        const double cents = measured > 0.0 ? 1200.0 * std::log2 (measured / target) : 1.0e6;
        const double rms = std::sqrt (energy / std::max (1, count));
        std::printf ("   выход %.6f Гц / эталон %.6f Гц = %+.4f цента; latency %d, RMS %.6f, NaN %d\n",
                     measured, target, cents, latency, rms, bad);
        check (missing == 0 && latency == 1024 && bad == 0 && crossings >= 8 && rms > 1.0e-4
               && std::fabs (cents) < 1.0,
               "поздний Position не расстраивает MIDI-ноту через VST3", String (cents, 4) + " cents");
        pitch->releaseResources();
    }
    pitch.reset();

    // Внешний хост и два настоящих VST3-инстанса: MIDI, одинаковый звук,
    // меняем только Delay Time. Отсчитываем 1024 сэмпла ROBIN отдельно.
    std::printf ("\n--- Delay Time через VST3-хост ---\n");
    String delayErrorA, delayErrorB;
    auto delayA = format->createInstanceFromDescription (d, 48000.0, 128, delayErrorA);
    auto delayB = format->createInstanceFromDescription (d, 48000.0, 128, delayErrorB);
    check (delayA != nullptr && delayB != nullptr, "два VST3-инстанса для Delay создаются", delayErrorA + delayErrorB);
    if (delayA != nullptr && delayB != nullptr)
    {
        int missing = 0;
        const auto setDelayParam = [&] (AudioPluginInstance& target, const String& name, float value) {
            for (auto* p : target.getParameters())
                if (p->getName (64) == name) { p->setValueNotifyingHost (value); return; }
            ++missing;
            std::printf ("   не найден параметр: %s\n", name.toRawUTF8());
        };
        for (auto* q : { delayA.get(), delayB.get() })
        {
            q->setCurrentProgram (0);
            q->prepareToPlay (48000.0, 128);
            setDelayParam (*q, "sA_Engine", 0.0f);
            setDelayParam (*q, "sA_On", 1.0f);
            for (const char* n : { "sB_On", "sC_On", "sD_On", "sE_On", "sF_On", "sG_On", "sH_On" })
                setDelayParam (*q, n, 0.0f);
            setDelayParam (*q, "Patch Mode", 0.0f); setDelayParam (*q, "Arp On", 0.0f);
            setDelayParam (*q, "sA_Level", 0.8f); setDelayParam (*q, "sA_Attack", 0.0f);
            setDelayParam (*q, "sA_Sustain", 1.0f); setDelayParam (*q, "sA_Tail", 0.0f);
            setDelayParam (*q, "sA_Cutoff", 1.0f); setDelayParam (*q, "sA_Reso", 0.0f);
            for (const char* n : { "Drive", "Chorus Mix", "Reverb Mix", "Plate Mix", "Material Mix",
                                   "Spectre Mix", "Glitch Mix", "Robin Mix", "Comb Mix" })
                setDelayParam (*q, n, 0.0f);
            setDelayParam (*q, "Delay Mix", 1.0f); setDelayParam (*q, "Delay Time", 0.0f);
            setDelayParam (*q, "Delay Fb", 0.0f);
            setDelayParam (*q, "Comp Ratio", 0.0f); setDelayParam (*q, "Comp Makeup", 0.0f);
        }
        MessageManager::getInstance()->runDispatchLoopUntil (200);
        // VST3-хост может заменить MIDI-вход выходным буфером. Отдельные
        // объекты обязательны: иначе первая обработка съедает Note On второй.
        MidiBuffer noteA, noteB, emptyA, emptyB;
        noteA.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
        noteB.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
        AudioBuffer<float> a (2, 128), b (2, 128);
        float prior = 0.0f;
        for (int block = 0; block < 150; ++block)
        {
            emptyA.clear(); emptyB.clear();
            delayA->processBlock (a, block == 0 ? noteA : emptyA);
            delayB->processBlock (b, block == 0 ? noteB : emptyB);
            for (int i = 0; i < 128; ++i)
                prior = std::max (prior, std::fabs (a.getSample (0, i) - b.getSample (0, i)));
        }
        const int latency = delayA->getLatencySamples();
        // Диапазон параметра 0.02…1.5 с; 0.08 с в нормализованной шкале.
        setDelayParam (*delayA, "Delay Time", (0.08f - 0.02f) / (1.5f - 0.02f));
        float first = -1.0f; double later = 0.0, referenceEnergy = 0.0; int count = 0, bad = 0;
        for (int block = 0; block < 60; ++block)
        {
            emptyA.clear(); emptyB.clear();
            delayA->processBlock (a, emptyA); delayB->processBlock (b, emptyB);
            for (int i = 0; i < 128; ++i)
            {
                const int at = block * 128 + i;
                const float x = a.getSample (0, i), y = b.getSample (0, i);
                if (! std::isfinite (x) || ! std::isfinite (y)) ++bad;
                if (at == latency) first = std::fabs (x - y);
                if (at >= latency + 2400)
                {
                    later += (double) (x - y) * (x - y);
                    referenceEnergy += (double) y * y;
                    ++count;
                }
            }
        }
        const double delta = std::sqrt (later / std::max (1, count));
        const double refRms = std::sqrt (referenceEnergy / std::max (1, count));
        std::printf ("   latency %d, до смены Δ %.8f, первый после задержки Δ %.8f, далее ΔRMS %.6f, ref RMS %.6f, NaN %d\n",
                     latency, prior, first, delta, refRms, bad);
        check (missing == 0 && latency == 1024 && prior < 1.0e-6f && first >= 0.0f && first < 1.0e-5f && bad == 0,
               "Delay Time доступен хосту и не скачет в первом отсчёте", String (first, 8));
        check (count > 0 && refRms > 0.001 && delta > 1.0e-4,
               "изменение Delay Time слышно после перехода", String (delta, 6));
        delayA->releaseResources(); delayB->releaseResources();
    }

    delayA.reset(); delayB.reset();

    // Реальный повторный цикл выгрузки динамической библиотеки. Оставшиеся
    // контролы или их таймеры раньше продолжали обращаться в выгруженный код.
    std::printf ("\n--- Повторное открытие и выгрузка VST3 ---\n");
    int closed = 0;
    for (int cycle = 0; cycle < 6; ++cycle)
    {
        String cycleError;
        auto unit = format->createInstanceFromDescription (d, 48000.0, 128, cycleError);
        if (unit == nullptr) { std::printf ("   ошибка: %s\n", cycleError.toRawUTF8()); break; }
        auto* editor = unit->createEditorAndMakeActive();
        if (editor == nullptr) break;
        MessageManager::getInstance()->runDispatchLoopUntil (50);
        unit->editorBeingDeleted (editor);
        delete editor;
        unit.reset();
        // Обрабатываем отложенные сообщения и таймеры уже после dlclose.
        MessageManager::getInstance()->runDispatchLoopUntil (120);
        ++closed;
    }
    check (closed == 6, "шесть циклов редактор → выгрузка → очередь сообщений без падения",
           String (closed) + "/6");

    std::printf ("\n=== %s (провалов: %d)\n", failures == 0 ? "ПЛАГИН ПРОЙДЕН" : "ЕСТЬ ПРОВАЛЫ", failures);
    return failures == 0 ? 0 : 1;
}
