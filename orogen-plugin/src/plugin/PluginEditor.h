// Редактор OROGEN — интерфейс по макету GUI (базовый размер 1280×800):
//
//   ┌ шапка: логотип · полоса Master · индикаторы маршрутов · LOAD/SAVE ┐
//   │ рейл A–H (216) │ дисплей · мод-редактор · ручки · цепь FX (736) │ патч-колонка (264) │
//   └ низ: секции · 5 пар ручек · 4 переключателя · векторный блок ─────┘
//
// Все элементы управления привязаны к реальным параметрам APVTS (см. Params.h) —
// нарисованных «регуляторов без параметра» нет. Кнопка-«матрица» в шапке
// патч-колонки открывает страницу PARAMS со всеми 294 параметрами, чтобы доступ
// к ним не потерялся: макет показывает ~50 самых нужных.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "OrLookAndFeel.h"

namespace orogen
{

class OrogenEditor : public juce::AudioProcessorEditor,
                     private juce::Timer
{
public:
    explicit OrogenEditor (OrogenProcessor&);
    ~OrogenEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    int visiblePage() const;
    juce::Component* paramsOverlay();           // для стендов/тестов: оверлей параметров
    void showPage (int index);                 // 0…7 — слот, 8…11 — секция колонки
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr float kDesignW = 1280.0f;  // система координат макета
    static constexpr float kDesignH = 800.0f;

private:
    struct Bound;            // слайдер + привязка к параметру
    class SlotCard;          // карточка A–H в рейле
    class ParamCard;         // карточка в патч-колонке
    class PatchColumn;       // колонка из 4 карточек (FX / MATRIX / ARP / LFO)
    class ParamsPage;        // полный список параметров (оверлей)
    class IconButton;        // кнопка-иконка секции
    class VectorPad;         // векторный блок справа внизу

    void timerCallback() override;
    void buildRail();
    void buildCentre();
    void buildBottom();
    void buildColumns();
    void buildParamsPage();
    void selectSlot (int slot);
    void setSection (int index);
    void toggleParamsPage (bool shouldShow);
    void loadPresetFromFile();
    void savePresetToFile();

    // координаты макета → пиксели окна
    juce::Rectangle<int> R (float x, float y, float w, float h) const;
    float kx() const { return (float) getWidth()  / kDesignW; }
    float ky() const { return (float) getHeight() / kDesignH; }

    // чтение значений для отрисовки
    float pv (const juce::String& id) const;
    int   pi (const juce::String& id) const;
    juce::String slotId (const char* field) const;
    juce::StringArray itemsOf (const juce::String& id) const;

    // отрисовка
    void paintTopBar (juce::Graphics&);
    void paintRail (juce::Graphics&);
    void paintDisplay (juce::Graphics&);
    void paintModEditor (juce::Graphics&);
    void paintCentreKnobs (juce::Graphics&);
    void paintChips (juce::Graphics&);
    void paintColumnFrame (juce::Graphics&);
    void paintBottom (juce::Graphics&);
    void paintVectorPad (juce::Graphics&);

    OrogenProcessor& proc;
    std::unique_ptr<OrLookAndFeel> lnf;

    int selectedSlot = 0;
    int section = 0;                 // 0 FX · 1 MATRIX · 2 ARP · 3 LFO
    bool paramsOpen = false;

    juce::Slider masterSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAtt;
    juce::TextButton loadButton { "LOAD" }, saveButton { "SAVE" };

    // выбор движка и фильтра выбранного слота (sA_type … sH_type / sA_filter …)
    juce::ComboBox engineBox, filterBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> engineAtt, filterAtt;

    juce::OwnedArray<SlotCard> cards;
    juce::OwnedArray<Bound> centreKnobs, centreBars;    // 3 + 3, зависят от слота
    juce::OwnedArray<Bound> bottomKnobs, bottomBars;    // 5 + 5, глобальные
    juce::OwnedArray<PatchColumn> columns;              // 4 секции
    juce::OwnedArray<IconButton> sectionButtons;        // 4 кнопки слева внизу
    juce::OwnedArray<juce::ToggleButton> switchButtons; // 4 переключателя
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> switchAtts;
    std::unique_ptr<VectorPad> vectorPad;
    std::unique_ptr<ParamsPage> paramsPage;

    std::array<float, 8> slotLevels { };
    float masterLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrogenEditor)
};

} // namespace orogen
