// Редактор OROGEN — отрисовка по макету GUI.
//
// Принцип: геометрия задаётся в координатах макета 1280×800 и умножается на
// масштаб окна (kx/ky). Поэтому окно можно тянуть, а все пропорции остаются
// макетными. Всё, что показывает значения (дуги ручек, кривые дисплея, уровень
// в рейле), берётся из реальных параметров APVTS и из EngineBank.
#include "PluginEditor.h"

#include "UiParamMap.h"
#include "content/wavetable_bank.h"

#include <cmath>
#include <functional>

namespace orogen
{

// ---------------------------------------------------------------------------
// Чипы эффектов на главном экране: все 10 эффектов с ручкой доли, по 5 на
// страницу. Раньше чипов было ровно пять — остальные эффекты жили только в
// колонке FX CHAIN и в оверлее.
// ---------------------------------------------------------------------------
namespace
{
    // Все эффекты, у которых есть ручка доли (mix): 10 штук, по 5 на страницу.
    // EQ, DYNAMICS и WIDTH доли не имеют — они всегда в цепи и правятся в
    // колонке FX CHAIN, поэтому чипов у них нет.
    struct ChipDef { const char* name; int glyph; const char* id; float onValue; };
    const ChipDef kChips[] =
    {
        { "DRIVE",   0, "fx_drive",       0.45f },
        { "CHORUS",  1, "fx_chorusMix",   0.50f },
        { "DELAY",   2, "fx_delayMix",    0.40f },
        { "REVERB",  3, "fx_reverbMix",   0.35f },
        { "PLATE",   3, "fx_plateMix",    0.40f },
        { "MATERIAL",4, "fx_materialMix", 0.45f },
        { "SPECTRE", 5, "fx_spectreMix",  0.50f },
        { "GLITCH",  6, "fx_glitchMix",   0.40f },
        { "ROBIN",   7, "fx_robinMix",    0.45f },
        { "COMB",    8, "fx_combMix",     0.45f }
    };
    constexpr int kChipTotal = (int) (sizeof (kChips) / sizeof (kChips[0]));
    constexpr int kChipsPerPage = 5;

    juce::Rectangle<float> chipPagerRect() { return { 886.0f, 578.0f, 78.0f, 16.0f }; }
    // сам дисплей с волной: раньше только показывал, теперь и тянется
    juce::Rectangle<float> displayRect() { return { 248.0f, 66.0f, 736.0f, 214.0f }; }
}

// ---------------------------------------------------------------------------
// Кэш вейвтейблов: банк строится один раз (≈70 мс) и переиспользуется всеми
// экземплярами редактора. Дисплей рисует настоящую форму волны выбранной
// таблицы, а не «декоративную синусоиду».
// ---------------------------------------------------------------------------
namespace
{
    struct WaveStore
    {
        WavetableBank bank;
        bool ready = false;

        const WavetableBank& get()
        {
            if (! ready) { bank.buildFactory (6); ready = true; }
            return bank;
        }
    };

    WaveStore& waveStore()
    {
        static WaveStore s;
        return s;
    }
}

//============================================================================
// Bound — слайдер с привязкой к параметру (привязку можно перевесить: у
// центральных ручек параметры зависят от выбранного слота A–H).
//============================================================================
struct OrogenEditor::Bound
{
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;

    void setup (juce::Slider::SliderStyle style, bool big)
    {
        slider.setSliderStyle (style);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.getProperties().set ("big", big);
        slider.setScrollWheelEnabled (true);
        slider.setVelocityBasedMode (false);
    }

    void bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        att.reset();
        slider.getProperties().set ("paramId", id);
        if (apvts.getParameter (id) != nullptr)
            att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, slider);
    }
};

//============================================================================
// Карточка слота в левом рейле A–H.
//============================================================================
class OrogenEditor::SlotCard : public juce::Component
{
public:
    SlotCard (OrogenProcessor& p, int index, std::function<void (int)> onSelect)
        : idx (index), selectFn (std::move (onSelect))
    {
        const auto pre = slotPrefix (index);

        onToggle.setButtonText ("");
        addAndMakeVisible (onToggle);
        onAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            p.apvts, pre + "on", onToggle);

        level.setSliderStyle (juce::Slider::LinearHorizontal);
        level.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        tune.setSliderStyle (juce::Slider::LinearHorizontal);
        tune.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        addAndMakeVisible (level);
        addAndMakeVisible (tune);

        levelAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            p.apvts, pre + "level", level);
        tuneAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            p.apvts, pre + "tune", tune);
    }

    void mouseDown (const juce::MouseEvent&) override { if (selectFn) selectFn (idx); }

    void resized() override
    {
        const float s = (float) getWidth() / 216.0f;
        auto sc = [s] (float v) { return juce::roundToInt (v * s); };

        onToggle.setBounds (sc (10.0f), sc (38.0f), sc (18.0f), sc (18.0f));
        level.setBounds (sc (80.0f), sc (41.0f), sc (126.0f), sc (8.0f));
        tune.setBounds  (sc (80.0f), sc (51.0f), sc (126.0f), sc (8.0f));
    }

    int idx;
    std::function<void (int)> selectFn;
    juce::ToggleButton onToggle;
    juce::Slider level, tune;

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAtt, tuneAtt;
};

//============================================================================
// Карточка патч-колонки: иконка, заголовок и до двух элементов управления,
// каждый из которых привязан к настоящему параметру.
//============================================================================
class OrogenEditor::ParamCard : public juce::Component
{
public:
    ParamCard (OrogenProcessor& p, juce::String t, int icon)
        : proc (p), title (std::move (t)), glyph (icon) {}

    ParamCard& combo (const juce::String& id, const juce::StringArray& items)
    {
        auto* box = new juce::ComboBox();
        for (int i = 0; i < items.size(); ++i) box->addItem (items[i], i + 1);
        addAndMakeVisible (box);
        comboAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            proc.apvts, id, *box));
        boxes.add (box);
        ids.add (id);                       // для теста «что видно на главном экране»
        return *this;
    }

    ParamCard& knob (const juce::String& id)
    {
        auto* s = new juce::Slider();
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        s->getProperties().set ("big", false);
        addAndMakeVisible (s);
        knobAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, id, *s));
        knobs.add (s);
        ids.add (id);
        return *this;
    }

    ParamCard& toggle (const juce::String& id)
    {
        auto* t = new juce::ToggleButton ("ON");
        addAndMakeVisible (t);
        toggleAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            proc.apvts, id, *t));
        toggles.add (t);
        ids.add (id);
        return *this;
    }

    // Компактная раскладка: два списка друг под другом слева, ручка справа.
    // Нужна карточкам мод-маршрута (источник + приёмник + количество): в
    // обычной раскладке третий контрол не влезал в карточку 184×72 и обрезался.
    ParamCard& compact (bool on = true) { compactLayout = on; return *this; }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        draw::panel (g, b.reduced (0.5f), 3.0f, Palette::panelAlt, Palette::line);

        draw::fxGlyph (g, glyph, { 10.0f, 9.0f, 15.0f, 15.0f }, Palette::textDim);
        draw::caption (g, title, { 30, 8, getWidth() - 38, 16 }, Palette::textDim, 7.5f);

        // значение ручки — под самой ручкой
        for (auto* s : knobs)
        {
            const auto r = s->getBounds();
            g.setColour (Palette::text);
            g.setFont (orFont (7.5f));
            g.drawText (s->getTextFromValue (s->getValue()),
                        juce::Rectangle<int> (r.getX() - 4, r.getBottom(), r.getWidth() + 8, 10),
                        juce::Justification::centred, false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 0);
        r.removeFromTop (24);

        if (compactLayout && knobs.size() == 1 && ! boxes.isEmpty())
        {
            auto row = getLocalBounds().reduced (10, 0);
            row.removeFromTop (18);
            auto knobArea = row.removeFromRight (40);
            knobs[0]->setBounds (knobArea.getX(), row.getY() - 2, 34, 34);
            for (auto* box : boxes)
                box->setBounds (row.removeFromTop (16).withTrimmedRight (4));
            return;
        }

        for (auto* box : boxes)
            box->setBounds (r.removeFromTop (18));

        if (! knobs.isEmpty())
        {
            // до трёх ручек в один ряд — так в карточку влезает больше параметров
            r.removeFromTop (2);
            auto row = r.removeFromTop (34);
            const int cell = juce::jmin (40, row.getWidth() / juce::jmax (1, knobs.size()));
            for (int i = 0; i < knobs.size(); ++i)
                knobs[i]->setBounds (row.getX() + i * cell, row.getY(),
                                     juce::jmin (cell - 4, 34), 34);
            r.removeFromTop (10);      // место под значения
        }
        for (auto* t : toggles)
            t->setBounds (r.removeFromTop (18));
    }

    OrogenProcessor& proc;
    juce::String title;
    int glyph;
    juce::StringArray ids;                  // параметры, к которым привязаны контролы карточки
    juce::OwnedArray<juce::ComboBox> boxes;
    juce::OwnedArray<juce::Slider> knobs;
    juce::OwnedArray<juce::ToggleButton> toggles;

private:
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAtts;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> knobAtts;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> toggleAtts;
    bool compactLayout = false;
};

//============================================================================
// Патч-колонка: 4 карточки и «кабели» между ними.
//============================================================================
class OrogenEditor::PatchColumn : public juce::Component
{
public:
    PatchColumn (OrogenProcessor& p, juce::String t) : proc (p), title (std::move (t)), pages (*this)
    {
        addAndMakeVisible (pages);
    }

    ParamCard& addCard (juce::String name, int icon)
    {
        auto* c = new ParamCard (proc, std::move (name), icon);
        addAndMakeVisible (c);
        cards.add (c);
        return *c;
    }

    void setAnimPhase (float p) { animPhase = p; }

    // Перерисовать только полосы между карточками: там живут «кометы».
    // Полный repaint колонки стоил больше, чем вся остальная анимация.
    void repaintCables()
    {
        for (int i = 0; i + 1 < cards.size(); ++i)
        {
            if (! cards[i]->isVisible() || ! cards[i + 1]->isVisible()) continue;
            const auto a = cards[i]->getBounds();
            const auto b = cards[i + 1]->getBounds();
            repaint (juce::Rectangle<int> (a.getCentreX() - 34, a.getBottom() - 6, 68,
                                           juce::jmax (8, b.getY() - a.getBottom() + 12)));
        }
    }

    int cableAreaPx() const
    {
        int area = 0;
        for (int i = 0; i + 1 < cards.size(); ++i)
            if (cards[i]->isVisible() && cards[i + 1]->isVisible()) area += 68 * 14;
        return area;
    }

    int pageCountPublic() const { return pageCount(); }
    int pagePublic() const { return page; }
    void nextPage() { setPage (page + 1); }

    // Кнопка в шапке колонки: листает страницы. Отдельный компонент, а не
    // mouseDown по колонке — иначе перехватывались бы клики дочерних карточек.
    class PageButton : public juce::Button
    {
    public:
        explicit PageButton (PatchColumn& o) : juce::Button ("pages"), owner (o) {}
        void paintButton (juce::Graphics& g, bool, bool) override
        {
            if (owner.pageCount() <= 1) return;
            const auto r = getLocalBounds().toFloat().reduced (0.5f);
            draw::panel (g, r, 3.0f, Palette::panelAlt, Palette::line);
            g.setColour (Palette::textDim);
            g.setFont (orFont (7.5f));
            g.drawText ("<  " + juce::String (owner.getPage() + 1) + " / "
                            + juce::String (owner.pageCount()) + "  >",
                        getLocalBounds(), juce::Justification::centred, false);
        }
        void clicked() override { owner.setPage (owner.getPage() + 1); }   // по кругу
        PatchColumn& owner;
    };

