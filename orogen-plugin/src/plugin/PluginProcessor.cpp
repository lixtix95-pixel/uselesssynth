#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstdio>
#include <cstdlib>

namespace orogen
{

OrogenProcessor::OrogenProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "OROGEN", makeLayout())
{
    // Фабричный банк: те же шесть патчей, что в стендах (patcher.h). Эффекты
    // подобраны под характер патча — как в демо инструмента.
    int count = 0;
    const Patch* list = factoryPatches (count);
    for (int i = 0; i < count; ++i)
    {
        Preset pr;
        pr.patch = list[i];
        pr.name = juce::String (list[i].name);
        pr.fx.compThreshold = -10.0f;
        pr.fx.compRatio = 2.5f;
        pr.fx.compMakeup = 2.0f;
        pr.fx.width = 1.1f;
        switch (i)
        {
            case 0: pr.fx.reverbMix = 0.22f; pr.fx.reverbSize = 0.6f; break;   // Init
            case 1: pr.fx.reverbMix = 0.30f; pr.fx.reverbSize = 0.7f;
                    pr.fx.delayMix = 0.10f; break;                             // Terra Pad
            case 2: pr.fx.reverbMix = 0.28f; pr.fx.delayMix = 0.30f;
                    pr.fx.delayTime = 0.31f; break;                            // Glass Keys
            case 3: pr.fx.reverbMix = 0.35f; pr.fx.drive = 0.25f;
                    pr.fx.delayMix = 0.22f; break;
            case 4: pr.fx.reverbMix = 0.18f; pr.fx.drive = 0.15f;
                    pr.fx.delayMix = 0.28f; break;
            default: pr.fx.reverbMix = 0.55f; pr.fx.reverbSize = 0.9f;
                     pr.fx.chorusMix = 0.25f; break;
        }
        presets.push_back (pr);
    }

    cachePointers();

    stagedTypeHash = slotTypeHash();
    startTimerHz (30);                    // подготовка движков и уборка — вне аудио

    // «живой» патч = первый пресет; параметры сразу отражают его состояние
    patch = presets.front().patch;
    fxSettings = presets.front().fx;
    applyToParams (patch, fxSettings);
    inst.setPatch (patch);
    inst.setFx (fxSettings);
}

// Порядок обязан совпадать с fxParamTable() в Params.h — это проверяется
// статической проверкой ниже (kFxCount против размера таблицы).
enum FxIdx
{
    FxDrive = 0, FxDriveTone, FxChorusMix, FxChorusDepth, FxChorusRate,
    FxChorusDelay, FxChorusFeedback, FxChorusWidth, FxChorusLowpass, FxChorusInput,
    FxDelayMix, FxDelayTime, FxDelayFb,
    FxReverbMix, FxReverbSize, FxReverbDamp,
    FxPlateMix, FxPlateDecay, FxPlateDamp, FxPlateDiffusion, FxPlatePredelay,
    FxEqLow, FxEqHigh, FxCompThreshold, FxCompRatio, FxCompMakeup, FxWidth,
    FxMaterialMix, FxMaterialExcite, FxMaterialTone, FxMaterialDamp,
    FxMaterialSize, FxMaterialFeedback,
    FxSpectreMix, FxSpectreProminence, FxSpectreShift, FxSpectreLow, FxSpectreHigh,
    FxGlitchMix, FxGlitchChance, FxGlitchLengthMs, FxGlitchRepeats,
    FxGlitchReverse, FxGlitchPitch, FxGlitchFeedback, FxGlitchFold, FxGlitchTone,
    FxCombMix, FxCombDriveDb, FxCombOutputDb, FxCombDepth, FxCombRate,
    FxCombEnvelope, FxCombFreqA, FxCombFreqB, FxCombResA, FxCombResB,
    FxCombBalance, FxCombDelayMs, FxCombFeedback, FxCombHighpass, FxCombTune,
    FxRobinMix, FxRobinSemis, FxRobinSize, FxRobinSpread,
    FxPlatePreFilter, FxPlateInputDiff1, FxPlateInputDiff2, FxPlateOutput,
    kFxCount
};

static_assert ((int) FxPlatePreFilter == kOriginalFxParamCount, "старый порядок FX нарушен");
static_assert ((int) kFxCount == kOriginalFxParamCount + kAddedPlateParamCount,
               "fxParamTable() и enum FxIdx разъехались");


