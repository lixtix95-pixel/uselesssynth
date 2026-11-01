// Included by verify_vst3 after HostGestureTrace. This exercises the real
// hosted VST3 editor with an external X11 Xdnd source, state and audio.
bool nativeFileDrop (juce::Component&, juce::Point<int>, const juce::File&,
                            const std::function<bool()>&, bool&, int&);

static void testHostedSampleDrop (juce::AudioPluginFormat& format, const juce::PluginDescription& description)
{
   #if JUCE_LINUX
    if (std::getenv ("OROGEN_GUI_XTEST") == nullptr) return;
    std::printf ("\n--- X11 Xdnd -> hosted VST3 -> sample state/audio ---\n");
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("orogen-hosted-drag-sample.wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    juce::AudioBuffer<float> source (1, 44100);
    for (int i = 0; i < 44100; ++i)
        source.setSample (0, i, (float) (.4 * std::sin (6.283185307179586 * 220.0 * i / 44100.0)));
    auto output = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer (output ? wav.createWriterFor (output.release(), 44100, 1, 16, {}, 0) : nullptr);
    const bool written = writer && writer->writeFromAudioSampleBuffer (source, 0, source.getNumSamples());
    writer.reset();
    if (! written) { check (false, "hosted sample fixture created"); return; }
    juce::String error;
    auto inst = format.createInstanceFromDescription (description, 48000.0, 128, error);
    if (! inst) { check (false, "hosted VST3 instance for sample drop", error); file.deleteFile(); return; }
    auto find = [] (juce::AudioPluginInstance& p, const juce::String& name) -> juce::AudioProcessorParameter* {
        for (auto* parameter : p.getParameters()) if (parameter->getName (64) == name) return parameter;
        return nullptr;
    };
    auto set = [&] (const juce::String& name, float value) {
        if (auto* p = find (*inst, name)) p->setValueNotifyingHost (value);
    };
    inst->prepareToPlay (48000, 128);
    for (int i = 0; i < 8; ++i) set ("s" + juce::String::charToString ((juce::juce_wchar) ('A' + i)) + "_On", i == 0 ? 1.0f : 0.0f);
    set ("sA_Engine", .5f);  // Granular index 4 / 8
    set ("sA_Detune", 0.0f);
    set ("sA_Attack", 0.0f);
    set ("sA_Sustain", 1.0f);
    set ("sA_P3", .8f);
    set ("sA_P4", 0.0f);
    for (const char* name : { "DRIVE On", "COMB On", "CHORUS On", "DELAY On", "REVERB On", "PLATE On",
                              "EQ On", "MATERIAL On", "SPECTRE On", "ROBIN On", "GLITCH On", "DYNAMICS On", "WIDTH On" }) set (name, 0.0f);
    auto* editor = inst->createEditorAndMakeActive();
    if (! editor) { check (false, "hosted VST3 editor for sample drop"); inst->releaseResources(); file.deleteFile(); return; }
    editor->addToDesktop (0); editor->setTopLeftPosition (20, 20); editor->setVisible (true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    auto* choice = find (*inst, "sA_User Granular Sample");
    HostGestureTrace gesture (choice);
    bool hovering = false; int selections = 0;
    const bool osDrop = nativeFileDrop (*editor, { 640, 320 }, file, [] { return false; }, hovering, selections);
    for (int i = 0; i < 500 && choice && choice->getValue() < .5f; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    const bool selected = choice && choice->getValue() > .5f;
    check (osDrop && selected && gesture.balanced(),
           "external X11 file drop reaches hosted Granular and sends host gesture",
           "drop=" + juce::String ((int) osDrop) + ", selected=" + juce::String ((int) selected) + ", gesture=" + gesture.info());
    juce::MemoryBlock state;
    inst->getStateInformation (state);
    const bool removed = file.deleteFile();
    auto restored = format.createInstanceFromDescription (description, 48000.0, 128, error);
    if (! restored) { check (false, "second hosted instance for sample state", error); editor->setVisible (false); inst->editorBeingDeleted (editor); delete editor; inst->releaseResources(); return; }
    restored->setStateInformation (state.getData(), (int) state.getSize());
    restored->prepareToPlay (48000, 128);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    auto* restoredChoice = find (*restored, "sA_User Granular Sample");
    juce::AudioBuffer<float> a (2, 128), b (2, 128);
    juce::MidiBuffer noteA, noteB, emptyA, emptyB;
    noteA.addEvent (juce::MidiMessage::noteOn (1, 60, .8f), 0);
    noteB.addEvent (juce::MidiMessage::noteOn (1, 60, .8f), 0);
    double energy = 0.0, diff = 0.0; int nonfinite = 0;
    for (int block = 0; block < 240; ++block)
    {
        a.clear(); b.clear();
        inst->processBlock (a, block == 0 ? noteA : emptyA);
        restored->processBlock (b, block == 0 ? noteB : emptyB);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
        {
            const double x = a.getSample (c, i), y = b.getSample (c, i);
            if (! std::isfinite (x) || ! std::isfinite (y)) ++nonfinite;
            energy += x*x; diff += (x-y)*(x-y);
        }
    }
    const double rms = std::sqrt (energy / (240.0 * 128 * 2));
    const double delta = std::sqrt (diff / (240.0 * 128 * 2));
    check (selected && removed && restoredChoice && restoredChoice->getValue() > .5f
           && state.getSize() > 0 && nonfinite == 0 && rms > .001 && delta < 1.0e-6,
           "hosted VST3 state embeds the dropped sample and restores identical MIDI audio after deleting source",
           "state=" + juce::String ((juce::int64) state.getSize()) + ", RMS=" + juce::String (rms, 6)
           + ", delta RMS=" + juce::String (delta, 9) + ", nonfinite=" + juce::String (nonfinite));
    restored->releaseResources();

    // The same real VST3 editor must accept both Wavetable drop modes, not
    // merely the direct AudioProcessorEditor test above.
    const auto packed = file.getSiblingFile ("orogen-hosted-packed.wav");
    const auto recording = file.getSiblingFile ("orogen-hosted-recording.wav");
    const auto writeMono = [&] (const juce::File& outFile, const juce::AudioBuffer<float>& data) {
        outFile.deleteFile();
        auto stream = outFile.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> w (stream ? wav.createWriterFor (stream.release(), 44100, 1, 16, {}, 0) : nullptr);
        return w && w->writeFromAudioSampleBuffer (data, 0, data.getNumSamples());
    };
    juce::AudioBuffer<float> packedAudio (1, 2048 * 3);
    for (int i = 0; i < packedAudio.getNumSamples(); ++i)
        packedAudio.setSample (0, i, (float) (.4 * std::sin (6.283185307179586 * (1 + i / 2048) * (i % 2048) / 2048.0)));
    const bool wtFiles = writeMono (packed, packedAudio) && writeMono (recording, source);
    set ("sA_Engine", 0.0f);  // Wavetable
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    auto* wtChoice = find (*inst, "sA_User Wavetable");
    bool wtResults[2] { false, false };
    for (int mode = 0; mode < 2; ++mode)
    {
        if (wtChoice) wtChoice->setValueNotifyingHost (0.0f);
        HostGestureTrace wtGesture (wtChoice);
        bool sawHover = false; int requests = 0;
        const bool dropped = nativeFileDrop (*editor, mode == 0 ? juce::Point<int> (420, 320) : juce::Point<int> (760, 320),
                                             mode == 0 ? packed : recording, [] { return false; }, sawHover, requests);
        for (int i = 0; i < 500 && wtChoice && wtChoice->getValue() < .5f; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        wtResults[mode] = wtFiles && dropped && wtChoice && wtChoice->getValue() > .5f && wtGesture.balanced();
        std::printf ("   hosted WT mode %d: drop=%d requests=%d gesture=%s\n", mode,
                     (int) dropped, requests, wtGesture.info().toRawUTF8());
    }
    check (wtResults[0] && wtResults[1],
           "hosted VST3 accepts X11 drops of packed Wavetable and ordinary WAV resynthesis with host gestures");
    packed.deleteFile(); recording.deleteFile();
    editor->setVisible (false); inst->editorBeingDeleted (editor); delete editor;
    // gesture must detach before the instance it references is destroyed.
    inst->releaseResources();
   #else
    juce::ignoreUnused (format, description);
   #endif
}