    void paint (juce::Graphics& g) override
    {
        paintHeader (g);

        // кабели: правая грань карточки i → левая грань карточки i+1
        for (int i = 0; i + 1 < cards.size(); ++i)
        {
            if (pagesEnabled && (pageOf (i) != page || pageOf (i + 1) != page)) continue;
            if (! cards[i]->isVisible() || ! cards[i + 1]->isVisible()) continue;
            const auto a = cards[i]->getBounds().toFloat();
            const auto b = cards[i + 1]->getBounds().toFloat();
            const float cx = a.getCentreX();
            const float y1 = a.getBottom() - 4.0f;
            const float y2 = b.getY() + 4.0f;

            juce::Path cable;
            cable.startNewSubPath (cx - 26.0f, y1);
            cable.cubicTo (cx - 26.0f, y1 + 18.0f, cx + 26.0f, y2 - 18.0f, cx + 26.0f, y2);
            draw::glowStroke (g, cable, 1.1f, Palette::lineHi, 0.10f);

            // бегущая «комета» по кабелю: короткий яркий отрезок с затуханием
            // к хвосту. Позиция — из общей фазы, у соседних кабелей сдвиг,
            // поэтому по колонке идёт волна сверху вниз.
            {
                const float len = cable.getLength();
                const float t0 = std::fmod (animPhase * 0.55f + 0.22f * (float) i, 1.0f);
                const float tail = 0.16f;
                juce::Path comet;
                bool started = false;
                for (int k = 0; k <= 8; ++k)
                {
                    const float t = t0 - tail * (float) k / 8.0f;
                    if (t < 0.0f) break;
                    const auto pt = cable.getPointAlongPath (len * t);
                    if (! started) { comet.startNewSubPath (pt); started = true; }
                    else            comet.lineTo (pt);
                }
                if (started)
                    draw::glowStroke (g, comet, 1.4f, Palette::signal.withAlpha (0.55f), 0.16f);

                const auto head = cable.getPointAlongPath (len * t0);
                g.setColour (Palette::signal.withAlpha (0.85f));
                g.fillEllipse (head.x - 1.8f, head.y - 1.8f, 3.6f, 3.6f);
                g.setColour (Palette::signal.withAlpha (0.12f));
                g.fillEllipse (head.x - 4.0f, head.y - 4.0f, 8.0f, 8.0f);
            }

            g.setColour (Palette::lineMid);
            g.fillEllipse (cx - 28.0f, y1 - 2.0f, 4.0f, 4.0f);
            g.fillEllipse (cx + 24.0f, y2 - 2.0f, 4.0f, 4.0f);
        }
    }

    void resized() override
    {
        pages.setBounds (getWidth() - 92, 3, 78, 16);
        // на странице — не больше kPerPage карточек; остальные скрыты
        int shown = 0;
        for (int i = 0; i < cards.size(); ++i)
        {
            const bool visible = ! pagesEnabled || pageOf (i) == page;
            cards[i]->setVisible (visible);
            if (visible) cards[i]->setBounds (40, 24 + shown++ * 80, 184, 72);
        }
    }

    int pageCount() const
    {
        if (! pagesEnabled || cards.isEmpty()) return 1;
        return (cards.size() + kPerPage - 1) / kPerPage;
    }
    int getPage() const { return page; }

    void setPage (int p)
    {
        const int n = pageCount();
        page = n <= 1 ? 0 : ((p % n) + n) % n;
        resized();
        repaint();
    }

    void paintHeader (juce::Graphics&) const {}   // индикатор страниц — PageButton

    juce::StringArray allIds() const
    {
        juce::StringArray out;
        for (auto* c : cards) out.addArray (c->ids);
        return out;
    }

    static constexpr int kPerPage = 6;
    bool pagesEnabled = true;             // у колонок с ≤ 6 карточками страниц нет

    int pageOf (int index) const { return index / kPerPage; }

    float animPhase = 0.0f;
    OrogenProcessor& proc;
    juce::String title;
    juce::OwnedArray<ParamCard> cards;
    PageButton pages;
    int page = 0;
};

//============================================================================
// Кнопка-иконка (нижний ряд секций).
//============================================================================
class OrogenEditor::IconButton : public juce::Button
{
public:
    IconButton (int kind, std::function<void()> cb) : juce::Button ("s"), glyphKind (kind)
    {
        onClick = std::move (cb);
        setClickingTogglesState (false);
    }

    void setActive (bool a) { if (active != a) { active = a; repaint(); } }
    void setAnimPhase (float p) { animPhase = p; }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        draw::panel (g, r, 2.5f, active ? Palette::panelAlt : juce::Colours::transparentBlack,
                     active ? Palette::lineHi : (highlighted ? Palette::lineMid : Palette::line));
        const float pulse = active
            ? 0.62f + 0.38f * (0.5f + 0.5f * std::sin (animPhase * juce::MathConstants<float>::twoPi * 0.5f))
            : 1.0f;
        draw::sectionGlyph (g, glyphKind, r.reduced (12.0f, 9.0f),
                            active ? Palette::signal.withMultipliedAlpha (pulse) : Palette::textDim);
    }

private:
    int glyphKind;
    bool active = false;
    float animPhase = 0.0f;
};

//============================================================================
// Векторный блок: X = Morph, Y = Glide (оба — реальные параметры).
//============================================================================
class OrogenEditor::VectorPad : public juce::Component
{
public:
    explicit VectorPad (OrogenProcessor& p) : proc (p) {}

    void setAnimPhase (float p) { animPhase = p; }


    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        draw::panel (g, r, 3.0f, Palette::panel, Palette::line);

        const auto c = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.34f;

        juce::Path diamond;
        diamond.startNewSubPath (c.x, c.y - rad);
        diamond.lineTo (c.x + rad * 0.72f, c.y);
        diamond.lineTo (c.x, c.y + rad);
        diamond.lineTo (c.x - rad * 0.72f, c.y);
        diamond.closeSubPath();
        draw::glowStroke (g, diamond, 1.2f, Palette::lineHi, 0.10f);

        const auto x = proc.apvts.getRawParameterValue ("gl_morph")->load();
        const auto y = proc.apvts.getRawParameterValue ("gl_glide")->load() * 0.5f;  // 0…2 c
        const juce::Point<float> p { c.x + (x - 0.5f) * 2.0f * rad * 0.72f,
                                     c.y + (0.5f - juce::jlimit (0.0f, 1.0f, y)) * 2.0f * rad };

        juce::Path cross;
        cross.startNewSubPath (c.x, c.y - rad);
        cross.lineTo (c.x, c.y + rad);
        cross.startNewSubPath (c.x - rad * 0.72f, c.y);
        cross.lineTo (c.x + rad * 0.72f, c.y);
        g.setColour (Palette::grid);
        g.strokePath (cross, juce::PathStrokeType (1.0f));

