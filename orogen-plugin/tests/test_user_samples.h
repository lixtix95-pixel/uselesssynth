// Included after test_plugin.cpp's helpers so these tests exercise the same
// AudioProcessor, host parameter and GUI paths as the other integration tests.
#include "../src/plugin/GranularFile.h"

static bool writeSampleFixture (const juce::File& file, double sr, int frames, int format)
{
    std::unique_ptr<juce::AudioFormat> encoder;
    if (format == 0) encoder = std::make_unique<juce::WavAudioFormat>();
    else if (format == 1) encoder = std::make_unique<juce::AiffAudioFormat>();
    else encoder = std::make_unique<juce::FlacAudioFormat>();
    juce::AudioBuffer<float> data (2, frames);
    for (int i = 0; i < frames; ++i)
    {
        const float v = (float) (0.4 * std::sin (orogen::kTwoPi * 220.0 * i / sr));
        data.setSample (0, i, v); data.setSample (1, i, -v);
    }
    file.deleteFile();
    auto stream = file.createOutputStream();
    if (! stream) return false;
    std::unique_ptr<juce::AudioFormatWriter> writer (encoder->createWriterFor (stream.release(), sr, 2, 16, {}, 0));
    return writer && writer->writeFromAudioSampleBuffer (data, 0, frames);
}