void OrogenProcessor::cachePointers()
{
    int n = 0;
    const SlotParam* st = slotParamTable (n);
    for (int s = 0; s < kNumSlots; ++s)
    {
        auto& r = slotRefs[(size_t) s];
        const juce::String pre = slotPrefix (s);
        r.type   = apvts.getRawParameterValue (pre + "type");
        r.on     = apvts.getRawParameterValue (pre + "on");
        r.filter = apvts.getRawParameterValue (pre + "filter");
        r.f.clear();
        for (int i = 0; i < n; ++i)
            r.f.push_back (apvts.getRawParameterValue (pre + st[i].id));
    }

    globRefs = { apvts.getRawParameterValue ("gl_gain"),
                 apvts.getRawParameterValue ("gl_master"),
                 apvts.getRawParameterValue ("gl_glide"),
                 apvts.getRawParameterValue ("gl_width"),
                 apvts.getRawParameterValue ("gl_mode"),
                 apvts.getRawParameterValue ("gl_modWheel"),
                 apvts.getRawParameterValue ("gl_morph") };

    for (int l = 0; l < 2; ++l)
    {
        const juce::String pre = "lfo" + juce::String (l + 1) + "_";
        lfoRefs[(size_t) l] = { apvts.getRawParameterValue (pre + "rate"),
                                apvts.getRawParameterValue (pre + "amount"),
                                apvts.getRawParameterValue (pre + "dest"),
                                apvts.getRawParameterValue (pre + "shape") };
    }

    int fn = 0;
    const FxParam* ft = fxParamTable (fn);
    jassert (fn == (int) kFxCount);
    fxRefs.clear();
    for (int i = 0; i < fn; ++i)
        fxRefs.push_back (apvts.getRawParameterValue (juce::String ("fx_") + ft[i].id));

    // порядок совпадает с choiceTable()/makeLayout()
    const char* choiceIds[] = { "fx_driveModel", "fx_chorusModel", "fx_materialMode",
                                "fx_spectreModel", "fx_spectreWindow", "fx_spectrePeaks",
                                "fx_spectreWidth", "fx_spectreFreeze", "fx_combModel",
                                "fx_combRouting", "fx_combNegative", "fx_combOversample",
                                "fx_combMidSide", "fx_materialFollow" };
    jassert ((int) fxChoiceRefs.size() == (int) (sizeof (choiceIds) / sizeof (choiceIds[0])));
    for (size_t i = 0; i < fxChoiceRefs.size(); ++i)
        fxChoiceRefs[i] = dynamic_cast<juce::RangedAudioParameter*> (
                              apvts.getParameter (choiceIds[i]));

    for (int r = 0; r < kModRoutes; ++r)
    {
        const juce::String pre = "mod" + juce::String (r + 1) + "_";
        auto& m = modRefs[(size_t) r];
        m.src = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (pre + "src"));
        m.dst = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (pre + "dst"));
        m.amt = apvts.getRawParameterValue (pre + "amt");
        if (m.src == nullptr || m.dst == nullptr || m.amt == nullptr)
            std::fprintf (stderr, "OROGEN: маршрут мод-матрицы %d не найден\n", r + 1);
    }

    const char* arpIds[] = { "arpOn", "arpMode", "arpDiv", "arpOctaves", "arpLatch" };
    static_assert ((int) (sizeof (arpIds) / sizeof (arpIds[0])) == 5, "arpRefs разъехался");
    for (size_t i = 0; i < arpRefs.size(); ++i)
        arpRefs[i] = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (arpIds[i]));
    arpGateRef  = apvts.getRawParameterValue ("arpGate");
    arpSwingRef = apvts.getRawParameterValue ("arpSwing");

    if (std::getenv ("OROGEN_DEBUG_PARAMS") != nullptr)
    {
        std::fprintf (stderr, "OROGEN: всего параметров %d\n", apvts.processor.getParameters().size());
        for (auto* prm : apvts.processor.getParameters())
            if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
                std::fprintf (stderr, "  id=%s name=%s\n", wid->paramID.toRawUTF8(),
                              prm->getName (64).toRawUTF8());
    }

    // диагностика: любой не найденный id — ошибка конфигурации, а не «тишина»
    for (int i = 0; i < kNumSlots; ++i)
    {
        const auto& r = slotRefs[(size_t) i];
        if (r.type == nullptr || r.on == nullptr || r.filter == nullptr)
            std::fprintf (stderr, "OROGEN: не найден параметр слота %d\n", i);
        for (size_t k = 0; k < r.f.size(); ++k)
            if (r.f[k] == nullptr)
                std::fprintf (stderr, "OROGEN: не найден параметр %d слота %d\n", (int) k, i);
    }
    for (size_t i = 0; i < globRefs.size(); ++i)
        if (globRefs[i] == nullptr) std::fprintf (stderr, "OROGEN: глобальный параметр %d не найден\n", (int) i);
    for (size_t i = 0; i < fxRefs.size(); ++i)
        if (fxRefs[i] == nullptr) std::fprintf (stderr, "OROGEN: параметр эффекта %d не найден\n", (int) i);
    for (size_t i = 0; i < fxChoiceRefs.size(); ++i)
        if (fxChoiceRefs[i] == nullptr) std::fprintf (stderr, "OROGEN: перечисление %d не найдено\n", (int) i);
    for (size_t i = 0; i < arpRefs.size(); ++i)
        if (arpRefs[i] == nullptr) std::fprintf (stderr, "OROGEN: параметр арпеджиатора %d не найден\n", (int) i);

}