        // гало дышит: радиус 16…22 px, альфа 0.10…0.20
        const float breath = 0.5f + 0.5f * std::sin (animPhase * juce::MathConstants<float>::twoPi * 0.55f);
        g.setColour (Palette::signal.withAlpha (0.10f + 0.10f * breath));
        g.fillEllipse (juce::Rectangle<float> (18.0f + 4.0f * breath,
                                               18.0f + 4.0f * breath).withCentre (p));
        g.setColour (Palette::signal);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (p));

        draw::caption (g, "morph x / glide y", r.reduced (8.0f).toNearestInt().removeFromBottom (12),
                       Palette::textFaint, 6.5f, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override { apply (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { apply (e.position); }

    float animPhase = 0.0f;

private:
    void apply (juce::Point<float> pos)
    {
        const auto r = getLocalBounds().toFloat();
        const auto c = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.34f;

        const float x = juce::jlimit (0.0f, 1.0f, 0.5f + (pos.x - c.x) / (2.0f * rad * 0.72f));
        const float y = juce::jlimit (0.0f, 1.0f, 0.5f - (pos.y - c.y) / (2.0f * rad));

        if (auto* pm = proc.apvts.getParameter ("gl_morph")) pm->setValueNotifyingHost (x);
        if (auto* pg = proc.apvts.getParameter ("gl_glide")) pg->setValueNotifyingHost (y);
    }

    OrogenProcessor& proc;
};

//============================================================================
// Оверлей параметров: 5 вкладок из UiParamMap.h, в каждой — секции карточками.
// Так у всех 294 параметров есть место в интерфейсе, а не плоский список.
//============================================================================
class OrogenEditor::ParamsPage : public juce::Component
{
public:
    explicit ParamsPage (OrogenProcessor& p) : proc (p), tabs (buildUiTabs())
    {
        setOpaque (true);

        addAndMakeVisible (viewport);
        viewport.setScrollBarsShown (true, false);

        addAndMakeVisible (closeButton);
        closeButton.onClick = [this] { if (onClose) onClose(); };

        // кнопки вкладок
        for (int i = 0; i < (int) tabs.size(); ++i)
        {
            auto* b = new TabButton (tabs[(size_t) i].title, [this, i] { setTab (i); });
            addAndMakeVisible (b);
            tabButtons.add (b);
        }

        addAndMakeVisible (allSectionsButton);
        allSectionsButton.onClick = [this] { setSection (-1); };
        sectionButtons.add (&allSectionsButton);

        // содержимое каждой вкладки строится один раз; переключение — видимостью
        for (const auto& tab : tabs)
        {
            auto* content = new TabContent (proc);
            content->build (tab);
            addChildComponent (content);
            contents.add (content);
        }

        setTab (0);
    }

    std::function<void()> onClose;

    void setTab (int index)
    {
        currentTab = juce::jlimit (0, (int) tabs.size() - 1, index);
        for (int i = 0; i < contents.size(); ++i) contents[i]->setVisible (i == currentTab);
        for (int i = 0; i < tabButtons.size(); ++i) tabButtons[i]->setActive (i == currentTab);
        rebuildSectionButtons();
        setSection (currentSection);        // держим выбранную секцию в новых границах
        viewport.setViewedComponent (contents[currentTab], false);
        viewport.setViewPosition (0, 0);
        resized();
        repaint();
    }

    int getTab() const { return currentTab; }
    int tabCount() const { return (int) tabs.size(); }

    // -1 — все секции вкладки, иначе номер секции (0…n-1)
    void setSection (int index)
    {
        auto* c = contents[currentTab];
        if (c == nullptr) return;
        const int n = c->sectionCount();
        currentSection = (index < 0 || n == 0) ? -1 : juce::jlimit (0, n - 1, index);
        c->setSectionFilter (currentSection < 0 ? juce::String() : c->sectionName (currentSection));
        for (int i = 0; i < sectionButtons.size(); ++i)
            sectionButtons[i]->setActive (currentSection < 0 ? i == 0 : i == currentSection + 1);
        viewport.setViewPosition (0, 0);
        resized();
        repaint();
    }

    int getSection() const { return currentSection; }
    auto* currentContent() const { return contents[currentTab]; }
    int sectionCount() const { return contents[currentTab] != nullptr ? contents[currentTab]->sectionCount() : 0; }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto head = r.removeFromTop (30);
        closeButton.setBounds (head.removeFromRight (70).reduced (0, 3));
        head.removeFromRight (10);
        head.removeFromLeft (166);          // место под заголовок слева

        int x = 0;
        for (auto* b : tabButtons)
        {
            const int w = juce::jmax (70, b->getButtonText().length() * 9 + 26);
            b->setBounds (head.getX() + x, head.getY(), w, head.getHeight());
            x += w + 6;
        }

        // второй ряд — секции текущей вкладки (ALL + по одной на секцию)
        auto secRow = r.removeFromTop (24);
        {
            int sx = 0;
            for (auto* b : sectionButtons)
            {
                const int w = juce::jmax (56, b->getButtonText().length() * 8 + 22);
                b->setBounds (secRow.getX() + sx, secRow.getY(), w, 22);
                sx += w + 5;
            }
        }

        r.removeFromTop (6);
        viewport.setBounds (r);

        if (auto* c = contents[currentTab])
        {
            const int w = juce::jmax (600, r.getWidth());
            c->setSize (w, c->neededHeight (w));
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::bg);
        g.setColour (Palette::panel);
        g.fillRect (getLocalBounds().removeFromTop (46));
        g.setColour (Palette::panel);
        g.fillRect (0, 47, getWidth(), 34);
        g.setColour (Palette::line);
        g.fillRect (0, 46, getWidth(), 1);
        g.fillRect (0, 81, getWidth(), 1);

        // заголовок слева от вкладок, чтобы не наезжать на первую кнопку
        draw::caption (g, "parameters", { 18, 6, 150, 10 }, Palette::textFaint, 6.0f);
        // подпись страницы — справа от кнопок секций, чтобы не наезжать на них
        draw::caption (g, currentSection < 0 ? juce::String ("all sections")
                                            : juce::String ("section ") + juce::String (currentSection + 1)
                                                  + " / " + juce::String (sectionCount()),
                       { getWidth() - 300, 54, 280, 10 }, Palette::textFaint, 6.0f);
        g.setColour (Palette::text);
        g.setFont (orFont (10.5f, true));
        g.drawText (spaced (tabs[(size_t) currentTab].title),
                    juce::Rectangle<int> (18, 17, 150, 16), juce::Justification::centredLeft, false);

        g.setColour (Palette::textDim);
        g.setFont (orFont (8.0f));
        g.drawText (juce::String (uiParamCount (tabs)) + " параметров · ← → · ↑ ↓ · ESC",
                    juce::Rectangle<int> (getWidth() - 300, 28, 210, 14),
                    juce::Justification::centredRight, false);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::leftKey)  { setTab (currentTab - 1); return true; }
        if (k == juce::KeyPress::rightKey) { setTab (currentTab + 1); return true; }
        if (k == juce::KeyPress::upKey)    { setSection (currentSection - 1); return true; }
        if (k == juce::KeyPress::downKey)  { setSection (currentSection + 1); return true; }
        if (k.getTextCharacter() >= '1' && k.getTextCharacter() <= '5')
        {
            setTab ((int) (k.getTextCharacter() - '1'));
            return true;
        }
        return false;
    }

private:
    // --- кнопка вкладки ---
    class TabButton : public juce::Button
    {
    public:
        TabButton (const juce::String& t, std::function<void()> cb) : juce::Button (t)
        {
            onClick = std::move (cb);
        }

        void setActive (bool a) { if (active != a) { active = a; repaint(); } }

        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            const auto r = getLocalBounds().toFloat().reduced (0.5f);
            draw::panel (g, r, 2.5f, active ? Palette::panelAlt : juce::Colours::transparentBlack,
                         active ? Palette::lineHi : (highlighted ? Palette::lineMid : Palette::line));
            g.setColour (active ? Palette::text : Palette::textDim);
            g.setFont (orFont (8.0f));
            g.drawText (spaced (getButtonText()),
                        getLocalBounds(), juce::Justification::centred, false);
        }

    private:
        bool active = false;
    };

    // --- содержимое одной вкладки: секции и привязанные контролы ---
    class TabContent : public juce::Component
    {
    public:
        struct Entry
        {
            juce::String section, name;
            juce::Component* ctl = nullptr;
            juce::Rectangle<int> captionRect, headerRect;
        };

        static int colsFor (int width) { return juce::jlimit (2, 5, width / 260); }

        explicit TabContent (OrogenProcessor& p) : proc (p) {}

        void build (const UiTab& tab)
        {
            for (const auto& sec : tab.sections)
            {
                for (const auto& id : sec.ids)
                {
                    auto* base = proc.apvts.getParameter (id);
                    if (base == nullptr) continue;          // карта и layout разъехались — ловит тест

                    auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base);
                    const juce::String name = rp != nullptr ? rp->getName (64) : id;

                    Entry e;
                    e.section = sec.title;
                    e.name = name;

                    if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (base))
                    {
                        auto* box = new juce::ComboBox();
                        int i = 1;
                        for (const auto& item : ch->getAllValueStrings()) box->addItem (item, i++);
                        addAndMakeVisible (box);
                        comboAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                            proc.apvts, id, *box));
                        e.ctl = box;
                    }
                    else if (dynamic_cast<juce::AudioParameterBool*> (base) != nullptr)
                    {
                        auto* t = new juce::ToggleButton ("ON");
                        addAndMakeVisible (t);
                        buttonAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                            proc.apvts, id, *t));
                        e.ctl = t;
                    }
                    else
                    {
                        auto* sl = new juce::Slider (juce::Slider::LinearHorizontal,
                                                     juce::Slider::TextBoxRight);
                        sl->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 15);
                        addAndMakeVisible (sl);
                        sliderAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                            proc.apvts, id, *sl));
                        e.ctl = sl;
                    }

                    entries.push_back (e);
                }
            }
        }

        // Показ одной секции (sectionFilter пустой — все секции вкладки).
        // Так 294 параметра не лежат одним списком, а разложены по страницам,
        // как секции на макете.
        void setSectionFilter (const juce::String& name)
        {
            sectionFilter = name;
            for (auto& e : entries)
                if (e.ctl != nullptr)
                    e.ctl->setVisible (sectionFilter.isEmpty() || e.section == sectionFilter);
            resized();
            repaint();
        }

        const juce::String& getSectionFilter() const { return sectionFilter; }

        int sectionCount() const
        {
            juce::StringArray names;
            for (const auto& e : entries) names.addIfNotAlreadyThere (e.section);
            return names.size();
        }

        juce::String sectionName (int index) const
        {
            juce::StringArray names;
            for (const auto& e : entries) names.addIfNotAlreadyThere (e.section);
            return names[index];
        }

        bool isVisibleEntry (const Entry& e) const
        {
            return sectionFilter.isEmpty() || e.section == sectionFilter;
        }

        int neededHeight (int width) const
        {
            const int cols = colsFor (width);
            int y = 0, col = 0;
            juce::String cur;
            for (const auto& e : entries)
            {
                if (! isVisibleEntry (e)) continue;
                if (e.section != cur)
                {
                    if (col != 0) { y += 46; col = 0; }
                    cur = e.section;
                    y += 26;
                }
                if (++col >= cols) { col = 0; y += 46; }
            }
            return y + (col != 0 ? 46 : 0) + 14;
        }

        void paint (juce::Graphics& g) override
        {
            juce::String cur;
            for (const auto& e : entries)
            {
                if (! isVisibleEntry (e)) continue;
                if (e.section != cur)
                {
                    cur = e.section;
                    const auto header = e.headerRect;
                    draw::panel (g, header.toFloat(), 2.5f, Palette::panelAlt, Palette::line);
                    draw::caption (g, e.section, header.withTrimmedLeft (10), Palette::text, 7.5f);
                }

                g.setColour (Palette::textFaint);
                g.setFont (orFont (7.0f));
                g.drawText (e.name, e.captionRect, juce::Justification::centredLeft, true);
            }
        }

        void resized() override
        {
            const int cols = colsFor (getWidth());
            const int cellW = getWidth() / cols;
            int y = 0;
            int col = 0;
            juce::String cur;

            for (auto& e : entries)
            {
                if (! isVisibleEntry (e)) continue;
                if (e.section != cur)
                {
                    if (col != 0) { y += 46; col = 0; }
                    cur = e.section;
                    e.headerRect = { 0, y, getWidth(), 22 };
                    y += 26;
                }

                const int x = col * cellW;
                auto cell = juce::Rectangle<int> (x, y, cellW - 8, 42);
                e.captionRect = cell.removeFromTop (14);
                e.ctl->setBounds (cell.reduced (0, 0).withTrimmedRight (4));

                if (++col >= cols) { col = 0; y += 46; }
            }
        }

        OrogenProcessor& proc;
        juce::String sectionFilter;          // пусто = показывать все секции
        std::vector<Entry> entries;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAtts;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAtts;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAtts;
    };

    void rebuildSectionButtons()
    {
        while (sectionButtons.size() > 1)
            sectionButtons.removeLast();            // сами компоненты удалит owned-массив
        sectionOwned.clear();
        auto* c = contents[currentTab];
        if (c == nullptr) return;
        for (int i = 0; i < c->sectionCount(); ++i)
        {
            auto* b = new TabButton (c->sectionName (i), [this, i] { setSection (i); });
            *sectionOwned.add (b);
            addAndMakeVisible (b);
            sectionButtons.add (b);
        }
    }

    OrogenProcessor& proc;
    std::vector<UiTab> tabs;
    int currentTab = 0;
    int currentSection = -1;

    juce::Viewport viewport;
    juce::OwnedArray<TabContent> contents;
    juce::OwnedArray<TabButton> tabButtons;
    juce::OwnedArray<TabButton> sectionOwned;      // кнопки секций (кроме ALL)
    juce::Array<TabButton*> sectionButtons;         // ALL + секции текущей вкладки
    TabButton allSectionsButton { "ALL", [this] { setSection (-1); } };
    juce::TextButton closeButton { "CLOSE" };
};

//============================================================================
// Конструктор редактора.
//============================================================================
OrogenEditor::OrogenEditor (OrogenProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p)
{
    lnf = std::make_unique<OrLookAndFeel>();
    setLookAndFeel (lnf.get());

    setOpaque (true);
    setResizable (true, false);
    setResizeLimits (960, 600, 2560, 1600);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kDesignW / (double) kDesignH);

    masterSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    addAndMakeVisible (masterSlider);
    masterAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, "gl_master", masterSlider);

    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    loadButton.onClick = [this] { loadPresetFromFile(); };
    saveButton.onClick = [this] { savePresetToFile(); };

    // программы: тот же банк, что у хоста (getNumPrograms/getProgramName)
    addAndMakeVisible (programBox);
    programBox.setTooltip ("PROGRAM");
    programBox.setTextWhenNothingSelected ("PROGRAM");
    {
        const auto names = proc.presetNames();
        for (int i = 0; i < names.size(); ++i) programBox.addItem (names[i].toUpperCase(), i + 1);
        programBox.setSelectedItemIndex (proc.getCurrentProgram(), juce::dontSendNotification);
    }
    programBox.addMouseListener (this, true);       // правый клик — меню банка
    loadButton.addMouseListener (this, true);       // правый клик — меню LOAD
    saveButton.addMouseListener (this, true);       // правый клик — меню SAVE
    programBox.onChange = [this]
    {
        const int idx = programBox.getSelectedItemIndex();
        if (idx >= 0 && idx != proc.getCurrentProgram())
        {
            proc.setCurrentProgram (idx);
            repaint();
        }
    };

    buildRail();
    buildCentre();
    buildBottom();
    buildColumns();
    buildParamsPage();

    selectSlot (0);
    setSection (1);              // по умолчанию открыта мод-матрица (как на макете)
    setSize ((int) kDesignW, (int) kDesignH);
    startTimerHz (20);        // анимации: 20 кадров/с, измерено в блоке 11
}

OrogenEditor::~OrogenEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

// ---------------------------------------------------------------------------
void OrogenEditor::buildRail()
{
    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* card = new SlotCard (proc, i, [this] (int s) { selectSlot (s); });
        addAndMakeVisible (card);
        cards.add (card);
    }
}

void OrogenEditor::buildCentre()
{
    // Селекторы движка и фильтра выбранного слота. Параметры sX_type/sX_filter —
    // это choice, поэтому ComboBox + ComboBoxAttachment. Без них тип движка
    // менялся только через страницу PARAMS, и это выглядело как «работает
    // только Wavetable».
    engineBox.addItemList (itemsOf ("sA_type"), 1);
    filterBox.addItemList (itemsOf ("sA_filter"), 1);
    addAndMakeVisible (engineBox);
    addAndMakeVisible (filterBox);
    engineBox.setTooltip ("Engine type of the selected slot (sA_type ... sH_type)");
    filterBox.setTooltip ("Filter model of the selected slot (sA_filter ... sH_filter)");

    // три ручки центра: параметры зависят от слота → привязываются в selectSlot()
    for (int i = 0; i < 3; ++i)
    {
        auto* k = new Bound();
        k->setup (juce::Slider::RotaryHorizontalVerticalDrag, true);
        addAndMakeVisible (k->slider);
        centreKnobs.add (k);

        auto* b = new Bound();
        b->setup (juce::Slider::LinearVertical, false);
        addAndMakeVisible (b->slider);
        centreBars.add (b);
    }
}