static void testGranularUserFilesAndState()
{
    std::printf ("\n--- Granular audio files, state, presets and bank ---\n");
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto wav = dir.getChildFile ("orogen-granular-input.wav");
    const auto aiff = dir.getChildFile ("orogen-granular-input.aiff");
    const auto flac = dir.getChildFile ("orogen-granular-input.flac");
    const auto packed = dir.getChildFile ("orogen-granular-other-wt.wav");
    const auto tooLong = dir.getChildFile ("orogen-granular-long.wav");
    const auto broken = dir.getChildFile ("orogen-granular-broken.wav");
    const auto preset = dir.getChildFile ("orogen-granular-user.orogenpreset");
    const auto bank = dir.getChildFile ("orogen-granular-user.orogenbank");
    const bool fixtures = writeSampleFixture (wav, 44100, 44100, 0)
                       && writeSampleFixture (aiff, 48000, 48000, 1)
                       && writeSampleFixture (flac, 96000, 96000, 2)
                       && writeTestWav (packed, true)
                       && writeSampleFixture (tooLong, 8000, 241000, 0);
    broken.replaceWithText ("not a WAV");
    check (fixtures, "create stereo WAV, AIFF and FLAC fixtures and an overlength WAV");
    if (! fixtures) return;
    auto a = loadGranularSample (wav), c = loadGranularSample (aiff), f = loadGranularSample (flac);
    check (a.sample && c.sample && f.sample && a.sample->sourceRate == 44100
           && c.sample->sourceRate == 48000 && f.sample->sourceRate == 96000,
           "decode WAV, AIFF and FLAC with their original sample rates",
           a.error + " " + c.error + " " + f.error);
    check (! loadGranularSample (tooLong).sample && ! loadGranularSample (broken).sample
           && ! loadGranularSample (dir.getChildFile ("not-a-sample.mp3")).sample,
           "too long, malformed and unsupported files are rejected");
    if (! a.sample || ! c.sample || ! f.sample) return;
    double leftEnergy = 0, worst = 0;
    const auto& data = a.sample->shot.levels[0];
    for (size_t i = 0; i < data.size(); ++i)
    {
        const double expected = 0.4 * std::sin (kTwoPi * 220.0 * (double) i / 44100.0);
        leftEnergy += data[i] * data[i];
        worst = std::max (worst, std::fabs ((double) data[i] - expected));
    }
    check (std::sqrt (leftEnergy / data.size()) > .2 && worst < .0001,
           "opposite-polarity stereo keeps the first channel rather than cancelling", "max error " + num (worst, 8));

    auto table = loadWavetableWav (packed, false, 2048);
    OrogenProcessor p;
    p.setPlayConfigDetails (0, 2, 48000, 512); p.prepareToPlay (48000, 512);
    soloSlot (p, 0, (int) EngineType::Granular);
    setP (p, "sA_detune", 0); setP (p, "sA_p3", 0); setP (p, "sA_grainRoot", 48);
    p.installUserSample (0, a.sample);
    p.installUserSample (2, c.sample);
    setP (p, "sC_grainRoot", 72);
    if (table.table) p.installUserWavetable (0, table.table);
    check (p.apvts.getRawParameterValue ("sA_grainUser")->load() > .5f
           && p.apvts.getRawParameterValue ("sC_grainUser")->load() > .5f,
           "loaded samples select the independent A and C host parameters");
    juce::MemoryBlock state; p.getStateInformation (state);
    OrogenProcessor q; q.setStateInformation (state.getData(), (int) state.getSize());
    const auto restored = q.engine().getUserSamples();
    check (restored[0] && restored[2] && ! restored[1]
           && restored[0]->shot.levels[0] == a.sample->shot.levels[0]
           && restored[2]->shot.levels[0] == c.sample->shot.levels[0]
           && restored[0]->sourceRate == 44100 && restored[2]->sourceRate == 48000
           && q.apvts.getRawParameterValue ("sA_grainRoot")->load() == 48
           && q.apvts.getRawParameterValue ("sC_grainRoot")->load() == 72
           && q.engine().getUserWavetables()[0] != nullptr,
           "DAW state embeds A/C samples, rates, root notes and the separate wavetable",
           num ((double) state.getSize(), 0) + " bytes");
    // Embedded samples, not file paths: remove the source before rendering.
    wav.deleteFile(); aiff.deleteFile();
    q.setPlayConfigDetails (0, 2, 48000, 512); q.prepareToPlay (48000, 512);
    const auto x = renderBlocks (p, 36000, 48000), y = renderBlocks (q, 36000, 48000);
    double diff = 0;
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < x.getNumSamples(); ++i)
    { const double d = x.getSample (ch, i) - y.getSample (ch, i); diff += d*d; }
    diff = std::sqrt (diff / (2 * x.getNumSamples()));
    check (rmsOf (x) > 1.e-4 && diff < 1.e-6,
           "audio survives DAW roundtrip without the source files", "delta RMS " + num (diff, 9));

    const bool saved = p.savePresetFile (preset, "sample test");
    OrogenProcessor r; const bool loaded = r.loadPresetFile (preset);
    const auto fromPreset = r.engine().getUserSamples();
    check (saved && loaded && fromPreset[0] && fromPreset[2]
           && fromPreset[0]->shot.levels[0] == a.sample->shot.levels[0]
           && r.apvts.getRawParameterValue ("sA_grainRoot")->load() == 48,
           "JSON preset stores both samples and the root note");
    const int program = p.addProgram ("samples in bank");
    const bool bankSaved = p.saveBankFile (bank);
    OrogenProcessor t; const bool bankLoaded = t.loadBankFile (bank);
    if (bankLoaded) t.setCurrentProgram (program);
    const auto fromBank = t.engine().getUserSamples();
    check (bankSaved && bankLoaded && fromBank[0] && fromBank[2]
           && fromBank[2]->shot.levels[0] == c.sample->shot.levels[0],
           "JSON program bank restores each selected program's samples");

    auto encoded = userSamplesToVar (p.engine().getUserSamples());
    encoded.getArray()->getReference (0).getDynamicObject()->setProperty ("data", "corrupt");
    auto destination = p.engine().getUserSamples();
    check (! userSamplesFromVar (encoded, destination) && destination[0] == a.sample && destination[2] == c.sample,
           "corrupt embedded sample is rejected without changing the destination");
    auto badTree = p.apvts.copyState();
    badTree.setProperty ("granular_assets", "{not-json", nullptr);
    juce::MemoryBlock badState;
    if (auto xml = badTree.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, badState);
    const auto retained = q.engine().getUserSamples()[0];
    q.setStateInformation (badState.getData(), (int) badState.getSize());
    check (q.engine().getUserSamples()[0] == retained
           && q.apvts.getRawParameterValue ("sA_grainRoot")->load() == 48,
           "malformed DAW sample JSON leaves both content and parameters untouched");
    auto legacy = p.apvts.copyState();
    for (int i = 0; i < 8; ++i)
        for (const char* suffix : { "grainUser", "grainRoot" })
            if (auto child = legacy.getChildWithProperty ("id", slotPrefix (i) + suffix); child.isValid())
                legacy.removeChild (child, nullptr);
    juce::MemoryBlock oldState;
    if (auto xml = legacy.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, oldState);
    q.setStateInformation (oldState.getData(), (int) oldState.getSize());
    check (! q.engine().getUserSamples()[0] && q.apvts.getRawParameterValue ("sA_grainUser")->load() == 0
           && q.apvts.getRawParameterValue ("sA_grainRoot")->load() == 60,
           "legacy state resets missing custom source and root note to factory defaults");
    for (const auto& file : { wav, aiff, flac, packed, tooLong, broken, preset, bank }) file.deleteFile();
}