std::uint64_t OrogenProcessor::hashParams() const noexcept
{
    std::uint64_t h = 1469598103934665603ull;                  // FNV-1a
    auto mix = [&h] (float v)
    {
        std::uint32_t bits;
        std::memcpy (&bits, &v, sizeof (bits));
        h = (h ^ bits) * 1099511628211ull;
    };
    for (const auto& r : slotRefs)
    {
        if (r.type)   mix (r.type->load());
        if (r.on)     mix (r.on->load());
        if (r.filter) mix (r.filter->load());
        for (auto* f : r.f) if (f) mix (f->load());
    }
    for (auto* g : globRefs) if (g) mix (g->load());
    for (const auto& l : lfoRefs) for (auto* p : l) if (p) mix (p->load());
    for (auto* f : fxRefs) if (f) mix (f->load());
    for (auto* c : fxChoiceRefs) mix (choiceValue (c));
    return h;
}

// Хэш только типов движков в слотах: по нему решается, что нужно строить заново.
std::uint64_t OrogenProcessor::slotTypeHash() const noexcept
{
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& r : slotRefs)
        h = (h ^ (std::uint64_t) std::lround (r.type != nullptr ? r.type->load() : 0.0f))
              * 1099511628211ull;
    return h;
}

void OrogenProcessor::timerCallback()
{
    // 1. уборка заменённых движков — единственное место, где они удаляются
    inst.drainRetired();

    // 2. подготовка движков, если типы слотов изменились
    const auto h = slotTypeHash();
    if (h != stagedTypeHash)
    {
        stagedTypeHash = h;
        Patch p;
        FxChain::Settings fx;
        collectPatch (p, fx);
        p.setName (patch.name);
        inst.stagePatch (p);        // аллокации здесь — и только здесь
    }
}

void OrogenProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate = sampleRate;

    // Сборка контента (таблицы, MIP-уровни записей, ресинтез) — самая дорогая
    // операция во всём плагине; хост даёт её на не-аудио-потоке.
    bank.build (sampleRate);
    inst.prepare (sampleRate, 16);
    inst.setPatch (patch);
    inst.setFx (fxSettings);
    inst.setMasterGain (masterGain);

    latencySamples.store (inst.fxLatency());
    triggerAsyncUpdate();
}

bool OrogenProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Инструмент стереофонический; моно-выход принимаем (хост сам сведёт),
    // моно/сайдчейн-вход и surround — нет.
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

// Индексы slotParamTable — см. Params.h. Здесь они названы явно, чтобы при
// добавлении параметра компилятор не «съел» сдвиг молча.
enum SlotIdx
{
    kLevel = 0, kPan, kTune, kDetune, kP0, kP1, kP2, kP3, kTable,
    kAttack, kDecay, kSustain, kRelease, kCutoff, kRes, kLoKey, kHiKey, kTail
};