void OrogenEditor::buildBottom()
{
    struct Pair { const char* knob; const char* bar; const char* label; };
    static const Pair pairs[5] =
    {
        { "gl_master", "gl_modWheel", "MASTER" },
        { "gl_gain",   "gl_width",    "PATCH GAIN" },
        { "gl_morph",  "gl_glide",    "MORPH" },
        { "arpGate",   "arpSwing",    "ARP GATE" },
        { "lfo1_rate", "lfo1_amount", "LFO 1 RATE" }
    };

    for (const auto& pr : pairs)
    {
        auto* k = new Bound();
        k->setup (juce::Slider::RotaryHorizontalVerticalDrag, true);
        addAndMakeVisible (k->slider);
        k->bind (proc.apvts, pr.knob);
        bottomKnobs.add (k);

        auto* b = new Bound();
        b->setup (juce::Slider::LinearVertical, false);
        addAndMakeVisible (b->slider);
        b->bind (proc.apvts, pr.bar);
        bottomBars.add (b);
    }

    // 4 переключателя: реальные булевы параметры
    static const char* ids[4] = { "arpOn", "arpLatch", "fx_combNegative", "fx_materialFollow" };
    static const char* names[4] = { "ARP", "LATCH", "COMB −", "FOLLOW" };
    for (int i = 0; i < 4; ++i)
    {
        auto* t = new juce::ToggleButton (names[i]);
        addAndMakeVisible (t);
        switchButtons.add (t);
        switchAtts.add (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            proc.apvts, ids[i], *t));
    }

    vectorPad = std::make_unique<VectorPad> (proc);
    addAndMakeVisible (*vectorPad);

    // кнопки секций патч-колонки (иконки в левом нижнем углу)
    static const char* titles[4] = { "FX CHAIN", "MOD MATRIX", "ARP", "LFO" };
    juce::ignoreUnused (titles);
    for (int i = 0; i < 4; ++i)
    {
        auto* b = new IconButton (i, [this, i] { setSection (i); });
        addAndMakeVisible (b);
        sectionButtons.add (b);
    }
}

void OrogenEditor::buildColumns()
{
    static const char* titles[4] = { "FX CHAIN", "MOD MATRIX", "ARP", "LFO" };
    for (int i = 0; i < 4; ++i)
    {
        auto* col = new PatchColumn (proc, titles[i]);
        addChildComponent (col);        // видимость — в setSection()
        columns.add (col);
    }

    // --- FX: три страницы по шесть карточек, чтобы с главного экрана были
    // доступны все 16 эффектов, а не только первые шесть
    {
        auto& c = *columns[0];
        // страница 1 — окрас и пространство
        c.addCard ("DRIVE", 0).combo ("fx_driveModel", itemsOf ("fx_driveModel"))
                              .knob ("fx_drive").knob ("fx_driveTone");
        // хорус: полный набор ручек ядра OS 1.32B — MIX/DEP/SPD и DEL/FB/WID/LP/INP
        c.addCard ("CHORUS", 1).combo ("fx_chorusModel", itemsOf ("fx_chorusModel"))
                              .knob ("fx_chorusMix").knob ("fx_chorusDepth");
        c.addCard ("CHORUS DELAY / FB / WID", 1).knob ("fx_chorusDelay")
                                          .knob ("fx_chorusFeedback").knob ("fx_chorusWidth");
        c.addCard ("CHORUS SPD / LP / IN", 1).knob ("fx_chorusRate")
                                       .knob ("fx_chorusLowpass").knob ("fx_chorusInput");
        c.addCard ("DELAY", 2).knob ("fx_delayMix").knob ("fx_delayTime").knob ("fx_delayFb");
        c.addCard ("REVERB", 3).knob ("fx_reverbMix").knob ("fx_reverbSize").knob ("fx_reverbDamp");

        // страница 2 — пространство и характер
        c.addCard ("PLATE", 3).knob ("fx_plateMix").knob ("fx_plateDecay").knob ("fx_plateDiffusion");
        c.addCard ("MATERIAL", 4).combo ("fx_materialMode", itemsOf ("fx_materialMode"))
                                .knob ("fx_materialMix").knob ("fx_materialExcite");
        c.addCard ("SPECTRE", 5).combo ("fx_spectreModel", itemsOf ("fx_spectreModel"))
                               .knob ("fx_spectreMix").knob ("fx_spectreProminence");
        c.addCard ("GLITCH", 6).knob ("fx_glitchMix").knob ("fx_glitchChance")
                              .knob ("fx_glitchLengthMs");
        c.addCard ("ROBIN", 7).knob ("fx_robinMix").knob ("fx_robinSemis").knob ("fx_robinSize");
        c.addCard ("COMB", 8).combo ("fx_combModel", itemsOf ("fx_combModel"))
                            .knob ("fx_combMix").knob ("fx_combFreqA");

        // страница 3 — тон, динамика, полоса
        c.addCard ("EQ", 9).knob ("fx_eqLow").knob ("fx_eqHigh");
        c.addCard ("DYNAMICS", 10).knob ("fx_compThreshold").knob ("fx_compRatio")
                                 .knob ("fx_compMakeup");
        c.addCard ("WIDTH / OUTPUT", 11).knob ("fx_width");
        c.addCard ("SPECTRE BAND", 5).knob ("fx_spectreLow").knob ("fx_spectreHigh")
                                    .toggle ("fx_spectreFreeze");
        c.addCard ("MATERIAL TONE", 4).knob ("fx_materialTone").knob ("fx_materialDamp")
                                     .knob ("fx_materialSize");
        c.addCard ("COMB TONE", 8).knob ("fx_combFeedback").knob ("fx_combHighpass")
                                 .knob ("fx_combTune");
    }
    // --- MOD MATRIX: все 8 маршрутов, у каждого источник, приёмник и количество.
    // Раньше карточек было четыре и без приёмника: маршруты 5…8 в ядре работали,
    // но с главного экрана до них было не добраться.
    {
        auto& c = *columns[1];
        for (int r = 1; r <= 8; ++r)
        {
            const auto pre = "mod" + juce::String (r) + "_";
            c.addCard ("ROUTE " + juce::String (r), 4)
                .combo (pre + "src", itemsOf (pre + "src"))
                .combo (pre + "dst", itemsOf (pre + "dst"))
                .knob (pre + "amt")
                .compact();
        }
    }
    // --- ARP ---
    {
        auto& c = *columns[2];
        c.addCard ("MODE", 11).combo ("arpMode", itemsOf ("arpMode")).knob ("arpGate");
        c.addCard ("DIVISION", 11).combo ("arpDiv", itemsOf ("arpDiv")).knob ("arpSwing");
        c.addCard ("OCTAVES", 11).knob ("arpOctaves");
        c.addCard ("SWITCHES", 11).toggle ("arpOn").toggle ("arpLatch");
    }
    // --- LFO ---
    {
        auto& c = *columns[3];
        c.addCard ("LFO 1 RATE", 3).knob ("lfo1_rate").knob ("lfo1_amount");
        c.addCard ("LFO 1 SHAPE", 3).combo ("lfo1_shape", itemsOf ("lfo1_shape")).combo ("lfo1_dest", itemsOf ("lfo1_dest"));
        c.addCard ("LFO 2 RATE", 3).knob ("lfo2_rate").knob ("lfo2_amount");
        c.addCard ("LFO 2 SHAPE", 3).combo ("lfo2_shape", itemsOf ("lfo2_shape")).combo ("lfo2_dest", itemsOf ("lfo2_dest"));
    }
}

void OrogenEditor::buildParamsPage()
{
    paramsPage = std::make_unique<ParamsPage> (proc);
    paramsPage->onClose = [this] { toggleParamsPage (false); };
    addChildComponent (*paramsPage);
}

// ---------------------------------------------------------------------------
void OrogenEditor::selectSlot (int slot)
{
    selectedSlot = juce::jlimit (0, kNumSlots - 1, slot);

    // центральные ручки: cutoff/res, shape (p0/p1), level/detune выбранного слота
    static const char* knobIds[3] = { "cutoff", "p0", "level" };
    static const char* barIds[3]  = { "res", "p1", "detune" };

    for (int i = 0; i < 3; ++i)
    {
        centreKnobs[i]->bind (proc.apvts, slotId (knobIds[i]));
        centreBars[i]->bind (proc.apvts, slotId (barIds[i]));
    }

    engineAtt.reset();
    filterAtt.reset();
    engineAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, slotId ("type"), engineBox);
    filterAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, slotId ("filter"), filterBox);

    repaint();
}

void OrogenEditor::setSection (int index)
{
    section = juce::jlimit (0, 3, index);
    for (int i = 0; i < columns.size(); ++i)
        columns[i]->setVisible (i == section);
    for (int i = 0; i < sectionButtons.size(); ++i)
        sectionButtons[i]->setActive (i == section);
    repaint();
}

void OrogenEditor::toggleParamsPage (bool shouldShow)
{
    paramsOpen = shouldShow;
    if (paramsPage != nullptr)
    {
        paramsPage->setVisible (paramsOpen);
        if (paramsOpen) { paramsPage->setBounds (getLocalBounds()); paramsPage->toFront (false); }
    }
    repaint();
}

// ---------------------------------------------------------------------------
juce::Rectangle<int> OrogenEditor::R (float x, float y, float w, float h) const
{
    return { juce::roundToInt (x * kx()), juce::roundToInt (y * ky()),
             juce::roundToInt (w * kx()), juce::roundToInt (h * ky()) };
}

float OrogenEditor::pv (const juce::String& id) const
{
    if (auto* r = proc.apvts.getRawParameterValue (id)) return r->load();
    if (auto* p = proc.apvts.getParameter (id)) return p->convertFrom0to1 (p->getValue());
    return 0.0f;
}

int OrogenEditor::pi (const juce::String& id) const
{
    if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        return ch->getIndex();
    return juce::roundToInt (pv (id));
}

juce::String OrogenEditor::slotId (const char* field) const
{
    return slotPrefix (selectedSlot) + field;
}

juce::StringArray OrogenEditor::itemsOf (const juce::String& id) const
{
    if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        return ch->getAllValueStrings();
    return { "OFF", "ON" };
}

juce::Component* OrogenEditor::paramsOverlay()
{
    return paramsPage.get();
}

int OrogenEditor::visiblePage() const
{
    return selectedSlot + 8 * section;
}

void OrogenEditor::showPage (int index)
{
    if (index >= 0 && index <= 7) { selectSlot (index); return; }
    if (index >= 8 && index <= 11) { setSection (index - 8); return; }
    if (index >= 100 && index <= 104)
    {
        toggleParamsPage (true);
        if (paramsPage != nullptr) { paramsPage->setTab (index - 100); paramsPage->setSection (-1); }
        return;
    }
    // 200 + tab*100 + section — конкретная секция оверлея (1 = первая, 0 = все).
    // Шаг 100, а не 10: во вкладке FX тринадцать секций, шаг 10 их перепутал бы.
    if (index >= 200)
    {
        const int tab = (index - 200) / 100;
        const int sec = (index - 200) % 100;
        toggleParamsPage (true);
        if (paramsPage != nullptr)
        {
            paramsPage->setTab (tab);
            paramsPage->setSection (sec - 1);
        }
        return;
    }
}

int OrogenEditor::paramsTabCount() const
{
    return paramsPage != nullptr ? paramsPage->tabCount() : 0;
}

// Параметры, до которых можно дотянуться с главного экрана (карточки всех
// четырёх колонок, а не только открытой) — нужно тесту «эффект есть в GUI».
// Для теста: сколько карточек-страниц в FX-колонке и листнуть её.
int OrogenEditor::fxColumnPageCount() const
{
    return columns.isEmpty() ? 0 : columns[0]->pageCountPublic();
}

int OrogenEditor::columnPageCount (int column) const
{
    if (column < 0 || column >= columns.size()) return 0;
    return columns[column]->pageCountPublic();
}

void OrogenEditor::nextColumnPage (int column)
{
    if (column < 0 || column >= columns.size()) return;
    columns[column]->nextPage();
    repaint();
}

juce::StringArray OrogenEditor::allColumnIds (int column) const
{
    if (column < 0 || column >= columns.size()) return {};
    return columns[column]->allIds();
}

void OrogenEditor::nextFxColumnPage()
{
    if (! columns.isEmpty()) columns[0]->nextPage();
}

juce::StringArray OrogenEditor::mainScreenParamIds() const
{
    juce::StringArray out;
    for (auto* c : columns) out.addArray (c->allIds());
    for (auto* k : centreKnobs) out.add (k->slider.getProperties().getWithDefault ("paramId", {}).toString());
    for (auto* b : centreBars)  out.add (b->slider.getProperties().getWithDefault ("paramId", {}).toString());
    out.removeEmptyStrings();
    return out;
}

juce::String OrogenEditor::paramsSectionName (int index) const
{
    if (paramsPage == nullptr) return {};
    auto* c = paramsPage->currentContent();
    return c != nullptr ? c->sectionName (index) : juce::String();
}

int OrogenEditor::paramsSectionCount() const
{
    return paramsPage != nullptr ? paramsPage->sectionCount() : 0;
}