static void testAudioFileDropUi()
{
    std::printf ("\n--- File drag paths: WT table, WT resynthesis, Granular ---\n");
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto packed = dir.getChildFile ("orogen-drag-table.wav");
    const auto ordinary = dir.getChildFile ("orogen-drag-resynth.wav");
    const auto granular = dir.getChildFile ("orogen-drag-granular.flac");
    check (writeTestWav (packed, true) && writeTestWav (ordinary, false)
           && writeSampleFixture (granular, 44100, 44100, 2), "create files for drag tests");
    OrogenProcessor p; OrogenEditor editor (p);
    editor.setSize (1280, 800); editor.setVisible (true); editor.setAnimationsEnabled (false);
    auto* mm = juce::MessageManager::getInstance();
    const auto snapshotDir = juce::File::getCurrentWorkingDirectory().getChildFile ("build");
    if (std::getenv ("OROGEN_CAPTURE_DROP") != nullptr) snapshotDir.createDirectory();
    const auto waitFor = [mm] (const std::function<bool()>& ready)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000.0;
        while (! ready() && juce::Time::getMillisecondCounterHiRes() < deadline) mm->runDispatchLoopUntil (40);
        return ready();
    };
    juce::ComboBox* source = nullptr; juce::ComboBox* root = nullptr;
    juce::Button* load = nullptr; juce::Component* overlay = nullptr;
    for (auto* child : editor.getChildren())
    {
        if (child->getName() == "Source for selected slot") source = dynamic_cast<juce::ComboBox*> (child);
        if (child->getName() == "Granular root note") root = dynamic_cast<juce::ComboBox*> (child);
        if (child->getName() == "Import Granular audio sample") load = dynamic_cast<juce::Button*> (child);
        if (child->getName() == "File drop overlay") overlay = child;
    }
    check (source && root && load && overlay, "drop overlay, sample loader, root and source controls exist");
    if (! source || ! root || ! load || ! overlay) return;
    const juce::StringArray tableFiles { packed.getFullPathName() };
    int scales = 0;
    for (int width : { 960, 1280, 1600 })
    {
        editor.setSize (width, width * 5 / 8);
        const float k = width / 1280.0f;
        const int x = juce::roundToInt (420*k), y = juce::roundToInt (320*k);
        if (editor.isInterestedInFileDrag (tableFiles))
        {
            editor.fileDragEnter (tableFiles, x, y);
            if (overlay->isVisible() && overlay->getBounds() == editor.getLocalBounds()) ++scales;
            if (width == 1280 && std::getenv ("OROGEN_CAPTURE_DROP") != nullptr)
            {
                auto image = editor.createComponentSnapshot (editor.getLocalBounds());
                const auto snapshot = snapshotDir.getChildFile ("drop-wavetable.png");
                snapshot.deleteFile();
                auto out = snapshot.createOutputStream();
                if (out) juce::PNGImageFormat().writeImageToStream (image, *out);
            }
            editor.fileDragExit (tableFiles);
        }
    }
    check (scales == 3 && ! overlay->isVisible(), "drop feedback scales to 960, 1280 and 1600 and clears on exit", juce::String (scales) + "/3");
    editor.setSize (1280, 800); editor.showPage (0);
    const juce::StringArray many { packed.getFullPathName(), ordinary.getFullPathName() };
    check (! editor.isInterestedInFileDrag (many)
           && ! editor.isInterestedInFileDrag (juce::StringArray { granular.getFullPathName() }),
           "Wavetable rejects multiple files and non-WAV extensions");
    editor.fileDragEnter (tableFiles, 50, 50);
    editor.filesDropped (tableFiles, 50, 50);
    mm->runDispatchLoopUntil (100);
    check (! p.engine().getUserWavetables()[0] && ! overlay->isVisible(),
           "dropping outside a visible box cancels without changing content");
    GestureTrace wtGesture (p, "sA_wtUser");
    editor.fileDragEnter (tableFiles, 420, 320);
    editor.filesDropped (tableFiles, 420, 320);
    editor.showPage (3); // destination remains A while decoding
    const bool tableDone = waitFor ([&] { return p.engine().getUserWavetables()[0] != nullptr; });
    const auto table = p.engine().getUserWavetables();
    check (tableDone && table[0] && table[0]->frames == 3 && ! table[3] && wtGesture.balanced(),
           "drop in READY WAVETABLE loads three frames into captured slot A with host gesture");

    editor.showPage (1); setP (p, "sB_type", (float) EngineType::Wavetable); mm->runDispatchLoopUntil (80);
    const juce::StringArray ordinaryFiles { ordinary.getFullPathName() };
    editor.fileDragEnter (ordinaryFiles, 760, 320);
    editor.filesDropped (ordinaryFiles, 760, 320);
    const bool synthDone = waitFor ([&] { return p.engine().getUserWavetables()[1] != nullptr; });
    const auto synth = p.engine().getUserWavetables()[1];
    check (synthDone && synth && synth->frames == 64,
           "drop in RESYNTH converts the ordinary WAV to 64 frames in slot B");

    editor.showPage (2); setP (p, "sC_type", (float) EngineType::Granular); mm->runDispatchLoopUntil (80);
    const juce::StringArray grainFiles { granular.getFullPathName() };
    check (load->isVisible() && root->isVisible() && source->getNumItems() == 9
           && ! source->isItemEnabled (9) && editor.isInterestedInFileDrag (grainFiles),
           "Granular shows LOAD SAMPLE and root; FLAC is accepted");
    GestureTrace sampleGesture (p, "sC_grainUser");
    editor.fileDragEnter (grainFiles, 640, 320);
    if (std::getenv ("OROGEN_CAPTURE_DROP") != nullptr)
    {
        auto image = editor.createComponentSnapshot (editor.getLocalBounds());
        const auto snapshot = snapshotDir.getChildFile ("drop-granular.png");
        snapshot.deleteFile();
        auto out = snapshot.createOutputStream();
        if (out) juce::PNGImageFormat().writeImageToStream (image, *out);
    }
    editor.filesDropped (grainFiles, 640, 320);
    editor.showPage (4);
    const bool grainDone = waitFor ([&] { return p.engine().getUserSamples()[2] != nullptr; });
    editor.showPage (2); mm->runDispatchLoopUntil (80);
    check (grainDone && p.engine().getUserSamples()[2] && ! p.engine().getUserSamples()[4]
           && source->getSelectedId() == 9 && source->getText().containsIgnoreCase ("OROGEN-DRAG-GRANULAR")
           && sampleGesture.balanced(),
           "Granular drop loads captured slot C, selects its name and sends a host gesture");
    sampleGesture.clear(); source->setSelectedId (1, juce::sendNotificationSync);
    const bool factory = p.apvts.getRawParameterValue ("sC_grainUser")->load() == 0 && sampleGesture.balanced();
    sampleGesture.clear(); source->setSelectedId (9, juce::sendNotificationSync);
    const bool user = p.apvts.getRawParameterValue ("sC_grainUser")->load() == 1 && sampleGesture.balanced();
    GestureTrace rootGesture (p, "sC_grainRoot");
    root->setSelectedId (58, juce::sendNotificationSync); // MIDI 57, A3
    check (factory && user && rootGesture.balanced()
           && p.apvts.getRawParameterValue ("sC_grainRoot")->load() == 57,
           "factory/user source and root note controls send independent host gestures");
    // PreviewWorker has its own bank: confirm it receives the immutable user
    // asset, rather than merely updating SOURCE while drawing factory audio.
    setP (p, "gl_mode", 0); setP (p, "sC_on", 1); setP (p, "sC_attack", .001f);
    setP (p, "sC_sustain", 1); setP (p, "sC_grainRoot", 60);
    setP (p, "sC_p0", .4f); setP (p, "sC_p2", .8f); setP (p, "sC_p3", 0);
    source->setSelectedId (1, juce::sendNotificationSync);
    editor.rebuildPreview (true); const bool factoryPreviewReady = editor.waitForPreview (15000);
    const auto factoryPreview = editor.previewWave();
    source->setSelectedId (9, juce::sendNotificationSync);
    editor.rebuildPreview (true); const bool userPreviewReady = editor.waitForPreview (15000);
    const auto userPreview = editor.previewWave();
    double previewDiff = 0.0;
    if (! factoryPreview.empty() && factoryPreview.size() == userPreview.size())
        for (size_t i = 0; i < factoryPreview.size(); ++i)
        {
            const double d = factoryPreview[i] - userPreview[i]; previewDiff += d*d;
        }
    previewDiff = factoryPreview.empty() ? 0.0 : std::sqrt (previewDiff / factoryPreview.size());
    check (factoryPreviewReady && userPreviewReady && previewDiff > .001,
           "Granular preview worker renders the imported sample rather than the factory source",
           "delta RMS " + num (previewDiff, 6));
    if (std::getenv ("OROGEN_CAPTURE_DROP") != nullptr)
    {
        mm->runDispatchLoopUntil (100);
        const auto snapshot = snapshotDir.getChildFile ("ui-granular-user.png");
        snapshot.deleteFile();
        if (auto out = snapshot.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (editor.createComponentSnapshot (editor.getLocalBounds()), *out);
    }
    editor.showPage (4); setP (p, "sE_type", (float) EngineType::FM); mm->runDispatchLoopUntil (60);
    check (! editor.isInterestedInFileDrag (grainFiles), "other engines do not claim audio file drops");
    packed.deleteFile(); ordinary.deleteFile(); granular.deleteFile();
}