void OrogenProcessor::collectPatch (Patch& p, FxChain::Settings& fx) const noexcept
{
    auto g = [this] (int i) { return globRefs[(size_t) i]->load(); };

    p.gain     = g (0);
    p.glide    = g (2);
    p.width    = g (3);
    p.mode     = (PatchMode) (int) std::lround (g (4));
    p.modWheel = juce::jlimit (0.0f, 1.0f, g (5) + 0.5f * pitchBend.load());
    p.morph    = g (6);

    for (int l = 0; l < 2; ++l)
    {
        const auto& r = lfoRefs[(size_t) l];
        p.lfo[l].rate   = r[0]->load();
        p.lfo[l].amount = r[1]->load();
        p.lfo[l].dest   = (ModDest) (int) std::lround (r[2]->load());
        p.lfo[l].shape  = (int) std::lround (r[3]->load());
    }

    // мод-матрица: 8 маршрутов, величина 0 = выключен
    p.bend = juce::jlimit (-1.0f, 1.0f, pitchBend.load());
    for (int r = 0; r < kModRoutes; ++r)
    {
        const auto& m = modRefs[(size_t) r];
        p.routes[r].source = (ModSource) juce::jlimit (0, (int) ModSource::Count - 1,
                                                      (int) choiceValue (m.src));
        p.routes[r].dest   = (ModDest) juce::jlimit (0, (int) ModDest::Count - 1,
                                                    (int) choiceValue (m.dst));
        p.routes[r].amount = m.amt != nullptr ? m.amt->load() : 0.0f;
    }

    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto& r = slotRefs[(size_t) s];
        auto& sp = p.slots[s];
        sp.type  = (EngineType) (int) std::lround (r.type->load());
        sp.on    = r.on->load() > 0.5f;
        sp.filterType = (int) std::lround (r.filter->load());

        sp.params.level      = r.f[(size_t) kLevel]->load();
        sp.params.pan        = r.f[(size_t) kPan]->load();
        sp.params.tuneSemis  = r.f[(size_t) kTune]->load();
        sp.params.detune     = r.f[(size_t) kDetune]->load();
        sp.params.p[0] = r.f[(size_t) kP0]->load();
        sp.params.p[1] = r.f[(size_t) kP1]->load();
        sp.params.p[2] = r.f[(size_t) kP2]->load();
        sp.params.p[3] = r.f[(size_t) kP3]->load();
        sp.params.table = (int) std::lround (r.f[(size_t) kTable]->load());
        sp.attack  = r.f[(size_t) kAttack]->load();
        sp.decay   = r.f[(size_t) kDecay]->load();
        sp.sustain = r.f[(size_t) kSustain]->load();
        sp.release = r.f[(size_t) kRelease]->load();
        sp.cutoff  = r.f[(size_t) kCutoff]->load();
        sp.res     = r.f[(size_t) kRes]->load();
        sp.loNote  = (int) std::lround (r.f[(size_t) kLoKey]->load());
        sp.hiNote  = (int) std::lround (r.f[(size_t) kHiKey]->load());
        sp.tail    = r.f[(size_t) kTail]->load();
    }

    // --- эффекты: порядок fxRefs = fxParamTable() (см. enum FxIdx выше) ---
    jassert ((int) fxRefs.size() == kFxCount);
    auto X = [this] (FxIdx i) { return fxRefs[(size_t) i]->load(); };

    fx.drive             = X (FxDrive);
    fx.driveTone         = X (FxDriveTone);
    fx.chorusMix         = X (FxChorusMix);
    fx.chorusDepth       = X (FxChorusDepth);
    fx.chorusRate        = X (FxChorusRate);
    // прошивочный хорус: DEL/DEP/SPD/MIX/FB/WID/LP/INP. DEP и SPD берём из
    // chorusDepth/chorusRate (0…1 и Гц), пересчитывая в шкалу прибора 0…127.
    fx.chorusFw[(size_t) 0] = (int) std::lround (X (FxChorusDelay));
    fx.chorusFw[(size_t) 1] = (int) std::lround (juce::jlimit (0.0f, 1.0f, X (FxChorusDepth)) * 127.0f);
    fx.chorusFw[(size_t) 2] = (int) std::lround (juce::jlimit (0.0f, 2.0f, X (FxChorusRate)) / 2.0f * 127.0f);
    fx.chorusFw[(size_t) 3] = 127;                       // MIX считается снаружи
    fx.chorusFw[(size_t) 4] = (int) std::lround (X (FxChorusFeedback));
    fx.chorusFw[(size_t) 5] = (int) std::lround (X (FxChorusWidth));
    fx.chorusFw[(size_t) 6] = (int) std::lround (X (FxChorusLowpass));
    fx.chorusFw[(size_t) 7] = (int) std::lround (X (FxChorusInput));
    fx.delayMix          = X (FxDelayMix);
    fx.delayTime         = X (FxDelayTime);
    fx.delayFb           = X (FxDelayFb);
    fx.reverbMix         = X (FxReverbMix);
    fx.reverbSize        = X (FxReverbSize);
    fx.reverbDamp        = X (FxReverbDamp);
    fx.plateMix          = X (FxPlateMix);
    fx.plateDecay        = X (FxPlateDecay);
    fx.plateDamp         = X (FxPlateDamp);
    fx.plateDiffusion    = X (FxPlateDiffusion);
    fx.platePredelay     = X (FxPlatePredelay);
    fx.platePreFilter    = X (FxPlatePreFilter);
    fx.plateInputDiff1   = X (FxPlateInputDiff1);
    fx.plateInputDiff2   = X (FxPlateInputDiff2);
    fx.plateOutput       = X (FxPlateOutput);
    fx.eqLow             = X (FxEqLow);
    fx.eqHigh            = X (FxEqHigh);
    fx.compThreshold     = X (FxCompThreshold);
    fx.compRatio         = X (FxCompRatio);
    fx.compMakeup        = X (FxCompMakeup);
    fx.width             = X (FxWidth);
    fx.materialMix       = X (FxMaterialMix);
    fx.materialExcite    = X (FxMaterialExcite);
    fx.materialTone      = X (FxMaterialTone);
    fx.materialDamp      = X (FxMaterialDamp);
    fx.materialSize      = X (FxMaterialSize);
    fx.materialFeedback  = X (FxMaterialFeedback);
    fx.spectreMix        = X (FxSpectreMix);
    fx.spectreProminence = X (FxSpectreProminence);
    fx.spectreShift      = X (FxSpectreShift);
    fx.spectreLow        = X (FxSpectreLow);
    fx.spectreHigh       = X (FxSpectreHigh);
    fx.robinMix          = X (FxRobinMix);
    fx.robinSemis        = X (FxRobinSemis);
    fx.robinSize         = X (FxRobinSize);
    fx.robinSpread       = X (FxRobinSpread);
    fx.glitchMix         = X (FxGlitchMix);
    fx.glitchChance      = X (FxGlitchChance);
    fx.glitchLengthMs    = X (FxGlitchLengthMs);
    fx.glitchRepeats     = X (FxGlitchRepeats);
    fx.glitchReverse     = X (FxGlitchReverse);
    fx.glitchPitch       = X (FxGlitchPitch);
    fx.glitchFeedback    = X (FxGlitchFeedback);
    fx.glitchFold        = X (FxGlitchFold);
    fx.glitchTone        = X (FxGlitchTone);
    fx.combMix           = X (FxCombMix);
    fx.combDriveDb       = X (FxCombDriveDb);
    fx.combOutputDb      = X (FxCombOutputDb);
    fx.combDepth         = X (FxCombDepth);
    fx.combRate          = X (FxCombRate);
    fx.combEnvelope      = X (FxCombEnvelope);
    fx.combFreqA         = X (FxCombFreqA);
    fx.combFreqB         = X (FxCombFreqB);
    fx.combResA          = X (FxCombResA);
    fx.combResB          = X (FxCombResB);
    fx.combBalance       = X (FxCombBalance);
    fx.combDelayMs       = X (FxCombDelayMs);
    fx.combFeedback      = X (FxCombFeedback);
    fx.combHighpass      = X (FxCombHighpass);
    fx.combTune          = X (FxCombTune);

    auto C = [this] (int i) { return (int) std::lround (choiceValue (fxChoiceRefs[(size_t) i])); };
    fx.driveModel     = C (0);
    fx.chorusModel    = C (1);
    fx.materialMode   = C (2);
    fx.spectreModel   = C (3);
    fx.spectreWindow  = C (4);
    fx.spectrePeaks   = C (5);
    fx.spectreWidth   = C (6);
    fx.spectreFreeze  = C (7) != 0;
    fx.combModel      = C (8);
    fx.combRouting    = C (9);
    fx.combNegative   = C (10) != 0;
    fx.combOversample = C (11) != 0;
    fx.combMidSide    = C (12) != 0;
    fx.materialFollow = C (13) != 0;
}

