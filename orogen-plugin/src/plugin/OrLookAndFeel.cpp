#include "OrLookAndFeel.h"

#include <cmath>

namespace orogen
{

juce::Font orFont (float size, bool bold)
{
    juce::FontOptions o (size);
    if (bold) o = o.withStyle ("Bold");
    return juce::Font (o);
}

juce::String spaced (const juce::String& s)
{
    juce::String r;
    for (int i = 0; i < s.length(); ++i)
    {
        r << s[i];
        if (i + 1 < s.length() && s[i] != ' ') r << " ";
    }
    return r;
}

//============================================================================
namespace draw
{

void glowStroke (juce::Graphics& g, const juce::Path& p, float thickness,
                 juce::Colour c, float glowAlpha)
{
    if (thickness <= 0.0f) thickness = 1.0f;

    juce::PathStrokeType wide (thickness + 3.0f, juce::PathStrokeType::curved,
                              juce::PathStrokeType::rounded);
    juce::PathStrokeType mid (thickness + 1.4f, juce::PathStrokeType::curved,
                              juce::PathStrokeType::rounded);
    juce::PathStrokeType thin (thickness, juce::PathStrokeType::curved,
                               juce::PathStrokeType::rounded);

    g.setColour (c.withMultipliedAlpha (glowAlpha * 0.45f));
    g.strokePath (p, wide);
    g.setColour (c.withMultipliedAlpha (glowAlpha));
    g.strokePath (p, mid);
    g.setColour (c);
    g.strokePath (p, thin);
}

void panel (juce::Graphics& g, juce::Rectangle<float> r, float radius,
            juce::Colour fill, juce::Colour border)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

void led (juce::Graphics& g, juce::Point<float> c, float radius, bool on, juce::Colour colour)
{
    if (on)
    {
        g.setColour (colour.withAlpha (0.10f));
        g.fillEllipse (juce::Rectangle<float> (radius * 6.0f, radius * 6.0f).withCentre (c));
        g.setColour (colour.withAlpha (0.22f));
        g.fillEllipse (juce::Rectangle<float> (radius * 3.2f, radius * 3.2f).withCentre (c));
        g.setColour (colour);
    }
    else
    {
        g.setColour (Palette::lineMid);
    }
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));

    if (! on)
    {
        g.setColour (Palette::bg.withAlpha (0.6f));
        g.fillEllipse (juce::Rectangle<float> (radius * 1.0f, radius * 1.0f).withCentre (c));
    }
}

void caption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
              juce::Colour colour, float size, juce::Justification just)
{
    g.setColour (colour);
    g.setFont (orFont (size));
    g.drawText (spaced (text.toUpperCase()), area, just, false);
}

