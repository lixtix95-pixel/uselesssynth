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
#include <cstdlib>
#include <set>

using namespace juce;

static constexpr int kNumApvtsParams = 890;   // см. Params.h / test_plugin

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

// Необязательный тест сквозного пути X11 -> UI внутри VST3 -> beginEdit /
// performEdit / endEdit -> параметры JUCE-хоста. XTEST двигает указатель,
// поэтому включать только в отдельном Xvfb (OROGEN_GUI_XTEST=1).
struct HostGestureTrace : AudioProcessorParameter::Listener
{
    AudioProcessorParameter* p = nullptr;
    int begins = 0, ends = 0, values = 0;
    bool active = false, valueInside = false, invalid = false;
    explicit HostGestureTrace (AudioProcessorParameter* parameter) : p (parameter)
    { if (p != nullptr) p->addListener (this); }
    ~HostGestureTrace() override { if (p != nullptr) p->removeListener (this); }
    void parameterValueChanged (int, float) override { ++values; if (active) valueInside = true; }
    void parameterGestureChanged (int, bool starting) override
    {
        if (starting) { if (active) invalid = true; active = true; ++begins; }
        else { if (! active) invalid = true; active = false; ++ends; }
    }
    bool balanced() const { return p != nullptr && begins == 1 && ends == 1 && ! active && ! invalid && valueInside; }
    String info() const { return String (begins) + "/" + String (values) + "/" + String (ends); }
};

