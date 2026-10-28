// Сериализация пресета: Patch + FxChain::Settings <-> ValueTree/JSON.
//
// Формат — обычный JSON через juce::JSON, файл .orogenpreset. Поля названы так
// же, как в структурах: patch.slots[3].attack, fx.reverbMix. Поэтому старые
// пресеты переживут добавление новых полей — отсутствующие берут значение по
// умолчанию, лишние игнорируются (fromTree читает по имени, а не по индексу).
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "instrument/patcher.h"
#include "instrument/fx.h"

namespace orogen
{

inline const char* engineTypeId (EngineType t) { return engineTypeName (t); }

inline EngineType engineTypeFromString (const juce::String& s)
{
    for (int i = 0; i < (int) EngineType::Count; ++i)
        if (s == engineTypeName ((EngineType) i)) return (EngineType) i;
    return EngineType::Wavetable;
}

// ---------- Patch -----------------------------------------------------------

inline juce::var slotToVar (const SlotPatch& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("engine", engineTypeId (s.type));
    o->setProperty ("on", s.on);
    o->setProperty ("p0", s.params.p[0]); o->setProperty ("p1", s.params.p[1]);
    o->setProperty ("p2", s.params.p[2]); o->setProperty ("p3", s.params.p[3]);
    o->setProperty ("level", s.params.level);
    o->setProperty ("pan", s.params.pan);
    o->setProperty ("tune", s.params.tuneSemis);
    o->setProperty ("detune", s.params.detune);
    o->setProperty ("table", s.params.table);
    o->setProperty ("attack", s.attack); o->setProperty ("decay", s.decay);
    o->setProperty ("sustain", s.sustain); o->setProperty ("release", s.release);
    o->setProperty ("cutoff", s.cutoff); o->setProperty ("res", s.res);
    o->setProperty ("filter", s.filterType);
    o->setProperty ("lo", s.loNote); o->setProperty ("hi", s.hiNote);
    o->setProperty ("tail", s.tail);
    return juce::var (o);
}

inline void slotFromVar (const juce::var& v, SlotPatch& s)
{
    auto* o = v.getDynamicObject();
    if (o == nullptr) return;
    auto f = [o] (const char* k, float def)
    {
        return o->hasProperty (k) ? (float) (double) o->getProperty (k) : def;
    };
    s.type = engineTypeFromString (o->getProperty ("engine").toString());
    s.on = (bool) o->getProperty ("on");
    s.params.p[0] = f ("p0", 0.3f); s.params.p[1] = f ("p1", 0.2f);
    s.params.p[2] = f ("p2", 0.0f); s.params.p[3] = f ("p3", 0.3f);
    s.params.level  = f ("level", 1.0f);
    s.params.pan    = f ("pan", 0.0f);
    s.params.tuneSemis = f ("tune", 0.0f);
    s.params.detune = f ("detune", 0.3f);
    s.params.table  = (int) o->getProperty ("table");
    s.attack  = f ("attack", 0.005f); s.decay  = f ("decay", 0.30f);
    s.sustain = f ("sustain", 0.85f); s.release = f ("release", 0.35f);
    s.cutoff  = f ("cutoff", 1.0f);   s.res     = f ("res", 0.0f);
    s.filterType = (int) o->getProperty ("filter");
    s.loNote = o->hasProperty ("lo") ? (int) o->getProperty ("lo") : 0;
    s.hiNote = o->hasProperty ("hi") ? (int) o->getProperty ("hi") : 127;
    s.tail   = f ("tail", 0.05f);
}

inline float ratioOr (const juce::var& v, float def) { return v.isVoid() ? def : (float) (double) v; }

inline juce::var patchToVar (const Patch& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", juce::String (p.name));
    o->setProperty ("mode", (int) p.mode);
    o->setProperty ("morph", p.morph);
    o->setProperty ("glide", p.glide);
    o->setProperty ("width", p.width);
    o->setProperty ("gain", p.gain);
    o->setProperty ("modWheel", p.modWheel);
    juce::Array<juce::var> slots;
    for (const auto& s : p.slots) slots.add (slotToVar (s));
    o->setProperty ("slots", slots);
    juce::Array<juce::var> lfos;
    for (const auto& l : p.lfo)
    {
        auto* lo = new juce::DynamicObject();
        lo->setProperty ("rate", l.rate);
        lo->setProperty ("amount", l.amount);
        lo->setProperty ("dest", (int) l.dest);
        lo->setProperty ("shape", l.shape);
        lfos.add (juce::var (lo));
    }
    o->setProperty ("lfo", lfos);

    // мод-матрица: массив из 8 маршрутов (source/dest/amount)
    juce::Array<juce::var> routes;
    for (const auto& rt : p.routes)
    {
        auto ro = std::make_unique<juce::DynamicObject>();
        ro->setProperty ("src", (int) rt.source);
        ro->setProperty ("dst", (int) rt.dest);
        ro->setProperty ("amt", rt.amount);
        routes.add (juce::var (ro.release()));
    }
    o->setProperty ("routes", routes);
    return juce::var (o);
}

inline Patch patchFromVar (const juce::var& v)
{
    Patch p = patchInit();
    auto* o = v.getDynamicObject();
    if (o == nullptr) return p;

    const auto name = o->getProperty ("name").toString();
    if (name.isNotEmpty()) p.setName (name.toRawUTF8());
    p.mode  = (PatchMode) (int) o->getProperty ("mode");
    p.morph = (float) (double) o->getProperty ("morph");
    p.glide = (float) (double) o->getProperty ("glide");
    p.width = ratioOr (o->getProperty ("width"), 1.0f);
    p.gain  = ratioOr (o->getProperty ("gain"), 0.67f);
    p.modWheel = (float) (double) o->getProperty ("modWheel");

    const auto slots = o->getProperty ("slots");
    if (auto* arr = slots.getArray())
        for (int i = 0; i < arr->size() && i < 8; ++i)
            slotFromVar ((*arr)[i], p.slots[i]);

    if (const auto* arr = o->getProperty ("routes").getArray())
        for (int i = 0; i < (int) arr->size() && i < kModRoutes; ++i)
            if (auto* ro = arr->getUnchecked (i).getDynamicObject())
            {
                p.routes[i].source = (ModSource) (int) ro->getProperty ("src");
                p.routes[i].dest   = (ModDest) (int) ro->getProperty ("dst");
                p.routes[i].amount = (float) (double) ro->getProperty ("amt");
            }

    const auto lfos = o->getProperty ("lfo");
    if (auto* arr = lfos.getArray())
        for (int i = 0; i < arr->size() && i < 2; ++i)
            if (auto* lo = (*arr)[i].getDynamicObject())
            {
                p.lfo[i].rate   = (float) (double) lo->getProperty ("rate");
                p.lfo[i].amount = (float) (double) lo->getProperty ("amount");
                p.lfo[i].dest   = (ModDest) (int) lo->getProperty ("dest");
                p.lfo[i].shape  = (int) lo->getProperty ("shape");
            }
    return p;
}

// ---------- FxChain::Settings ----------------------------------------------
// Пишем только те поля, что реально читаются в FxChain::set(); структура POD,
// поэтому проще перечислить поля явно — так JSON остаётся читаемым.

inline juce::var fxToVar (const FxChain::Settings& s)
{
    auto* o = new juce::DynamicObject();
    auto put = [o] (const char* k, auto value) { o->setProperty (k, value); };
    put ("drive", s.drive);           put ("driveModel", s.driveModel);
    put ("driveTone", s.driveTone);
    put ("chorusMix", s.chorusMix);   put ("chorusDepth", s.chorusDepth);
    put ("chorusRate", s.chorusRate); put ("chorusModel", s.chorusModel);
    put ("delayMix", s.delayMix);     put ("delayTime", s.delayTime);
    put ("delayFb", s.delayFb);
    put ("reverbMix", s.reverbMix);   put ("reverbSize", s.reverbSize);
    put ("reverbDamp", s.reverbDamp);
    put ("plateMix", s.plateMix);     put ("plateDecay", s.plateDecay);
    put ("plateDamp", s.plateDamp);   put ("plateDiffusion", s.plateDiffusion);
    put ("platePredelay", s.platePredelay);
    put ("eqLow", s.eqLow);           put ("eqHigh", s.eqHigh);
    put ("compThreshold", s.compThreshold); put ("compRatio", s.compRatio);
    put ("compMakeup", s.compMakeup); put ("width", s.width);
    put ("materialMix", s.materialMix); put ("materialMode", s.materialMode);
    put ("materialExcite", s.materialExcite); put ("materialTone", s.materialTone);
    put ("materialDamp", s.materialDamp); put ("materialSize", s.materialSize);
    put ("materialFeedback", s.materialFeedback); put ("materialFollow", s.materialFollow);
    put ("spectreMix", s.spectreMix); put ("spectreModel", s.spectreModel);
    put ("spectreWindow", s.spectreWindow); put ("spectreProminence", s.spectreProminence);
    put ("spectreShift", s.spectreShift); put ("spectrePeaks", s.spectrePeaks);
    put ("spectreWidth", s.spectreWidth); put ("spectreFreeze", s.spectreFreeze);
    put ("spectreLow", s.spectreLow); put ("spectreHigh", s.spectreHigh);
    put ("robinMix", s.robinMix);     put ("robinSemis", s.robinSemis);
    put ("robinSize", s.robinSize);   put ("robinSpread", s.robinSpread);
    put ("glitchMix", s.glitchMix);   put ("glitchChance", s.glitchChance);
    put ("glitchLengthMs", s.glitchLengthMs); put ("glitchRepeats", s.glitchRepeats);
    put ("glitchReverse", s.glitchReverse); put ("glitchPitch", s.glitchPitch);
    put ("glitchFeedback", s.glitchFeedback); put ("glitchFold", s.glitchFold);
    put ("glitchTone", s.glitchTone);
    put ("combMix", s.combMix);       put ("combModel", s.combModel);
    put ("combDriveDb", s.combDriveDb); put ("combOutputDb", s.combOutputDb);
    put ("combDepth", s.combDepth);   put ("combRate", s.combRate);
    put ("combEnvelope", s.combEnvelope);
    put ("combFreqA", s.combFreqA);   put ("combFreqB", s.combFreqB);
    put ("combResA", s.combResA);     put ("combResB", s.combResB);
    put ("combBalance", s.combBalance); put ("combRouting", s.combRouting);
    put ("combNegative", s.combNegative);
    put ("combDelayMs", s.combDelayMs); put ("combFeedback", s.combFeedback);
    put ("combHighpass", s.combHighpass); put ("combTune", s.combTune);
    put ("combOversample", s.combOversample); put ("combMidSide", s.combMidSide);
    juce::Array<juce::var> fw;
    for (int v : s.chorusFw) fw.add (v);
    o->setProperty ("chorusFw", fw);
    return juce::var (o);
}

inline void fxFromVar (const juce::var& v, FxChain::Settings& s)
{
    auto* o = v.getDynamicObject();
    if (o == nullptr) return;
    auto f = [o] (const char* k, float def)
    {
        return o->hasProperty (k) ? (float) (double) o->getProperty (k) : def;
    };
    auto i = [o] (const char* k, int def)
    {
        return o->hasProperty (k) ? (int) o->getProperty (k) : def;
    };
    auto b = [o] (const char* k, bool def)
    {
        return o->hasProperty (k) ? (bool) o->getProperty (k) : def;
    };
    s.drive = f ("drive", s.drive);           s.driveModel = i ("driveModel", s.driveModel);
    s.driveTone = f ("driveTone", s.driveTone);
    s.chorusMix = f ("chorusMix", s.chorusMix); s.chorusDepth = f ("chorusDepth", s.chorusDepth);
    s.chorusRate = f ("chorusRate", s.chorusRate); s.chorusModel = i ("chorusModel", s.chorusModel);
    s.delayMix = f ("delayMix", s.delayMix);   s.delayTime = f ("delayTime", s.delayTime);
    s.delayFb = f ("delayFb", s.delayFb);
    s.reverbMix = f ("reverbMix", s.reverbMix); s.reverbSize = f ("reverbSize", s.reverbSize);
    s.reverbDamp = f ("reverbDamp", s.reverbDamp);
    s.plateMix = f ("plateMix", s.plateMix);   s.plateDecay = f ("plateDecay", s.plateDecay);
    s.plateDamp = f ("plateDamp", s.plateDamp); s.plateDiffusion = f ("plateDiffusion", s.plateDiffusion);
    s.platePredelay = f ("platePredelay", s.platePredelay);
    s.eqLow = f ("eqLow", s.eqLow);           s.eqHigh = f ("eqHigh", s.eqHigh);
    s.compThreshold = f ("compThreshold", s.compThreshold); s.compRatio = f ("compRatio", s.compRatio);
    s.compMakeup = f ("compMakeup", s.compMakeup); s.width = f ("width", s.width);
    s.materialMix = f ("materialMix", s.materialMix); s.materialMode = i ("materialMode", s.materialMode);
    s.materialExcite = f ("materialExcite", s.materialExcite); s.materialTone = f ("materialTone", s.materialTone);
    s.materialDamp = f ("materialDamp", s.materialDamp); s.materialSize = f ("materialSize", s.materialSize);
    s.materialFeedback = f ("materialFeedback", s.materialFeedback);
    s.materialFollow = b ("materialFollow", s.materialFollow);
    s.spectreMix = f ("spectreMix", s.spectreMix); s.spectreModel = i ("spectreModel", s.spectreModel);
    s.spectreWindow = i ("spectreWindow", s.spectreWindow);
    s.spectreProminence = f ("spectreProminence", s.spectreProminence);
    s.spectreShift = f ("spectreShift", s.spectreShift); s.spectrePeaks = i ("spectrePeaks", s.spectrePeaks);
    s.spectreWidth = i ("spectreWidth", s.spectreWidth); s.spectreFreeze = b ("spectreFreeze", s.spectreFreeze);
    s.spectreLow = f ("spectreLow", s.spectreLow); s.spectreHigh = f ("spectreHigh", s.spectreHigh);
    s.glitchMix = f ("glitchMix", s.glitchMix); s.glitchChance = f ("glitchChance", s.glitchChance);
    s.glitchLengthMs = f ("glitchLengthMs", s.glitchLengthMs); s.glitchRepeats = f ("glitchRepeats", s.glitchRepeats);
    s.glitchReverse = f ("glitchReverse", s.glitchReverse); s.glitchPitch = f ("glitchPitch", s.glitchPitch);
    s.glitchFeedback = f ("glitchFeedback", s.glitchFeedback);
    s.glitchFold = f ("glitchFold", s.glitchFold);
    s.glitchTone = f ("glitchTone", s.glitchTone);
    s.robinMix = f ("robinMix", s.robinMix); s.robinSemis = f ("robinSemis", s.robinSemis);
    s.robinSize = f ("robinSize", s.robinSize); s.robinSpread = f ("robinSpread", s.robinSpread);
    s.combMix = f ("combMix", s.combMix);     s.combModel = i ("combModel", s.combModel);
    s.combDriveDb = f ("combDriveDb", s.combDriveDb); s.combOutputDb = f ("combOutputDb", s.combOutputDb);
    s.combDepth = f ("combDepth", s.combDepth); s.combRate = f ("combRate", s.combRate);
    s.combEnvelope = f ("combEnvelope", s.combEnvelope);
    s.combFreqA = f ("combFreqA", s.combFreqA); s.combFreqB = f ("combFreqB", s.combFreqB);
    s.combResA = f ("combResA", s.combResA);   s.combResB = f ("combResB", s.combResB);
    s.combBalance = f ("combBalance", s.combBalance); s.combRouting = i ("combRouting", s.combRouting);
    s.combNegative = b ("combNegative", s.combNegative);
    s.combDelayMs = f ("combDelayMs", s.combDelayMs); s.combFeedback = f ("combFeedback", s.combFeedback);
    s.combHighpass = f ("combHighpass", s.combHighpass); s.combTune = f ("combTune", s.combTune);
    s.combOversample = b ("combOversample", s.combOversample);
    s.combMidSide = b ("combMidSide", s.combMidSide);
    if (auto* arr = o->getProperty ("chorusFw").getArray())
        for (int k = 0; k < arr->size() && k < 8; ++k)
            s.chorusFw[(size_t) k] = (int) (*arr)[k];
}

// ---------- пресет целиком -------------------------------------------------

inline juce::var presetToVar (const juce::String& name, const Patch& p, const FxChain::Settings& fx)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "OROGEN preset");
    o->setProperty ("version", 1);
    o->setProperty ("author", "OROGEN");
    o->setProperty ("presetName", name);
    o->setProperty ("patch", patchToVar (p));
    o->setProperty ("fx", fxToVar (fx));
    return juce::var (o);
}

inline bool presetFromVar (const juce::var& v, Patch& p, FxChain::Settings& fx, juce::String& name)
{
    auto* o = v.getDynamicObject();
    if (o == nullptr) return false;
    if (! o->hasProperty ("patch")) return false;
    p = patchFromVar (o->getProperty ("patch"));
    fx = FxChain::Settings();
    fxFromVar (o->getProperty ("fx"), fx);
    name = o->getProperty ("presetName").toString();
    if (name.isEmpty()) name = juce::String (p.name);
    return true;
}

} // namespace orogen
