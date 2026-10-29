// Внешний вид OROGEN — монохромный «приборный» стиль по макету GUI:
// почти чёрный фон, тонкие линии 1 px, свечение вокруг кривых и дуг,
// кольцевые ручки с насечками, узкие вертикальные слайдеры.
//
// Отдельный LookAndFeel нужен, чтобы вид не зависел от системной темы хоста
// (в FL/Cubase/Reaper дефолт JUCE выглядит по-разному).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace orogen
{

struct Palette
{
    // Фон и панели
    static inline const juce::Colour bg        { 0xff06070a };   // общий фон окна
    static inline const juce::Colour panel     { 0xff0a0c10 };   // панель/карточка
    static inline const juce::Colour panelAlt  { 0xff0d1015 };   // вложенная панель
    static inline const juce::Colour disc      { 0xff0b0d11 };   // тело ручки

    // Линии
    static inline const juce::Colour line      { 0x1affffff };   // 10 % белого — рамки
    static inline const juce::Colour lineMid   { 0x33ffffff };   // 20 %
    static inline const juce::Colour lineHi    { 0x8cffffff };   // 55 % — активная рамка
    static inline const juce::Colour grid      { 0x0fffffff };   // сетка дисплея
    static inline const juce::Colour tickOff   { 0x24ffffff };   // неактивная насечка

    // Текст и сигнал
    static inline const juce::Colour text      { 0xffe9eef3 };
    static inline const juce::Colour textDim   { 0x8ae9eef3 };    // 54 %
    static inline const juce::Colour textFaint { 0x66ffffff };
    static inline const juce::Colour signal    { 0xffffffff };   // кривые, дуги, заливки
    static inline const juce::Colour hot       { 0xffff6a5c };

    // Цвет слота A–H: только для точки-идентификатора в рейле, чтобы слоты
    // различались, когда включено несколько сразу.
    static juce::Colour slot (int i)
    {
        static const juce::Colour c[8] =
        {
            juce::Colour (0xffeaf2f8), juce::Colour (0xffb9c6d2), juce::Colour (0xff9fb0c0),
            juce::Colour (0xffc8b6a4), juce::Colour (0xffa8b8a4), juce::Colour (0xffbfb6cf),
            juce::Colour (0xffc6aaa8), juce::Colour (0xff9db4bf)
        };
        return c[(size_t) juce::jlimit (0, 7, i)];
    }
};

// Шрифты интерфейса: одна гарнитура, разный размер; заголовки — с разрядкой.
juce::Font orFont (float size, bool bold = false);

// Разрядка (letterspacing) для подписей в стиле макета.
juce::String spaced (const juce::String& s);

class OrLookAndFeel : public juce::LookAndFeel_V4
{
public:
    OrLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrLookAndFeel)
};

// ---------------------------------------------------------------------------
// Общие примитивы рисования: ими пользуются и LookAndFeel, и сам редактор.
// ---------------------------------------------------------------------------
namespace draw
{
    // Линия со свечением: широкая полупрозрачная подложка + тонкая яркая линия.
    void glowStroke (juce::Graphics&, const juce::Path&, float thickness,
                     juce::Colour, float glowAlpha = 0.22f);

    // Рамка панели.
    void panel (juce::Graphics&, juce::Rectangle<float>, float radius = 3.0f,
                juce::Colour fill = Palette::panel, juce::Colour border = Palette::line);

    // Точка-индикатор со свечением.
    void led (juce::Graphics&, juce::Point<float>, float radius, bool on,
              juce::Colour colour = Palette::signal);

    // Подпись с разрядкой.
    void caption (juce::Graphics&, const juce::String&, juce::Rectangle<int>,
                  juce::Colour, float size = 8.5f, juce::Justification = juce::Justification::centredLeft);

    // Иконка типа движка (0…8 — порядок EngineType).
    void engineGlyph (juce::Graphics&, int type, juce::Rectangle<float>, juce::Colour);

    // Иконка эффекта: 0 drive, 1 chorus, 2 delay, 3 reverb, 4 spectre,
    // 5 comb, 6 material, 7 glitch, 8 eq, 9 comp, 10 width, 11 arp.
    void fxGlyph (juce::Graphics&, int kind, juce::Rectangle<float>, juce::Colour);

    // Иконки нижнего ряда кнопок: 0 matrix, 1 fx, 2 arp, 3 lfo.
    void sectionGlyph (juce::Graphics&, int kind, juce::Rectangle<float>, juce::Colour);

    // Кольцевая ручка по макету.
    void ring (juce::Graphics&, juce::Rectangle<float>, float value01,
               float startAngle = -2.36f, float endAngle = 2.36f, bool big = true,
               float hover = 0.0f);
}

} // namespace orogen