void OrogenProcessor::applyToParams (const Patch& p, const FxChain::Settings& fx)
{
    auto set = [this] (const juce::String& id, float value)
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id)))
            rp->setValueNotifyingHost (rp->convertTo0to1 (value));
    };

    set ("gl_gain", p.gain);     set ("gl_glide", p.glide);
    set ("gl_width", p.width);   set ("gl_morph", p.morph);
    set ("gl_modWheel", p.modWheel);
    set ("gl_mode", (float) (int) p.mode);
    for (int l = 0; l < 2; ++l)
    {
        const juce::String pre = "lfo" + juce::String (l + 1) + "_";
        set (pre + "rate", p.lfo[l].rate);
        set (pre + "amount", p.lfo[l].amount);
        set (pre + "dest", (float) (int) p.lfo[l].dest);
        set (pre + "shape", (float) p.lfo[l].shape);
    }

    for (int r = 0; r < kModRoutes; ++r)
    {
        const juce::String pre = "mod" + juce::String (r + 1) + "_";
        set (pre + "src", (float) (int) p.routes[r].source);
        set (pre + "dst", (float) (int) p.routes[r].dest);
        set (pre + "amt", p.routes[r].amount);
    }

    for (int s = 0; s < kNumSlots; ++s)
    {
        const juce::String pre = slotPrefix (s);
        const auto& sp = p.slots[s];
        set (pre + "type", (float) (int) sp.type);
        set (pre + "on", sp.on ? 1.0f : 0.0f);
        set (pre + "filter", (float) sp.filterType);
        set (pre + "level", sp.params.level);
        set (pre + "pan", sp.params.pan);
        set (pre + "tune", sp.params.tuneSemis);
        set (pre + "detune", sp.params.detune);
        for (int k = 0; k < 4; ++k)
            set (pre + "p" + juce::String (k), sp.params.p[k]);
        set (pre + "table", (float) sp.params.table);
        set (pre + "attack", sp.attack);
        set (pre + "decay", sp.decay);
        set (pre + "sustain", sp.sustain);
        set (pre + "release", sp.release);
        set (pre + "cutoff", sp.cutoff);
        set (pre + "res", sp.res);
        set (pre + "lo", (float) sp.loNote);
        set (pre + "hi", (float) sp.hiNote);
        set (pre + "tail", sp.tail);
    }

    // Эффекты: пишем через ту же таблицу, что читаем, — раскладка гарантированно
    // симметрична, и новое поле невозможно забыть только в одну сторону.
    auto put = [&set] (const char* id, float v) { set (juce::String ("fx_") + id, v); };
    put ("drive", fx.drive);                 put ("driveTone", fx.driveTone);
    put ("chorusMix", fx.chorusMix);         put ("chorusDepth", fx.chorusDepth);
    put ("chorusDelay",    (float) fx.chorusFw[(size_t) 0]);
    put ("chorusFeedback", (float) fx.chorusFw[(size_t) 4]);
    put ("chorusWidth",    (float) fx.chorusFw[(size_t) 5]);
    put ("chorusLowpass",  (float) fx.chorusFw[(size_t) 6]);
    put ("chorusInput",    (float) fx.chorusFw[(size_t) 7]);
    put ("chorusRate", fx.chorusRate);
    put ("delayMix", fx.delayMix);           put ("delayTime", fx.delayTime);
    put ("delayFb", fx.delayFb);
    put ("reverbMix", fx.reverbMix);         put ("reverbSize", fx.reverbSize);
    put ("reverbDamp", fx.reverbDamp);
    put ("plateMix", fx.plateMix);           put ("plateDecay", fx.plateDecay);
    put ("plateDamp", fx.plateDamp);         put ("plateDiffusion", fx.plateDiffusion);
    put ("platePredelay", fx.platePredelay);
    put ("platePreFilter", fx.platePreFilter);
    put ("plateInputDiff1", fx.plateInputDiff1); put ("plateInputDiff2", fx.plateInputDiff2);
    put ("plateOutput", fx.plateOutput);
    put ("eqLow", fx.eqLow);                 put ("eqHigh", fx.eqHigh);
    put ("compThreshold", fx.compThreshold); put ("compRatio", fx.compRatio);
    put ("compMakeup", fx.compMakeup);       put ("width", fx.width);
    put ("materialMix", fx.materialMix);     put ("materialExcite", fx.materialExcite);
    put ("materialTone", fx.materialTone);   put ("materialDamp", fx.materialDamp);
    put ("materialSize", fx.materialSize);   put ("materialFeedback", fx.materialFeedback);
    put ("spectreMix", fx.spectreMix);       put ("spectreProminence", fx.spectreProminence);
    put ("spectreLow", fx.spectreLow);       put ("spectreHigh", fx.spectreHigh);
    put ("spectreShift", fx.spectreShift);
    put ("glitchMix", fx.glitchMix);         put ("glitchChance", fx.glitchChance);
    put ("glitchFeedback", fx.glitchFeedback); put ("glitchFold", fx.glitchFold);
    put ("glitchTone", fx.glitchTone);
    put ("glitchLengthMs", fx.glitchLengthMs); put ("glitchRepeats", fx.glitchRepeats);
    put ("glitchReverse", fx.glitchReverse); put ("glitchPitch", fx.glitchPitch);
    put ("robinMix", fx.robinMix);           put ("robinSemis", fx.robinSemis);
    put ("robinSize", fx.robinSize);         put ("robinSpread", fx.robinSpread);
    put ("combMix", fx.combMix);             put ("combDriveDb", fx.combDriveDb);
    put ("combOutputDb", fx.combOutputDb);   put ("combDepth", fx.combDepth);
    put ("combRate", fx.combRate);           put ("combEnvelope", fx.combEnvelope);
    put ("combFreqA", fx.combFreqA);         put ("combFreqB", fx.combFreqB);
    put ("combResA", fx.combResA);           put ("combResB", fx.combResB);
    put ("combBalance", fx.combBalance);     put ("combDelayMs", fx.combDelayMs);
    put ("combFeedback", fx.combFeedback);   put ("combHighpass", fx.combHighpass);
    put ("combTune", fx.combTune);

    put ("driveModel", (float) fx.driveModel);
    put ("chorusModel", (float) fx.chorusModel);
    put ("materialMode", (float) fx.materialMode);
    put ("spectreModel", (float) fx.spectreModel);
    put ("spectreWindow", (float) fx.spectreWindow);
    put ("spectrePeaks", (float) fx.spectrePeaks);
    put ("spectreWidth", (float) fx.spectreWidth);
    put ("spectreFreeze", fx.spectreFreeze ? 1.0f : 0.0f);
    put ("combModel", (float) fx.combModel);
    put ("combRouting", (float) fx.combRouting);
    put ("combNegative", fx.combNegative ? 1.0f : 0.0f);
    put ("combOversample", fx.combOversample ? 1.0f : 0.0f);
    put ("combMidSide", fx.combMidSide ? 1.0f : 0.0f);
    put ("materialFollow", fx.materialFollow ? 1.0f : 0.0f);
}