static void testHostedGuiGestures (AudioPluginFormat& format, const PluginDescription& d)
{
   #if JUCE_LINUX
    if (std::getenv ("OROGEN_GUI_XTEST") == nullptr) return;
    std::printf ("\n--- X11 → VST3 → жесты хоста ---\n");
    DynamicLibrary x11, xtest;
    if (! x11.open ("libX11.so.6") || ! xtest.open ("libXtst.so.6"))
    { check (false, "XTEST: библиотеки libX11/libXtst доступны"); return; }
    const auto openDisplay = reinterpret_cast<void* (*) (const char*)> (x11.getFunction ("XOpenDisplay"));
    const auto closeDisplay = reinterpret_cast<int (*) (void*)> (x11.getFunction ("XCloseDisplay"));
    const auto flushDisplay = reinterpret_cast<int (*) (void*)> (x11.getFunction ("XFlush"));
    const auto move = reinterpret_cast<int (*) (void*, int, int, int, unsigned long)> (xtest.getFunction ("XTestFakeMotionEvent"));
    const auto button = reinterpret_cast<int (*) (void*, unsigned int, int, unsigned long)> (xtest.getFunction ("XTestFakeButtonEvent"));
    if (openDisplay == nullptr || closeDisplay == nullptr || flushDisplay == nullptr || move == nullptr || button == nullptr)
    { check (false, "XTEST: функции X11 доступны"); return; }
    void* display = openDisplay (nullptr);
    if (display == nullptr) { check (false, "XTEST: открыт X11 display"); return; }
    {
        String error;
        auto inst = format.createInstanceFromDescription (d, 48000.0, 512, error);
        if (inst == nullptr) { check (false, "инстанс VST3 для теста UI", error); closeDisplay (display); return; }
        auto findParam = [&] (const String& name) -> AudioProcessorParameter* {
            for (auto* p : inst->getParameters()) if (p->getName (64) == name) return p;
            return nullptr;
        };
        auto* editor = inst->createEditorAndMakeActive();
        if (editor == nullptr) { check (false, "редактор VST3 для теста UI"); closeDisplay (display); return; }
        editor->addToDesktop (0); editor->setTopLeftPosition (0, 0); editor->setVisible (true);
        auto* mm = MessageManager::getInstance(); mm->runDispatchLoopUntil (700);
        auto drag = [&] (Point<int> a, Point<int> b)
        {
            const auto pa = editor->localPointToGlobal (a), pb = editor->localPointToGlobal (b);
            move (display, -1, pa.x, pa.y, 0); flushDisplay (display); mm->runDispatchLoopUntil (80);
            button (display, 1, 1, 0); flushDisplay (display); mm->runDispatchLoopUntil (80);
            move (display, -1, pb.x, pb.y, 0); flushDisplay (display); mm->runDispatchLoopUntil (120);
            button (display, 1, 0, 0); flushDisplay (display); mm->runDispatchLoopUntil (180);
        };
        {
            HostGestureTrace p1 (findParam ("sA_P1")), p2 (findParam ("sA_P2"));
            drag ({ 500, 170 }, { 535, 155 });
            check (p1.balanced() && p2.balanced(),
                   "VST3 передаёт хосту begin/value/end для P1/P2 при drag дисплея",
                   p1.info() + " / " + p2.info());
        }
        {
            HostGestureTrace morph (findParam ("Morph")), glide (findParam ("Glide"));
            drag ({ 1186, 715 }, { 1215, 687 });
            check (morph.balanced() && glide.balanced(),
                   "VST3 передаёт хосту begin/value/end для Morph/Glide на VectorPad",
                   morph.info() + " / " + glide.info());
        }
        // The separate rack is tested through the hosted VST3, not through
        // internal OrogenEditor helpers: X11 -> VST3 edit callbacks -> host.
        {
            const auto click = [&] (Point<int> pos) {
                const auto pt = editor->localPointToGlobal (pos);
                move (display, -1, pt.x, pt.y, 0); flushDisplay (display); mm->runDispatchLoopUntil (80);
                button (display, 1, 1, 0); flushDisplay (display); mm->runDispatchLoopUntil (80);
                button (display, 1, 0, 0); flushDisplay (display); mm->runDispatchLoopUntil (180);
            };
            click ({ 306, 32 }); // FX RACK
            HostGestureTrace chorus (findParam ("CHORUS On"));
            click ({ 211, 240 }); // Chorus OFF -> ON
            check (chorus.balanced() && chorus.p->getValue() > 0.5f,
                   "VST3 FX rack: Chorus ON передаёт begin/value/end хосту", chorus.info());
            click ({ 95, 240 });  // select Chorus settings
            auto* delay = findParam ("Chorus Delay");
            if (delay != nullptr) delay->setValueNotifyingHost (0.5f);
            const float before = delay != nullptr ? delay->getValue() : -1.0f;
            HostGestureTrace knob (delay);
            drag ({ 1150, 301 }, { 1150, 260 });
            const float after = delay != nullptr ? delay->getValue() : -1.0f;
            check (knob.balanced() && std::fabs (after - before) > 0.05f,
                   "VST3 FX rack: регулятор DEL изменяет параметр хоста", knob.info() + " / " + String (before, 3) + " -> " + String (after, 3));
        }
        // Сначала уничтожаем hosted editor: его деструктор отсоединяет XEmbed
        // до уничтожения родительского native peer. removeFromDesktop() раньше
        // удалял X11-окно преждевременно и вызывал BadWindow в тестовом хосте.
        inst->editorBeingDeleted (editor); delete editor;
        inst.reset(); mm->runDispatchLoopUntil (150);
    }
    closeDisplay (display);
   #else
    ignoreUnused (format, d);
   #endif
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
    std::set<String> hostIds;
    for (int i = 0; i < params.size(); ++i)
        if (const auto* parameter = inst->getHostedParameter (i)) hostIds.insert (parameter->getParameterID());
    check (hostIds.size() == (size_t) params.size(), "все VST3 ParamID уникальны, включая копии FX",
           String ((int) hostIds.size()) + "/" + String (params.size()));

    // по одному представителю каждой группы: имена как в Params.h
    for (const char* probe : { "sA_Level", "sA_Cutoff", "sA_Filter", "sA_Engine",
                               "Reverb Mix", "Comb Hz A", "Master", "mod1_Amount",
                               "Arp On", "lfo1_Rate", "Plate Pre Filter", "Plate Input Diff 1",
                               "Plate Input Diff 2", "Plate Output", "sA_Grain Reverse", "sH_Grain Reverse" })
        check (names.count (String (probe)) == 1, String::fromUTF8 ("виден хосту: ") + probe);

    const char* rackNames[] = { "DRIVE On", "COMB On", "CHORUS On", "DELAY On", "REVERB On", "PLATE On",
                                "EQ On", "MATERIAL On", "SPECTRE On", "ROBIN On", "GLITCH On", "DYNAMICS On", "WIDTH On" };
    AudioProcessorParameter* rackParams[13] {};
    AudioProcessorParameter* rackMix = nullptr;
    AudioProcessorParameter* rackCross = nullptr;
    for (auto* parameter : params)
    {
        for (int i = 0; i < 13; ++i) if (parameter->getName (64) == rackNames[i]) rackParams[i] = parameter;
        if (parameter->getName (64) == "Drive Mix") rackMix = parameter;
        if (parameter->getName (64) == "Comb Cross %") rackCross = parameter;
    }
    MemoryBlock initialState;
    inst->getStateInformation (initialState);
    // Verify that host automation for an extra copy has its own parameter ID,
    // and that its rank + value survive an actual VST3 state roundtrip.
    AudioProcessorParameter *driveFirst = nullptr, *driveSecond = nullptr, *driveRank = nullptr, *driveSecondOn = nullptr;
    for (auto* parameter : params)
    {
        const auto name = parameter->getName (64);
        if (name == "Drive") driveFirst = parameter;
        if (name == "Drive #2") driveSecond = parameter;
        if (name == "DRIVE #2 Rack Position") driveRank = parameter;
        if (name == "DRIVE On #2") driveSecondOn = parameter;
    }
    if (driveFirst) driveFirst->setValueNotifyingHost (.18f);
    if (driveSecond) driveSecond->setValueNotifyingHost (.79f);
    if (driveRank) driveRank->setValueNotifyingHost (14.0f / 16.0f);
    if (driveSecondOn) driveSecondOn->setValueNotifyingHost (1.0f);
    MemoryBlock duplicatedState; inst->getStateInformation (duplicatedState);
    if (driveSecond) driveSecond->setValueNotifyingHost (.05f);
    if (driveRank) driveRank->setValueNotifyingHost (0.0f);
    if (driveSecondOn) driveSecondOn->setValueNotifyingHost (0.0f);
    inst->setStateInformation (duplicatedState.getData(), (int) duplicatedState.getSize());
    check (driveFirst && driveSecond && driveRank && driveSecondOn
           && std::fabs (driveFirst->getValue() - .18f) < .002f
           && std::fabs (driveSecond->getValue() - .79f) < .002f
           && std::fabs (driveRank->getValue() - 14.0f/16.0f) < .002f && driveSecondOn->getValue() > .5f,
           "VST3: независимая автоматизация копии и позиция восстанавливаются из host state");
    inst->setStateInformation (initialState.getData(), (int) initialState.getSize());

    // Two genuine VST3 instances with identical MIDI/state, differing only in
    // the order of EQ and DRIVE. This tests the hosted audio path, not labels.
    {
        String errorA, errorB;
        auto a = format->createInstanceFromDescription (d, 48000.0, 128, errorA);
        auto b = format->createInstanceFromDescription (d, 48000.0, 128, errorB);
        auto setNamed = [] (AudioPluginInstance& plugin, const String& name, float value) {
            for (auto* parameter : plugin.getParameters()) if (parameter->getName (64) == name)
            { parameter->setValueNotifyingHost (value); return true; }
            return false;
        };
        bool configured = a != nullptr && b != nullptr;
        const char* modules[] = { "DRIVE", "COMB", "CHORUS", "DELAY", "REVERB", "PLATE", "EQ", "MATERIAL", "SPECTRE", "ROBIN", "GLITCH", "DYNAMICS", "WIDTH" };
        if (configured)
        {
            for (auto* plugin : { a.get(), b.get() })
            {
                for (const auto* module : modules)
                    configured = setNamed (*plugin, String (module) + " #1 Rack Position", 0.0f) && configured;
                configured = setNamed (*plugin, "DRIVE On", 1.0f) && configured;
                configured = setNamed (*plugin, "EQ On", 1.0f) && configured;
                configured = setNamed (*plugin, "Drive", .8f) && configured;
                configured = setNamed (*plugin, "Drive Mix", .9f) && configured;
                configured = setNamed (*plugin, "EQ Low", .9f) && configured;
                configured = setNamed (*plugin, "EQ High", .1f) && configured;
            }
            configured = setNamed (*a, "DRIVE #1 Rack Position", 1.0f / 16.0f) && configured;
            configured = setNamed (*a, "EQ #1 Rack Position", 2.0f / 16.0f) && configured;
            configured = setNamed (*b, "EQ #1 Rack Position", 1.0f / 16.0f) && configured;
            configured = setNamed (*b, "DRIVE #1 Rack Position", 2.0f / 16.0f) && configured;
            a->prepareToPlay (48000.0, 128); b->prepareToPlay (48000.0, 128);
            AudioBuffer<float> xa (2, 128), xb (2, 128); MidiBuffer ma, mb, emptyA, emptyB;
            ma.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
            mb.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
            double diff = 0.0, energy = 0.0; int invalid = 0;
            for (int block = 0; block < 240; ++block)
            {
                xa.clear(); xb.clear(); a->processBlock (xa, block == 0 ? ma : emptyA); b->processBlock (xb, block == 0 ? mb : emptyB);
                for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 128; ++i)
                {
                    const double x = xa.getSample (ch, i), y = xb.getSample (ch, i);
                    if (! std::isfinite (x) || ! std::isfinite (y)) ++invalid;
                    diff += (x-y)*(x-y); energy += x*x;
                }
            }
            const double rms = std::sqrt (diff / (240.0 * 128.0 * 2.0));
            const double reference = std::sqrt (energy / (240.0 * 128.0 * 2.0));
            check (configured && invalid == 0 && reference > .001 && rms > .001,
                   "VST3: перестановка EQ и DRIVE меняет реальный звук при одинаковом MIDI",
                   "delta RMS=" + String (rms, 9) + ", ref=" + String (reference, 6));
            a->releaseResources(); b->releaseResources();
        }
        else check (false, "два VST3-инстанса для сравнения порядка", errorA + " / " + errorB);
    }

    // The host must receive changes in algorithmic latency, including an
    // OFF duplicate ROBIN and the return to the fixed 1024-sample dry path.
    {
        String latencyError;
        auto plugin = format->createInstanceFromDescription (d, 48000.0, 128, latencyError);
        AudioProcessorParameter* robinRank = nullptr;
        if (plugin != nullptr)
            for (auto* parameter : plugin->getParameters())
                if (parameter->getName (64) == "ROBIN #2 Rack Position") robinRank = parameter;
        if (robinRank != nullptr) robinRank->setValueNotifyingHost (14.0f / 16.0f);
        int added = -1, removed = -1;
        if (plugin != nullptr && robinRank != nullptr)
        {
            plugin->prepareToPlay (48000.0, 128);
            AudioBuffer<float> audio (2, 128); MidiBuffer midi;
            auto blocks = [&] {
                for (int i = 0; i < 24; ++i) { audio.clear(); plugin->processBlock (audio, midi); }
                MessageManager::getInstance()->runDispatchLoopUntil (100);
            };
            blocks(); added = plugin->getLatencySamples();
            robinRank->setValueNotifyingHost (0.0f); blocks(); removed = plugin->getLatencySamples();
            plugin->releaseResources();
        }
        check (added == 2048 && removed == 1024,
               "VST3 сообщает хосту latency копии ROBIN и после её удаления",
               String (added) + " -> " + String (removed));
    }

    int rackOff = 0;
    for (auto* parameter : rackParams) if (parameter != nullptr && parameter->getValue() < 0.5f) ++rackOff;
    check (rackOff == 13 && rackMix != nullptr && rackCross != nullptr,
           "VST3-хост видит 13 выключенных FX и новые регуляторы", String (rackOff) + "/13");
    for (int i = 0; i < 13; ++i) if (rackParams[i] != nullptr) rackParams[i]->setValueNotifyingHost ((i % 2) == 0 ? 1.0f : 0.0f);
    if (rackMix) rackMix->setValueNotifyingHost (0.37f);
    if (rackCross) rackCross->setValueNotifyingHost (0.64f);

    const char* monoNames[] = { "Chorus Mix Mode", "Glitch Timing", "Glitch Tempo Sync", "Glitch Length Division",
        "Glitch Highpass", "Comb ODIN Mix", "Spectre Reject Level dB", "Spectre Attack ms", "Spectre Release ms",
        "Spectre Window Extended index", "Glitch Dist", "Glitch Dry Level", "Glitch Wet Level",
        "Glitch Size Drift %", "Glitch Pitch Step semitones", "Glitch Reroll %" };
    std::array<AudioProcessorParameter*, 16> monoParams {};
    std::array<float, 16> monoValues {};
    for (size_t i = 0; i < monoParams.size(); ++i)
        for (auto* parameter : params) if (parameter->getName (64) == monoNames[i]) monoParams[i] = parameter;
    check (monoParams[0] != nullptr && monoParams[1] != nullptr
           && std::fabs (monoParams[0]->getDefaultValue() - 1.0f) < 1e-6f
           && std::fabs (monoParams[1]->getDefaultValue() - 1.0f) < 1e-6f
           && std::fabs (monoParams[0]->getValue() - 1.0f) < 1e-6f
           && std::fabs (monoParams[1]->getValue() - 1.0f) < 1e-6f,
           "VST3 host defaults согласованы с новыми режимами INIT");
    // VST3's controller can temporarily echo an arbitrary normalised value
    // even for a stepped parameter; the processor snaps it to a legal step.
    // Use distinct, representable steps so this compares the saved values,
    // rather than 0.20 against the actual Chorus choice 0.0, etc.
    const std::array<float, 16> monoRequested {
        .5f, 1.0f, 1.0f, .625f, 1.0f, .4f, .44f, .48f,
        .52f, .625f, .6f, .64f, .68f, .72f, .75f, .8f
    };
    for (size_t i = 0; i < monoParams.size(); ++i) if (monoParams[i] != nullptr) {
        monoParams[i]->setValueNotifyingHost (monoRequested[i]);
        monoValues[i] = monoParams[i]->getValue();
    }

    // --- состояние: сохранить/загрузить через настоящий VST3-хост ---
    const char* plateNames[] = { "Plate Pre Filter", "Plate Input Diff 1", "Plate Input Diff 2", "Plate Output" };
    const float plateValues[] = { 0.23f, 0.41f, 0.73f, 0.635f }; // нормализованный диапазон хоста
    AudioProcessorParameter* plateParams[4] {};
    for (auto* parameter : params)
        for (int i = 0; i < 4; ++i)
            if (parameter->getName (64) == plateNames[i]) plateParams[i] = parameter;
    for (int i = 0; i < 4; ++i)
        if (plateParams[i] != nullptr) plateParams[i]->setValueNotifyingHost (plateValues[i]);
    AudioProcessorParameter* reverseParams[8] {};
    int reverseDefaults = 0;
    for (int i = 0; i < 8; ++i)
    {
        const String name = "s" + String::charToString ((juce_wchar) ('A' + i)) + "_Grain Reverse";
        for (auto* parameter : params) if (parameter->getName (64) == name) reverseParams[i] = parameter;
        if (reverseParams[i] != nullptr)
        {
            if (std::fabs (reverseParams[i]->getValue()) < 1.0e-6f) ++reverseDefaults;
            reverseParams[i]->setValueNotifyingHost (0.1f * (float) (i + 1));
        }
    }
    check (reverseDefaults == 8, "8 параметров Reverse видны хосту и по умолчанию равны 0", String (reverseDefaults) + "/8");
    MemoryBlock state;
    inst->getStateInformation (state);
    check (state.getSize() > 0, "состояние сериализуется", String (state.getSize()) + String::fromUTF8 (" байт"));
    for (auto* parameter : plateParams)
        if (parameter != nullptr) parameter->setValueNotifyingHost (0.05f);
    for (auto* parameter : reverseParams)
        if (parameter != nullptr) parameter->setValueNotifyingHost (0.0f);
    for (auto* parameter : rackParams) if (parameter != nullptr) parameter->setValueNotifyingHost (0.0f);
    if (rackMix) rackMix->setValueNotifyingHost (0.9f);
    if (rackCross) rackCross->setValueNotifyingHost (0.1f);
    for (auto* parameter : monoParams) if (parameter != nullptr) parameter->setValueNotifyingHost (0.0f);
    inst->setStateInformation (state.getData(), (int) state.getSize());
    int monoRestored = 0;
    for (size_t i = 0; i < monoParams.size(); ++i)
    {
        if (monoParams[i] == nullptr)
            std::printf ("   MonoHuice state: parameter missing: %s\n", monoNames[i]);
        else if (std::fabs (monoParams[i]->getValue() - monoValues[i]) < .002f)
            ++monoRestored;
        else
            std::printf ("   MonoHuice state: %s expected %.6f restored %.6f\n",
                         monoNames[i], monoValues[i], monoParams[i]->getValue());
    }
    check (monoRestored == (int) monoParams.size(), "VST3 host state сохраняет режимы и новые ручки MonoHuice", String (monoRestored) + "/16");
    int restoredRack = 0;
    for (int i = 0; i < 13; ++i)
        if (rackParams[i] != nullptr && std::fabs (rackParams[i]->getValue() - ((i % 2) == 0 ? 1.0f : 0.0f)) < 0.001f) ++restoredRack;
    check (restoredRack == 13 && rackMix && rackCross
           && std::fabs (rackMix->getValue() - 0.37f) < 0.002f && std::fabs (rackCross->getValue() - 0.64f) < 0.002f,
           "VST3 host state сохраняет 13 power и новые настройки", String (restoredRack) + "/13");
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
    float maxReverseDiff = 0.0f;
    int foundReverse = 0;
    for (int i = 0; i < 8; ++i)
        if (reverseParams[i] != nullptr)
        {
            ++foundReverse;
            maxReverseDiff = std::max (maxReverseDiff, std::fabs (reverseParams[i]->getValue() - 0.1f * (float) (i + 1)));
        }
    check (foundReverse == 8 && maxReverseDiff < 0.002f,
           "8 параметров Reverse переживают VST3 host state", String (maxReverseDiff, 6));

    // При наличии предыдущего настоящего VST3 проверяем не только синтетически
    // удалённые XML-узлы, но и реальное состояние, которое сериализовал хост.
    if (argc > 3)
    {
        OwnedArray<PluginDescription> oldTypes;
        format->findAllTypesForFile (oldTypes, File (argv[3]).getFullPathName());
        String oldError;
        auto old = ! oldTypes.isEmpty() ? format->createInstanceFromDescription (*oldTypes[0], 48000.0, 128, oldError)
                                      : std::unique_ptr<AudioPluginInstance>();
        check (old != nullptr, "предыдущий VST3 загружается для проверки старой сессии", oldError);
        if (old != nullptr)
        {
            // Compare real hosted VST3 ParamIDs/ranges for every old user
            // parameter, not just their labels or XML state properties.
            int oldUserParams = 0, stableIds = 0;
            const auto oldParams = old->getParameters();
            for (int oi = 0; oi < oldParams.size(); ++oi)
            {
                auto* op = oldParams[oi]; const auto name = op->getName (64);
                if (name.containsIgnoreCase ("MIDI CC") || name == "Program" || name == "Bypass") continue;
                ++oldUserParams;
                const auto* oldHosted = old->getHostedParameter (oi);
                for (int ni = 0; ni < params.size(); ++ni)
                    if (params[ni]->getName (64) == name)
                    {
                        const auto* newHosted = inst->getHostedParameter (ni);
                        if (oldHosted && newHosted && oldHosted->getParameterID() == newHosted->getParameterID()
                            && op->getNumSteps() == params[ni]->getNumSteps()
                            && std::fabs (op->getDefaultValue() - params[ni]->getDefaultValue()) < 1.0e-6f
                            && op->getText (0.0f, 100) == params[ni]->getText (0.0f, 100)
                            && op->getText (0.5f, 100) == params[ni]->getText (0.5f, 100)
                            && op->getText (1.0f, 100) == params[ni]->getText (1.0f, 100))
                            ++stableIds;
                        break;
                    }
            }
            check (oldUserParams > 0 && stableIds == oldUserParams,
                   "все прежние VST3 ParamID, шаги и defaults сохранены",
                   String (stableIds) + "/" + String (oldUserParams));
            AudioProcessorParameter* oldP1 = nullptr;
            for (auto* p : old->getParameters()) if (p->getName (64) == "sA_P1") oldP1 = p;
            if (oldP1 != nullptr) oldP1->setValueNotifyingHost (0.713f);
            std::array<AudioProcessorParameter*, 13> oldPowers {};
            AudioProcessorParameter *oldChorusMode = nullptr, *oldGlitchMode = nullptr, *oldDriveMix = nullptr;
            for (auto* parameter : old->getParameters())
            {
                const auto name = parameter->getName (64);
                for (int i = 0; i < 13; ++i) if (name == rackNames[i]) oldPowers[(size_t) i] = parameter;
                if (name == "Chorus Mix Mode") oldChorusMode = parameter;
                if (name == "Glitch Timing") oldGlitchMode = parameter;
                if (name == "Drive Mix") oldDriveMix = parameter;
            }
            const bool hasPowers = oldPowers[0] != nullptr;
            if (hasPowers) for (int i = 0; i < 13; ++i)
                if (oldPowers[(size_t) i]) oldPowers[(size_t) i]->setValueNotifyingHost ((i == 0 || i == 2 || i == 3 || i == 6) ? 1.0f : 0.0f);
            if (oldChorusMode) oldChorusMode->setValueNotifyingHost (0.5f);
            if (oldGlitchMode) oldGlitchMode->setValueNotifyingHost (1.0f);
            if (oldDriveMix) oldDriveMix->setValueNotifyingHost (0.73f);
            MemoryBlock oldState;
            old->getStateInformation (oldState);
            for (auto* p : reverseParams) if (p != nullptr) p->setValueNotifyingHost (0.9f);
            inst->setStateInformation (oldState.getData(), (int) oldState.getSize());
            int zeros = 0;
            for (auto* p : reverseParams) if (p != nullptr && std::fabs (p->getValue()) < 1.0e-6f) ++zeros;
            AudioProcessorParameter* restoredP1 = nullptr;
            for (auto* p : params) if (p->getName (64) == "sA_P1") restoredP1 = p;
            check (oldState.getSize() > 0 && oldP1 != nullptr && restoredP1 != nullptr
                   && std::fabs (restoredP1->getValue() - 0.713f) < 0.002f && zeros == 8,
                   "реальная сессия предыдущего VST3 сохраняет старый P1 и сбрасывает Reverse",
                   String (zeros) + "/8, P1=" + (restoredP1 != nullptr ? String (restoredP1->getValue(), 4) : "нет"));
            int matchingPower = 0;
            for (int i = 0; i < 13; ++i)
            {
                const bool expected = hasPowers ? (i == 0 || i == 2 || i == 3 || i == 6) : true;
                if (rackParams[i] && (rackParams[i]->getValue() > .5f) == expected) ++matchingPower;
            }
            const float expectedMix = oldDriveMix ? .73f : 1.0f;
            check (matchingPower == 13 && rackMix != nullptr && std::fabs (rackMix->getValue() - expectedMix) < .002f,
                   "реальная старая сессия VST3 сохраняет power и Drive Mix", String (matchingPower) + "/13");
            const float expectedChorus = oldChorusMode ? .5f : 0.0f;
            const float expectedGlitch = oldGlitchMode ? 1.0f : 0.0f;
            check (monoParams[0] && monoParams[1] && std::fabs (monoParams[0]->getValue() - expectedChorus) < .002f
                   && std::fabs (monoParams[1]->getValue() - expectedGlitch) < .002f,
                   "реальная старая сессия сохраняет режимы CHORUS и GLITCH или выбирает Legacy");

            // Both plugin instances render the very same legacy state/MIDI.
            old->prepareToPlay (48000.0, 128); inst->prepareToPlay (48000.0, 128);
            MessageManager::getInstance()->runDispatchLoopUntil (200);
            AudioBuffer<float> x (2, 128), y (2, 128); MidiBuffer ma, mb, emptyA, emptyB;
            ma.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
            mb.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
            double diff = 0.0, energy = 0.0, peakDiff = 0.0; int invalid = 0;
            for (int b = 0; b < 360; ++b)
            {
                x.clear(); y.clear(); old->processBlock (x, b == 0 ? ma : emptyA); inst->processBlock (y, b == 0 ? mb : emptyB);
                for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 128; ++i)
                {
                    const double a = x.getSample (ch, i), c = y.getSample (ch, i), d = a-c;
                    if (! std::isfinite (a) || ! std::isfinite (c)) ++invalid;
                    diff += d*d; energy += a*a; peakDiff = std::max (peakDiff, std::fabs (d));
                }
            }
            const double rms = std::sqrt (diff / (360.0 * 128.0 * 2.0));
            const double refRms = std::sqrt (energy / (360.0 * 128.0 * 2.0));
            check (invalid == 0 && refRms > 0.001 && rms < 1.0e-6,
                   "старая VST3-сессия звучит как прежний плагин (одинаковый MIDI)",
                   "delta RMS=" + String (rms, 9) + ", max=" + String (peakDiff, 9) + ", ref=" + String (refRms, 6));
            old->releaseResources(); inst->releaseResources();
            inst->setStateInformation (state.getData(), (int) state.getSize());
        }
    }

    testHostedGuiGestures (*format, d);
    inst->setStateInformation (initialState.getData(), (int) initialState.getSize());

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

    // Два реальных VST3-инстанса, одна и та же удержанная нота. Меняется
    // только host-параметр Reverse; сравниваем сигнал после ROBIN latency.
    std::printf ("\n--- Reverse через VST3-хост на удержанной ноте ---\n");
    String err0, err1;
    auto reference = format->createInstanceFromDescription (d, 48000.0, 128, err0);
    auto reversed = format->createInstanceFromDescription (d, 48000.0, 128, err1);
    check (reference != nullptr && reversed != nullptr, "оба VST3-инстанса Reverse созданы", err0 + " " + err1);
    if (reference != nullptr && reversed != nullptr)
    {
        int missing = 0;
        auto setHost = [&] (AudioPluginInstance& instance, const String& name, float value) {
            for (auto* parameter : instance.getParameters())
                if (parameter->getName (64) == name) { parameter->setValueNotifyingHost (value); return; }
            ++missing; std::printf ("   не найден параметр: %s\n", name.toRawUTF8());
        };
        for (auto* instance : { reference.get(), reversed.get() })
        {
            instance->prepareToPlay (48000.0, 128);
            setHost (*instance, "sA_Engine", 0.5f); setHost (*instance, "sA_On", 1.0f);
            for (const char* name : { "sB_On", "sC_On", "sD_On", "sE_On", "sF_On", "sG_On", "sH_On" })
                setHost (*instance, name, 0.0f);
            setHost (*instance, "Patch Mode", 0.0f); setHost (*instance, "Arp On", 0.0f);
            setHost (*instance, "sA_P1", 0.35f); setHost (*instance, "sA_P2", 0.45f);
            setHost (*instance, "sA_P3", 0.7f); setHost (*instance, "sA_P4", 0.3f);
            setHost (*instance, "sA_Table", 0.0f); setHost (*instance, "sA_Detune", 0.0f);
            setHost (*instance, "sA_Grain Reverse", 0.0f); setHost (*instance, "sA_Attack", 0.0f);
            setHost (*instance, "sA_Sustain", 1.0f); setHost (*instance, "sA_Tail", 0.0f);
            for (const char* name : { "Drive", "Chorus Mix", "Delay Mix", "Reverb Mix", "Plate Mix",
                                      "Material Mix", "Spectre Mix", "Glitch Mix", "Robin Mix", "Comb Mix" })
                setHost (*instance, name, 0.0f);
            setHost (*instance, "Comp Ratio", 0.0f); setHost (*instance, "Comp Makeup", 0.0f);
        }
        MessageManager::getInstance()->runDispatchLoopUntil (250);
        AudioBuffer<float> a (2, 128), b (2, 128);
        MidiBuffer noteA, noteB, noneA, noneB;
        noteA.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
        noteB.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
        float beforeMax = 0.0f;
        for (int block = 0; block < 80; ++block)
        {
            a.clear(); b.clear();
            reference->processBlock (a, block == 0 ? noteA : noneA);
            reversed->processBlock (b, block == 0 ? noteB : noneB);
            for (int i = 0; i < 128; ++i)
                beforeMax = std::max (beforeMax, std::fabs (a.getSample (0, i) - b.getSample (0, i)));
        }
        setHost (*reversed, "sA_Grain Reverse", 1.0f);
        double difference = 0.0, energy = 0.0; int bad = 0, n = 0;
        for (int block = 0; block < 500; ++block)
        {
            a.clear(); b.clear(); reference->processBlock (a, noneA); reversed->processBlock (b, noneB);
            if (block < 16) continue;
            for (int i = 0; i < 128; ++i)
            {
                const float x = a.getSample (0, i), y = b.getSample (0, i);
                if (! std::isfinite (x) || ! std::isfinite (y)) ++bad;
                const double diff = (double) x - y;
                difference += diff * diff; energy += (double) x * x; ++n;
            }
        }
        const double relative = std::sqrt (difference / std::max (1.0e-20, energy));
        const double referenceRms = std::sqrt (energy / std::max (1, n));
        std::printf ("   до изменения max |Δ| %.9f; после ΔRMS/RMS %.6f, RMS %.6f, NaN %d\n",
                     beforeMax, relative, referenceRms, bad);
        check (missing == 0 && beforeMax < 1.0e-6f && relative > 0.1 && referenceRms > 0.001 && bad == 0,
               "автоматизация Reverse через VST3 меняет звук без повторного note-on", String (relative, 6));
        reference->releaseResources(); reversed->releaseResources();
    }
    reference.reset(); reversed.reset();

    // Два одинаковых слота в противоположных каналах. При Spread=0
    // измеряем только независимость Reverse; при Spread=0.6 проверяем,
    // что её же не требуется для независимых потоков положения и pitch.
    std::printf ("\n--- Независимость Reverse/Spread A/B через реальный VST3 ---\n");
    double stereoDiff[2][3] {}, corr[2][3] {};
    int stereoMissing = 0, stereoBad = 0, stereoCreated = 0;
    for (int spreadCase = 0; spreadCase < 2; ++spreadCase)
    for (int mode = 0; mode < 3; ++mode)
    {
        String createError;
        auto stereo = format->createInstanceFromDescription (d, 48000.0, 128, createError);
        if (stereo == nullptr) { std::printf ("   ошибка инстанса: %s\n", createError.toRawUTF8()); continue; }
        ++stereoCreated;
        stereo->prepareToPlay (48000.0, 128);
        const auto set = [&] (const String& name, float value) {
            for (auto* parameter : stereo->getParameters())
                if (parameter->getName (64) == name) { parameter->setValueNotifyingHost (value); return; }
            ++stereoMissing; std::printf ("   не найден: %s\n", name.toRawUTF8());
        };
        for (const char letter : { 'A', 'B' })
        {
            const String prefix = "s" + String::charToString ((juce_wchar) letter) + "_";
            set (prefix + "Engine", 0.5f); set (prefix + "On", 1.0f);
            set (prefix + "P1", 0.35f); set (prefix + "P2", 0.45f);
            set (prefix + "P3", 0.7f); set (prefix + "P4", spreadCase == 0 ? 0.0f : 0.6f);
            set (prefix + "Table", 0.0f); set (prefix + "Detune", 0.0f);
            set (prefix + "Grain Reverse", 0.5f * (float) mode);
            set (prefix + "Level", 0.4f); set (prefix + "Pan", letter == 'A' ? 0.0f : 1.0f);
            set (prefix + "Attack", 0.0f); set (prefix + "Decay", 0.1f);
            set (prefix + "Sustain", 1.0f); set (prefix + "Release", 0.1f);
            set (prefix + "Tail", 0.0f); set (prefix + "Cutoff", 1.0f); set (prefix + "Reso", 0.0f);
        }
        for (const char* name : { "sC_On", "sD_On", "sE_On", "sF_On", "sG_On", "sH_On" }) set (name, 0.0f);
        set ("Patch Mode", 0.0f); set ("Arp On", 0.0f); set ("Stereo Width", 1.0f); set ("Width", 2.0f / 3.0f);
        for (const char* name : { "Drive", "Chorus Mix", "Delay Mix", "Reverb Mix", "Plate Mix", "Material Mix",
                                  "Spectre Mix", "Glitch Mix", "Robin Mix", "Comb Mix", "Comp Ratio", "Comp Makeup" }) set (name, 0.0f);
        MessageManager::getInstance()->runDispatchLoopUntil (200);
        AudioBuffer<float> block (2, 128);
        MidiBuffer note, none; note.addEvent (MidiMessage::noteOn (1, 60, 0.8f), 0);
        double dl = 0.0, ll = 0.0, rr = 0.0, lr = 0.0;
        for (int b = 0; b < 900; ++b)
        {
            block.clear(); stereo->processBlock (block, b == 0 ? note : none);
            if (b < 16) continue;
            for (int i = 0; i < 128; ++i)
            {
                const double l = block.getSample (0, i), r = block.getSample (1, i);
                if (! std::isfinite (l) || ! std::isfinite (r)) ++stereoBad;
                dl += (l-r)*(l-r); ll += l*l; rr += r*r; lr += l*r;
            }
        }
        stereoDiff[spreadCase][mode] = std::sqrt (dl / std::max (1.0e-20, 0.5 * (ll + rr)));
        corr[spreadCase][mode] = lr / std::sqrt (std::max (1.0e-20, ll * rr));
        stereo->releaseResources();
    }
    for (int s = 0; s < 2; ++s)
        std::printf ("   Spread %.1f, Reverse 0/50/100%%: ΔRMS/RMS %.7f / %.7f / %.7f, корреляция %.5f / %.5f / %.5f, NaN %d\n",
                     s ? 0.6 : 0.0, stereoDiff[s][0], stereoDiff[s][1], stereoDiff[s][2],
                     corr[s][0], corr[s][1], corr[s][2], stereoBad);
    check (stereoCreated == 6 && stereoMissing == 0 && stereoBad == 0
           && stereoDiff[0][0] < 0.001 && stereoDiff[0][1] > 0.1 && stereoDiff[0][2] < 0.001,
           "при Spread 0 Reverse 50% разносит A/B, а 0/100% совпадают через VST3",
           String (stereoDiff[0][0], 6) + " / " + String (stereoDiff[0][1], 6) + " / " + String (stereoDiff[0][2], 6));
    check (stereoCreated == 6 && stereoMissing == 0 && stereoBad == 0
           && stereoDiff[1][0] > 0.1 && stereoDiff[1][1] > 0.1 && stereoDiff[1][2] > 0.1,
           "Spread 0.6 разносит A/B при Reverse 0/50/100% через VST3",
           String (stereoDiff[1][0], 6) + " / " + String (stereoDiff[1][1], 6) + " / " + String (stereoDiff[1][2], 6));

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
            // INIT now bypasses every effect, independently of Mix. Activate
            // Delay explicitly so this remains an audible Time automation test.
            setDelayParam (*q, "DELAY On", 1.0f);
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
