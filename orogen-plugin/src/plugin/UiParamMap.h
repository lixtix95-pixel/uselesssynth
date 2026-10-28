// Карта «параметр → место в интерфейсе».
//
// Единственное место, где сказано, на какой странице и в какой секции живёт
// каждый из 294 параметров. Редактор строит по ней оверлеи, а тест
// (tests/test_plugin.cpp, блок 9) проверяет, что покрытие полное: каждый
// параметр APVTS встречается в карте ровно один раз. Так «забытый» параметр
// ловится тестом, а не пользователем.
#pragma once

#include "Params.h"

#include <vector>

namespace orogen
{

struct UiSection
{
    juce::String title;
    juce::StringArray ids;
};

struct UiTab
{
    juce::String title;
    std::vector<UiSection> sections {};
};

namespace detail
{
    inline juce::StringArray fxChoiceIds()
    {
        return { "fx_driveModel", "fx_chorusModel", "fx_materialMode",
                 "fx_spectreModel", "fx_spectreWindow", "fx_spectrePeaks",
                 "fx_spectreWidth", "fx_spectreFreeze",
                 "fx_combModel", "fx_combRouting", "fx_combNegative",
                 "fx_combOversample", "fx_combMidSide", "fx_materialFollow" };
    }

    // Секция эффекта по префиксу параметра. Порядок правил важен: первый
    // подошедший префикс выигрывает.
    inline juce::String fxSectionOf (const juce::String& id)
    {
        static const struct { const char* prefix; const char* section; } rules[] =
        {
            { "fx_drive",    "DRIVE" },
            { "fx_chorus",   "CHORUS" },
            { "fx_delay",    "DELAY" },
            { "fx_reverb",   "REVERB" },
            { "fx_plate",    "PLATE" },
            { "fx_eq",       "EQ" },
            { "fx_comp",     "DYNAMICS" },
            { "fx_width",    "OUTPUT" },
            { "fx_material", "MATERIAL" },
            { "fx_spectre",  "SPECTRE" },
            { "fx_glitch",   "GLITCH" },
            { "fx_robin",    "ROBIN" },
            { "fx_comb",     "COMB" }
        };

        for (const auto& r : rules)
            if (id.startsWith (r.prefix)) return r.section;

        return "OUTPUT";
    }
}

// Полная карта: 5 вкладок. Собирается из тех же таблиц Params.h, что и
// ParameterLayout, поэтому расхождение «параметр есть, а места нет» ловится.
inline std::vector<UiTab> buildUiTabs()
{
    std::vector<UiTab> tabs;

    int slotCount = 0;
    const SlotParam* slotTable = slotParamTable (slotCount);
    int fxCount = 0;
    const FxParam* fxTable = fxParamTable (fxCount);

    // --- вкладка SLOT: по секции на слот A–H ---
    {
        UiTab tab { "SLOT", {} };
        for (int s = 0; s < kNumSlots; ++s)
        {
            const auto pre = slotPrefix (s);
            UiSection sec;
            sec.title = juce::String ("SLOT ") + juce::String::charToString ((juce::juce_wchar) kSlotLetters[s]);
            sec.ids.add (pre + "type");
            sec.ids.add (pre + "on");
            sec.ids.add (pre + "filter");
            for (int i = 0; i < slotCount; ++i) sec.ids.add (pre + slotTable[i].id);
            tab.sections.push_back (sec);
        }
        tabs.push_back (tab);
    }

    // --- вкладка FX: по секции на эффект ---
    {
        UiTab tab { "FX", {} };
        juce::StringArray all = detail::fxChoiceIds();
        for (int i = 0; i < fxCount; ++i) all.add (juce::String ("fx_") + fxTable[i].id);

        juce::StringArray titles;
        for (const auto& id : all)
        {
            const auto t = detail::fxSectionOf (id);
            if (! titles.contains (t)) titles.add (t);
        }

        for (const auto& t : titles)
        {
            UiSection sec;
            sec.title = t;
            for (const auto& id : all)
                if (detail::fxSectionOf (id) == t) sec.ids.add (id);
            tab.sections.push_back (sec);
        }
        tabs.push_back (tab);
    }

    // --- вкладка MOD: по секции на маршрут ---
    {
        UiTab tab { "MOD", {} };
        for (int r = 1; r <= kModRoutes; ++r)
        {
            const auto pre = "mod" + juce::String (r) + "_";
            UiSection sec;
            sec.title = "ROUTE " + juce::String (r);
            sec.ids.add (pre + "src");
            sec.ids.add (pre + "dst");
            sec.ids.add (pre + "amt");
            tab.sections.push_back (sec);
        }
        tabs.push_back (tab);
    }

    // --- вкладка ARP / LFO ---
    {
        UiTab tab { "ARP / LFO", {} };
        tab.sections.push_back ({ "ARP", { "arpOn", "arpMode", "arpDiv", "arpOctaves",
                                           "arpGate", "arpSwing", "arpLatch" } });
        for (int l = 1; l <= 2; ++l)
        {
            const auto pre = "lfo" + juce::String (l) + "_";
            tab.sections.push_back ({ "LFO " + juce::String (l),
                                      { pre + "rate", pre + "amount", pre + "dest", pre + "shape" } });
        }
        tabs.push_back (tab);
    }

    // --- вкладка GLOBAL ---
    {
        UiTab tab { "GLOBAL", {} };
        tab.sections.push_back ({ "PATCH", { "gl_gain", "gl_master", "gl_glide", "gl_width",
                                             "gl_morph", "gl_modWheel", "gl_mode" } });
        tabs.push_back (tab);
    }

    return tabs;
}

// Сколько уникальных id в карте (для теста и подписи в интерфейсе).
inline int uiParamCount (const std::vector<UiTab>& tabs)
{
    juce::StringArray all;
    for (const auto& t : tabs)
        for (const auto& s : t.sections)
            for (const auto& id : s.ids)
                if (! all.contains (id)) all.add (id);
    return all.size();
}

} // namespace orogen