void OrogenProcessor::renderRange (juce::AudioBuffer<float>& buf, int start, int num)
{
    if (num <= 0) return;
    auto* l = buf.getWritePointer (0);
    auto* r = buf.getNumChannels() > 1 ? buf.getWritePointer (1) : nullptr;

    for (int i = start; i < start + num; ++i)
    {
        float sl = 0.0f, srr = 0.0f;
        inst.render (sl, srr);
        if (r != nullptr) { l[i] = sl; r[i] = srr; }
        else              { l[i] = (sl + srr) * 0.5f; }
    }
    rendered.fetch_add (num);
}

void OrogenProcessor::processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi)
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    juce::ScopedNoDenormals noDenormals;
    buf.clear();

    collectPatch (patch, fxSettings);

    masterGain = globRefs[1]->load();
    inst.setMasterGain (masterGain);

    // applyPatch() = копия POD-структуры + подхват движков, подготовленных
    // message-потоком. Ни аллокаций, ни обращения к куче — можно каждый блок.
    inst.applyPatch (patch);

    // настройки эффектов применяем только при изменении: FxChain::set()
    // переустанавливает линии задержки, и повторный вызов меняет звук
    const auto hash = hashParams();
    if (hash != lastParamHash)
    {
        lastParamHash = hash;
        patch.setName (presets.empty() ? "Untitled"
                                       : presets[(size_t) currentProgram].name.toRawUTF8());
        inst.setFx (fxSettings);
    }

    const int lat = inst.fxLatency();
    if (lat != latencySamples.load())
    {
        latencySamples.store (lat);
        triggerAsyncUpdate();
    }

    // --- арпеджиатор: параметры + темп из playhead ---------------------------
    // Arp стоит ДО голосов (внутри Instrument), поэтому события арпеджиатора
    // рождаются в render(), а не здесь. Нам нужны только настройки и BPM.
    {
        ArpSettings as;
        as.on      = choiceValue (arpRefs[0]) > 0.5f;
        as.mode    = (ArpMode) juce::jlimit (0, (int) ArpMode::Count - 1,      (int) choiceValue (arpRefs[1]));
        as.div     = (ArpDiv)  juce::jlimit (0, (int) ArpDiv::Count - 1,       (int) choiceValue (arpRefs[2]));
        as.octaves = (int) choiceValue (arpRefs[3]);
        as.latch   = choiceValue (arpRefs[4]) > 0.5f;
        as.gate    = arpGateRef  != nullptr ? arpGateRef->load()  : 0.6f;
        as.swing   = arpSwingRef != nullptr ? arpSwingRef->load() : 0.0f;

        double bpm = 120.0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto b = pos->getBpm())
                    bpm = *b;
        inst.setTempo (bpm);
        inst.setArp (as);
    }

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, buf.getNumSamples(), meta.samplePosition);
        renderRange (buf, pos, at - pos);
        pos = at;

        const auto m = meta.getMessage();
        if (m.isNoteOn())        inst.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())  inst.noteOff (m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff()) inst.allNotesOff();
        else if (m.isPitchWheel())
            pitchBend.store ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isController() && m.getControllerNumber() == 1)
            globRefs[5]->store ((float) m.getControllerValue() / 127.0f);
        else if (m.isController() && m.getControllerNumber() == 64 && m.getControllerValue() < 64)
            inst.allNotesOff();
    }
    renderRange (buf, pos, buf.getNumSamples() - pos);

    blockSeconds.store (juce::Time::highResolutionTicksToSeconds (
                                juce::Time::getHighResolutionTicks() - t0));
}