// ---------------------------------------------------------------------------
void OrogenEditor::resized()
{
    masterSlider.setBounds (R (420.0f, 26.0f, 540.0f, 14.0f));
    programBox.setBounds (R (58.0f, 30.0f, 250.0f, 16.0f));
    loadButton.setBounds (R (1104.0f, 22.0f, 56.0f, 20.0f));
    saveButton.setBounds (R (1166.0f, 22.0f, 56.0f, 20.0f));

    for (int i = 0; i < cards.size(); ++i)
        cards[i]->setBounds (R (16.0f, 66.0f + 70.0f * (float) i, 216.0f, 62.0f));

    engineBox.setBounds (R (262.0f, 92.0f, 168.0f, 20.0f));
    filterBox.setBounds (R (438.0f, 92.0f, 124.0f, 20.0f));

    for (int i = 0; i < centreKnobs.size(); ++i)
    {
        const float gx = 262.0f + 200.0f * (float) i;
        centreKnobs[i]->slider.setBounds (R (gx, 484.0f, 84.0f, 84.0f));
        centreBars[i]->slider.setBounds  (R (gx + 96.0f, 486.0f, 12.0f, 88.0f));
    }

    for (int i = 0; i < bottomKnobs.size(); ++i)
    {
        const float gx = 86.0f + 170.0f * (float) i;
        bottomKnobs[i]->slider.setBounds (R (gx + 6.0f, 660.0f, 84.0f, 84.0f));
        bottomBars[i]->slider.setBounds  (R (gx + 100.0f, 662.0f, 12.0f, 80.0f));
    }

    for (auto* col : columns)
        col->setBounds (R (1000.0f, 114.0f, 264.0f, 520.0f));

    for (int i = 0; i < sectionButtons.size(); ++i)
        sectionButtons[i]->setBounds (R (20.0f, 652.0f + 33.0f * (float) i, 46.0f, 30.0f));

    for (int i = 0; i < switchButtons.size(); ++i)
        switchButtons[i]->setBounds (R (954.0f + 70.0f * (float) (i % 2),
                                         652.0f + 64.0f * (float) (i / 2), 68.0f, 58.0f));

    if (vectorPad != nullptr) vectorPad->setBounds (R (1108.0f, 646.0f, 156.0f, 138.0f));
    if (paramsPage != nullptr && paramsOpen) paramsPage->setBounds (getLocalBounds());
}

// ---------------------------------------------------------------------------

//==============================================================================
// Анимации
//
// Всё, что «дышит» на экране, считается от одной фазы — времени с момента
// создания редактора. Фазу можно выставить руками (setAnimationPhase), поэтому
// любой кадр воспроизводим: тест рендерит редактор при фазе 0 и 0.5 и сравнивает
// пиксели, а render_ui собирает из кадров GIF.
//==============================================================================

float OrogenEditor::animationPhase() const
{
    const double period = 2.0;                       // секунд на цикл
    const double p = std::fmod (animSeconds, period) / period;
    return (float) (p < 0.0 ? p + 1.0 : p);
}

void OrogenEditor::setAnimationPhase (double seconds)
{
    animSeconds = seconds;
    // фаза живёт в редакторе, но рисуют её и дочерние компоненты — раздаём всем,
    // иначе кадр снаружи (тест, render_ui) будет статичным
    // при выключенных анимациях детям отдаётся нулевая фаза: кадр замирает
    const float v = animOn ? (float) seconds : 0.0f;
    for (auto* c : columns) c->setAnimPhase (v);
    if (vectorPad != nullptr) vectorPad->setAnimPhase (v);
    for (auto* b : sectionButtons) b->setAnimPhase (v);
    repaint();
}

void OrogenEditor::repaintAnimated()
{
    // Полный repaint() на 24 кадрах/с стоил 8.8 мс (≈21 % ядра). Двигается же
    // меньше десятой доли процента площади, поэтому перерисовываем только те
    // зоны, где анимация действительно есть.
    repaint (R (988.0f, 24.0f, 100.0f, 16.0f));                    // LED маршрутов
    repaint (R (16.0f, 66.0f + 70.0f * (float) selectedSlot, 216.0f, 62.0f));  // выбранный слот
    repaint (R (248.0f, 348.0f, 736.0f, 62.0f));                   // дорожка LFO с playhead
    repaint (R (248.0f, 570.0f, 736.0f, 70.0f));                   // чипы FX + их страницы

    for (auto* c : columns) if (c->isVisible()) c->repaintCables();  // кометы в кабелях
    if (vectorPad != nullptr) vectorPad->repaint();                // дыхание точки
    for (auto* b : sectionButtons) b->repaint();                   // пульс активной секции
}

int OrogenEditor::animatedAreaPercent() const
{
    // та же геометрия, что в repaintAnimated(), в процентах от 1280×800
    const double area = 100.0 * 16.0 + 216.0 * 62.0 + 736.0 * 62.0 + 736.0 * 70.0;
    double total = area;
    for (auto* c : columns) if (c->isVisible()) total += (double) c->cableAreaPx();
    if (vectorPad != nullptr) total += (double) vectorPad->getWidth() * (double) vectorPad->getHeight();
    for (auto* b : sectionButtons) total += (double) b->getWidth() * (double) b->getHeight();
    return juce::jlimit (0, 100, juce::roundToInt (100.0 * total / (1280.0 * 800.0)));
}

void OrogenEditor::setAnimationsEnabled (bool on)
{
    animOn = on;
    const float v = on ? (float) animSeconds : 0.0f;
    for (auto* c : columns) c->setAnimPhase (v);
    if (vectorPad != nullptr) vectorPad->setAnimPhase (v);
    for (auto* b : sectionButtons) b->setAnimPhase (v);
    repaint();
}

float OrogenEditor::animPhaseFor (double speed, double offset) const
{
    const double p = animSeconds * speed + offset;
    const double f = p - std::floor (p);
    return (float) f;
}

void OrogenEditor::timerCallback()
{
    // время считаем по часам, а не по числу тиков: при просадках кадров
    // анимация замедляется, но не «скачет»
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = lastTimerMs > 0.0 ? juce::jlimit (0.0, 0.25, (now - lastTimerMs) / 1000.0) : 0.0;
    lastTimerMs = now;

    bool changed = false;

    if (animOn && dt > 0.0)
    {
        animSeconds += dt;
        for (auto* c : columns) c->setAnimPhase ((float) animSeconds);
        if (vectorPad != nullptr) vectorPad->setAnimPhase ((float) animSeconds);
        for (auto* b : sectionButtons) b->setAnimPhase ((float) animSeconds);
        repaintAnimated();                  // только живые зоны, не весь экран
    }

    float db[8] = { -120.0f, -120.0f, -120.0f, -120.0f, -120.0f, -120.0f, -120.0f, -120.0f };
    proc.engine().slotLevelsDb (db);

    for (int i = 0; i < 8; ++i)
    {
        // метры: спад не мгновенный, а с постоянной времени ~120 мс
        const float target = db[i];
        float& sm = slotLevelsSmooth[(size_t) i];
        sm += (target - sm) * (dt > 0.0 ? (float) juce::jmin (1.0, dt / 0.12) : 0.2f);
        if (std::abs (sm - slotLevels[(size_t) i]) > 0.2f) { slotLevels[(size_t) i] = sm; changed = true; }
    }

    // программа могла смениться извне (хост, MIDI program change) — держим
    // селектор в шапке синхронным с банком
    if (programBox.getSelectedItemIndex() != proc.getCurrentProgram())
    {
        programBox.setSelectedItemIndex (proc.getCurrentProgram(), juce::dontSendNotification);
        changed = true;
    }

    if (changed) repaint();
}

// ---------------------------------------------------------------------------
void OrogenEditor::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg);
    g.saveState();
    g.addTransform (juce::AffineTransform::scale (kx(), ky()));

    paintTopBar (g);
    paintRail (g);
    paintDisplay (g);
    paintModEditor (g);
    paintCentreKnobs (g);
    paintChips (g);
    paintColumnFrame (g);
    paintBottom (g);

    g.restoreState();
}

// ------- отрисовка: шапка ---------------------------------------------------
void OrogenEditor::paintTopBar (juce::Graphics& g)
{
    draw::panel (g, { 16.0f, 12.0f, 1248.0f, 42.0f }, 3.0f);

    draw::engineGlyph (g, 0, { 30.0f, 22.0f, 22.0f, 22.0f }, Palette::text);

    g.setColour (Palette::text);
    g.setFont (orFont (11.0f, true));
    g.drawText (spaced ("OROGEN"), juce::Rectangle<int> (58, 17, 160, 16),
                juce::Justification::centredLeft, false);

    draw::caption (g, "master", { 420, 13, 120, 10 }, Palette::textFaint, 6.5f);

    // индикаторы первых четырёх маршрутов мод-матрицы: активный дышит,
    // у каждого маршрута своя задержка — видно, что они независимы
    for (int r = 0; r < 4; ++r)
    {
        const float amt = pv ("mod" + juce::String (r + 1) + "_amt");
        const bool on = std::abs (amt) > 0.001f;
        const float pulse = animOn
            ? 0.45f + 0.55f * (0.5f + 0.5f * std::sin ((animPhaseFor (0.5, 0.18 * r)) * juce::MathConstants<float>::twoPi))
            : 1.0f;
        draw::led (g, { 1000.0f + 24.0f * (float) r, 33.0f }, 3.0f, on,
                   Palette::signal.withAlpha (on ? pulse : 1.0f));
    }
}

// ------- отрисовка: рейл A–H --------------------------------------------------
namespace
{
    const char* engineName (int type)
    {
        static const char* n[9] = { "WAVETABLE", "FM", "HARMONIC", "SAMPLE", "GRANULAR",
                                    "MODAL", "RESYNTH", "ORGANICS", "CHOP" };
        return n[(size_t) juce::jlimit (0, 8, type)];
    }

    const char* filterName (int f)
    {
        static const char* n[16] = { "LP12", "LP24", "BP12", "HP24", "NOTCH12", "LP6", "HP6", "HP12",
                                     "BP24", "NOTCH24", "PEAK", "ALLPASS", "LP36", "LADDER", "COMB", "VOWEL" };
        return n[(size_t) juce::jlimit (0, 15, f)];
    }

    // Мини-волна типа движка: та же форма, что и иконка, но растянутая в линию.
    void miniWave (juce::Graphics& g, juce::Rectangle<float> r, int type, float amp, juce::Colour c)
    {
        juce::Path p;
        const int N = 96;
        for (int i = 0; i <= N; ++i)
        {
            const float t = (float) i / (float) N;
            const float ph = t * juce::MathConstants<float>::twoPi;
            float v = 0.0f;

            switch (type)
            {
                case 0: v = std::sin (ph); break;
                case 1: v = std::sin (ph * 2.0f + 2.2f * std::sin (ph * 3.0f)); break;
                case 2: v = 0.55f * std::sin (ph) + 0.30f * std::sin (ph * 3.0f) + 0.15f * std::sin (ph * 7.0f); break;
                case 3: v = std::sin (ph) * (0.6f + 0.4f * std::sin (ph * 5.0f)); break;
                case 4: v = std::sin (ph) * (0.35f + 0.65f * std::exp (-3.0f * std::fmod (t * 6.0f, 1.0f))); break;
                case 5: v = std::exp (-2.4f * std::fmod (t * 3.0f, 1.0f)) * std::sin (ph * 6.0f); break;
                case 6: v = std::sin (ph) > 0.0f ? 0.85f : -0.35f * std::sin (ph); break;
                case 7: v = 0.7f * std::sin (ph) + 0.3f * std::sin (ph * 2.0f + 1.2f * std::sin (ph)); break;
                default: v = std::floor (std::sin (ph) * 3.0f) / 3.0f; break;
            }

            const float x = r.getX() + t * r.getWidth();
            const float y = r.getCentreY() - v * amp * r.getHeight() * 0.5f;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (1.1f));
    }
}

