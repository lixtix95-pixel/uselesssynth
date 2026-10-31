// Список параметров плагина = «зеркало» структур Patch и FxChain::Settings.
//
// Правило: единственный источник истины для пресетов — Patch + FxChain::Settings
// (JSON). APVTS нужен хосту для автоматизации и UI, поэтому каждое поле структур
// имеет параметр с предсказуемым id: sA_level, sB_cutoff, fx_reverbMix, gl_master.
// Историческая первая копия читается в структуры; независимые копии и их
// позиции дополнительно хранятся в APVTS, DAW state и JSON-пресетах.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "instrument/patcher.h"
#include "instrument/fx.h"
#include "instrument/arpeggiator.h"

namespace orogen
{

inline const char* kSlotLetters = "ABCDEFGH";
inline constexpr int kNumSlots = 8;

inline juce::String slotPrefix (int slot) { return juce::String ("s") + kSlotLetters[slot] + "_"; }

// Добавленные в rev22 параметры FX размещаются в САМОМ КОНЦЕ ParameterLayout,
// чтобы индексы первых 294 параметров хоста остались прежними.
inline constexpr int kOriginalFxParamCount = 66;
inline constexpr int kAddedPlateParamCount = 4;

// Имена параметров для UI: «A · Level» и т.п.
struct SlotParam
{
    const char* id;         // без префикса слота
    const char* name;
    float min, max, def, step;
};

// Порядок и значения по умолчанию совпадают с EngineParams/SlotPatch.
inline const SlotParam* slotParamTable (int& count)
{
    static const SlotParam table[] =
    {
        { "level",    "Level",     0.0f,   1.0f,  1.0f,   0.001f },
        { "pan",      "Pan",      -1.0f,   1.0f,  0.0f,   0.001f },
        { "tune",     "Tune",    -24.0f,  24.0f,  0.0f,   0.01f  },
        { "detune",   "Detune",    0.0f,   1.0f,  0.3f,   0.001f },
        { "p0",       "P1",        0.0f,   1.0f,  0.3f,   0.001f },
        { "p1",       "P2",        0.0f,   1.0f,  0.2f,   0.001f },
        { "p2",       "P3",        0.0f,   1.0f,  0.0f,   0.001f },
        { "p3",       "P4",        0.0f,   1.0f,  0.3f,   0.001f },
        { "table",    "Table",     0.0f,   7.0f,  0.0f,   1.0f   },
        { "attack",   "Attack",    0.001f, 8.0f,  0.005f, 0.001f },
        { "decay",    "Decay",     0.005f, 8.0f,  0.30f,  0.001f },
        { "sustain",  "Sustain",   0.0f,   1.0f,  0.85f,  0.001f },
        { "release",  "Release",   0.005f, 10.0f, 0.35f,  0.001f },
        { "cutoff",   "Cutoff",    0.0f,   1.0f,  1.0f,   0.001f },
        { "res",      "Reso",      0.0f,   1.0f,  0.0f,   0.001f },
        { "lo",       "Low Key",   0.0f, 127.0f,  0.0f,   1.0f   },
        { "hi",       "High Key",  0.0f, 127.0f, 127.0f,  1.0f   },
        { "tail",     "Tail",      0.0f,   1.0f,  0.05f,  0.001f }
    };
    count = (int) (sizeof (table) / sizeof (table[0]));
    return table;
}

// Эффекты: имя поля FxChain::Settings -> подпись в UI.
struct FxParam
{
    const char* id;
    const char* name;
    float min, max, def, step;
};

inline const FxParam* fxParamTable (int& count)
{
    static const FxParam table[] =
    {
        { "drive",            "Drive",         0.0f,   1.0f,  0.0f,  0.001f },
        { "driveTone",        "Drive Tone",   -1.0f,   1.0f,  0.0f,  0.001f },
        { "chorusMix",        "Chorus Mix",    0.0f,   1.0f,  0.0f,  0.001f },
        { "chorusDepth",      "Chorus Depth",  0.0f,   1.0f,  0.5f,  0.001f },
        { "chorusRate",       "Chorus Rate",   0.0f,   2.0f,  0.35f, 0.001f },
        // остальные ручки ядра OS 1.32B — те, что есть на панели прибора:
        // DEL / FB / WID / LP / INP (0…127, как в прошивке). DEP и SPD — это
        // уже chorusDepth и chorusRate выше.
        { "chorusDelay",      "Chorus Delay",  0.0f, 127.0f, 40.0f,  1.0f   },
        { "chorusFeedback",   "Chorus Fb",     0.0f, 127.0f, 30.0f,  1.0f   },
        { "chorusWidth",      "Chorus Width",  0.0f, 127.0f, 80.0f,  1.0f   },
        { "chorusLowpass",    "Chorus LP",     0.0f, 127.0f, 100.0f, 1.0f   },
        { "chorusInput",      "Chorus In",     0.0f, 127.0f, 100.0f, 1.0f   },
        { "delayMix",         "Delay Mix",     0.0f,   1.0f,  0.0f,  0.001f },
        { "delayTime",        "Delay Time",    0.02f,  1.5f,  0.33f, 0.001f },
        { "delayFb",          "Delay Fb",      0.0f,   0.95f, 0.35f, 0.001f },
        { "reverbMix",        "Reverb Mix",    0.0f,   1.0f,  0.0f,  0.001f },
        { "reverbSize",       "Reverb Size",   0.0f,   1.0f,  0.6f,  0.001f },
        { "reverbDamp",       "Reverb Damp",   0.0f,   1.0f,  0.4f,  0.001f },
        { "plateMix",         "Plate Mix",     0.0f,   1.0f,  0.0f,  0.001f },
        { "plateDecay",       "Plate Decay",   0.0f,   1.0f,  0.75f, 0.001f },
        { "plateDamp",        "Plate Damp",    0.0f,   1.0f,  0.6f,  0.001f },
        { "plateDiffusion",   "Plate Diff",    0.0f,   1.0f,  0.7f,  0.001f },
        { "platePredelay",    "Plate Pre",     0.0f,   0.2f,  0.01f, 0.001f },
        { "eqLow",            "EQ Low",      -15.0f,  15.0f,  0.0f,  0.01f  },
        { "eqHigh",           "EQ High",     -15.0f,  15.0f,  0.0f,  0.01f  },
        { "compThreshold",    "Comp Thr",    -48.0f,   0.0f, -8.0f,  0.01f  },
        { "compRatio",        "Comp Ratio",    1.0f,  20.0f,  3.0f,  0.01f  },
        { "compMakeup",       "Comp Makeup",   0.0f,  24.0f,  2.0f,  0.01f  },
        { "width",            "Width",         0.0f,   1.5f,  1.0f,  0.001f },
        { "materialMix",      "Material Mix",  0.0f,   1.0f,  0.0f,  0.001f },
        { "materialExcite",   "Mat Excite",    0.0f,   1.0f,  0.5f,  0.001f },
        { "materialTone",     "Mat Tone",      0.0f,   1.0f,  0.5f,  0.001f },
        { "materialDamp",     "Mat Damp",      0.0f,   1.0f,  0.5f,  0.001f },
        { "materialSize",     "Mat Size",      0.0f,   1.0f,  0.5f,  0.001f },
        { "materialFeedback", "Mat Fb",        0.0f,   1.0f,  0.4f,  0.001f },
        { "spectreMix",       "Spectre Mix",   0.0f,   1.0f,  0.0f,  0.001f },
        { "spectreProminence","Spec Prom",     1.0f, 100.0f, 10.0f,  0.01f  },
        { "spectreShift",     "Spec Shift",  -24.0f,  24.0f,  0.0f,  0.01f  },
        { "spectreLow",       "Spec Lo Hz",    0.0f, 8000.0f, 0.0f,  1.0f   },
        { "spectreHigh",      "Spec Hi Hz",    0.0f,20000.0f, 0.0f,  1.0f   },
        { "glitchMix",        "Glitch Mix",    0.0f,   1.0f,  0.0f,  0.001f },
        { "glitchChance",     "Glitch Chance", 0.0f,   1.0f,  0.35f, 0.001f },
        { "glitchLengthMs",   "Glitch Len",    5.0f, 400.0f, 90.0f,  0.1f   },
        { "glitchRepeats",    "Glitch Rep",    1.0f,  16.0f,  3.0f,  0.01f  },
        { "glitchReverse",    "Glitch Rev",    0.0f,   1.0f,  0.0f,  0.001f },
        { "glitchPitch",      "Glitch Pitch", -12.0f,  12.0f,  0.0f,  0.01f  },
        { "glitchFeedback",   "Glitch Fb",     0.0f,   1.0f,  0.0f,  0.001f },
        { "glitchFold",       "Glitch Fold",   0.0f,  10.0f,  0.0f,  0.01f  },
        { "glitchTone",       "Glitch Tone", 200.0f,24000.0f,24000.0f,1.0f  },
        { "combMix",          "Comb Mix",      0.0f,   1.0f,  0.0f,  0.001f },
        { "combDriveDb",      "Comb Drive",  -24.0f,  24.0f,  0.0f,  0.01f  },
        { "combOutputDb",     "Comb Out",    -24.0f,  24.0f,  0.0f,  0.01f  },
        { "combDepth",        "Comb Depth",    0.0f,   1.0f,  0.0f,  0.001f },
        { "combRate",         "Comb Rate",     0.01f,  8.0f,  0.5f,  0.001f },
        { "combEnvelope",     "Comb Env",      0.0f,   1.0f,  0.0f,  0.001f },
        { "combFreqA",        "Comb Hz A",    20.0f, 2000.0f, 220.0f, 0.1f  },
        { "combFreqB",        "Comb Hz B",    20.0f, 2000.0f, 331.0f, 0.1f  },
        { "combResA",         "Comb Res A",    0.0f, 100.0f, 75.0f,  0.1f   },
        { "combResB",         "Comb Res B",    0.0f, 100.0f, 75.0f,  0.1f   },
        { "combBalance",      "Comb Balance",  0.0f,   1.0f,  0.5f,  0.001f },
        { "combDelayMs",      "Comb Delay",    1.0f, 500.0f, 100.0f, 0.1f   },
        { "combFeedback",     "Comb Fb",       0.0f,  99.0f, 25.0f,  0.1f   },
        { "combHighpass",     "Comb HP",       0.0f, 500.0f,  5.0f,  0.1f   },
        { "combTune",         "Comb Tune",   -24.0f,  24.0f,  0.0f,  0.01f  },
        { "robinMix",         "Robin Mix",     0.0f,   1.0f,  0.0f,  0.001f },
        { "robinSemis",       "Robin Semis", -24.0f,  24.0f,  0.0f,  0.01f  },
        { "robinSize",        "Robin Size",    0.0f,   1.0f,  0.5f,  0.001f },
        { "robinSpread",      "Robin Spread",  0.0f,   1.0f,  0.0f,  0.001f },
        // Новые независимые органы Plate — обязательно в конце таблицы.
        { "platePreFilter",   "Plate Pre Filter", 0.0f, 1.0f, 0.850f, 0.001f },
        { "plateInputDiff1",  "Plate Input Diff 1", 0.0f, 1.0f, 0.750f, 0.001f },
        { "plateInputDiff2",  "Plate Input Diff 2", 0.0f, 1.0f, 0.625f, 0.001f },
        { "plateOutput",      "Plate Output", 0.0f, 2.0f, 0.600f, 0.001f }
    };
    count = (int) (sizeof (table) / sizeof (table[0]));
    return table;
}

// Explicit rack metadata. Indices must match FxChain::Module and DSP order.
struct FxModuleInfo { const char* name; const char* section; const char* onId; const char* description; };
inline constexpr FxModuleInfo kFxModules[FxChain::ModuleCount] =
{
    { "DRIVE",    "DRIVE",    "fx_driveOn",    "Saturation and distortion" },
    { "COMB",     "COMB",     "fx_combOn",     "Odin and parallel comb filters" },
    { "CHORUS",   "CHORUS",   "fx_chorusOn",   "Simple or firmware chorus" },
    { "DELAY",    "DELAY",    "fx_delayOn",    "Stereo ping-pong delay" },
    { "REVERB",   "REVERB",   "fx_reverbOn",   "Algorithmic reverb" },
    { "PLATE",    "PLATE",    "fx_plateOn",    "Dattorro plate reverb" },
    { "EQ",       "EQ",       "fx_eqOn",       "Low and high shelving EQ" },
    { "MATERIAL", "MATERIAL", "fx_materialOn", "Modal material resonator" },
    { "SPECTRE",  "SPECTRE",  "fx_spectreOn",  "Spectral processor" },
    { "ROBIN",    "ROBIN",    "fx_robinOn",    "Stereo pitch shifter" },
    { "GLITCH",   "GLITCH",   "fx_glitchOn",   "Stutter, reverse and pitch" },
    { "DYNAMICS", "DYNAMICS", "fx_compOn",     "Peak compressor" },
    { "WIDTH",    "OUTPUT",   "fx_widthOn",    "Mid / side stereo width" }
};

// Extra, previously hidden DSP controls are appended after all 306 legacy IDs.
// The Settings member pointer is used by APVTS, JSON and the processor in both
// directions, so a new control cannot silently exist only in the GUI.
struct FxExtraParam
{
    const char* id; const char* name;
    float min, max, def, step;
    float FxChain::Settings::*field;
};
inline const FxExtraParam* fxExtraParamTable (int& count)
{
    using S = FxChain::Settings;
    static const FxExtraParam table[] = {
        { "driveMix",       "Drive Mix",          0.0f,     1.0f,  1.0f, 0.001f, &S::driveMix },
        { "combDecayMs",    "Comb Env Decay ms", 5.0f,  4000.0f,180.0f, 0.1f,   &S::combDecayMs },
        { "combTapA",       "Comb Tap A ms",      -1.0f,  4000.0f, -1.0f, 0.1f,   &S::combTapA },
        { "combTapB",       "Comb Tap B ms",      -1.0f,  4000.0f, -1.0f, 0.1f,   &S::combTapB },
        { "combTapC",       "Comb Tap C ms",      -1.0f,  4000.0f, -1.0f, 0.1f,   &S::combTapC },
        { "combTapD",       "Comb Tap D ms",      -1.0f,  4000.0f, -1.0f, 0.1f,   &S::combTapD },
        { "combMultiplier", "Comb Delay Multiply", 0.0f,     1.0f,  1.0f, 0.001f, &S::combMultiplier },
        { "combParRate",    "Comb Parallel Rate", 0.0f,     4.0f,  0.5f, 0.001f, &S::combParRate },
        { "combSmoothMs",   "Comb Smooth ms",      0.0f,100000.0f,  0.0f, 0.1f,   &S::combSmoothMs },
        { "combCross",      "Comb Cross %",        0.0f,   100.0f,  0.0f, 0.1f,   &S::combCross },
        { "combAm",         "Comb AM %",           0.0f,   100.0f,  0.0f, 0.1f,   &S::combAm },
        { "combLean",       "Comb Lean %",      -100.0f,   100.0f,  0.0f, 0.1f,   &S::combLean },
        { "combGateDb",     "Comb Gate dB",     -140.0f,     0.0f,-140.0f,0.1f,   &S::combGateDb },
        { "combGateMs",     "Comb Gate ms",        0.1f, 10000.0f, 10.0f, 0.1f,   &S::combGateMs },
        { "combLimiterMs",  "Comb Limiter ms",     0.001f,16000.0f,200.0f,0.001f, &S::combLimiterMs },
        { "combDryDb",      "Comb Dry dB",       -60.0f,     6.0f,  0.0f, 0.1f,   &S::combDryDb },
        { "combWetDb",      "Comb Wet dB",       -60.0f,     6.0f,  0.0f, 0.1f,   &S::combWetDb },
        // Parameters above were introduced with the FX page (indices 319–335).
        // Append after the two existing choices to retain all first 338 IDs.
        { "combOdinMix", "Comb ODIN Mix", 0,1,1,.001f,&S::combOdinMix },
        { "combDryDbExt", "Comb Dry Extended dB", -61,24,-61,.1f,&S::combDryDbExt },
        { "combWetDbExt", "Comb Wet Extended dB", -61,60,-61,.1f,&S::combWetDbExt },
        { "combDriveExt", "Comb Drive Extended dB", -1,36,-1,.1f,&S::combDriveExt },
        { "combOutputExt", "Comb Output Extended dB", -31,12,-31,.1f,&S::combOutputExt },
        { "combDepthExt", "Comb Depth Extended semitones", -1,24,-1,.1f,&S::combDepthExt },
        { "combRateExt", "Comb Speed Extended Hz", -1,20,-1,.01f,&S::combRateExt },
        { "combEnvelopeExt", "Comb Envelope Extended semitones", -37,36,-37,.1f,&S::combEnvelopeExt },
        { "combFreqAExt", "Comb Frequency A Extended Hz", -1,16000,-1,1,&S::combFreqAExt },
        { "combFreqBExt", "Comb Frequency B Extended Hz", -1,16000,-1,1,&S::combFreqBExt },
        { "combFeedbackExt", "Comb Feedback Extended %", -1,100,-1,.1f,&S::combFeedbackExt },
        { "combHighpassExt", "Comb Highpass Extended Hz", -1,24000,-1,1,&S::combHighpassExt },
        { "combTuneExt", "Comb Tune Extended semitones", -49,48,-49,.1f,&S::combTuneExt },
        { "spectreReject", "Spectre Reject Level dB", -120,0,-120,.1f,&S::spectreReject },
        { "spectreAttack", "Spectre Attack ms", 0,2000,0,1,&S::spectreAttack },
        { "spectreRelease", "Spectre Release ms", 0,5000,0,1,&S::spectreRelease },
        { "spectreWindowExt", "Spectre Window Extended index", -1,7,-1,1,&S::spectreWindowExt },
        { "spectrePeaksExt", "Spectre Peaks Extended", -1,512,-1,1,&S::spectrePeaksExt },
        { "spectreWidthExt", "Spectre Width Extended", -1,24,-1,1,&S::spectreWidthExt },
        { "spectreLowExt", "Spectre Low Extended Hz", -1,96000,-1,1,&S::spectreLowExt },
        { "spectreHighExt", "Spectre High Extended Hz", -1,96000,-1,1,&S::spectreHighExt },
        { "glitchFoldDrive", "Glitch Dist", 0,20,0,.01f,&S::glitchFoldDrive },
        { "glitchDryLevel", "Glitch Dry Level", -1,1,-1,.001f,&S::glitchDryLevel },
        { "glitchWetLevel", "Glitch Wet Level", -1,1,-1,.001f,&S::glitchWetLevel },
        { "glitchOffset", "Glitch Capture Offset %", 0,100,100,.1f,&S::glitchOffset },
        { "glitchCrossfade", "Glitch Crossfade %", 0,100,100,.1f,&S::glitchCrossfade },
        { "glitchResonance", "Glitch Resonance %", 0,95,0,.1f,&S::glitchResonance },
        { "glitchDrift", "Glitch Size Drift %", -100,100,0,.1f,&S::glitchDrift },
        { "glitchPitchStep", "Glitch Pitch Step semitones", -12,12,0,.1f,&S::glitchPitchStep },
        { "glitchPitchRandom", "Glitch Pitch Random semitones", 0,12,0,.1f,&S::glitchPitchRandom },
        { "glitchReroll", "Glitch Reroll %", 0,100,0,.1f,&S::glitchReroll },
        { "glitchDecay", "Glitch Repeat Decay %", 0,90,0,.1f,&S::glitchDecay },
        // 0 = FOLLOW; 1–500 ms matches the original GLITCH control. A
        // fractional value below 1 ms would be silently clamped by the DSP.
        { "glitchLengthExt", "Glitch Length Extended ms", 0,500,0,1,&S::glitchLengthExt }
    };
    count = (int) (sizeof (table) / sizeof (table[0]));
    return table;
}

// Параметры-перечисления (choice / bool / int) — отдельно, у них нет диапазона.
struct ChoiceParam { const char* id; const char* name; };

inline const ChoiceParam* choiceTable (int& count)
{
    static const ChoiceParam table[] =
    {
        { "driveModel",     "Drive Model" },
        { "chorusModel",    "Chorus Model" },
        { "materialMode",   "Material Mode" },
        { "spectreModel",   "Spectre Model" },
        { "spectreWindow",  "Spectre Window" },
        { "spectrePeaks",   "Spectre Peaks" },
        { "spectreWidth",   "Spectre Width" },
        { "spectreFreeze",  "Spectre Freeze" },
        { "combModel",      "Comb Model" },
        { "combRouting",    "Comb Routing" },
        { "combNegative",   "Comb Negative" },
        { "combOversample", "Comb Oversample" },
        { "combMidSide",    "Comb MidSide" },
        { "materialFollow", "Material Follow" },
        { "arpMode",        "Arp Mode" },
        { "arpDiv",         "Arp Div" },
        { "gl_morph",       "Morph" },
        { "gl_modWheel",    "Mod Wheel" }
    };
    count = (int) (sizeof (table) / sizeof (table[0]));
    return table;
}

// Copy zero deliberately uses the original VST3 IDs. Other copies have their
// own stable IDs and independently automatable parameter objects.
inline juce::String fxCopyId (const juce::String& original, int copy)
{
    if (copy <= 0 || ! original.startsWith ("fx_")) return original;
    return "fx" + juce::String (copy + 1) + "_" + original.substring (3);
}
inline int fxIdCopy (const juce::String& id)
{
    for (int c = 1; c < FxChain::kCopiesPerModule; ++c)
        if (id.startsWith ("fx" + juce::String (c + 1) + "_")) return c;
    return 0;
}
inline juce::String fxOriginalId (const juce::String& id)
{
    const int copy = fxIdCopy (id);
    return copy == 0 ? id : "fx_" + id.fromFirstOccurrenceOf ("_", false, false);
}
inline juce::String fxRankId (int node) { return "rack_order_" + juce::String (node); }

struct RecordingParameterLayout
{
    juce::AudioProcessorValueTreeState::ParameterLayout result;
    std::vector<juce::RangedAudioParameter*> originalFx;
    template <typename T> void add (std::unique_ptr<T> parameter)
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (parameter.get()))
            if (p->paramID.startsWith ("fx_")) originalFx.push_back (p);
        result.add (std::move (parameter));
    }
};

