// Редактор OROGEN — интерфейс по макету GUI (базовый размер 1280×800):
//
//   ┌ шапка: логотип · полоса Master · индикаторы маршрутов · LOAD/SAVE ┐
//   │ рейл A–H (216) │ дисплей · мод-редактор · ручки · цепь FX (736) │ патч-колонка (264) │
//   └ низ: секции · 5 пар ручек · 4 переключателя · векторный блок ─────┘
//
// Все элементы управления привязаны к реальным параметрам APVTS (см. Params.h) —
// нарисованных «регуляторов без параметра» нет. Кнопка-«матрица» в шапке
// патч-колонки открывает страницу PARAMS со всеми 306 параметрами, чтобы доступ
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
    void showPage (int index);                 // 0…7 — слот, 8…11 — секция колонки,
                                               // 100+t — вкладка оверлея, 200+t*100+s — секция
    int paramsTabCount() const;
    int paramsSectionCount() const;
    juce::String paramsSectionName (int index) const;
    juce::StringArray mainScreenParamIds() const;   // что привязано к карточкам главного экрана
    juce::String selectedMacroLabel (int macro) const; // названия P1…P4 выбранного движка
    juce::String selectedSourceLabel() const;           // фактически выбранный фабричный источник
    int fxColumnPageCount() const;                  // страниц в FX-колонке (для теста)
    void nextFxColumnPage();
    int  columnPageCount (int column) const;        // страниц в любой из 4 колонок
    void nextColumnPage (int column);               // листнуть колонку по кругу
    juce::StringArray allColumnIds (int column) const;   // все параметры колонки, со всех страниц
    int  columnPageShift (int column) const;             // текущий сдвиг при плавном листании
    bool columnTransitioning (int column) const;
    void hoverKnob (int index, bool on);                 // hover-подсветка центральных ручек
    double knobHover (int index) const;

    // --- главный экран: интерактивные зоны (чипы FX, дорожки мод-редактора) ---
    // Тот же путь, что и у мыши: mouseDown/mouseDrag/mouseUp вызывают эти методы,
    // поэтому тест дёргает ровно ту логику, что и пользователь.
    enum class MainHit { none, fxChip, fxChipPage, display, modNode, envAttack, envDecay, envSustain, lfoLane };
    MainHit hitMain (juce::Point<float> designPos, int& index) const;
    bool clickMain (juce::Point<float> designPos);      // клик: переключение чипа FX
    MainHit beginMainDrag (juce::Point<float> designPos);   // нажатие мыши
    bool dragMain  (juce::Point<float> designPos);      // перетаскивание узла/чипа
    void releaseMain();
    int  mainScreenHitCount() const;                    // сколько целей на экране (для теста)
    int  chipPageCount() const;                         // страниц чипов FX
    int  chipCount() const;                             // всего чипов (эффекты с ручкой доли)
    float chipPageTransition() const { return chipTransition; }
    juce::String chipParamId (int index) const;         // параметр чипа по порядку

    // --- анимации ---
    // Фаза — честное время с момента создания редактора, в секундах. Тест и
    // инструмент render_ui могут выставить её вручную, поэтому кадр
    // воспроизводим: одна и та же фаза даёт один и тот же пиксель.
    float animationPhase() const;                       // 0…1 (период 2 с)
    void  setAnimationPhase (double seconds);
    bool  animationsEnabled() const { return animOn; }
    void  setAnimationsEnabled (bool on);
    void  repaintAnimated();          // перерисовать только «живые» зоны
    // Превью: C4, настоящий DSP-голос и фильтры, до мастер-FX. В Stack —
    // выбранный слот; в Morph/Split — фактические слоты и диапазоны нот.
    // Рендер происходит в отдельном потоке; эти методы ожидания — для стендов.
    void  rebuildPreview (bool force = false);
    bool  waitForPreview (int timeoutMs = 10000);
    bool  isPreviewPending() const { return prevPending; }
    double lastPreviewRenderMs() const { return prevRenderMs; }
    const std::vector<float>& previewWave() const { return prevWave; }
    const juce::Path& previewPath() const { return prevPath; }
    int   animatedAreaPercent() const;// сколько площади экрана реально двигается

    // --- банк программ (был только в API процессора) ---
    bool saveBankTo (const juce::File& f);              // весь банк в JSON
    bool loadBankFrom (const juce::File& f);            // банк из JSON + обновление списка
    int  addProgramFromCurrent (const juce::String& name);   // текущий патч в конец банка
    void refreshProgramList();                          // перечитать банк в шапке
    int  programItemCount() const;                      // пунктов в селекторе (для теста)
    // Быстрый банк: фиксированный файл в каталоге приложения, без диалога —
    // чтобы сохранить/загрузить весь набор можно было одним пунктом меню.
    juce::File quickBankFile() const;
    bool saveBankQuick();
    bool loadBankQuick();
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
    class PreviewWorker;     // владеет DSP-голосом вне message/audio thread

    void timerCallback() override;
    void syncLfoPreviewCycles (int cycles);
    void repaintChangedValues();
    void takePreviewResult();
    void buildRail();
    void buildCentre();
    void buildBottom();
    void buildColumns();
    void buildParamsPage();
    void selectSlot (int slot);
    void refreshEngineContext();
    void setSection (int index);
    void toggleParamsPage (bool shouldShow);
    void showBankMenu();
    void showIoMenu (bool saving);      // меню кнопок LOAD / SAVE
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

    // геометрия интерактивных зон в координатах макета
    juce::Rectangle<float> chipRect (int i) const;
    juce::Rectangle<float> modLane (int lane) const;    // 0 матрица · 1 LFO · 2 конверт
    void envNodePoints (juce::Point<float>& aEnd, juce::Point<float>& dEnd,
                        juce::Point<float>& sEnd, float& ySus) const;
    bool nudgeParam (const juce::String& id, float deltaNorm);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

    OrogenProcessor& proc;
    std::unique_ptr<OrLookAndFeel> lnf;

    int selectedSlot = 0;
    int contextSlot = -1, contextEngine = -1;
    int section = 0;                 // 0 FX · 1 MATRIX · 2 ARP · 3 LFO
    bool paramsOpen = false;

    juce::Slider masterSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAtt;
    juce::TextButton loadButton { "LOAD" }, saveButton { "SAVE" };

    // Банк программ: в шапке был только нарисованный текст имени, теперь это
    // настоящий селектор из getNumPrograms() пунктов (host-программы OROGEN).
    juce::ComboBox programBox;

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

    // состояние перетаскивания на главном экране
    MainHit dragTarget = MainHit::none;
    int dragIndex = -1;
    juce::Point<float> dragLast;
    bool dragMoved = false;
    // последняя ненулевая величина каждого чипа FX (для включения по клику)
    std::array<float, 10> chipLast { 0.45f, 0.50f, 0.40f, 0.35f, 0.45f,
                                     0.50f, 0.45f, 0.40f, 0.45f, 0.50f };
    int chipPage = 0;
    float chipTransition = 0.0f;

    // Превью с независимым DSP и коалесцированием частых изменений.
    std::unique_ptr<PreviewWorker> previewWorker;
    std::vector<float> prevWave;
    juce::Path prevPath;
    std::uint64_t prevHash = 0, prevGeneration = 0;
    bool prevHashValid = false, prevPending = false;
    double prevStampMs = 0.0, prevRenderMs = 0.0;

    double animSeconds = 0.0;         // время анимации, с
    double lfoPreviewPhase = 0.0;     // координата playhead на дорожке, 0…1
    double lfoPreviewOffset = 0.0;    // фаза самой кривой при изменении числа циклов
    int lfoPreviewCycleCount = 0;
    double lastTimerMs = 0.0;
    bool   animOn = true;
    bool visualHashesValid = false;
    std::array<std::uint64_t, 8> railVisualHashes {};
    std::uint64_t laneVisualHash = 0, chipVisualHash = 0, padVisualHash = 0, displayVisualHash = 0;
    std::array<float, 8> slotLevelsSmooth { };
    std::uint32_t lastMeterRevision = 0;
    double meterUnchangedSeconds = 0.0;
    bool meterRevisionSeen = false;
    float animPhaseFor (double speed, double offset = 0.0) const;   // 0…1

    std::array<float, 8> slotLevels { };
    float masterLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrogenEditor)
};

} // namespace orogen