void OrogenEditor::paintRail (juce::Graphics& g)
{
    draw::panel (g, { 16.0f, 66.0f, 216.0f, 560.0f }, 3.0f, juce::Colours::transparentBlack, Palette::line);

    for (int i = 0; i < kNumSlots; ++i)
    {
        const float y = 66.0f + 70.0f * (float) i;
        const bool sel = (i == selectedSlot);
        const bool on  = pv (slotPrefix (i) + "on") > 0.5f;
        const int type = pi (slotPrefix (i) + "type");

        if (sel)
        {
            // выбранный слот «дышит» рамкой: 0.55…1.0 от яркости линии
            const float pulse = animOn ? 0.55f + 0.45f * (0.5f + 0.5f * std::sin (
                                    animPhaseFor (0.5) * juce::MathConstants<float>::twoPi)) : 1.0f;
            draw::panel (g, { 16.0f, y, 216.0f, 62.0f }, 3.0f, Palette::panelAlt,
                         Palette::lineHi.withMultipliedAlpha (pulse));
        }
        else
        {
            draw::panel (g, { 16.0f, y, 216.0f, 62.0f }, 3.0f, Palette::panel, Palette::line);
        }

        // уровень слота (реальные метры из EngineBank, дБ → 0…1)
        const float lvl = juce::jlimit (0.0f, 1.0f, (slotLevels[(size_t) i] + 54.0f) / 54.0f);
        draw::led (g, { 28.0f, y + 31.0f }, 3.2f, on,
                   Palette::slot (i).withMultipliedAlpha (0.30f + 0.70f * lvl));

        g.setColour (sel ? Palette::text : Palette::textDim);
        g.setFont (orFont (9.0f, true));
        g.drawText (juce::String::charToString ((juce::juce_wchar) kSlotLetters[i]),
                    juce::Rectangle<int> (10, (int) y + 6, 20, 12),
                    juce::Justification::centred, false);

        const juce::Rectangle<float> iconBox (40.0f, y + 11.0f, 34.0f, 34.0f);
        g.setColour (Palette::line);
        g.drawRoundedRectangle (iconBox.reduced (0.5f), 2.5f, 1.0f);
        draw::engineGlyph (g, type, iconBox.reduced (8.0f), on ? Palette::text : Palette::textFaint);

        draw::caption (g, engineName (type), { 80, (int) y + 6, 130, 10 },
                       on ? Palette::textDim : Palette::textFaint, 6.5f);

        const float amp = (on ? 1.0f : 0.45f) * (0.35f + 0.65f * pv (slotPrefix (i) + "level"));
        miniWave (g, { 80.0f, y + 18.0f, 128.0f, 17.0f }, type, amp,
                  (on ? Palette::signal : Palette::textFaint).withAlpha (on ? 0.85f : 0.45f));
    }
}

// ------- отрисовка: дисплей формы волны -------------------------------------
void OrogenEditor::paintDisplay (juce::Graphics& g)
{
    const juce::Rectangle<float> area (248.0f, 66.0f, 736.0f, 214.0f);
    draw::panel (g, area, 3.0f);

    const auto inner = area.reduced (12.0f, 10.0f).withTrimmedTop (34.0f);

    g.setColour (Palette::grid);
    for (float x = inner.getX(); x < inner.getRight(); x += 46.0f)
        g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
    for (float y = inner.getY(); y < inner.getBottom(); y += 30.0f)
        g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());

    g.setColour (Palette::lineMid);
    g.drawHorizontalLine ((int) inner.getCentreY(), inner.getX(), inner.getRight());

    const int type = pi (slotId ("type"));
    const int table = juce::jlimit (0, 7, pi (slotId ("table")));
    const auto& bank = waveStore().get();
    juce::String tableName = "-";

    if (table < (int) bank.tables.size())
    {
        const auto& wt = bank.tables[(size_t) table];
        tableName = juce::String (wt.name.c_str()).toUpperCase();

        const int frames = juce::jmax (1, wt.frames);
        const float pos = juce::jlimit (0.0f, 1.0f,
                                       0.70f * pv (slotId ("p0")) + 0.30f * pv (slotId ("p1")));
        const int frame = juce::jlimit (0, frames - 1,
                                        juce::roundToInt (pos * (float) (frames - 1)));

        if (! wt.levels.empty() && ! wt.lens.empty() && wt.lens[0] > 8)
        {
            const int len = wt.lens[0];
            const float* data = wt.levels[0].data() + (size_t) frame * (size_t) len;

            const int N = 640;
            juce::Path wave, body;
            for (int i = 0; i <= N; ++i)
            {
                const float t = (float) i / (float) N;
                const int idx = juce::jlimit (0, len - 1, (int) (t * (float) (len - 1)));
                const float v = juce::jlimit (-1.4f, 1.4f, data[(size_t) idx]);

                const float x = inner.getX() + t * inner.getWidth();
                const float y = inner.getCentreY() - v * inner.getHeight() * 0.42f;

                if (i == 0) { wave.startNewSubPath (x, y); body.startNewSubPath (x, inner.getCentreY()); body.lineTo (x, y); }
                else        { wave.lineTo (x, y); body.lineTo (x, y); }
            }
            body.lineTo (inner.getRight(), inner.getCentreY());
            body.closeSubPath();

            g.setColour (Palette::signal.withAlpha (0.045f));
            g.fillPath (body);
            draw::glowStroke (g, wave, 1.7f, Palette::signal, 0.30f);
        }
    }

    draw::caption (g, "engine", { (int) area.getX() + 14, (int) area.getY() + 18, 120, 10 },
                   Palette::textFaint, 6.0f);
    draw::caption (g, "filter", { (int) area.getX() + 190, (int) area.getY() + 18, 100, 10 },
                   Palette::textFaint, 6.0f);

    draw::caption (g, tableName + "  " + juce::String (juce::roundToInt (pv (slotId ("table")))) + "/8",
                   { (int) inner.getX(), (int) inner.getBottom() - 14, 300, 12 },
                   Palette::textFaint, 6.5f);

    g.setColour (Palette::textFaint);
    g.setFont (orFont (7.0f));
    g.drawText ("SLOT " + juce::String::charToString ((juce::juce_wchar) kSlotLetters[selectedSlot]),
                juce::Rectangle<int> ((int) area.getRight() - 132, (int) area.getY() + 8, 120, 12),
                juce::Justification::centredRight, false);
}

// ------- отрисовка: мод-редактор (3 дорожки) --------------------------------
void OrogenEditor::paintModEditor (juce::Graphics& g)
{
    const juce::Rectangle<float> area (248.0f, 290.0f, 736.0f, 178.0f);
    draw::panel (g, area, 3.0f);

    const float laneH = area.getHeight() / 3.0f;
    for (int i = 1; i < 3; ++i)
    {
        g.setColour (Palette::grid);
        g.drawHorizontalLine ((int) (area.getY() + laneH * (float) i), area.getX() + 8.0f, area.getRight() - 8.0f);
    }

    // --- дорожка 1: 8 маршрутов мод-матрицы (ступенчатая линия) ---
    {
        const auto lane = juce::Rectangle<float> (area.getX() + 14.0f, area.getY() + 8.0f,
                                                  area.getWidth() - 28.0f, laneH - 18.0f);
        juce::Path step;
        const int n = 8;
        const float stepW = lane.getWidth() / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const float amt = pv ("mod" + juce::String (i + 1) + "_amt");
            const float y = lane.getCentreY() - juce::jlimit (-1.0f, 1.0f, amt) * lane.getHeight() * 0.46f;
            const float x0 = lane.getX() + stepW * (float) i;
            const float x1 = x0 + stepW;

            if (i == 0) step.startNewSubPath (x0, y); else step.lineTo (x0, y);
            step.lineTo (x1, y);
        }
        g.setColour (Palette::lineMid);
        g.drawHorizontalLine ((int) lane.getCentreY(), lane.getX(), lane.getRight());
        draw::glowStroke (g, step, 1.3f, Palette::signal.withAlpha (0.9f), 0.18f);

        // точки на каждом маршруте — как «узлы» на макете
        for (int i = 0; i < n; ++i)
        {
            const float amt = pv ("mod" + juce::String (i + 1) + "_amt");
            const float y = lane.getCentreY() - juce::jlimit (-1.0f, 1.0f, amt) * lane.getHeight() * 0.46f;
            const float x = lane.getX() + stepW * ((float) i + 0.5f);
            g.setColour (Palette::signal.withAlpha (0.9f));
            g.fillEllipse (x - 2.2f, y - 2.2f, 4.4f, 4.4f);
        }

        draw::caption (g, "mod matrix 1-8", { (int) lane.getRight() - 110, (int) lane.getY() - 4, 108, 10 },
                       Palette::textFaint, 6.0f, juce::Justification::centredRight);
    }

    // --- дорожка 2: LFO 1 (форма из параметра shape, амплитуда из amount) ---
    {
        const auto lane = juce::Rectangle<float> (area.getX() + 14.0f, area.getY() + laneH + 8.0f,
                                                  area.getWidth() - 28.0f, laneH - 18.0f);
        const int shape = pi ("lfo1_shape");
        const float amount = pv ("lfo1_amount");
        const float rate = pv ("lfo1_rate");
        const int cycles = juce::jlimit (1, 6, juce::roundToInt (rate * 0.6f) + 1);

        juce::Path curve;
        const int N = 400;
        for (int i = 0; i <= N; ++i)
        {
            const float t = (float) i / (float) N;
            const float ph = t * juce::MathConstants<float>::twoPi * (float) cycles;
            float v = 0.0f;
            if (shape == 0)      v = std::sin (ph);
            else if (shape == 1) v = 2.0f * std::abs (2.0f * (float) (ph / juce::MathConstants<float>::twoPi
                                                                    - std::floor (ph / juce::MathConstants<float>::twoPi + 0.5f))) - 1.0f;
            else                 v = ((int) (ph * 3.0f) % 5) / 2.0f - 1.0f;

            const float x = lane.getX() + t * lane.getWidth();
            const float y = lane.getCentreY() - v * (0.15f + 0.85f * amount) * lane.getHeight() * 0.46f;
            if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        g.setColour (Palette::lineMid);
        g.drawHorizontalLine ((int) lane.getCentreY(), lane.getX(), lane.getRight());
        draw::glowStroke (g, curve, 1.3f, Palette::signal.withAlpha (0.9f), 0.18f);

        // «ручки» на четвертях — узлы, как на макете
        for (int i = 0; i <= 3; ++i)
        {
            const float t = (float) i / 3.0f;
            const float x = lane.getX() + t * lane.getWidth();
            g.setColour (Palette::signal);
            g.fillEllipse (x - 2.0f, lane.getCentreY() - 2.0f, 4.0f, 4.0f);
        }

        // playhead: точка, бегущая по кривой с частотой, взятой из параметра
        // lfo1_rate. Это визуализация частоты (та же шкала, что у cycles выше),
        // а не «декоративная точка»: остановишь LFO — остановится и она.
        if (animOn)
        {
            const float t = animPhaseFor (0.25 + 1.75 * (double) rate);
            const float ph = t * juce::MathConstants<float>::twoPi * (float) cycles;
            float v = 0.0f;
            if (shape == 0)      v = std::sin (ph);
            else if (shape == 1) v = 2.0f * std::abs (2.0f * (float) (ph / juce::MathConstants<float>::twoPi
                                                                    - std::floor (ph / juce::MathConstants<float>::twoPi + 0.5f))) - 1.0f;
            else                 v = ((int) (ph * 3.0f) % 5) / 2.0f - 1.0f;

            const float px = lane.getX() + t * lane.getWidth();
            const float py = lane.getCentreY() - v * (0.15f + 0.85f * amount) * lane.getHeight() * 0.46f;
            g.setColour (Palette::signal.withAlpha (0.16f));
            g.fillEllipse (px - 5.0f, py - 5.0f, 10.0f, 10.0f);
            g.setColour (Palette::signal);
            g.fillEllipse (px - 2.0f, py - 2.0f, 4.0f, 4.0f);
        }

        draw::caption (g, "lfo 1", { (int) lane.getRight() - 110, (int) lane.getY() - 4, 108, 10 },
                       Palette::textFaint, 6.0f, juce::Justification::centredRight);
    }

    // --- дорожка 3: огибающая выбранного слота (ADSR) ---
    {
        const auto lane = juce::Rectangle<float> (area.getX() + 14.0f, area.getY() + laneH * 2.0f + 8.0f,
                                                  area.getWidth() - 28.0f, laneH - 18.0f);
        const float a = pv (slotId ("attack"));
        const float d = pv (slotId ("decay"));
        const float s = pv (slotId ("sustain"));
        const float r = pv (slotId ("release"));

        const float total = juce::jmax (0.05f, a + d + juce::jmax (0.15f, (a + d) * 0.35f) + r);
        const float top = lane.getY() + 2.0f;
        const float bot = lane.getBottom();
        const float ySus = bot - (bot - top) * juce::jlimit (0.0f, 1.0f, s);

        const float xA = lane.getX() + lane.getWidth() * (a / total);
        const float xD = xA + lane.getWidth() * (d / total);
        const float xS = xD + lane.getWidth() * (juce::jmax (0.15f, (a + d) * 0.35f) / total);
        const float xR = lane.getX() + lane.getWidth();

        juce::Path env, fill;
        env.startNewSubPath (lane.getX(), bot);
        env.lineTo (xA, top);
        env.lineTo (xD, ySus);
        env.lineTo (xS, ySus);
        env.lineTo (xR, bot);

        fill.startNewSubPath (lane.getX(), bot);
        fill.lineTo (xA, top);
        fill.lineTo (xD, ySus);
        fill.lineTo (xS, ySus);
        fill.lineTo (xR, bot);
        fill.closeSubPath();

        g.setColour (Palette::signal.withAlpha (0.07f));
        g.fillPath (fill);
        draw::glowStroke (g, env, 1.3f, Palette::signal.withAlpha (0.92f), 0.22f);

        for (const auto& pt : { juce::Point<float> (xA, top), juce::Point<float> (xD, ySus),
                                juce::Point<float> (xS, ySus) })
        {
            g.setColour (Palette::signal);
            g.fillEllipse (pt.x - 2.4f, pt.y - 2.4f, 4.8f, 4.8f);
        }

        draw::caption (g, "env a/d/s/r", { (int) lane.getRight() - 110, (int) lane.getY() - 4, 108, 10 },
                       Palette::textFaint, 6.0f, juce::Justification::centredRight);
    }
}