// Единственное место, где перечисляются id параметров.
inline juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    RecordingParameterLayout layout;

    auto addFloat = [&layout] (const String& id, const String& name,
                               float mn, float mx, float def, float step)
    {
        NormalisableRange<float> range (mn, mx, step);
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def));
    };

    // --- слоты A–H ---
    int n = 0;
    const SlotParam* st = slotParamTable (n);
    for (int s = 0; s < kNumSlots; ++s)
    {
        const String pre = slotPrefix (s);
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { pre + "type", 1 }, pre + "Engine",
            StringArray { "Wavetable", "FM", "Harmonic", "Sample",
                          "Granular", "Modal", "Resynth", "Organics", "Chop" }, 0));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { pre + "on", 1 }, pre + "On", s == 0));
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { pre + "filter", 1 }, pre + "Filter",
            StringArray { "LP12", "LP24", "BP12", "HP24", "Notch12", "LP6", "HP6", "HP12", "BP24", "Notch24", "Peak", "AllPass", "LP36", "Ladder", "Comb", "Vowel" }, 0));
        for (int i = 0; i < n; ++i)
            addFloat (pre + st[i].id, pre + st[i].name, st[i].min, st[i].max, st[i].def, st[i].step);
    }

    // --- глобальные ---
    addFloat ("gl_gain",   "Patch Gain", 0.05f, 4.0f, 0.67f, 0.001f);
    addFloat ("gl_master", "Master",     0.0f,  2.0f, 0.8f,  0.001f);
    addFloat ("gl_glide",  "Glide",      0.0f,  2.0f, 0.0f,  0.001f);
    addFloat ("gl_width",  "Stereo Width", 0.0f, 1.0f, 1.0f, 0.001f);
    addFloat ("gl_morph",  "Morph",        0.0f, 1.0f, 0.0f, 0.001f);
    addFloat ("gl_modWheel", "Mod Wheel",  0.0f, 1.0f, 0.0f, 0.001f);
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "gl_mode", 1 }, "Patch Mode",
        StringArray { "Stack", "Morph", "Split" }, 0));
    for (int l = 0; l < 2; ++l)
    {
        const String pre = "lfo" + String (l + 1) + "_";
        addFloat (pre + "rate",   pre + "Rate",   0.01f, 20.0f, 1.0f, 0.001f);
        addFloat (pre + "amount", pre + "Amount", 0.0f,  1.0f,  0.0f, 0.001f);
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { pre + "dest", 1 }, pre + "Dest",
            StringArray { "Cutoff", "Pitch", "Morph", "Level" }, 0));
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { pre + "shape", 1 }, pre + "Shape",
            StringArray { "Sine", "Triangle", "S&H" }, 0));
    }

    // --- эффекты ---
    int fn = 0;
    const FxParam* ft = fxParamTable (fn);
    jassert (fn == kOriginalFxParamCount + kAddedPlateParamCount);
    for (int i = 0; i < kOriginalFxParamCount; ++i)
        addFloat (String ("fx_") + ft[i].id, ft[i].name, ft[i].min, ft[i].max, ft[i].def, ft[i].step);

    // список моделей берётся из движка, чтобы не разъезжаться с enum DriveModel
    StringArray driveNames;
    for (int m = 0; m < (int) DriveModel::Count; ++m)
        driveNames.add (driveModelName ((DriveModel) m));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_driveModel", 1 }, "Drive Model", driveNames,
        (int) DriveModel::Tanh));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_chorusModel", 1 }, "Chorus Model",
        StringArray { "Simple", "Firmware" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_materialMode", 1 }, "Material Mode",
        StringArray { "Latex", "Spring", "Metal", "Glass" }, 2));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_spectreModel", 1 }, "Spectre Model",
        StringArray { "Extended", "Classic" }, 0));
    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { "fx_spectreWindow", 1 }, "Spectre Window", 0, 6, 3));
    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { "fx_spectrePeaks", 1 }, "Spectre Peaks", 0, 64, 0));
    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { "fx_spectreWidth", 1 }, "Spectre Width", 0, 8, 1));
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "fx_spectreFreeze", 1 }, "Spectre Freeze", false));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_combModel", 1 }, "Comb Model",
        StringArray { "Odin", "Parallel" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_combRouting", 1 }, "Comb Routing",
        StringArray { "A", "A>B", "A+B" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "fx_combNegative", 1 }, "Comb Negative", false));
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "fx_combOversample", 1 }, "Comb OS", false));
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "fx_combMidSide", 1 }, "Comb M/S", false));
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "fx_materialFollow", 1 }, "Material Follow", false));

    // --- мод-матрица: 8 маршрутов × (источник, назначение, величина) ---
    {
        StringArray srcs, dsts;
        for (int m = 0; m < (int) ModSource::Count; ++m) srcs.add (modSourceName ((ModSource) m));
        for (int d = 0; d < (int) ModDest::Count;   ++d) dsts.add (modDestName   ((ModDest) d));
        for (int r = 0; r < kModRoutes; ++r)
        {
            const String pre = "mod" + String (r + 1) + "_";
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { pre + "src", 1 }, pre + "Source", srcs, 0));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { pre + "dst", 1 }, pre + "Dest", dsts, 0));
            addFloat (pre + "amt", pre + "Amount", -1.0f, 1.0f, 0.0f, 0.001f);
        }
    }

    // --- арпеджиатор (стоит до голосов, поэтому не в цепочке эффектов) ---
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "arpOn", 1 }, "Arp On", false));
    StringArray arpModes, arpDivs;
    for (int m = 0; m < (int) ArpMode::Count; ++m) arpModes.add (arpModeName ((ArpMode) m));
    for (int d = 0; d < (int) ArpDiv::Count; ++d) arpDivs.add (arpDivName ((ArpDiv) d));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "arpMode", 1 }, "Arp Mode", arpModes, 0));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "arpDiv", 1 }, "Arp Div", arpDivs, 1));
    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { "arpOctaves", 1 }, "Arp Octaves", 1, 4, 1));
    addFloat ("arpGate",  "Arp Gate",  0.05f, 1.0f, 0.6f, 0.001f);
    addFloat ("arpSwing", "Arp Swing", 0.0f,  0.6f, 0.0f, 0.001f);
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "arpLatch", 1 }, "Arp Latch", false));

    // Только после ВСЕХ старых параметров, включая ARP: стабильный порядок
    // и ID автоматизации существующих сессий. Карта UI при этом группирует
    // эти четыре значения с остальными настройками PLATE по префиксу.
    for (int i = kOriginalFxParamCount; i < fn; ++i)
        addFloat (String ("fx_") + ft[i].id, ft[i].name, ft[i].min, ft[i].max, ft[i].def, ft[i].step);

    // Новые параметры только после всех 298 прежних ID; slotParamTable и
    // порядок автоматизации старых проектов остаются неизменными.
    for (int s = 0; s < kNumSlots; ++s)
    {
        const String pre = slotPrefix (s);
        addFloat (pre + "grainReverse", pre + "Grain Reverse", 0.0f, 1.0f, 0.0f, 0.001f);
    }

    // Only after all 306 previous parameters: preserve the exact old IDs/order.
    for (const auto& module : kFxModules)
        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { module.onId, 1 }, String (module.name) + " On", false));
    int extraCount = 0;
    const auto* extra = fxExtraParamTable (extraCount);
    constexpr int kOriginalFxExtraCount = 17;
    for (int i = 0; i < std::min (extraCount, kOriginalFxExtraCount); ++i)
        addFloat (String ("fx_") + extra[i].id, extra[i].name,
                  extra[i].min, extra[i].max, extra[i].def, extra[i].step);
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "fx_plateDc", 1 }, "Plate DC Block", true));
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fx_combNegativeB", 1 }, "Comb Negative B",
        StringArray { "Follow A", "Positive", "Negative" }, 0));
    for (int i = kOriginalFxExtraCount; i < extraCount; ++i)
        addFloat (String ("fx_") + extra[i].id, extra[i].name,
                  extra[i].min, extra[i].max, extra[i].def, extra[i].step);
    // Host reset/default agrees with the new INIT. Old states without these
    // IDs are explicitly migrated to Legacy in setStateInformation().
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "fx_chorusMode", 1 }, "Chorus Mix Mode", StringArray { "Legacy", "V1 internal", "V2 external" }, 2));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "fx_glitchTimingMode", 1 }, "Glitch Timing", StringArray { "Legacy", "Host rate" }, 1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "fx_glitchSync", 1 }, "Glitch Tempo Sync", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "fx_glitchDivision", 1 }, "Glitch Length Division", StringArray { "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128" }, 3));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "fx_glitchHighpass", 1 }, "Glitch Highpass", false));

    // Append, never interleave, so the original 376 IDs and ordering survive.
    // Clone their actual ranges, discrete choices and defaults from the source
    // objects rather than maintaining a second divergent parameter table.
    const auto originalFx = layout.originalFx;
    for (int copy = 1; copy < FxChain::kCopiesPerModule; ++copy)
        for (auto* source : originalFx)
        {
            const auto id = fxCopyId (source->paramID, copy);
            const auto name = source->getName (100) + " #" + String (copy + 1);
            const auto value = source->convertFrom0to1 (source->getDefaultValue());
            if (auto* choice = dynamic_cast<AudioParameterChoice*> (source))
                layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name,
                                                                      choice->getAllValueStrings(), (int) std::lround (value)));
            else if (dynamic_cast<AudioParameterBool*> (source) != nullptr)
                layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, value > 0.5f));
            else if (dynamic_cast<AudioParameterInt*> (source) != nullptr)
            {
                const auto range = source->getNormalisableRange();
                layout.add (std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, name,
                                                                  (int) range.start, (int) range.end, (int) std::lround (value)));
            }
            else
                layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                                    source->getNormalisableRange(), value));
        }
    for (int node = 0; node < FxChain::kNodeCount; ++node)
    {
        const int copy = node / FxChain::ModuleCount, module = node % FxChain::ModuleCount;
        layout.add (std::make_unique<AudioParameterInt> (
            ParameterID { fxRankId (node), 1 },
            String (kFxModules[module].name) + " #" + String (copy + 1) + " Rack Position",
            0, FxChain::kRackLimit, copy == 0 ? module + 1 : 0));
    }
    // New per-slot selection is appended so every existing host ID/index,
    // including all four FX copies and rack ranks, remains untouched.
    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto id = slotPrefix (s) + "wtUser";
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 },
                                                          slotPrefix (s) + "User Wavetable", false));
    }
    // FM graph + operator ratios are appended after all previously published
    // IDs. Choice 0 keeps the original FM engine for old projects.
    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto pre = slotPrefix (s);
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { pre + "fmAlgorithm", 1 }, pre + "FM Algorithm",
            StringArray { "Legacy", "Algorithm 1", "Algorithm 2", "Algorithm 3", "Algorithm 4",
                          "Algorithm 5", "Algorithm 6", "Algorithm 7", "Algorithm 8" }, 0));
        addFloat (pre + "fmRatioC", pre + "FM Ratio C", .25f, 16.0f, 1.0f, .01f);
        addFloat (pre + "fmRatioB1", pre + "FM Ratio B1", .25f, 16.0f, 1.0f, .01f);
        addFloat (pre + "fmRatioB2", pre + "FM Ratio B2", .25f, 16.0f, 1.0f, .01f);
        addFloat (pre + "fmMix", pre + "FM X/Y Mix", 0.0f, 1.0f, .5f, .001f);
    }
    return std::move (layout.result);
}

} // namespace orogen