void OrogenProcessor::handleAsyncUpdate()
{
    setLatencySamples (latencySamples.load());
}

// ---------- программы и состояние ------------------------------------------

juce::StringArray OrogenProcessor::presetNames() const
{
    juce::StringArray names;
    for (const auto& p : presets) names.add (p.name);
    return names;
}

void OrogenProcessor::setCurrentProgram (int index) { loadPreset (index); }

void OrogenProcessor::loadPreset (int index)
{
    if (index < 0 || index >= (int) presets.size()) return;
    currentProgram = index;
    patch = presets[(size_t) index].patch;
    fxSettings = presets[(size_t) index].fx;
    applyToParams (patch, fxSettings);
    inst.stagePatch (patch);
    inst.setFx (fxSettings);
    stagedTypeHash = slotTypeHash();
    if (auto* ed = getActiveEditor()) ed->repaint();
}

const juce::String OrogenProcessor::getProgramName (int index)
{
    if (index < 0 || index >= (int) presets.size()) return {};
    return presets[(size_t) index].name;
}

void OrogenProcessor::changeProgramName (int index, const juce::String& newName)
{
    if (index < 0 || index >= (int) presets.size()) return;
    presets[(size_t) index].name = newName;
}

bool OrogenProcessor::loadPresetFile (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    auto parsed = juce::JSON::parse (f.loadFileAsString());
    Patch p;
    FxChain::Settings fx;
    juce::String name;
    if (! presetFromVar (parsed, p, fx, name)) return false;

    patch = p;
    fxSettings = fx;
    applyToParams (patch, fxSettings);
    inst.stagePatch (patch);            // движки построит message thread
    inst.setFx (fxSettings);
    stagedTypeHash = slotTypeHash();
    return true;
}