// ------- отрисовка: три ручки центра ---------------------------------------
void OrogenEditor::paintCentreKnobs (juce::Graphics& g)
{
    struct L { const char* name; const char* bar; };
    static const L labels[3] = { { "CUTOFF", "RESO" }, { "SHAPE", "P2" }, { "LEVEL", "DETUNE" } };

    for (int i = 0; i < 3; ++i)
    {
        const float gx = 262.0f + 200.0f * (float) i;

        draw::caption (g, labels[i].name, { (int) gx, 570, 96, 12 }, Palette::textDim, 7.5f,
                       juce::Justification::centred);

        g.setColour (Palette::text);
        g.setFont (orFont (8.0f));
        g.drawText (centreKnobs[i]->slider.getTextFromValue (centreKnobs[i]->slider.getValue()),
                    juce::Rectangle<int> ((int) gx + 112, 520, 84, 14),
                    juce::Justification::centredLeft, false);

        draw::caption (g, labels[i].bar, { (int) (gx + 92), 570, 44, 12 }, Palette::textFaint, 6.0f,
                       juce::Justification::centredLeft);
    }
}

// ------- отрисовка: цепь эффектов ------------------------------------------
void OrogenEditor::paintChips (juce::Graphics& g)
{
    // чипы эффектов: по 5 на страницу, все 10 эффектов с ручкой доли
    for (int i = 0; i < kChipsPerPage; ++i)
    {
        const int idx = chipPage * kChipsPerPage + i;
        if (idx >= kChipTotal) break;

        const auto r = chipRect (i);
        const float mix = juce::jlimit (0.0f, 1.0f, pv (kChips[idx].id));
        const bool on = mix > 0.005f;
        // включённый чип подсвечивается волной: задержка по индексу
        const float glow = (animOn && on)
            ? 0.72f + 0.28f * (0.5f + 0.5f * std::sin ((animPhaseFor (0.4, 0.12 * i)) * juce::MathConstants<float>::twoPi))
            : 1.0f;

        draw::panel (g, r, 3.0f, on ? Palette::panelAlt : Palette::panel,
                     on ? Palette::lineMid : Palette::line);

        draw::fxGlyph (g, kChips[idx].glyph, { r.getX() + 9.0f, r.getCentreY() - 8.0f, 16.0f, 16.0f },
                       on ? Palette::text : Palette::textFaint);

        g.setColour (on ? Palette::textDim.withMultipliedAlpha (glow) : Palette::textFaint);
        g.setFont (orFont (6.8f));
        g.drawText (spaced (kChips[idx].name),
                    juce::Rectangle<int> ((int) r.getX() + 30, (int) r.getY() + 6, 80, 10),
                    juce::Justification::centredLeft, false);

        // индикатор доли эффекта в сигнале
        juce::Rectangle<float> bar (r.getX() + 30.0f, r.getY() + 20.0f, 78.0f, 3.0f);
        g.setColour (Palette::lineMid);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (Palette::signal.withAlpha (0.9f * glow));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (0.0f, bar.getWidth() * mix)), 1.5f);

        // пунктирный «кабель» между чипами
        if (i < kChipsPerPage - 1 && idx + 1 < kChipTotal)
        {
            const float x0 = r.getRight() + 3.0f, x1 = r.getRight() + 23.0f;
            const float y = r.getCentreY();
            for (float d = x0; d < x1; d += 5.0f)
            {
                g.setColour (Palette::lineMid);
                g.fillRect (d, y, 2.5f, 1.0f);
            }
            g.setColour (Palette::lineMid);
            g.fillEllipse (x1 + 1.0f, y - 1.5f, 3.0f, 3.0f);
        }
    }

    // кнопка страниц чипов (та же логика, что у колонки FX CHAIN)
    if (chipPageCount() > 1)
    {
        const auto rr = chipPagerRect().reduced (0.5f);
        draw::panel (g, rr, 3.0f, Palette::panelAlt, Palette::line);
        g.setColour (Palette::textDim);
        g.setFont (orFont (7.5f));
        g.drawText ("<  " + juce::String (chipPage + 1) + " / " + juce::String (chipPageCount()) + "  >",
                    rr.toNearestInt(), juce::Justification::centred, false);
    }
}

// ------- отрисовка: рамка патч-колонки --------------------------------------
void OrogenEditor::paintColumnFrame (juce::Graphics& g)
{
    draw::panel (g, { 1000.0f, 66.0f, 264.0f, 40.0f }, 3.0f);
    draw::panel (g, { 1000.0f, 114.0f, 264.0f, 520.0f }, 3.0f);

    // иконка-«квадрат» слева в шапке
    g.setColour (Palette::line);
    g.drawRoundedRectangle (juce::Rectangle<float> (1012.0f, 78.0f, 16.0f, 16.0f), 2.0f, 1.0f);

    static const char* titles[4] = { "FX CHAIN", "MOD MATRIX", "ARP", "LFO" };
    draw::caption (g, titles[juce::jlimit (0, 3, section)],
                   juce::Rectangle<int> (1036, 74, 160, 20), Palette::text, 7.5f);

    // кнопка «матрица» справа в шапке — открывает все параметры
    const juce::Rectangle<float> m (1232.0f, 78.0f, 20.0f, 16.0f);
    g.setColour (paramsOpen ? Palette::lineHi : Palette::line);
    g.drawRoundedRectangle (m, 2.0f, 1.0f);
    for (int i = 0; i < 4; ++i)
    {
        g.setColour (paramsOpen ? Palette::signal : Palette::textDim);
        g.fillEllipse (m.getX() + 4.0f + 6.0f * (float) (i % 2), m.getY() + 4.0f + 6.0f * (float) (i / 2),
                       2.4f, 2.4f);
    }
}

// ------- отрисовка: нижняя полоса -------------------------------------------
void OrogenEditor::paintBottom (juce::Graphics& g)
{
    draw::panel (g, { 16.0f, 646.0f, 1248.0f, 138.0f }, 3.0f);
    draw::panel (g, { 16.0f, 646.0f, 54.0f, 138.0f }, 3.0f, juce::Colours::transparentBlack, Palette::line);
    draw::panel (g, { 948.0f, 646.0f, 148.0f, 138.0f }, 3.0f, juce::Colours::transparentBlack, Palette::line);

    static const char* labels[5] = { "MASTER", "PATCH GAIN", "MORPH", "ARP GATE", "LFO 1 RATE" };
    static const char* bars[5]   = { "MOD WHEEL", "WIDTH", "GLIDE", "SWING", "AMOUNT" };

    for (int i = 0; i < 5; ++i)
    {
        const float gx = 86.0f + 170.0f * (float) i;

        draw::caption (g, labels[i], { (int) gx + 6, 748, 160, 12 }, Palette::textDim, 7.5f);
        draw::caption (g, bars[i],   { (int) gx + 6, 762, 160, 12 }, Palette::textFaint, 6.0f);

        g.setColour (Palette::text);
        g.setFont (orFont (8.0f));
        g.drawText (bottomKnobs[i]->slider.getTextFromValue (bottomKnobs[i]->slider.getValue()),
                    juce::Rectangle<int> ((int) gx + 116, 690, 48, 12),
                    juce::Justification::centredLeft, false);
    }
}

void OrogenEditor::paintVectorPad (juce::Graphics& g)
{
    juce::ignoreUnused (g);   // рисует сам компонент VectorPad
}

// ---------------------------------------------------------------------------

//==============================================================================
// Банк программ в интерфейсе
//
// Банк (все программы одним JSON) жил только в API процессора: из GUI можно
// было загрузить/сохранить один патч файлом, а набор — нет. Теперь правый клик
// по селектору программ открывает меню банка, и те же операции доступны
// методами ниже (ими пользуется тест, поэтому проверяется настоящий путь).
//==============================================================================

void OrogenEditor::refreshProgramList()
{
    programBox.clear (juce::dontSendNotification);
    const auto names = proc.presetNames();
    for (int i = 0; i < names.size(); ++i)
        programBox.addItem (names[i].toUpperCase(), i + 1);
    programBox.setSelectedItemIndex (juce::jlimit (0, juce::jmax (0, names.size() - 1),
                                                   proc.getCurrentProgram()),
                                     juce::dontSendNotification);
    repaint();
}

int OrogenEditor::programItemCount() const
{
    return programBox.getNumItems();
}

bool OrogenEditor::saveBankTo (const juce::File& f)
{
    return proc.saveBankFile (f);
}

bool OrogenEditor::loadBankFrom (const juce::File& f)
{
    const bool ok = proc.loadBankFile (f);
    if (ok) refreshProgramList();
    return ok;
}

int OrogenEditor::addProgramFromCurrent (const juce::String& name)
{
    const int idx = proc.addProgram (name);
    refreshProgramList();
    return idx;
}

juce::File OrogenEditor::quickBankFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("OROGEN").getChildFile ("bank.json");
}

bool OrogenEditor::saveBankQuick()
{
    const auto f = quickBankFile();
    f.getParentDirectory().createDirectory();
    return proc.saveBankFile (f);
}

bool OrogenEditor::loadBankQuick()
{
    const auto f = quickBankFile();
    if (! f.existsAsFile()) return false;
    return loadBankFrom (f);
}

void OrogenEditor::showIoMenu (bool saving)
{
    juce::PopupMenu menu;
    menu.addSectionHeader (saving ? "SAVE" : "LOAD");
    if (saving)
    {
        menu.addItem (1, "Save patch...");
        menu.addItem (2, "Save bank...");
        menu.addItem (3, "Save bank (quick)");
    }
    else
    {
        menu.addItem (4, "Load patch...");
        menu.addItem (5, "Load bank...");
        menu.addItem (6, "Load bank (quick)");
    }

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (saving ? &saveButton : &loadButton),
                        [this] (int result)
    {
        switch (result)
        {
            case 1: savePresetToFile(); break;
            case 2:
            {
                auto chooser = std::make_shared<juce::FileChooser> ("Save bank",
                    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                        .getChildFile ("OROGEN-bank.json"), "*.json");
                chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [this, chooser] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (f != juce::File()) saveBankTo (f.withFileExtension ("json"));
                });
                break;
            }
            case 3: saveBankQuick(); break;
            case 4: loadPresetFromFile(); break;
            case 5:
            {
                auto chooser = std::make_shared<juce::FileChooser> ("Load bank",
                    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.json");
                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [this, chooser] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (f.existsAsFile()) loadBankFrom (f);
                });
                break;
            }
            case 6: loadBankQuick(); break;
            default: break;
        }
    });
}