static void testNativeAudioFileDrop()
{
   #if JUCE_LINUX
    std::printf ("\n--- External X11/Xdnd -> editor file drop ---\n");
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto packed = dir.getChildFile ("orogen-native table 50%.wav");
    const auto ordinary = dir.getChildFile ("orogen-native recording 50%.wav");
    const auto grain = dir.getChildFile (juce::String::fromUTF8 ("orogen-native сэмпл 50%.flac"));
    if (! writeTestWav (packed, true) || ! writeTestWav (ordinary, false)
        || ! writeSampleFixture (grain, 44100, 44100, 2)) { check (false, "native drop fixtures"); return; }
    {
        OrogenProcessor p; OrogenEditor editor (p);
        editor.setSize (1280, 800); editor.setAnimationsEnabled (false);
        editor.addToDesktop (0); editor.setTopLeftPosition (0, 0); editor.setVisible (true);
        auto* mm = juce::MessageManager::getInstance(); mm->runDispatchLoopUntil (500);
        juce::Component* overlay = nullptr;
        for (auto* c : editor.getChildren()) if (c->getName() == "File drop overlay") overlay = c;
        const auto waitFor = [mm] (const std::function<bool()>& ready) {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000.0;
            while (! ready() && juce::Time::getMillisecondCounterHiRes() < deadline) mm->runDispatchLoopUntil (40);
            return ready();
        };
        int imported = 0, visible = 0, finished = 0, requests = 0;
        for (int i = 0; i < 3; ++i)
        {
            editor.showPage (i);
            setP (p, slotPrefix (i) + "type", i == 2 ? (float) EngineType::Granular : (float) EngineType::Wavetable);
            mm->runDispatchLoopUntil (120);
            bool hover = false; int selections = 0;
            const auto point = i == 0 ? juce::Point<int> (420, 320)
                             : i == 1 ? juce::Point<int> (760, 320) : juce::Point<int> (640, 320);
            const auto file = i == 0 ? packed : i == 1 ? ordinary : grain;
            const bool done = nativeFileDrop (editor, point, file,
                                               [overlay] { return overlay != nullptr && overlay->isVisible(); },
                                               hover, selections);
            if (done) ++finished; if (hover) ++visible; requests += selections;
            const bool loaded = waitFor ([&] {
                return i < 2 ? p.engine().getUserWavetables()[(size_t) i] != nullptr
                             : p.engine().getUserSamples()[(size_t) i] != nullptr;
            });
            if (loaded) ++imported;
            std::printf ("   target %d: Xdnd finished=%d, selection requests=%d, hover=%d, imported=%d\n",
                         i, (int) done, selections, (int) hover, (int) loaded);
        }
        check (finished == 3 && imported == 3,
               "external Xdnd loads packed, resynth and sample assets (including escaped/Unicode paths) into selected slots");
        check (visible == 3, "drop choices are visible before releasing the mouse", juce::String (visible) + "/3");
        juce::ignoreUnused (requests);
        editor.removeFromDesktop();
    }
    packed.deleteFile(); ordinary.deleteFile(); grain.deleteFile();
   #else
    check (false, "native file-drop test is available on Linux/X11");
   #endif
}