bool OrogenProcessor::savePresetFile (const juce::File& f, const juce::String& name) const
{
    Patch p;
    FxChain::Settings fx;
    collectPatch (p, fx);
    p.setName (name.toRawUTF8());
    const auto json = juce::JSON::toString (presetToVar (name, p, fx), false);
    return f.replaceWithText (json);
}


juce::var OrogenProcessor::bankToVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "OROGEN bank");
    o->setProperty ("version", 1);
    juce::Array<juce::var> list;
    for (const auto& pr : presets)
        list.add (presetToVar (pr.name, pr.patch, pr.fx));
    o->setProperty ("programs", list);
    return juce::var (o);
}

bool OrogenProcessor::saveBankFile (const juce::File& f) const
{
    return f.replaceWithText (juce::JSON::toString (bankToVar(), false));
}

bool OrogenProcessor::loadBankFile (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    auto parsed = juce::JSON::parse (f.loadFileAsString());
    auto* o = parsed.getDynamicObject();
    if (o == nullptr) return false;
    const auto arr = o->getProperty ("programs");
    if (auto* list = arr.getArray())
    {
        if (list->isEmpty()) return false;

        // разбираем целиком, прежде чем что-то менять: битый банк не должен
        // оставить плагин без пресетов
        std::vector<Preset> loaded;
        loaded.reserve ((size_t) list->size());
        for (const auto& item : *list)
        {
            Patch p;
            FxChain::Settings fx;
            juce::String name;
            if (! presetFromVar (item, p, fx, name)) return false;
            if (name.isEmpty()) name = "Program " + juce::String ((int) loaded.size() + 1);
            loaded.push_back ({ name, p, fx });
        }

        presets = std::move (loaded);
        currentProgram = 0;
        loadPreset (0);
        return true;
    }

    // одиночный пресет тоже принимаем как банк из одной программы
    Patch p;
    FxChain::Settings fx;
    juce::String name;
    if (! presetFromVar (parsed, p, fx, name)) return false;
    presets.clear();
    presets.push_back ({ name.isEmpty() ? juce::String ("Program 1") : name, p, fx });
    currentProgram = 0;
    loadPreset (0);
    return true;
}

int OrogenProcessor::addProgram (const juce::String& name)
{
    Patch p;
    FxChain::Settings fx;
    collectPatch (p, fx);
    presets.push_back ({ name.isEmpty() ? juce::String ("Program ") + juce::String ((int) presets.size() + 1)
                                         : name, p, fx });
    return (int) presets.size() - 1;
}

void OrogenProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    // Состояние = параметры + имя патча. Пресетный JSON хранится отдельно:
    // в сессии важно то, что реально звучит, а не файл, из которого пришли.
    juce::ValueTree state = apvts.copyState();
    state.setProperty ("patchName", juce::String (patch.name), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void OrogenProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;

    // В состояниях до этих четырёх параметров записи отсутствуют. JUCE при
    // replaceState иначе сохраняет ТЕКУЩЕЕ значение отсутствующего параметра,
    // а не его default. Старый проект должен звучать как раньше даже при
    // загрузке поверх уже отредактированного экземпляра.
    for (const char* id : { "fx_platePreFilter", "fx_plateInputDiff1",
                            "fx_plateInputDiff2", "fx_plateOutput" })
    {
        if (state.getChildWithProperty ("id", id).isValid()) continue;
        if (auto* parameter = apvts.getParameter (id))
        {
            juce::ValueTree entry ("PARAM");
            entry.setProperty ("id", id, nullptr);
            entry.setProperty ("value", parameter->convertFrom0to1 (parameter->getDefaultValue()), nullptr);
            state.appendChild (entry, nullptr);
        }
    }

    apvts.replaceState (state);
    collectPatch (patch, fxSettings);
    patch.setName (state.getProperty ("patchName", "Untitled").toString().toRawUTF8());
    inst.stagePatch (patch);
    inst.setFx (fxSettings);
    stagedTypeHash = slotTypeHash();
}

juce::AudioProcessorEditor* OrogenProcessor::createEditor()
{
    return new OrogenEditor (*this);
}

} // namespace orogen

// Точка входа VST3
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new orogen::OrogenProcessor();
}