void OrogenEditor::showBankMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("BANK");
    menu.addItem (1, "Save bank...");
    menu.addItem (2, "Load bank...");
    menu.addSeparator();
    menu.addItem (3, "Add current patch as program...");
    menu.addSeparator();
    menu.addItem (7, animOn ? "Animations: on" : "Animations: off", true, false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (programBox),
                        [this] (int result)
    {
        if (result == 1)
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Save bank",
                                                               juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                                   .getChildFile ("OROGEN-bank.json"),
                                                               "*.json");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this, chooser] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File()) saveBankTo (f.withFileExtension ("json"));
            });
        }
        else if (result == 2)
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Load bank",
                                                               juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                               "*.json");
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this, chooser] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f.existsAsFile()) loadBankFrom (f);
            });
        }
        else if (result == 3)
        {
            static int n = 1;
            addProgramFromCurrent ("USER " + juce::String (n++));
        }
        else if (result == 7)
        {
            setAnimationsEnabled (! animOn);
        }
    });
}

//==============================================================================
// Интерактивные зоны главного экрана
//
// До этой правки чипы FX и дорожки мод-редактора рисовались по настоящим
// параметрам, но не принимали ввод: значение можно было менять только в
// оверлее. Теперь любой узел, нарисованный на главном экране, тянется мышью,
// а чип эффекта включается и выключается кликом.
//==============================================================================

namespace
{
    // сколько пикселей макета соответствует полному ходу параметра
    constexpr float kPxPerFullRange = 260.0f;
    constexpr float kNodeGrabPx = 13.0f;
}

juce::Rectangle<float> OrogenEditor::chipRect (int i) const
{
    return { 254.0f + 146.0f * (float) i, 598.0f, 120.0f, 34.0f };
}

int OrogenEditor::chipCount() const { return kChipTotal; }

int OrogenEditor::chipPageCount() const
{
    return (kChipTotal + kChipsPerPage - 1) / kChipsPerPage;
}

juce::String OrogenEditor::chipParamId (int index) const
{
    if (index < 0 || index >= kChipTotal) return {};
    return kChips[index].id;
}


juce::Rectangle<float> OrogenEditor::modLane (int lane) const
{
    const juce::Rectangle<float> area (248.0f, 290.0f, 736.0f, 178.0f);
    const float laneH = area.getHeight() / 3.0f;
    return { area.getX() + 14.0f, area.getY() + laneH * (float) lane + 8.0f,
             area.getWidth() - 28.0f, laneH - 18.0f };
}

void OrogenEditor::envNodePoints (juce::Point<float>& aEnd, juce::Point<float>& dEnd,
                                  juce::Point<float>& sEnd, float& ySus) const
{
    const auto lane = modLane (2);
    const float a = pv (slotId ("attack"));
    const float d = pv (slotId ("decay"));
    const float s = pv (slotId ("sustain"));
    const float r = pv (slotId ("release"));

    const float total = juce::jmax (0.05f, a + d + juce::jmax (0.15f, (a + d) * 0.35f) + r);
    const float top = lane.getY() + 2.0f;
    const float bot = lane.getBottom();
    ySus = bot - (bot - top) * juce::jlimit (0.0f, 1.0f, s);

    const float xA = lane.getX() + lane.getWidth() * (a / total);
    const float xD = xA + lane.getWidth() * (d / total);
    const float xS = xD + lane.getWidth() * (juce::jmax (0.15f, (a + d) * 0.35f) / total);

    aEnd = { xA, top };
    dEnd = { xD, ySus };
    sEnd = { xS, ySus };
}

OrogenEditor::MainHit OrogenEditor::hitMain (juce::Point<float> p, int& index) const
{
    index = -1;

    if (chipPagerRect().contains (p)) { index = -1; return MainHit::fxChipPage; }

    for (int i = 0; i < kChipsPerPage && chipPage * kChipsPerPage + i < kChipTotal; ++i)
        if (chipRect (i).contains (p))
        {
            index = chipPage * kChipsPerPage + i;
            return MainHit::fxChip;
        }

    // дисплей: горизонталь — позиция в таблице (p0), вертикаль — варп (p1)
    if (displayRect().contains (p)) { index = -1; return MainHit::display; }

    // дорожка мод-матрицы: узел каждого из 8 маршрутов
    {
        const auto lane = modLane (0);
        const float stepW = lane.getWidth() / 8.0f;
        for (int i = 0; i < 8; ++i)
        {
            const float amt = pv ("mod" + juce::String (i + 1) + "_amt");
            const float y = lane.getCentreY() - juce::jlimit (-1.0f, 1.0f, amt) * lane.getHeight() * 0.46f;
            const juce::Point<float> node (lane.getX() + stepW * ((float) i + 0.5f), y);
            if (node.getDistanceFrom (p) <= kNodeGrabPx) { index = i; return MainHit::modNode; }
        }
        // попадание в саму полосу (но не в узел) — тоже тянем ближайший маршрут
        if (lane.contains (p))
        {
            index = juce::jlimit (0, 7, (int) ((p.x - lane.getX()) / stepW));
            return MainHit::modNode;
        }
    }

    // дорожка LFO 1: горизонталь — частота, вертикаль — глубина
    if (modLane (1).contains (p)) return MainHit::lfoLane;

    // дорожка конверта: три узла (attack · decay · sustain/release)
    {
        juce::Point<float> aEnd, dEnd, sEnd; float ySus = 0.0f;
        envNodePoints (aEnd, dEnd, sEnd, ySus);
        if (aEnd.getDistanceFrom (p) <= kNodeGrabPx)               return MainHit::envAttack;
        if (dEnd.getDistanceFrom (p) <= kNodeGrabPx)               return MainHit::envDecay;
        if (sEnd.getDistanceFrom (p) <= kNodeGrabPx)               return MainHit::envSustain;
        if (modLane (2).contains (p))                              return MainHit::envSustain;
    }

    return MainHit::none;
}

bool OrogenEditor::nudgeParam (const juce::String& id, float deltaNorm)
{
    if (auto* prm = proc.apvts.getParameter (id))
    {
        const float n = juce::jlimit (0.0f, 1.0f, prm->getValue() + deltaNorm);
        prm->setValueNotifyingHost (n);
        return true;
    }
    return false;
}

int OrogenEditor::mainScreenHitCount() const
{
    // дисплей + 5 чипов на странице + кнопка страниц + 8 узлов матрицы + полоса LFO + 3 узла конверта
    return 1 + kChipsPerPage + 1 + 8 + 1 + 3;
}

bool OrogenEditor::clickMain (juce::Point<float> designPos)
{
    int index = -1;
    const auto hit = hitMain (designPos, index);

    if (hit == MainHit::fxChipPage)
    {
        const int n = chipPageCount();
        chipPage = n <= 1 ? 0 : (chipPage + 1) % n;
        repaint();
        return true;
    }

    if (hit == MainHit::fxChip && index >= 0 && index < kChipTotal)
    {
        if (auto* prm = proc.apvts.getParameter (kChips[index].id))
        {
            const float now = prm->getValue();
            if (now > 0.001f)
            {
                chipLast[(size_t) index] = now;      // запомнили, чтобы вернуть как было
                prm->setValueNotifyingHost (0.0f);
            }
            else
            {
                float target = chipLast[(size_t) index];
                if (target <= 0.001f) target = kChips[index].onValue;
                prm->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, target));
            }
            repaint();
            return true;
        }
    }
    return false;
}

bool OrogenEditor::dragMain (juce::Point<float> designPos)
{
    if (dragTarget == MainHit::none) return false;

    const juce::Point<float> d = designPos - dragLast;
    dragLast = designPos;
    if (d.getDistanceFromOrigin() > 0.0f) dragMoved = true;

    switch (dragTarget)
    {
        case MainHit::fxChip:
            if (dragIndex >= 0 && dragIndex < kChipTotal)
                nudgeParam (kChips[dragIndex].id, d.x / kPxPerFullRange);
            break;

        case MainHit::fxChipPage:
            break;

        case MainHit::display:
            // волна на дисплее следует за p0/p1, поэтому тянем именно их
            nudgeParam (slotId ("p0"), d.x / kPxPerFullRange * 2.2f);
            nudgeParam (slotId ("p1"), -d.y / juce::jmax (1.0f, displayRect().getHeight()) * 2.2f);
            break;

        case MainHit::modNode:
            // вертикаль: полный ход дорожки = весь диапазон −1…+1
            nudgeParam ("mod" + juce::String (dragIndex + 1) + "_amt",
                        -d.y / juce::jmax (1.0f, modLane (0).getHeight()));
            break;

        case MainHit::lfoLane:
            nudgeParam ("lfo1_rate",   d.x / kPxPerFullRange * 1.6f);
            nudgeParam ("lfo1_amount", -d.y / juce::jmax (1.0f, modLane (1).getHeight()));
            break;

        case MainHit::envAttack:  nudgeParam (slotId ("attack"),  d.x / kPxPerFullRange); break;
        case MainHit::envDecay:   nudgeParam (slotId ("decay"),   d.x / kPxPerFullRange); break;
        case MainHit::envSustain:
            nudgeParam (slotId ("release"),  d.x / kPxPerFullRange);
            nudgeParam (slotId ("sustain"), -d.y / juce::jmax (1.0f, modLane (2).getHeight()));
            break;

        case MainHit::none:
        default:
            return false;
    }

    repaint();
    return true;
}

void OrogenEditor::releaseMain()
{
    dragTarget = MainHit::none;
    dragIndex = -1;
    dragMoved = false;
}

OrogenEditor::MainHit OrogenEditor::beginMainDrag (juce::Point<float> designPos)
{
    int index = -1;
    const auto hit = hitMain (designPos, index);

    dragTarget = hit;
    dragIndex = index;
    dragLast = designPos;
    dragMoved = false;
    return hit;
}

void OrogenEditor::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent != this)
    {
        // события дочерних контролов не должны попадать в разбор главного экрана
        if (e.eventComponent == &programBox && e.mods.isPopupMenu()) showBankMenu();
        else if (e.eventComponent == &loadButton && e.mods.isPopupMenu()) showIoMenu (false);
        else if (e.eventComponent == &saveButton && e.mods.isPopupMenu()) showIoMenu (true);
        return;
    }

    beginMainDrag ({ e.position.x / juce::jmax (0.01f, kx()),
                     e.position.y / juce::jmax (0.01f, ky()) });
}

void OrogenEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragTarget == MainHit::none) return;
    const juce::Point<float> designPos { e.position.x / juce::jmax (0.01f, kx()),
                                         e.position.y / juce::jmax (0.01f, ky()) };
    dragMain (designPos);
}

void OrogenEditor::mouseUp (const juce::MouseEvent& e)
{
    const juce::Point<float> designPos { e.position.x / juce::jmax (0.01f, kx()),
                                         e.position.y / juce::jmax (0.01f, ky()) };
    if ((dragTarget == MainHit::fxChip || dragTarget == MainHit::fxChipPage) && ! dragMoved)
        clickMain (designPos);
    releaseMain();
}

void OrogenEditor::loadPresetFromFile()
{
    auto chooser = std::make_shared<juce::FileChooser> (
        "Загрузить пресет OROGEN",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.orogenpreset");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              if (proc.loadPresetFile (file)) repaint();
                          });
}

void OrogenEditor::savePresetToFile()
{
    const auto suggested = proc.getProgramName (proc.getCurrentProgram()).replaceCharacter (' ', '_');
    auto chooser = std::make_shared<juce::FileChooser> (
        "Сохранить пресет OROGEN",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile (suggested + ".orogenpreset"),
        "*.orogenpreset");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, chooser] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              proc.savePresetFile (file, proc.getProgramName (proc.getCurrentProgram()));
                          });
}

bool OrogenEditor::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    if (c >= 'a' && c <= 'h') { selectSlot ((int) (c - 'a')); return true; }
    if (c >= 'A' && c <= 'H') { selectSlot ((int) (c - 'A')); return true; }
    if (c >= '1' && c <= '4') { setSection ((int) (c - '1')); return true; }

    if (key == juce::KeyPress::escapeKey && paramsOpen) { toggleParamsPage (false); return true; }
    if (c == 'p' || c == 'P') { toggleParamsPage (! paramsOpen); return true; }
    return false;
}

} // namespace orogen