// --- иконки движков ---------------------------------------------------------
void engineGlyph (juce::Graphics& g, int type, juce::Rectangle<float> r, juce::Colour c)
{
    const auto cx = r.getCentreX(), cy = r.getCentreY();
    const auto w = r.getWidth(), h = r.getHeight();
    juce::Path p;
    juce::PathStrokeType st (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    switch (type)
    {
        case 0:   // Wavetable — синус с «полками»
        default:
            for (int i = 0; i <= 24; ++i)
            {
                const float x = r.getX() + w * (float) i / 24.0f;
                const float y = cy - h * 0.34f * std::sin ((float) i / 24.0f * juce::MathConstants<float>::twoPi);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            break;

        case 1:   // FM — две частоты
            for (int i = 0; i <= 40; ++i)
            {
                const float t = (float) i / 40.0f;
                const float x = r.getX() + w * t;
                const float y = cy - h * 0.30f * std::sin (t * juce::MathConstants<float>::twoPi * 3.0f
                                                            * (1.0f + 1.6f * std::sin (t * juce::MathConstants<float>::twoPi)));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            break;

        case 2:   // Harmonic — спектральные столбики
            for (int i = 0; i < 7; ++i)
            {
                const float x = r.getX() + w * (0.08f + 0.14f * (float) i);
                const float hh = h * (0.15f + 0.6f * (float) ((i * 37) % 11) / 11.0f);
                p.startNewSubPath (x, cy + h * 0.4f);
                p.lineTo (x, cy + h * 0.4f - hh);
            }
            break;

        case 3:   // Sample — «рваная» волна
            p.startNewSubPath (r.getX() + 1.0f, cy);
            p.lineTo (r.getX() + w * 0.18f, cy - h * 0.30f);
            p.lineTo (r.getX() + w * 0.30f, cy + h * 0.18f);
            p.lineTo (r.getX() + w * 0.45f, cy - h * 0.12f);
            p.lineTo (r.getX() + w * 0.62f, cy + h * 0.30f);
            p.lineTo (r.getX() + w * 0.78f, cy - h * 0.26f);
            p.lineTo (r.getRight() - 1.0f, cy + h * 0.06f);
            break;

        case 4:   // Granular — сетка точек
            for (int i = 0; i < 16; ++i)
            {
                const float x = r.getX() + w * (0.15f + 0.23f * (float) (i % 4));
                const float y = r.getY() + h * (0.20f + 0.20f * (float) (i / 4));
                const float rr = (i % 3 == 0) ? 1.9f : 1.0f;
                g.setColour (c.withAlpha (i % 3 == 0 ? 0.95f : 0.45f));
                g.fillEllipse (x - rr, y - rr, rr * 2.0f, rr * 2.0f);
            }
            return;

        case 5:   // Modal — концентрические окружности
            for (int i = 0; i < 3; ++i)
                p.addEllipse (juce::Rectangle<float> (w * (0.16f + 0.12f * (float) i),
                                                      h * (0.16f + 0.12f * (float) i),
                                                      w * (0.68f - 0.24f * (float) i),
                                                      h * (0.68f - 0.24f * (float) i)).withCentre ({ cx, cy }));
            break;

        case 6:   // Resynth — резонансные пики
            p.startNewSubPath (r.getX() + 1.0f, cy + h * 0.34f);
            p.lineTo (r.getX() + w * 0.22f, cy + h * 0.34f);
            p.lineTo (r.getX() + w * 0.30f, cy - h * 0.36f);
            p.lineTo (r.getX() + w * 0.38f, cy + h * 0.34f);
            p.lineTo (r.getX() + w * 0.58f, cy + h * 0.34f);
            p.lineTo (r.getX() + w * 0.66f, cy - h * 0.20f);
            p.lineTo (r.getX() + w * 0.74f, cy + h * 0.34f);
            p.lineTo (r.getRight() - 1.0f, cy + h * 0.34f);
            break;

        case 7:   // Organics — «жидкие» волны
            for (int i = 0; i < 3; ++i)
            {
                const float off = h * (-0.22f + 0.22f * (float) i);
                for (int k = 0; k <= 20; ++k)
                {
                    const float t = (float) k / 20.0f;
                    const float x = r.getX() + w * t;
                    const float y = cy + off + h * 0.10f * std::sin (t * juce::MathConstants<float>::twoPi * 2.0f + (float) i);
                    if (k == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
                }
            }
            break;
    }

    g.setColour (c);
    g.strokePath (p, st);
}

// --- иконки эффектов --------------------------------------------------------
void fxGlyph (juce::Graphics& g, int kind, juce::Rectangle<float> r, juce::Colour c)
{
    const auto cx = r.getCentreX(), cy = r.getCentreY();
    const auto w = r.getWidth(), h = r.getHeight();
    juce::Path p;
    juce::PathStrokeType st (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    switch (kind)
    {
        case 0:   // drive — срезанная волна
            for (int i = 0; i <= 32; ++i)
            {
                const float t = (float) i / 32.0f;
                const float x = r.getX() + w * t;
                const float y = cy - h * 0.30f * std::tanh (3.2f * std::sin (t * juce::MathConstants<float>::twoPi));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            break;

        case 1:   // chorus — шевроны
            for (int i = 0; i < 3; ++i)
            {
                const float x = r.getX() + w * (0.20f + 0.28f * (float) i);
                p.startNewSubPath (x - w * 0.10f, cy - h * 0.22f);
                p.lineTo (x, cy);
                p.lineTo (x - w * 0.10f, cy + h * 0.22f);
            }
            break;

        case 2:   // delay — серия затухающих импульсов
            for (int i = 0; i < 4; ++i)
            {
                const float x = r.getX() + w * (0.12f + 0.24f * (float) i);
                const float hh = h * (0.34f - 0.07f * (float) i);
                p.startNewSubPath (x, cy + h * 0.34f);
                p.lineTo (x, cy + h * 0.34f - hh * 2.0f);
            }
            break;

        case 3:   // reverb — расходящиеся дуги
            for (int i = 0; i < 3; ++i)
                p.addArc (cx - w * (0.14f + 0.11f * (float) i), cy - h * (0.14f + 0.11f * (float) i),
                          w * (0.28f + 0.22f * (float) i), h * (0.28f + 0.22f * (float) i),
                          -0.9f, 1.8f, true);
            break;

        case 4:   // spectre — столбики спектра
            for (int i = 0; i < 8; ++i)
            {
                const float x = r.getX() + w * (0.08f + 0.12f * (float) i);
                const float hh = h * (0.12f + 0.62f * (float) ((i * 53) % 9) / 9.0f);
                p.startNewSubPath (x, cy + h * 0.36f);
                p.lineTo (x, cy + h * 0.36f - hh);
            }
            break;

        case 5:   // comb — гребёнка
            for (int i = 0; i < 6; ++i)
            {
                const float x = r.getX() + w * (0.10f + 0.16f * (float) i);
                p.startNewSubPath (x, cy - h * 0.30f);
                p.lineTo (x, cy + h * 0.30f);
            }
            break;

        case 6:   // material — эллипсы-резонаторы
            for (int i = 0; i < 3; ++i)
                p.addEllipse (juce::Rectangle<float> (w * 0.20f, h * (0.10f + 0.10f * (float) i),
                                                      w * 0.60f, h * (0.30f - 0.08f * (float) i)).withCentre ({ cx, cy }));
            break;

        case 7:   // glitch — ступеньки
            p.startNewSubPath (r.getX() + 1.0f, cy + h * 0.28f);
            for (int i = 0; i < 5; ++i)
            {
                const float x = r.getX() + w * (0.12f + 0.19f * (float) i);
                const float y = cy + ((i % 2) ? h * 0.28f : -h * 0.28f);
                p.lineTo (x, y);
                p.lineTo (x + w * 0.19f, y);
            }
            break;

        case 8:   // eq — две полки
            p.startNewSubPath (r.getX() + 1.0f, cy + h * 0.20f);
            p.lineTo (r.getX() + w * 0.35f, cy + h * 0.20f);
            p.lineTo (r.getX() + w * 0.55f, cy - h * 0.24f);
            p.lineTo (r.getRight() - 1.0f, cy - h * 0.24f);
            break;

        case 9:   // comp — сужение
            p.startNewSubPath (r.getX() + 1.0f, cy - h * 0.32f);
            p.lineTo (cx + w * 0.10f, cy);
            p.lineTo (r.getRight() - 1.0f, cy - h * 0.32f);
            p.startNewSubPath (r.getX() + 1.0f, cy + h * 0.32f);
            p.lineTo (cx + w * 0.10f, cy);
            p.lineTo (r.getRight() - 1.0f, cy + h * 0.32f);
            break;

        case 10:  // width — две расходящиеся стрелки
            p.startNewSubPath (cx, cy);
            p.lineTo (r.getX() + 2.0f, cy);
            p.startNewSubPath (cx, cy);
            p.lineTo (r.getRight() - 2.0f, cy);
            p.startNewSubPath (r.getX() + 2.0f, cy - h * 0.18f);
            p.lineTo (r.getX() + 2.0f, cy + h * 0.18f);
            p.startNewSubPath (r.getRight() - 2.0f, cy - h * 0.18f);
            p.lineTo (r.getRight() - 2.0f, cy + h * 0.18f);
            break;

        default:  // arp — лесенка
            p.startNewSubPath (r.getX() + 1.0f, cy + h * 0.30f);
            p.lineTo (r.getX() + w * 0.30f, cy + h * 0.30f);
            p.lineTo (r.getX() + w * 0.30f, cy);
            p.lineTo (r.getX() + w * 0.60f, cy);
            p.lineTo (r.getX() + w * 0.60f, cy - h * 0.30f);
            p.lineTo (r.getRight() - 1.0f, cy - h * 0.30f);
            break;
    }

    g.setColour (c);
    g.strokePath (p, st);
}

// --- иконки секций нижнего ряда --------------------------------------------
void sectionGlyph (juce::Graphics& g, int kind, juce::Rectangle<float> r, juce::Colour c)
{
    juce::Path p;
    juce::PathStrokeType st (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    const auto cy = r.getCentreY();
    const auto w = r.getWidth(), h = r.getHeight();

    switch (kind)
    {
        case 0:   // матрица: 4 точки и связи
            for (int i = 0; i < 4; ++i)
            {
                const float x = r.getX() + w * (0.22f + 0.28f * (float) (i % 2));
                const float y = r.getY() + h * (0.26f + 0.30f * (float) (i / 2));
                g.setColour (c.withAlpha (i == 1 ? 0.95f : 0.45f));
                g.fillEllipse (x - 1.7f, y - 1.7f, 3.4f, 3.4f);
            }
            p.startNewSubPath (r.getX() + w * 0.22f, r.getY() + h * 0.26f);
            p.lineTo (r.getX() + w * 0.78f, r.getY() + h * 0.56f);
            break;

        case 1:   // fx: волна с изломом
            for (int i = 0; i <= 20; ++i)
            {
                const float t = (float) i / 20.0f;
                const float x = r.getX() + w * t;
                const float y = cy - h * 0.30f * std::tanh (2.6f * std::sin (t * juce::MathConstants<float>::twoPi));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            break;

        case 2:   // arp: ступеньки
            p.startNewSubPath (r.getX() + 1.0f, cy + h * 0.28f);
            p.lineTo (r.getX() + w * 0.33f, cy + h * 0.28f);
            p.lineTo (r.getX() + w * 0.33f, cy - h * 0.04f);
            p.lineTo (r.getX() + w * 0.66f, cy - h * 0.04f);
            p.lineTo (r.getX() + w * 0.66f, cy - h * 0.30f);
            p.lineTo (r.getRight() - 1.0f, cy - h * 0.30f);
            break;

        default:  // lfo: синус
            for (int i = 0; i <= 20; ++i)
            {
                const float t = (float) i / 20.0f;
                const float x = r.getX() + w * t;
                const float y = cy - h * 0.30f * std::sin (t * juce::MathConstants<float>::twoPi);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            break;
    }

    g.setColour (c);
    g.strokePath (p, st);
}

// --- кольцевая ручка --------------------------------------------------------
void ring (juce::Graphics& g, juce::Rectangle<float> area, float value01,
           float startAngle, float endAngle, bool big, float hover)
{
    value01 = juce::jlimit (0.0f, 1.0f, value01);

    const auto d = juce::jmin (area.getWidth(), area.getHeight());
    const auto c = area.getCentre();
    const float rOuter = d * 0.5f;
    const float rDisc  = rOuter - (big ? 9.0f : 6.0f);
    const float rArc   = rOuter - (big ? 4.0f : 3.0f);

    // насечки по кольцу
    const int nTicks = big ? 44 : 30;
    const float angle = startAngle + (endAngle - startAngle) * value01;
    juce::Path ticks;
    for (int i = 0; i < nTicks; ++i)
    {
        const float t = (float) i / (float) (nTicks - 1);
        const float a = startAngle + (endAngle - startAngle) * t;
        const float r0 = rArc - (big ? 5.0f : 3.5f);
        const float r1 = rArc;
        ticks.startNewSubPath (c.x + std::sin (a) * r0, c.y - std::cos (a) * r0);
        ticks.lineTo (c.x + std::sin (a) * r1, c.y - std::cos (a) * r1);
    }
    // hover: насечки светлее на 0…1 — видно, за какую ручку взялся курсор
    g.setColour (hover > 0.001f
                     ? Palette::tickOff.withMultipliedAlpha (1.0f)
                           .interpolatedWith (Palette::textDim, juce::jlimit (0.0f, 0.55f, hover * 0.55f))
                     : Palette::tickOff);
    g.strokePath (ticks, juce::PathStrokeType (big ? 1.6f : 1.2f));

    // подсвеченные насечки до текущего значения
    if (value01 > 0.001f)
    {
        juce::Path lit;
        for (int i = 0; i < nTicks; ++i)
        {
            const float t = (float) i / (float) (nTicks - 1);
            if (t > value01) break;
            const float a = startAngle + (endAngle - startAngle) * t;
            const float r0 = rArc - (big ? 5.0f : 3.5f);
            lit.startNewSubPath (c.x + std::sin (a) * r0, c.y - std::cos (a) * r0);
            lit.lineTo (c.x + std::sin (a) * rArc, c.y - std::cos (a) * rArc);
        }
        g.setColour (Palette::signal.withAlpha (0.92f));
        g.strokePath (lit, juce::PathStrokeType (big ? 1.6f : 1.2f));
    }

    // Тонкое белое кольцо видно даже при значении 100 %, когда активные
    // насечки перекрывают все неактивные. Не меняет геометрию ручки.
    if (hover > 0.001f)
    {
        const auto halo = juce::Rectangle<float> (d - 1.0f, d - 1.0f).withCentre (c);
        g.setColour (Palette::signal.withAlpha (0.28f * juce::jlimit (0.0f, 1.0f, hover)));
        g.drawEllipse (halo, 1.0f);
    }

    // тело ручки
    g.setColour (Palette::disc);
    g.fillEllipse (juce::Rectangle<float> (rDisc * 2.0f, rDisc * 2.0f).withCentre (c));
    g.setColour (Palette::lineMid);
    g.drawEllipse (juce::Rectangle<float> (rDisc * 2.0f, rDisc * 2.0f).withCentre (c), 1.0f);

    // заливка сектора (как на макете: тёмный клин в теле ручки)
    if (value01 > 0.02f)
    {
        juce::Path sector;
        sector.addPieSegment (juce::Rectangle<float> (rDisc * 2.0f, rDisc * 2.0f).withCentre (c),
                              startAngle, angle, 0.0f);
        g.setColour (Palette::signal.withAlpha (0.10f));
        g.fillPath (sector);
    }

    // дуга значения со свечением
    if (value01 > 0.004f)
    {
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, rArc, rArc, 0.0f, startAngle, angle, true);
        glowStroke (g, arc, big ? 2.2f : 1.6f, Palette::signal.withAlpha (0.92f), 0.20f);
    }

    // указатель
    const float rp0 = rDisc * 0.30f, rp1 = rDisc * 0.90f;
    juce::Path ptr;
    ptr.startNewSubPath (c.x + std::sin (angle) * rp0, c.y - std::cos (angle) * rp0);
    ptr.lineTo (c.x + std::sin (angle) * rp1, c.y - std::cos (angle) * rp1);
    g.setColour (Palette::signal);
    g.strokePath (ptr, juce::PathStrokeType (big ? 2.0f : 1.5f,
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
}

} // namespace draw

//============================================================================
OrLookAndFeel::OrLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::bg);
    setColour (juce::PopupMenu::backgroundColourId, Palette::panelAlt);
    setColour (juce::PopupMenu::textColourId, Palette::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::lineMid);
    setColour (juce::PopupMenu::highlightedTextColourId, Palette::text);
    setColour (juce::ComboBox::textColourId, Palette::text);
    setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, Palette::textDim);
    setColour (juce::Slider::textBoxTextColourId, Palette::textDim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Palette::text);
    setColour (juce::TextButton::textColourOffId, Palette::textDim);
    setColour (juce::TextButton::textColourOnId, Palette::text);
    setColour (juce::ToggleButton::textColourId, Palette::textDim);
    setColour (juce::ToggleButton::tickColourId, Palette::signal);
}

void OrLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                      juce::Slider& s)
{
    juce::ignoreUnused (rotaryStartAngle, rotaryEndAngle);
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const bool big = s.getProperties().getWithDefault ("big", false);
    const float hover = (float) (double) s.getProperties().getWithDefault ("hover", 0.0);
    draw::ring (g, area, sliderPos, -2.36f, 2.36f, big, hover);
}

void OrLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPos, float minSliderPos, float maxSliderPos,
                                      juce::Slider::SliderStyle style, juce::Slider& s)
{
    juce::ignoreUnused (minSliderPos, maxSliderPos, s);
    const bool vertical = (style == juce::Slider::LinearVertical
                        || style == juce::Slider::LinearBarVertical);
    const float thick = vertical ? 3.0f : 3.0f;

    if (vertical)
    {
        const float cx = (float) x + (float) width * 0.5f;
        const float top = (float) y + 2.0f, bot = (float) y + (float) height - 2.0f;
        juce::Rectangle<float> track (cx - thick * 0.5f, top, thick, bot - top);
        g.setColour (Palette::lineMid);
        g.fillRoundedRectangle (track, thick * 0.5f);

        juce::Rectangle<float> fill (track.getX(), juce::jlimit (top, bot, sliderPos),
                                     thick, bot - juce::jlimit (top, bot, sliderPos));
        g.setColour (Palette::signal.withAlpha (0.88f));
        g.fillRoundedRectangle (fill, thick * 0.5f);

        // «утолщение» на позиции — как маленький ползунок
        g.setColour (Palette::signal);
        g.fillRoundedRectangle (juce::Rectangle<float> (cx - 5.0f, sliderPos - 1.5f, 10.0f, 3.0f), 1.5f);
    }
    else
    {
        const float cy = (float) y + (float) height * 0.5f;
        const float l = (float) x + 2.0f, r = (float) x + (float) width - 2.0f;
        juce::Rectangle<float> track (l, cy - thick * 0.5f, r - l, thick);
        g.setColour (Palette::lineMid);
        g.fillRoundedRectangle (track, thick * 0.5f);

        juce::Rectangle<float> fill (l, cy - thick * 0.5f,
                                     juce::jlimit (l, r, sliderPos) - l, thick);
        g.setColour (Palette::signal.withAlpha (0.88f));
        g.fillRoundedRectangle (fill, thick * 0.5f);

        g.setColour (Palette::signal);
        g.fillRoundedRectangle (juce::Rectangle<float> (sliderPos - 1.5f, cy - 5.0f, 3.0f, 10.0f), 1.5f);
    }
}

void OrLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                  int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f);
    draw::panel (g, r, 2.5f, Palette::panelAlt,
                 box.isMouseOver (false) ? Palette::lineMid : Palette::line);

    juce::Path arrow;
    const float cx = (float) width - 9.0f, cy = (float) height * 0.5f;
    arrow.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.0f);
    g.setColour (Palette::textDim);
    g.fillPath (arrow);
}

void OrLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                          const juce::Colour&, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto border = Palette::line;
    if (b.getToggleState())            border = Palette::lineHi;
    if (highlighted || down)           border = Palette::lineHi;

    draw::panel (g, r, 2.5f, down ? Palette::panelAlt : juce::Colours::transparentBlack, border);
}

void OrLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                                      bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    draw::panel (g, r, 2.5f, Palette::panelAlt, (highlighted || down) ? Palette::lineHi : Palette::line);
    draw::led (g, { r.getX() + 9.0f, r.getCentreY() }, 2.6f, b.getToggleState());

    g.setColour (b.getToggleState() ? Palette::text : Palette::textDim);
    g.setFont (orFont (8.5f));
    g.drawText (spaced (b.getButtonText().toUpperCase()),
                r.withTrimmedLeft (18.0f).toNearestInt(),
                juce::Justification::centredLeft, false);
}

juce::Font OrLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return orFont (9.5f);
}

juce::Font OrLookAndFeel::getLabelFont (juce::Label&)
{
    return orFont (9.5f);
}

juce::Font OrLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return orFont (9.0f);
}

juce::Font OrLookAndFeel::getPopupMenuFont()
{
    return orFont (10.0f);
}

} // namespace orogen
