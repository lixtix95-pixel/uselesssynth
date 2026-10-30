// Список параметров плагина = «зеркало» структур Patch и FxChain::Settings.
//
// Правило: единственный источник истины для пресетов — Patch + FxChain::Settings
// (JSON). APVTS нужен хосту для автоматизации и UI, поэтому каждое поле структур
// имеет параметр с предсказуемым id: sA_level, sB_cutoff, fx_reverbMix, gl_master.
// Пресет читается в структуры, затем структуры пишутся в APVTS (см. applyToParams).
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

// Единственное место, где перечисляются id параметров.
inline juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

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

    return layout;
}

} // namespace orogen
