// Проверки обёртки. Здесь важно доказать три вещи:
//   1) плагин звучит ТЕМ ЖЕ кодом, что стенды (нулевой тест против Instrument);
//   2) параметры и структуры не разъезжаются (patch -> params -> patch);
//   3) пресет переживает сохранение в файл и состояние сессии.
#include "../src/plugin/PluginProcessor.h"
#include "../src/plugin/PluginEditor.h"
#include "../src/plugin/UiParamMap.h"
#include "../src/plugin/EngineUi.h"

#include <cstdio>
#include <csignal>
#if ! defined (_WIN32)
#include <execinfo.h>
#endif
#include <cstdlib>

static void crashTrace (int sig)
{
    std::fprintf (stderr, "signal %d\n", sig);
#if ! defined (_WIN32)
    void* frames[24];
    const int n = backtrace (frames, 24);
    backtrace_symbols_fd (frames, n, 2);
#endif
    std::_Exit (3);
}
#include <cmath>
#include <functional>
#include <vector>

using namespace orogen;

static int failures = 0;

static void check (bool ok, const char* name, const juce::String& info = {})
{
    std::printf ("  %s %s%s%s\n", ok ? "[ OK ]" : "[FAIL]", name,
                 info.isEmpty() ? "" : "  —  ", info.toRawUTF8());
    if (! ok) ++failures;
}

static juce::String num (double v, int dp = 3)
{
    return juce::String (v, dp);
}

// Рендер N сэмплов через AudioProcessor с одним аккордом.
static juce::AudioBuffer<float> renderBlocks (OrogenProcessor& p, int total, double sr)
{
    juce::AudioBuffer<float> full (2, total);
    full.clear();
    juce::MidiBuffer midi;
    int pos = 0;
    const int onAt = (int) (sr * 0.05), offAt = (int) (sr * 0.45);
    // velocity берём такую, которая точно представима в 7-битном MIDI:
    // 102/127, иначе float округлится и нулевой тест покажет «разницу тракта»
    const float vel = 102.0f / 127.0f;
    const int notes[3] = { 48, 55, 60 };
    while (pos < total)
    {
        const int n = juce::jmin (512, total - pos);
        juce::AudioBuffer<float> block (2, n);
        block.clear();
        midi.clear();
        if (onAt >= pos && onAt < pos + n)
            for (int k = 0; k < 3; ++k)
                midi.addEvent (juce::MidiMessage::noteOn (1, notes[k], vel), onAt - pos);
        if (offAt >= pos && offAt < pos + n)
            for (int k = 0; k < 3; ++k)
                midi.addEvent (juce::MidiMessage::noteOff (1, notes[k]), offAt - pos);
        p.processBlock (block, midi);
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, n);
        pos += n;
    }
    return full;
}

static juce::AudioBuffer<float> renderProcessor (OrogenProcessor& p, int total, double sr,
                                                 int presetIndex)
{
    p.setPlayConfigDetails (0, 2, sr, 512);
    p.prepareToPlay (sr, 512);
    if (presetIndex >= 0) p.setCurrentProgram (presetIndex);
    return renderBlocks (p, total, sr);
}

static double rmsOf (const juce::AudioBuffer<float>& b)
{
    double s = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double v = b.getReadPointer (ch)[i];
            s += v * v;
        }
    return std::sqrt (s / (double) (b.getNumSamples() * b.getNumChannels()));
}


// Рендер с настройкой параметров ПОСЛЕ загрузки программы и с прокруткой
// очереди сообщений: именно так работают хосты — UI меняет параметр в
// message-потоке, а движок готовится в timerCallback().
static juce::AudioBuffer<float> renderWithSetup (OrogenProcessor& p, int total, double sr,
                                                 const std::function<void (OrogenProcessor&)>& setup)
{
    p.setPlayConfigDetails (0, 2, sr, 512);
    p.prepareToPlay (sr, 512);
    p.setCurrentProgram (0);
    if (setup) setup (p);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (80);   // timerCallback 30 Гц
    return renderBlocks (p, total, sr);
}

// Записать значение параметра так, как это делает хост/UI.
static void setP (OrogenProcessor& p, const juce::String& id, float value)
{
    if (auto* prm = p.apvts.getParameter (id))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// Один слот звучит, остальные выключены — чтобы RMS однозначно
// принадлежал выбранному движку.
static void soloSlot (OrogenProcessor& p, int slot, int type)
{
    for (int s = 0; s < 8; ++s)
        setP (p, "s" + juce::String::charToString ((juce::juce_wchar) ('A' + s)) + "_on", s == slot ? 1.0f : 0.0f);
    setP (p, "s" + juce::String::charToString ((juce::juce_wchar) ('A' + slot)) + "_type", (float) type);
}

static double renderRms (OrogenProcessor& p, int total, double sr)
{
    auto b = renderProcessor (p, total, sr, 0);
    return rmsOf (b);
}

// Считает контролы в оверлее, привязанные к параметрам (Slider/ComboBox/ToggleButton)
// на активной вкладке. Нужен, чтобы доказать: карта не просто список id, а
// реально построенный UI.
// Эффективная видимость: важно не только собственное флаговое значение, но и
// все родители (скрытая вкладка оверлея не должна добавлять свои контролы).
static bool effectivelyVisible (juce::Component& c, juce::Component* stopAt)
{
    for (juce::Component* p = &c; p != nullptr && p != stopAt; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    return true;
}

static int countBoundControls (juce::Component& root, const orogen::UiTab& tab)
{
    int found = 0;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (! effectivelyVisible (*child, &root)) continue;   // скрытые вкладки/секции
            if (dynamic_cast<juce::Slider*> (child) != nullptr
                || dynamic_cast<juce::ComboBox*> (child) != nullptr
                || dynamic_cast<juce::ToggleButton*> (child) != nullptr)
                ++found;
            walk (*child);
        }
    };
    walk (root);
    juce::ignoreUnused (tab);
    return found;
}

static juce::StringArray visibleBoundIds (juce::Component& root)
{
    juce::StringArray ids;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (! effectivelyVisible (*child, &root)) continue;
            const auto id = child->getProperties().getWithDefault ("paramId", {}).toString();
            if (id.isNotEmpty()) ids.add (id);
            walk (*child);
        }
    };
    walk (root);
    return ids;
}

// Контролы PARAMS создаются динамически. Component не владеет детьми:
// проверяем их физическое уничтожение при закрытии редактора, включая
// скрытые вкладки и секции, а не только удаление из визуальной иерархии.
static void testEditorControlLifetime()
{
    std::printf ("\n--- Время жизни контролов PARAMS ---\n");
    OrogenProcessor p;
    std::vector<juce::Component::SafePointer<juce::Component>> refs;
    {
        std::unique_ptr<juce::AudioProcessorEditor> base (p.createEditor());
        auto* editor = dynamic_cast<OrogenEditor*> (base.get());
        auto* overlay = editor != nullptr ? editor->paramsOverlay() : nullptr;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (child->getProperties().contains ("paramId")) refs.emplace_back (child);
                walk (*child);
            }
        };
        if (overlay != nullptr) walk (*overlay);
        std::printf ("   создано %zu контролов PARAMS\n", refs.size());
    }
    int alive = 0;
    for (const auto& ref : refs) if (ref.getComponent() != nullptr) ++alive;
    std::printf ("   после закрытия осталось %d из %zu\n", alive, refs.size());
    check (refs.size() == 306 && alive == 0,
           "закрытие редактора уничтожает все контролы PARAMS",
           juce::String (alive) + "/" + juce::String ((int) refs.size()));
}

// Регрессия GUI: реальные слоты/зоны Morph, не только разница пикселей за
// счёт надписей. Отдельный режим --gui-only даёт короткий прогон при отладке.
static void testGuiRegressions()
{
    std::printf ("\n--- 15. Проверка реальных слотов Morph и событий GUI ---\n");
    OrogenProcessor p;
    p.setCurrentProgram (4);             // Morph Eight: восемь разных слотов
    std::unique_ptr<juce::AudioProcessorEditor> base (p.createEditor());
    auto* editor = dynamic_cast<OrogenEditor*> (base.get());
    check (editor != nullptr, "редактор для GUI-регрессий создан");
    if (editor == nullptr) return;
    editor->setSize (1280, 800);
    editor->setAnimationsEnabled (false);
    editor->showPage (0);

    auto refresh = [&]
    {
        editor->rebuildPreview (true);
        return editor->waitForPreview (15000);
    };
    auto diffWave = [] (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size() || a.empty()) return a.empty() && b.empty() ? 0.0 : 1.0;
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const double d = (double) a[i] - (double) b[i];
            sum += d * d;
        }
        return std::sqrt (sum / (double) a.size());
    };
    auto capture = [&] (juce::Component& c)
    {
        juce::Image img (juce::Image::ARGB, c.getWidth(), c.getHeight(), true);
        juce::Graphics g (img);
        c.paintEntireComponent (g, true);
        return img;
    };
    auto diffRegion = [] (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> bounds)
    {
        int count = 0;
        juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData db (b, juce::Image::BitmapData::readOnly);
        bounds = bounds.getIntersection (a.getBounds()).getIntersection (b.getBounds());
        for (int y = bounds.getY(); y < bounds.getBottom(); ++y)
            for (int x = bounds.getX(); x < bounds.getRight(); ++x)
                if (da.getPixelColour (x, y) != db.getPixelColour (x, y)) ++count;
        return count;
    };

    // Таймер превью обязан учитывать именно новый параметр, без forced rebuild.
    for (int i = 0; i < 8; ++i) setP (p, slotPrefix (i) + "on", i == 0 ? 1.0f : 0.0f);
    setP (p, "gl_mode", 0.0f); setP (p, "sA_type", 4.0f);
    setP (p, "sA_table", 0.0f); setP (p, "sA_level", 0.8f);
    setP (p, "sA_p0", 0.35f); setP (p, "sA_p1", 0.45f); setP (p, "sA_p2", 0.8f);
    setP (p, "sA_p3", 0.2f); setP (p, "sA_grainReverse", 0.0f);
    const bool previewForwardReady = refresh();
    const auto previewForward = editor->previewWave();
    setP (p, "sA_grainReverse", 1.0f);
    double reversePreviewDelta = 0.0;
    const double reversePreviewStart = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - reversePreviewStart < 3000.0)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        reversePreviewDelta = diffWave (previewForward, editor->previewWave());
        if (reversePreviewDelta > 0.02 && ! editor->isPreviewPending()) break;
    }
    check (previewForwardReady && ! previewForward.empty() && reversePreviewDelta > 0.02
           && ! editor->isPreviewPending(),
           "таймер обновляет превью при автоматизации Grain Reverse", num (reversePreviewDelta, 4));
    setP (p, "sA_grainReverse", 0.0f);

    // Отдельные воспроизводимые движки A и B, вместо 50 % всего диапазона
    // восьми слотов (0.5 было бы между D и E, а не между A и B).
    for (int i = 0; i < 8; ++i)
        setP (p, "s" + juce::String::charToString ((juce::juce_wchar) ('A' + i)) + "_on", i < 2 ? 1.0f : 0.0f);
    for (const auto* id : { "lfo1_amount", "lfo2_amount", "mod1_amt", "mod2_amt", "mod3_amt", "mod4_amt",
                            "mod5_amt", "mod6_amt", "mod7_amt", "mod8_amt" }) setP (p, id, 0.0f);
    for (const char* pre : { "sA_", "sB_" })
    {
        const juce::String prefix (pre);
        setP (p, prefix + "detune", 0.0f);
        setP (p, prefix + "level", 0.8f);
        setP (p, prefix + "pan", 0.0f);
        setP (p, prefix + "lo", 0.0f);
        setP (p, prefix + "hi", 127.0f);
        setP (p, prefix + "cutoff", 0.9f);
        setP (p, prefix + "attack", 0.005f);
    }
    setP (p, "sA_type", 5.0f);          // Modal
    setP (p, "sB_type", 1.0f);          // FM
    setP (p, "gl_mode", 1.0f);
    setP (p, "gl_morph", 0.071f);       // A/B: 0.071 × 7 ≈ 0.5
    const bool readyAB = refresh();
    const double actualRenderMs = editor->lastPreviewRenderMs();
    const auto ab = editor->previewWave();
    const auto imageAB = capture (*editor);

    // Выбран по-прежнему A; меняется только реальный движок B.
    setP (p, "sB_type", 5.0f);
    const bool readyB = refresh();
    const auto changedB = editor->previewWave();
    const auto imageChangedB = capture (*editor);
    const double deltaB = diffWave (ab, changedB);
    const int pixelsB = diffRegion (imageAB, imageChangedB, { 260, 112, 712, 138 });
    std::printf ("   движок B при выбранном A: ΔRMS %.5f, внутри графика %d пикселей\n", deltaB, pixelsB);
    check (readyAB && readyB && ! ab.empty() && ! changedB.empty() && deltaB > 0.02 && pixelsB > 500,
           "Morph использует фактический движок соседнего слота", num (deltaB, 4) + ", " + juce::String (pixelsB) + " px");
    std::printf ("   фоновый рендер работающей пары A/B: %.2f мс\n", actualRenderMs);

    // Путь в реальном редакторе: после автоматизации таймер сам сдаёт новый
    // снимок воркеру и забирает результат; не вызываем rebuildPreview вручную.
    setP (p, "sB_p0", 0.91f);
    const auto beforeTimer = editor->previewWave();
    double timerDelta = 0.0;
    const double timerStart = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - timerStart < 2000.0)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        timerDelta = diffWave (beforeTimer, editor->previewWave());
        if (timerDelta > 0.02 && ! editor->isPreviewPending()) break;
    }
    check (timerDelta > 0.02 && ! editor->isPreviewPending(),
           "таймер сам обновляет превью при автоматизации соседнего слота", num (timerDelta, 4));

    setP (p, "sB_on", 0.0f);
    const bool readyA = refresh();
    const auto onlyA = editor->previewWave();
    const double deltaMute = diffWave (changedB, onlyA);
    check (readyA && ! onlyA.empty() && deltaMute > 0.02,
           "выключение B изменяет микс Morph", num (deltaMute, 4));

    setP (p, "sA_on", 0.0f);
    const bool readyNone = refresh();
    check (readyNone && editor->previewWave().empty() && editor->previewPath().isEmpty(),
           "выключенные слоты Morph не создают фиктивный сигнал");

    // При B = 1/7 в позиции B; канал R тоже входит в монопревью.
    setP (p, "sB_on", 1.0f);
    setP (p, "gl_morph", 1.0f / 7.0f);
    setP (p, "sB_pan", 1.0f);
    const bool readyRight = refresh();
    check (readyRight && ! editor->previewWave().empty(), "крайняя правая панорама не исчезает из превью");

    setP (p, "sB_hi", 59.0f);          // C4 = MIDI 60 вне зоны
    const bool readyOut = refresh();
    const bool outSilent = editor->previewWave().empty();
    setP (p, "sB_hi", 60.0f);
    const bool readyIn = refresh();
    check (readyOut && outSilent && readyIn && ! editor->previewWave().empty(),
           "Morph учитывает фактическую зону ноты C4");

    setP (p, "sB_hi", 127.0f);
    setP (p, "sB_cutoff", 0.95f);
    const bool readyFilter1 = refresh();
    const auto bOpen = editor->previewWave();

    // Независимый прямой рендер Voice с тем же снимком параметров. Сравнение
    // самих 4096 нормированных отсчётов, а не подписей / кадра интерфейса.
    Patch refPatch;
    FxChain::Settings unusedFx;
    p.collectPatch (refPatch, unusedFx);
    EngineBank referenceBank;
    referenceBank.build (48000.0);
    Voice referenceVoice;
    referenceVoice.init (referenceBank, 48000.0);
    referenceVoice.stageSlots (refPatch, referenceBank);
    referenceVoice.applyPendingEngines();
    referenceVoice.start (refPatch, 60, 1.0f, 60.0f, 1);
    std::vector<float> refSamples (24000, 0.0f);
    for (auto& value : refSamples)
    {
        float l = 0.0f, r = 0.0f;
        if (! referenceVoice.render (refPatch, l, r)) break;
        value = 0.5f * (l + r);
    }
    size_t refStart = 0;
    double refEnergy = -1.0;
    for (size_t start = 0; start + 4096 <= refSamples.size(); start += 512)
    {
        double energy = 0.0;
        for (size_t i = 0; i < 4096; ++i) energy += (double) refSamples[start + i] * refSamples[start + i];
        if (energy > refEnergy) { refEnergy = energy; refStart = start; }
    }
    std::vector<float> refWave (refSamples.begin() + (std::ptrdiff_t) refStart,
                                refSamples.begin() + (std::ptrdiff_t) (refStart + 4096));
    double refMean = 0.0;
    for (const auto value : refWave) refMean += value;
    refMean /= (double) refWave.size();
    float refPeak = 0.0f;
    for (auto& value : refWave) { value -= (float) refMean; refPeak = std::max (refPeak, std::abs (value)); }
    if (refPeak > 1.0e-5f) for (auto& value : refWave) value /= refPeak;
    else refWave.clear();
    const double refError = diffWave (bOpen, refWave);
    std::printf ("   прямой DSP vs превью: ΔRMS %.8f (4096 отсчётов)\n", refError);
    // Воркер переиспользует голос: состояние фильтра/движка после предыдущей
    // ноты не обязано совпадать побитово со свежеинициализированным Voice.
    check (readyFilter1 && ! bOpen.empty() && refError < 0.001,
           "превью близко к независимому рендеру DSP-голоса", num (refError, 8));

    setP (p, "sB_cutoff", 0.15f);
    const bool readyFilter2 = refresh();
    const double filterDiff = diffWave (bOpen, editor->previewWave());
    check (readyFilter1 && readyFilter2 && filterDiff > 0.02,
           "фильтр соседнего слота пересобирает превью Morph", num (filterDiff, 4));

    // Стек — только выбранный слот; Split — все включённые в зоне C4.
    setP (p, "gl_mode", 0.0f);
    const bool readyStackA = refresh();
    const bool stackAEmpty = editor->previewWave().empty();
    editor->showPage (1);
    const bool readyStackB = editor->waitForPreview (15000);
    const bool stackBActive = ! editor->previewWave().empty();
    check (readyStackA && stackAEmpty && readyStackB && stackBActive,
           "Stack показывает только выбранный слот, учитывая его on/off");

    editor->showPage (0);
    setP (p, "gl_mode", 2.0f);
    setP (p, "sA_on", 1.0f);
    setP (p, "sA_hi", 59.0f);
    setP (p, "sB_lo", 60.0f);
    setP (p, "sB_on", 1.0f);
    const bool readySplit = refresh();
    const bool splitActive = ! editor->previewWave().empty();
    setP (p, "sB_on", 0.0f);
    const bool readySplitMuted = refresh();
    check (readySplit && splitActive && readySplitMuted && editor->previewWave().empty(),
           "Split показывает только реальные включённые слоты в зоне C4");

    // Запросы при быстром движении перезаписывают очередь, а не оставляют
    // устаревшую картинку. Все вызовы submit выполняются на message thread.
    setP (p, "gl_mode", 1.0f);
    setP (p, "sA_hi", 127.0f);
    setP (p, "sB_on", 1.0f);
    const double tStart = juce::Time::getMillisecondCounterHiRes();
    double maxSubmitMs = 0.0;
    for (int i = 0; i < 30; ++i)
    {
        setP (p, "gl_morph", (float) i / 29.0f);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        editor->rebuildPreview (true);
        maxSubmitMs = juce::jmax (maxSubmitMs, juce::Time::getMillisecondCounterHiRes() - t0);
    }
    const double submitMs = juce::Time::getMillisecondCounterHiRes() - tStart;
    setP (p, "sA_on", 0.0f);
    setP (p, "sB_on", 0.0f);
    const bool readyLatest = refresh();
    std::printf ("   30 запросов: %.2f мс суммарно, максимум %.2f мс; последняя генерация %.2f мс\n",
                 submitMs, maxSubmitMs, editor->lastPreviewRenderMs());
    check (readyLatest && editor->previewWave().empty(), "очередь превью публикует последнее, а не устаревшее состояние");
    check (maxSubmitMs < 50.0, "рендер превью не блокирует GUI на каждом запросе", num (maxSubmitMs, 2) + " мс максимум");

    // Без анимаций листание не застревает в сдвинутом положении; после
    // повторного включения снова виден плавный переход.
    editor->showPage (8);
    editor->nextColumnPage (0);
    check (! editor->columnTransitioning (0) && editor->columnPageShift (0) == 0,
           "при выключенных анимациях страница меняется без сдвига");
    editor->setAnimationsEnabled (true);
    editor->nextColumnPage (0);
    const int shiftStart = editor->columnPageShift (0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    const int shiftEnd = editor->columnPageShift (0);
    std::printf ("   переход: %d → %d px\n", shiftStart, shiftEnd);
    check (shiftStart > 0 && shiftEnd == 0 && ! editor->columnTransitioning (0),
           "переход следующей страницы завершается, в том числе на циклическом переходе");
    editor->nextColumnPage (0);
    editor->setAnimationsEnabled (false);
    check (! editor->columnTransitioning (0) && editor->columnPageShift (0) == 0,
           "выключение анимаций во время перехода сразу выравнивает карточки");

    // Страницы полосы FX тоже плавно проявляются, а при выключенных
    // анимациях переключаются сразу, не блокируя клики по чипам.
    editor->clickMain ({ 925.0f, 586.0f });
    const float chipOff = editor->chipPageTransition();
    editor->setAnimationsEnabled (true);
    editor->clickMain ({ 925.0f, 586.0f });
    const float chipStart = editor->chipPageTransition();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    const float chipEnd = editor->chipPageTransition();
    check (chipOff == 0.0f && chipStart > 0.99f && chipEnd < 0.01f,
           "страницы чипов FX плавные только при включённых анимациях",
           num (chipStart, 2) + " → " + num (chipEnd, 2));
    editor->setAnimationsEnabled (false);

    // Тестируем отправку настоящих callbacks mouseEnter/mouseExit компонентам,
    // включая маленькие кольца в карточках, а не только внутренний тестовый API.
    std::function<juce::Slider* (juce::Component&, const juce::String&)> findSlider;
    findSlider = [&findSlider] (juce::Component& parent, const juce::String& id) -> juce::Slider*
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* slider = dynamic_cast<juce::Slider*> (child))
                if (slider->getProperties().getWithDefault ("paramId", "").toString() == id)
                    return slider;
            if (auto* found = findSlider (*child, id)) return found;
        }
        return nullptr;
    };
    auto hoverEvent = [] (juce::Slider& slider, bool enter)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto time = juce::Time::getCurrentTime();
        const auto pos = slider.getLocalBounds().getCentre().toFloat();
        const juce::MouseEvent event (source, pos, juce::ModifierKeys(), 0.0f,
                                      0.0f, 0.0f, 0.0f, 0.0f,
                                      &slider, &slider, time, pos, time, 0, false);
        if (enter) slider.mouseEnter (event); else slider.mouseExit (event);
    };
    editor->showPage (0);
    auto* mainKnob = findSlider (*editor, "sA_cutoff");
    auto* bottomKnob = findSlider (*editor, "gl_morph");
    auto* smallKnob = findSlider (*editor, "fx_drive");
    check (mainKnob != nullptr && bottomKnob != nullptr && smallKnob != nullptr,
           "главные и малые кольцевые ручки найдены в GUI");
    if (mainKnob != nullptr && bottomKnob != nullptr && smallKnob != nullptr)
    {
        int immediate = 0, cleared = 0;
        for (auto* knob : { mainKnob, bottomKnob, smallKnob })
        {
            hoverEvent (*knob, true);
            if ((double) knob->getProperties().getWithDefault ("hover", 0.0) > 0.99) ++immediate;
            hoverEvent (*knob, false);
            if ((double) knob->getProperties().getWithDefault ("hover", 1.0) < 0.01) ++cleared;
        }
        check (immediate == 3 && cleared == 3, "hover работает на центральной, нижней и малой ручках без анимаций",
               juce::String (immediate) + "/3 on, " + juce::String (cleared) + "/3 off");

        // На 100 % все активные насечки заняты: подсветка всё равно видна.
        setP (p, "sA_cutoff", 1.0f);
        const auto idle = capture (*mainKnob);
        hoverEvent (*mainKnob, true);
        const auto hovered = capture (*mainKnob);
        const int hoverPixels = diffRegion (idle, hovered, idle.getBounds());
        hoverEvent (*mainKnob, false);
        std::printf ("   hover при 100%%: %d пикселей\n", hoverPixels);
        check (hoverPixels > 20, "подсветка заметна даже при максимальном значении ручки",
               juce::String (hoverPixels) + " px");

        editor->setAnimationsEnabled (true);
        hoverEvent (*mainKnob, true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (350);
        const double levelOn = (double) mainKnob->getProperties().getWithDefault ("hover", 0.0);
        hoverEvent (*mainKnob, false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (350);
        const double levelOff = (double) mainKnob->getProperties().getWithDefault ("hover", 1.0);
        check (levelOn > 0.99 && levelOff < 0.01, "mouseEnter/mouseExit плавно зажигают и гасят ручку",
               num (levelOn, 2) + " → " + num (levelOff, 2));
    }

    // Реальная точка playhead сравнивается по пикселям с кадром без анимации.
    // При изменении скорости фаза не должна перескакивать, а по неподвижной
    // скорости за 0.25 с должен пройти именно четверть цикла LFO.
    {
        auto headPosition = [] (const juce::Image& baseImage, const juce::Image& movingImage)
        {
            double weight = 0.0, weightedX = 0.0, weightedY = 0.0;
            for (int y = 352; y < 405; ++y)
                for (int x = 262; x < 970; ++x)
                {
                    const double d = (double) movingImage.getPixelAt (x, y).getBrightness()
                                   - (double) baseImage.getPixelAt (x, y).getBrightness();
                    if (d > 0.01) { weight += d; weightedX += d * x; weightedY += d * y; }
                }
            return weight > 0.5 ? std::array<double, 2> { weightedX / weight, weightedY / weight }
                                : std::array<double, 2> { -1.0, -1.0 };
        };
        auto headX = [&headPosition] (const juce::Image& a, const juce::Image& b) {
            return headPosition (a, b)[0];
        };
        setP (p, "lfo1_shape", 0.0f);
        setP (p, "lfo1_amount", 0.8f);
        setP (p, "lfo1_rate", 0.05f);
        editor->setAnimationsEnabled (false);
        editor->setAnimationPhase (12.345);
        const auto lowBase = capture (*editor);
        editor->setAnimationsEnabled (true);
        const auto lowOn = capture (*editor);
        const double x1 = headX (lowBase, lowOn);
        setP (p, "lfo1_rate", 0.1f);
        editor->setAnimationsEnabled (false);
        const auto highBase = capture (*editor);
        editor->setAnimationsEnabled (true);
        const auto highOn = capture (*editor);
        const double x2 = headX (highBase, highOn);
        std::printf ("   playhead при смене Rate: x %.2f -> %.2f px\n", x1, x2);
        check (x1 >= 0.0 && x2 >= 0.0 && std::fabs (x1 - x2) < 3.0,
               "автоматизация LFO Rate не перескакивает фазу анимации",
               num (std::fabs (x1 - x2), 2) + " px");

        // При смене 1→2 видимых циклов X раньше оставался на месте, но сама
        // кривая пересчитывалась относительно начала дорожки и Y прыгал.
        // Измеряем точку по пикселям для синуса, треугольника и sample&hold.
        double worstX = 0.0, worstY = 0.0; int found = 0;
        for (int shape = 0; shape < 3; ++shape)
        {
            setP (p, "lfo1_shape", (float) shape);
            setP (p, "lfo1_rate", 0.05f); // 1 отображаемый цикл
            editor->setAnimationsEnabled (false);
            editor->setAnimationPhase (12.345); // примерно 62% ширины дорожки
            const auto beforeBase = capture (*editor);
            editor->setAnimationsEnabled (true);
            auto before = headPosition (beforeBase, capture (*editor));
            for (float nextRate : { 2.0f, 10.0f, 0.05f }) // 1 → 2 → 6 → 1 циклов
            {
                setP (p, "lfo1_rate", nextRate);
                editor->setAnimationsEnabled (false);
                const auto afterBase = capture (*editor);
                editor->setAnimationsEnabled (true);
                const auto after = headPosition (afterBase, capture (*editor));
                if (before[0] >= 0.0 && after[0] >= 0.0)
                {
                    ++found;
                    worstX = std::max (worstX, std::fabs (before[0] - after[0]));
                    worstY = std::max (worstY, std::fabs (before[1] - after[1]));
                }
                std::printf ("   LFO shape %d, Rate -> %.2f: x %.2f -> %.2f, y %.2f -> %.2f\n",
                             shape, nextRate, before[0], after[0], before[1], after[1]);
                before = after;
            }
        }
        check (found == 9 && worstX < 3.0 && worstY < 3.0,
               "при смене масштаба LFO точка не прыгает ни по X, ни по Y",
               num (worstX, 2) + "/" + num (worstY, 2) + " px, " + juce::String (found) + "/9");

        // S&H: граница экрана после изменения масштаба может оказаться
        // внутри LFO-цикла, а не на границе ступени. Запускаем точку у правого
        // края при 4 циклах, переводим на 1 и пересекаем край таймером.
        // Здесь значение на входе/выходе должно быть одним и тем же.
        setP (p, "lfo1_shape", 2.0f);
        setP (p, "lfo1_rate", 5.0f);
        editor->setAnimationsEnabled (false);
        editor->setAnimationPhase (0.792); // X ~ 99%; абсолютная фаза ~3.96
        setP (p, "lfo1_rate", 0.05f);
        const auto wrapBeforeBase = capture (*editor);
        editor->setAnimationsEnabled (true);
        const auto wrapBefore = headPosition (wrapBeforeBase, capture (*editor));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (350);
        editor->setAnimationsEnabled (false);
        const auto wrapAfterBase = capture (*editor);
        editor->setAnimationsEnabled (true);
        const auto wrapAfter = headPosition (wrapAfterBase, capture (*editor));
        std::printf ("   S&H wrap: x %.2f -> %.2f, y %.2f -> %.2f\n",
                     wrapBefore[0], wrapAfter[0], wrapBefore[1], wrapAfter[1]);
        check (wrapBefore[0] > 920.0 && wrapAfter[0] > 262.0 && wrapAfter[0] < 310.0
               && wrapBefore[1] >= 0.0 && wrapAfter[1] >= 0.0
               && std::fabs (wrapBefore[1] - wrapAfter[1]) < 3.0,
               "S&H не меняет ступень на границе экрана внутри цикла",
               num (std::fabs (wrapBefore[1] - wrapAfter[1]), 2) + " px");

        setP (p, "lfo1_shape", 0.0f);
        setP (p, "lfo1_rate", 1.0f); // два цикла на дорожке, 1 Гц в DSP
        editor->setAnimationsEnabled (false);
        editor->setAnimationPhase (0.2);
        const auto oneHzBase = capture (*editor);
        editor->setAnimationsEnabled (true);
        const double startX = headX (oneHzBase, capture (*editor));
        editor->setAnimationPhase (0.45);
        const double endX = headX (oneHzBase, capture (*editor));
        std::printf ("   playhead 1 Гц за 0.25 с: %.2f px по дорожке\n", endX - startX);
        check (startX >= 0.0 && endX - startX > 78.0 && endX - startX < 100.0,
               "скорость playhead соответствует параметру LFO 1 Гц");
    }

    // В отличие от paintEntireComponent(), снимок НАТИВНОГО окна показывает
    // только реально перерисованные области. Так ловим устаревшие значения,
    // графики и чипы при автоматизации и выключенных анимациях.
   #if JUCE_LINUX || JUCE_BSD
    if (std::getenv ("OROGEN_GUI_NATIVE") != nullptr)
    {
        editor->setAnimationsEnabled (false);
        editor->showPage (0);
        setP (p, "sA_cutoff", 0.10f);
        setP (p, "fx_drive", 0.0f);
        setP (p, "lfo1_amount", 0.0f);
        setP (p, "gl_morph", 0.1f);
        setP (p, "sB_on", 0.0f);
        setP (p, "sB_type", 0.0f);
        setP (p, "sA_on", 0.0f);
        setP (p, "sA_type", (float) EngineType::Granular);
        setP (p, "sA_grainReverse", 0.0f);
        editor->addToDesktop (0);
        editor->setTopLeftPosition (0, 0);
        editor->setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2200);
        auto native = [&] {
            return editor->getPeer() != nullptr
                ? juce::createSnapshotOfNativeWindow (editor->getPeer()->getNativeHandle())
                : juce::Image();
        };
        auto nativeChange = [&] (const juce::String& id, float value, juce::Rectangle<int> region) {
            const auto beforeNative = native();
            const auto beforeFresh = capture (*editor);
            setP (p, id, value);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            const auto afterNative = native();
            const auto afterFresh = capture (*editor);
            if (! beforeNative.isValid() || ! afterNative.isValid()) return std::array<int, 2> { -1, -1 };
            return std::array<int, 2> { diffRegion (beforeNative, afterNative, region),
                                        diffRegion (beforeFresh, afterFresh, region) };
        };
        const auto label = nativeChange ("sA_cutoff", 0.90f, { 374, 520, 84, 14 });
        const auto chip = nativeChange ("fx_drive", 0.80f, { 254, 598, 120, 34 });
        const auto lfo = nativeChange ("lfo1_amount", 0.90f, { 262, 356, 708, 44 });
        const auto rail = nativeChange ("sB_type", 4.0f, { 16, 136, 216, 62 });
        const auto pad = nativeChange ("gl_morph", 0.80f, { 1108, 646, 156, 138 });
        const auto reverseOff = nativeChange ("sA_grainReverse", 0.73f, { 840, 96, 130, 10 });
        setP (p, "sA_on", 1.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
        const auto reverseOn = nativeChange ("sA_grainReverse", 0.31f, { 840, 96, 130, 10 });
        std::printf ("   native/fresh px: значение %d/%d, FX %d/%d, LFO %d/%d, слот B %d/%d, pad %d/%d, Reverse off/on %d/%d / %d/%d\n",
                     label[0], label[1], chip[0], chip[1], lfo[0], lfo[1], rail[0], rail[1], pad[0], pad[1],
                     reverseOff[0], reverseOff[1], reverseOn[0], reverseOn[1]);
        check (label[1] > 5 && label[0] > 5, "значение центральной ручки перерисовывается в настоящем окне");
        check (chip[1] > 20 && chip[0] > 20, "автоматизация FX обновляет чип при выключенных анимациях");
        check (lfo[1] > 20 && lfo[0] > 20, "автоматизация LFO обновляет график при выключенных анимациях");
        check (rail[1] > 20 && rail[0] > 20, "автоматизация типа выключенного слота обновляет рейл");
        check (pad[1] > 20 && pad[0] > 20, "автоматизация Morph обновляет векторный блок");
        check (reverseOff[0] > 5 && reverseOff[1] > 5 && reverseOn[0] > 5 && reverseOn[1] > 5,
               "автоматизация Grain Reverse обновляет статус в нативном окне при выключенном и включённом слоте");
        editor->removeFromDesktop();
    }
    else std::printf ("   native-window тесты пропущены: нужен оконный менеджер и OROGEN_GUI_NATIVE=1\n");
   #endif

    // Fade секций оверлея: доступность контролов не зависит от эффекта.
    editor->setAnimationsEnabled (true);
    editor->showPage (100);
    editor->showPage (200);        // ALL вкладки 0
    auto* overlay = editor->paramsOverlay();
    juce::Viewport* viewport = nullptr;
    if (overlay != nullptr)
        for (auto* child : overlay->getChildren())
            if (auto* v = dynamic_cast<juce::Viewport*> (child)) viewport = v;
    editor->showPage (201);        // первая секция
    const float alphaStart = viewport != nullptr ? viewport->getAlpha() : 1.0f;
    juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    const float alphaEnd = viewport != nullptr ? viewport->getAlpha() : 0.0f;
    std::printf ("   оверлей PARAMS: alpha %.2f → %.2f\n", alphaStart, alphaEnd);
    check (viewport != nullptr && alphaStart < 0.99f && alphaEnd > 0.99f,
           "вкладки и секции PARAMS плавно проявляются");
    editor->setAnimationsEnabled (false);
    editor->showPage (200);
    check (viewport != nullptr && viewport->getAlpha() > 0.99f,
           "без анимаций оверлей сразу полностью виден");

    std::printf ("\n--- 16. Контекстные макросы и Plate в настоящем GUI ---\n");
    editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
    editor->showPage (1);  // слот B, A остаётся независимым
    setP (p, "sA_type", 1.0f);
    const char* expected[9][4] = {
        { "POSITION", "WARP", "FOLD", "TILT" },
        { "RATIO", "FM 1", "FM 2", "FEEDBACK" },
        { "TILT", "SHAPE", "SPREAD", "MOTION" },
        { "SCAN", "GRAIN", "TONE", "AIR" },
        { "POSITION", "SIZE", "DENSITY", "SPREAD" },
        { "BRIGHT", "DECAY", "HARDNESS", "NONLINEAR" },
        { "STRETCH", "PITCH", "TILT", "DRIFT" },
        { "BODY", "AIR", "DRIFT", "BLOOM" },
        { "SLICE", "LENGTH", "REPEATS", "REVERSE" }
    };
    int correctLabels = 0;
    for (int type = 0; type < 9; ++type)
    {
        setP (p, "sB_type", (float) type);
        for (int macro = 0; macro < 4; ++macro)
            if (editor->selectedMacroLabel (macro) == expected[type][macro]) ++correctLabels;
    }
    check (correctLabels == 36 && (int) p.apvts.getRawParameterValue ("sA_type")->load() == 1,
           "контекстные подписи P1…P4 для всех 9 движков относятся к выбранному слоту",
           juce::String (correctLabels) + " / 36");

    setP (p, "sB_type", 1.0f);
    const auto fmSource = editor->selectedSourceLabel();
    setP (p, "sB_type", 4.0f); setP (p, "sB_table", 2.0f);
    const auto granularSource = editor->selectedSourceLabel();
    setP (p, "sB_type", 8.0f); setP (p, "sB_table", 7.0f);
    const auto chopSource = editor->selectedSourceLabel();
    std::printf ("   источник: FM=%s; Granular=%s; Chop=%s\n",
                 fmSource.toRawUTF8(), granularSource.toRawUTF8(), chopSource.toRawUTF8());
    check (fmSource == "NO TABLE" && granularSource.contains ("CHOIR AH 3/8")
           && chopSource.contains ("DUB STABS 3/3 MAX"),
           "источники в дисплее соответствуют фактическому банку движка");

    setP (p, "sB_type", 4.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    const auto granularImage = capture (*editor);
    setP (p, "sB_type", 5.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    const auto modalImage = capture (*editor);
    const int captionDiff = diffRegion (granularImage, modalImage, { 462, 570, 96, 12 });
    check (captionDiff > 10, "подпись центрального P1 действительно перерисована",
           juce::String (captionDiff) + " px в области подписи");

    // На ALL вкладки SLOT разные слоты одновременно показывают собственный
    // смысл P1. Хостовая автоматизация B должна обновить только подпись B.
    setP (p, "sA_type", 1.0f); setP (p, "sB_type", 4.0f);
    editor->showPage (100);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    auto* aP1 = overlay != nullptr ? findSlider (*overlay, "sA_p0") : nullptr;
    auto* bP1 = overlay != nullptr ? findSlider (*overlay, "sB_p0") : nullptr;
    auto* aTable = overlay != nullptr ? findSlider (*overlay, "sA_table") : nullptr;
    auto* bTable = overlay != nullptr ? findSlider (*overlay, "sB_table") : nullptr;
    auto* aReverse = overlay != nullptr ? findSlider (*overlay, "sA_grainReverse") : nullptr;
    auto* bReverse = overlay != nullptr ? findSlider (*overlay, "sB_grainReverse") : nullptr;
    const auto labelOf = [] (juce::Slider* s)
    {
        return s != nullptr ? s->getProperties().getWithDefault ("contextLabel", "").toString()
                            : juce::String();
    };
    const auto aLabel = labelOf (aP1), bLabel = labelOf (bP1);
    const auto unusedTable = labelOf (aTable), sourceTable = labelOf (bTable);
    const auto unusedReverse = labelOf (aReverse), granularReverse = labelOf (bReverse);
    setP (p, "sB_type", 5.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    const auto modelTable = labelOf (bTable);
    check (aLabel.contains ("RATIO") && bLabel.contains ("POSITION")
           && labelOf (aP1).contains ("RATIO") && labelOf (bP1).contains ("BRIGHT"),
           "подписи оверлея учитывают тип каждого слота и автоматизацию хоста");
    setP (p, "sB_type", 8.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    check (unusedTable.contains ("UNUSED") && sourceTable.contains ("SOURCE 0-7")
           && modelTable.contains ("MODEL 0-7") && labelOf (bTable).contains ("LOOP 0-2"),
           "общий Table в оверлее честно показывает реальную функцию или UNUSED");
    check (unusedReverse.contains ("UNUSED") && granularReverse.contains ("PROBABILITY")
           && labelOf (bReverse).contains ("UNUSED"),
           "Grain Reverse помечен доступным только у Granular при смене движка");

    int visibleReverse = 0, boundReverse = 0;
    for (int s = 0; s < 8; ++s)
    {
        editor->showPage (201 + s); // SLOT A–H
        const auto id = slotPrefix (s) + "grainReverse";
        auto* slider = overlay != nullptr ? findSlider (*overlay, id) : nullptr;
        const float value = 0.1f * (float) (s + 1);
        if (slider != nullptr && effectivelyVisible (*slider, overlay))
        {
            ++visibleReverse;
            slider->setValue (value, juce::sendNotificationSync);
            auto* raw = p.apvts.getRawParameterValue (id);
            if (raw != nullptr && std::fabs (raw->load() - value) < 1.0e-4f) ++boundReverse;
        }
    }
    Patch afterReverse; FxChain::Settings unusedAfterReverse;
    p.collectPatch (afterReverse, unusedAfterReverse);
    bool collectedReverse = true;
    for (int s = 0; s < 8; ++s)
        collectedReverse = collectedReverse && std::fabs (afterReverse.slots[s].params.grainReverse - 0.1f * (float) (s + 1)) < 1.0e-4f;
    check (visibleReverse == 8 && boundReverse == 8 && collectedReverse,
           "восемь регуляторов Reverse доступны в секциях SLOT и связаны с DSP",
           juce::String (visibleReverse) + "/8 видимы, " + juce::String (boundReverse) + "/8 связаны");

    // Все подписи, включая девять вариантов смысла макросов и источника,
    // должны помещаться при увеличенном шрифте в исходной геометрии 1280×800.
    editor->showPage (201);
    setP (p, "sA_type", (float) EngineType::Granular);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    const int contentWidth = viewport != nullptr && viewport->getViewedComponent() != nullptr
                           ? viewport->getViewedComponent()->getWidth() : 0;
    const int captionWidth = contentWidth > 0 ? contentWidth / juce::jlimit (2, 5, contentWidth / 260) - 4 : 0;
    float widest = 0.0f;
    juce::String longest;
    int measured = 0, tooWide = 0;
    auto measureLabel = [&] (const juce::String& label)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (orFont (Palette::paramLabelSize), label, 0.0f, 0.0f);
        const float w = glyphs.getBoundingBox (0, -1, true).getWidth();
        if (w > widest) { widest = w; longest = label; }
        ++measured;
        if (w > (float) captionWidth) ++tooWide;
    };
    for (const auto& tab : buildUiTabs())
        for (const auto& section : tab.sections)
            for (const auto& id : section.ids)
                if (auto* parameter = p.apvts.getParameter (id))
                {
                    const auto name = parameter->getName (64);
                    const bool slotId = id.length() >= 5 && id[0] == 's' && id[1] >= 'A' && id[1] <= 'H' && id[2] == '_';
                    const auto field = slotId ? id.substring (3) : juce::String();
                    const bool macro = field.length() == 2 && field[0] == 'p' && field[1] >= '0' && field[1] <= '3';
                    if (macro || field == "table" || field == "grainReverse")
                    {
                        for (int type = 0; type < 9; ++type)
                        {
                            const juce::String meaning = macro ? engineMacroName (type, (int) (field[1] - '0'))
                                : field == "grainReverse" ? (type == (int) EngineType::Granular ? "PROBABILITY 0-1" : "UNUSED")
                                                         : engineSourceControlName (type);
                            measureLabel (name + "  ·  " + meaning);
                        }
                    }
                    else measureLabel (name);
                }
    auto nonMonoPixels = [] (const juce::Image& img)
    {
        long long count = 0;
        for (int y = 0; y < img.getHeight(); ++y)
            for (int x = 0; x < img.getWidth(); ++x)
            {
                const auto c = img.getPixelAt (x, y);
                if (c.getRed() != c.getGreen() || c.getGreen() != c.getBlue()) ++count;
            }
        return count;
    };
    long long nonMono = nonMonoPixels (capture (*editor));
    // И страницы ALL, где появляется полоса прокрутки: стандартный JUCE
    // раньше рисовал её синим, хотя короткая страница выглядела монохромной.
    for (int tab = 0; tab < 5; ++tab)
    {
        editor->showPage (100 + tab);
        nonMono += nonMonoPixels (capture (*editor));
    }
    std::printf ("   PARAMS: шрифт %.1f px, %d вариантов, max %.1f/%d px (%s), не-монохромных пикселей %lld на 6 страницах\n",
                 Palette::paramLabelSize, measured, widest, captionWidth, longest.toRawUTF8(), nonMono);
    check (measured >= 306 && captionWidth > 0 && tooWide == 0,
           "все подписи параметров и девять контекстов помещаются в колонку",
           juce::String (tooWide) + " обрезаны");
    check (nonMono == 0, "шесть страниц PARAMS монохромны, включая поля и полосу прокрутки",
           juce::String (nonMono) + " пикселей");
   #if JUCE_LINUX || JUCE_BSD
    if (std::getenv ("OROGEN_GUI_NATIVE") != nullptr)
    {
        editor->showPage (201);
        editor->addToDesktop (0); editor->setTopLeftPosition (0, 0); editor->setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        if (aReverse != nullptr) aReverse->showTextBox();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        juce::Label* valueBox = nullptr;
        if (aReverse != nullptr)
            for (auto* child : aReverse->getChildren())
                if (auto* label = dynamic_cast<juce::Label*> (child)) valueBox = label;
        const bool editing = valueBox != nullptr && valueBox->getCurrentTextEditor() != nullptr;
        const auto focusedImage = capture (*editor);
        const auto focusedNonMono = nonMonoPixels (focusedImage);
        std::printf ("   редактирование числа: активно %s, не-монохромных пикселей %lld, LNF %s\n",
                     editing ? "да" : "нет", focusedNonMono,
                     aReverse != nullptr && dynamic_cast<OrLookAndFeel*> (&aReverse->getLookAndFeel()) != nullptr ? "OROGEN" : "другой");
        if (focusedNonMono != 0)
        {
            int shown = 0;
            for (int y = 0; y < focusedImage.getHeight() && shown < 5; ++y)
                for (int x = 0; x < focusedImage.getWidth() && shown < 5; ++x)
                {
                    const auto c = focusedImage.getPixelAt (x, y);
                    if (c.getRed() != c.getGreen() || c.getGreen() != c.getBlue())
                    {
                        std::printf ("   цветной пиксель %d,%d = %u,%u,%u\n", x, y, c.getRed(), c.getGreen(), c.getBlue());
                        ++shown;
                    }
                }
        }
        check (editing && focusedNonMono == 0,
               "фокус и выделение числового поля остаются монохромными",
               juce::String (focusedNonMono) + " пикселей");
        if (valueBox != nullptr) valueBox->hideEditor (true);
        editor->removeFromDesktop();
    }
   #endif
    for (int s = 0; s < 8; ++s) setP (p, slotPrefix (s) + "grainReverse", 0.0f);

    // Все четыре новых параметра представлены живыми SliderAttachment.
    const auto tabs = buildUiTabs();
    int plateSection = -1;
    for (int i = 0; i < (int) tabs[1].sections.size(); ++i)
        if (tabs[1].sections[(size_t) i].title == "PLATE") plateSection = i;
    if (plateSection >= 0) editor->showPage (301 + plateSection);
    const char* ids[4] = { "fx_platePreFilter", "fx_plateInputDiff1", "fx_plateInputDiff2", "fx_plateOutput" };
    const float values[4] = { 0.23f, 0.41f, 0.73f, 1.27f };
    int visiblePlate = 0, updatedPlate = 0;
    for (int i = 0; i < 4; ++i)
    {
        auto* slider = overlay != nullptr ? findSlider (*overlay, ids[i]) : nullptr;
        if (slider != nullptr && effectivelyVisible (*slider, overlay))
        {
            ++visiblePlate;
            slider->setValue (values[i], juce::sendNotificationSync);
            auto* raw = p.apvts.getRawParameterValue (ids[i]);
            if (raw != nullptr && std::fabs (raw->load() - values[i]) < 1.0e-4f) ++updatedPlate;
        }
    }
    Patch afterGui; FxChain::Settings plateAfterGui;
    p.collectPatch (afterGui, plateAfterGui);
    check (visiblePlate == 4 && updatedPlate == 4
           && std::fabs (plateAfterGui.platePreFilter - values[0]) < 1.0e-4f
           && std::fabs (plateAfterGui.plateInputDiff1 - values[1]) < 1.0e-4f
           && std::fabs (plateAfterGui.plateInputDiff2 - values[2]) < 1.0e-4f
           && std::fabs (plateAfterGui.plateOutput - values[3]) < 1.0e-4f,
           "четыре регулятора Plate в GUI меняют реальные настройки FX",
           juce::String (visiblePlate) + "/4 видимы, " + juce::String (updatedPlate) + "/4 связаны");

    editor->setSize (960, 600);     // минимальный поддерживаемый размер окна
    editor->showPage (101);        // FX / ALL
    int sectionBtns = 0, boundsErrors = 0, lastRight = 0;
    juce::Button* robinButton = nullptr;
    for (auto* child : overlay->getChildren())
        if (auto* button = dynamic_cast<juce::Button*> (child))
            if (button->isVisible() && button->getY() == 46)
            {
                ++sectionBtns;
                if (button->getX() < lastRight || button->getRight() > overlay->getWidth() - 140) ++boundsErrors;
                lastRight = button->getRight();
                if (button->getButtonText() == "ROBIN") robinButton = button;
            }
    if (robinButton != nullptr) robinButton->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (80);
    const auto narrowIds = visibleBoundIds (*overlay);
    check (sectionBtns == 14 && boundsErrors == 0 && narrowIds.contains ("fx_robinMix"),
           "в узком окне все 13 FX-секций помещаются и последняя открывается мышью",
           juce::String (sectionBtns) + " кнопок, " + juce::String (boundsErrors) + " ошибок геометрии");
    editor->setSize (1280, 800);

    // Пользователь может менять ENGINE, пока MIDI-нота удерживается. Сравниваем
    // с идентичным инстансом без переключения, НЕ посылая повторный Note On.
    std::printf ("\n--- 17. Смена движка GUI на удержанной ноте ---\n");
    editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
    editor->showPage (0);
    OrogenProcessor unchanged;
    for (auto* processor : { &p, &unchanged })
    {
        processor->setCurrentProgram (0);
        processor->setPlayConfigDetails (0, 2, 48000.0, 512);
        processor->prepareToPlay (48000.0, 512);
        soloSlot (*processor, 0, 0);
        setP (*processor, "gl_mode", 0.0f);
        setP (*processor, "sA_sustain", 0.8f);
        setP (*processor, "sA_tail", 0.0f);
        for (const char* id : { "fx_drive", "fx_chorusMix", "fx_delayMix", "fx_reverbMix", "fx_plateMix",
                                "fx_materialMix", "fx_spectreMix", "fx_glitchMix", "fx_robinMix", "fx_combMix" })
            setP (*processor, id, 0.0f);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    juce::ComboBox* selector = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* box = dynamic_cast<juce::ComboBox*> (child))
            if (box->isVisible() && box->getNumItems() == 9) { selector = box; break; }
    juce::AudioBuffer<float> a (2, 512), b (2, 512);
    juce::MidiBuffer noteOn, emptyMidi;
    noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    double beforeEnergy = 0.0;
    float lastBefore = 0.0f;
    for (int block = 0; block < 30; ++block)
    {
        auto& midi = block == 0 ? noteOn : emptyMidi;
        p.processBlock (a, midi); unchanged.processBlock (b, midi);
        if (block >= 20)
            for (int i = 0; i < 512; ++i) beforeEnergy += (double) a.getSample (0, i) * a.getSample (0, i);
        lastBefore = a.getSample (0, 511);
    }
    if (selector != nullptr) selector->setSelectedItemIndex (4, juce::sendNotificationSync);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    double afterEnergy = 0.0, diffEnergy = 0.0;
    float firstAfter = 0.0f, prevA = lastBefore, prevB = lastBefore;
    float maxStepA = 0.0f, maxStepB = 0.0f;
    int maxStepAt = 0, badSwap = 0;
    for (int block = 0; block < 60; ++block)
    {
        p.processBlock (a, emptyMidi); unchanged.processBlock (b, emptyMidi);
        if (block == 0) firstAfter = a.getSample (0, 0);
        for (int i = 0; i < 512; ++i)
        {
            const float x = a.getSample (0, i), y = b.getSample (0, i);
            if (! std::isfinite (x) || ! std::isfinite (y)) ++badSwap;
            if (block < 20)
            {
                if (std::fabs (x - prevA) > maxStepA) { maxStepA = std::fabs (x - prevA); maxStepAt = block * 512 + i; }
                maxStepB = std::max (maxStepB, std::fabs (y - prevB));
            }
            prevA = x; prevB = y;
            if (block >= 20)
            {
                afterEnergy += (double) x * x;
                const double delta = (double) x - y;
                diffEnergy += delta * delta;
            }
        }
    }
    const double beforeRms = std::sqrt (beforeEnergy / (10.0 * 512.0));
    const double afterRms = std::sqrt (afterEnergy / (40.0 * 512.0));
    const double swapDiff = std::sqrt (diffEnergy / (40.0 * 512.0));
    std::printf ("   до %.5f, после %.5f, ΔRMS с неизменённым %.5f, переход %.5f, NaN %d\n",
                 beforeRms, afterRms, swapDiff, std::fabs (firstAfter - lastBefore), badSwap);
    std::printf ("   максимальный соседний скачок после смены %.5f на отсчёте %d; без смены %.5f\n",
                 maxStepA, maxStepAt, maxStepB);
    check (selector != nullptr && (int) p.apvts.getRawParameterValue ("sA_type")->load() == 4
           && beforeRms > 1.0e-3 && afterRms > 1.0e-3 && swapDiff > 1.0e-3 && badSwap == 0,
           "GUI меняет звук удержанной ноты без повторного Note On",
           num (afterRms, 5) + " RMS после");
}

// Те же проблемы Granular проверяем через AudioProcessor/APVTS и MIDI, а не
// только вызовом GranularEngine. Два инстанса дают одинаковый детерминированный
// сигнал; во втором Density не трогаем, чтобы отделить щелчок от формы волны.
static void testGranularProcessorRegression()
{
    std::printf ("\n--- 18. Granular через AudioProcessor: атака и автоматизация Density ---\n");
    OrogenProcessor changed, reference;
    for (auto* pr : { &changed, &reference })
    {
        pr->setPlayConfigDetails (0, 2, 48000.0, 128);
        pr->prepareToPlay (48000.0, 128);
        pr->setCurrentProgram (0);
        soloSlot (*pr, 0, 4);
        setP (*pr, "gl_mode", 0.0f);
        setP (*pr, "gl_gain", 0.7f);
        setP (*pr, "gl_master", 0.8f);
        setP (*pr, "arpOn", 0.0f);
        setP (*pr, "lfo1_amount", 0.0f); setP (*pr, "lfo2_amount", 0.0f);
        for (int k = 1; k <= 8; ++k) setP (*pr, "mod" + juce::String (k) + "_amt", 0.0f);
        setP (*pr, "sA_level", 0.8f); setP (*pr, "sA_table", 0.0f);
        setP (*pr, "sA_detune", 0.0f);
        setP (*pr, "sA_p0", 0.35f); setP (*pr, "sA_p1", 0.45f);
        setP (*pr, "sA_p2", 0.0f); setP (*pr, "sA_p3", 0.0f);
        setP (*pr, "sA_attack", 0.001f); setP (*pr, "sA_sustain", 1.0f);
        setP (*pr, "sA_tail", 0.0f); setP (*pr, "sA_cutoff", 1.0f); setP (*pr, "sA_res", 0.0f);
        for (const char* id : { "fx_drive", "fx_chorusMix", "fx_delayMix", "fx_reverbMix", "fx_plateMix",
                                "fx_materialMix", "fx_spectreMix", "fx_glitchMix", "fx_robinMix", "fx_combMix" })
            setP (*pr, id, 0.0f);
        setP (*pr, "fx_compRatio", 1.0f); setP (*pr, "fx_compMakeup", 0.0f);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (160);

    juce::AudioBuffer<float> a (2, 128), b (2, 128);
    juce::MidiBuffer note, empty;
    note.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    int first = -1, bad = 0;
    for (int block = 0; block < 400; ++block)
    {
        auto& m = block == 0 ? note : empty;
        changed.processBlock (a, m); reference.processBlock (b, m);
        for (int i = 0; i < 128; ++i)
        {
            const float x = a.getSample (0, i), y = b.getSample (0, i);
            if (! std::isfinite (x) || ! std::isfinite (y)) ++bad;
            if (first < 0 && std::fabs (x) > 1.0e-5f) first = block * 128 + i;
        }
    }
    // ROBIN держит постоянную задержку 1024 отсчёта даже при mix=0; отделяем
    // её от времени атаки движка и не выдаём тест за полностью сухой тракт.
    const int latency = changed.reportedLatency();
    const double onsetMs = first >= 0 ? first / 48.0 : -1.0;
    const double intrinsicMs = first >= latency ? (first - latency) / 48.0 : -1.0;
    std::printf ("   note-on: первый отсчёт %d / %.3f мс, latency %d, после неё %.3f мс; NaN %d\n",
                 first, onsetMs, latency, intrinsicMs, bad);
    check (latency == 1024 && first >= latency && first - latency < 960 && bad == 0,
           "при минимальной Density атака доходит через плагин без 250-мс паузы",
           num (onsetMs, 3) + " мс, latency " + juce::String (latency));

    // Оба процессора начали одну и ту же ноту с Detune = 0 и Spread = 0.
    // Меняем только APVTS-параметр первого, не отправляя повторный note-on.
    setP (changed, "sA_detune", 1.0f);
    double detuneDelta = 0.0, detuneReference = 0.0;
    for (int block = 0; block < 200; ++block)
    {
        changed.processBlock (a, empty); reference.processBlock (b, empty);
        if (block < 16) continue; // исключить ROBIN latency
        for (int i = 0; i < 128; ++i)
        {
            const double x = a.getSample (0, i), y = b.getSample (0, i), d = x - y;
            detuneDelta += d * d; detuneReference += y * y;
        }
    }
    const double detuneRelative = std::sqrt (detuneDelta / std::max (1.0e-20, detuneReference));
    std::printf ("   APVTS Detune 0 -> 1 на удержанной ноте: относительная ΔRMS %.6f\n", detuneRelative);
    check (detuneRelative > 0.01, "Detune Granular действует через плагин и MIDI", num (detuneRelative, 6));

    // Вернуть оба детерминированных планировщика к тем же параметрам и дать
    // затухнуть старым зёрнам и DC-блокеру, прежде чем измерять Density.
    setP (changed, "sA_detune", 0.0f);
    for (int block = 0; block < 320; ++block)
    {
        changed.processBlock (a, empty); reference.processBlock (b, empty);
    }
    // При 4 зёрнах/с есть настоящие паузы. Ищем начало следующего зерна,
    // чтобы проверка после latency не оказалась в нулевом участке и не
    // объявила тишину доказательством отсутствия щелчка.
    bool gapSeen = false, onsetSeen = false;
    for (int block = 0; block < 300 && ! onsetSeen; ++block)
    {
        changed.processBlock (a, empty); reference.processBlock (b, empty);
        double energy = 0.0;
        for (int i = 0; i < 128; ++i) energy += (double) b.getSample (0, i) * b.getSample (0, i);
        const double blockRms = std::sqrt (energy / 128.0);
        if (blockRms < 1.0e-4) gapSeen = true;
        if (gapSeen && blockRms > 0.005) onsetSeen = true;
    }
    check (gapSeen && onsetSeen, "для Density найдено звучащее зерно после паузы");

    setP (changed, "sA_p2", 1.0f);
    float earlyDiff = 0.0f;
    double referenceEnergy = 0.0;
    int measured = 0;
    // Измеряем именно сэмплы, которые прошли через изменённый движок, а не
    // старый сигнал, ещё находящийся в задержке эффекта.
    const int blocksToMeasure = (latency + 128 + 127) / 128;
    for (int block = 0; block < blocksToMeasure; ++block)
    {
        changed.processBlock (a, empty); reference.processBlock (b, empty);
        for (int i = 0; i < 128; ++i)
        {
            const int at = block * 128 + i;
            if (at < latency || at >= latency + 128) continue;
            const float x = a.getSample (0, i), y = b.getSample (0, i);
            earlyDiff = std::max (earlyDiff, std::fabs (x - y));
            referenceEnergy += (double) y * y;
            ++measured;
        }
    }
    const double refRms = std::sqrt (referenceEnergy / std::max (1, measured));
    std::printf ("   после задержки, Density 0 → 1: max |Δ| %.8f, RMS эталона %.6f, %d отсч.\n",
                 earlyDiff, refRms, measured);
    check (measured == 128 && earlyDiff < 1.0e-5f && refRms > 1.0e-4,
           "Density не скачком меняет звучащее зерно в плагине",
           "max diff " + num (earlyDiff, 8));

    double deltaEnergy = 0.0;
    for (int block = 0; block < 40; ++block)
    {
        changed.processBlock (a, empty); reference.processBlock (b, empty);
        for (int i = 0; i < 128; ++i)
        {
            const double d = (double) a.getSample (0, i) - b.getSample (0, i);
            deltaEnergy += d * d;
        }
    }
    const double diffRms = std::sqrt (deltaEnergy / (40.0 * 128.0));
    std::printf ("   последующие 107 мс: ΔRMS %.6f\n", diffRms);
    check (diffRms > 1.0e-4, "автоматизация Density действительно меняет звук", num (diffRms, 6));
}

// Через настоящий APVTS и удержанную ноту: изменение только Reverse должно
// пройти hashParams -> collectPatch -> Voice::paramsEqual -> новые зёрна.
static void testGranularReverseProcessor()
{
    std::printf ("\n--- Granular Reverse через AudioProcessor и MIDI ---\n");
    OrogenProcessor changed, reference;
    for (auto* pr : { &changed, &reference })
    {
        pr->setPlayConfigDetails (0, 2, 48000.0, 128); pr->prepareToPlay (48000.0, 128);
        pr->setCurrentProgram (0); soloSlot (*pr, 0, 4);
        setP (*pr, "gl_mode", 0.0f); setP (*pr, "gl_gain", 0.7f); setP (*pr, "gl_master", 0.8f);
        setP (*pr, "arpOn", 0.0f); setP (*pr, "lfo1_amount", 0.0f); setP (*pr, "lfo2_amount", 0.0f);
        for (int k = 1; k <= 8; ++k) setP (*pr, "mod" + juce::String (k) + "_amt", 0.0f);
        setP (*pr, "sA_level", 0.8f); setP (*pr, "sA_table", 0.0f); setP (*pr, "sA_detune", 0.0f);
        setP (*pr, "sA_p0", 0.35f); setP (*pr, "sA_p1", 0.45f);
        setP (*pr, "sA_p2", 0.7f); setP (*pr, "sA_p3", 0.3f);
        setP (*pr, "sA_grainReverse", 0.0f);
        setP (*pr, "sA_attack", 0.001f); setP (*pr, "sA_sustain", 1.0f);
        setP (*pr, "sA_tail", 0.0f); setP (*pr, "sA_cutoff", 1.0f); setP (*pr, "sA_res", 0.0f);
        for (const char* id : { "fx_drive", "fx_chorusMix", "fx_delayMix", "fx_reverbMix", "fx_plateMix",
                                "fx_materialMix", "fx_spectreMix", "fx_glitchMix", "fx_robinMix", "fx_combMix" })
            setP (*pr, id, 0.0f);
        setP (*pr, "fx_compRatio", 1.0f); setP (*pr, "fx_compMakeup", 0.0f);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (160);
    juce::AudioBuffer<float> a (2, 128), b (2, 128);
    juce::MidiBuffer noteA, noteB, emptyA, emptyB;
    noteA.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    noteB.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    for (int block = 0; block < 80; ++block)
    {
        changed.processBlock (a, block == 0 ? noteA : emptyA);
        reference.processBlock (b, block == 0 ? noteB : emptyB);
    }
    setP (changed, "sA_grainReverse", 1.0f);
    double difference = 0.0, referenceEnergy = 0.0;
    int bad = 0, n = 0;
    for (int block = 0; block < 500; ++block)
    {
        changed.processBlock (a, emptyA); reference.processBlock (b, emptyB);
        if (block < 16) continue; // ROBIN latency и переход зерна
        for (int i = 0; i < 128; ++i)
        {
            const float x = a.getSample (0, i), y = b.getSample (0, i);
            if (! std::isfinite (x) || ! std::isfinite (y)) ++bad;
            const double d = (double) x - y;
            difference += d * d; referenceEnergy += (double) y * y; ++n;
        }
    }
    const double relative = std::sqrt (difference / std::max (1.0e-20, referenceEnergy));
    const double referenceRms = std::sqrt (referenceEnergy / std::max (1, n));
    std::printf ("   Reverse 0 -> 100%% на удержанной ноте: ΔRMS/RMS %.6f, эталон %.6f, NaN %d\n",
                 relative, referenceRms, bad);
    check (bad == 0 && referenceRms > 1.0e-3 && relative > 0.1,
           "APVTS Reverse меняет звук через MIDI без повторного Note On", num (relative, 6));
}

// Заводской Sub Bass на 96 кГц: проверяем реальную частоту на выходе
// процессора/MIDI после задержки ROBIN, а не только внутреннюю позицию зерна.
static void testGranularPitchProcessorRegression()
{
    std::printf ("\n--- Granular pitch 96 кГц через AudioProcessor и MIDI ---\n");
    constexpr double sr = 96000.0;
    OrogenProcessor p;
    p.setPlayConfigDetails (0, 2, sr, 128); p.prepareToPlay (sr, 128); p.setCurrentProgram (0);
    soloSlot (p, 0, 4); setP (p, "gl_mode", 0.0f); setP (p, "arpOn", 0.0f);
    setP (p, "gl_gain", 0.7f); setP (p, "gl_master", 0.8f);
    setP (p, "lfo1_amount", 0.0f); setP (p, "lfo2_amount", 0.0f);
    for (int k = 1; k <= 8; ++k) setP (p, "mod" + juce::String (k) + "_amt", 0.0f);
    setP (p, "sA_table", 7.0f); setP (p, "sA_p0", 0.95f); setP (p, "sA_p1", 1.0f);
    setP (p, "sA_p2", 0.0f); setP (p, "sA_p3", 0.0f);
    setP (p, "sA_tune", 0.0f); setP (p, "sA_detune", 0.0f); setP (p, "sA_level", 0.8f);
    setP (p, "sA_attack", 0.001f); setP (p, "sA_sustain", 1.0f); setP (p, "sA_tail", 0.0f);
    setP (p, "sA_cutoff", 1.0f); setP (p, "sA_res", 0.0f);
    for (const char* id : { "fx_drive", "fx_chorusMix", "fx_delayMix", "fx_reverbMix", "fx_plateMix",
                            "fx_materialMix", "fx_spectreMix", "fx_glitchMix", "fx_robinMix", "fx_combMix" })
        setP (p, id, 0.0f);
    setP (p, "fx_compRatio", 1.0f); setP (p, "fx_compMakeup", 0.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
    const int latency = p.reportedLatency();
    juce::MidiBuffer note, empty; note.addEvent (juce::MidiMessage::noteOn (1, 37, 0.8f), 0);
    juce::AudioBuffer<float> block (2, 128);
    double first = 0.0, last = 0.0, energy = 0.0;
    float prev = 0.0f; int n = 0, bad = 0, crossings = 0;
    for (int b = 0; b < 180; ++b)
    {
        empty.clear(); p.processBlock (block, b == 0 ? note : empty);
        for (int i = 0; i < 128; ++i)
        {
            const int at = b * 128 + i;
            const float x = block.getSample (0, i);
            if (! std::isfinite (x)) ++bad;
            if (at >= latency + 3840 && at < latency + 19200)
            {
                energy += (double) x * x; ++n;
                if (prev < 0.0f && x >= 0.0f)
                {
                    const double cross = at - 1.0 + (double) (-prev) / (double) (x - prev);
                    if (crossings++ == 0) first = cross;
                    last = cross;
                }
            }
            prev = x;
        }
    }
    const double measured = crossings > 1 ? (crossings - 1) * sr / (last - first) : 0.0;
    const double expected = 110.0 * std::pow (2.0, -8.0 / 12.0);
    const double cents = measured > 0.0 ? 1200.0 * std::log2 (measured / expected) : 1.0e6;
    const double rms = std::sqrt (energy / std::max (1, n));
    std::printf ("   выход %.6f Гц / эталон %.6f Гц = %+.4f цента; latency %d, RMS %.6f, NaN %d\n",
                 measured, expected, cents, latency, rms, bad);
    check (latency == 1024 && bad == 0 && crossings >= 8 && rms > 1.0e-4 && std::fabs (cents) < 1.0,
           "поздний Position не расстраивает MIDI-ноту в AudioProcessor", num (cents, 4) + " cents");
}

// Проверка Delay через APVTS, MIDI и реальную задержку ROBIN. Два процессора
// получают одинаковую ноту, различается только автоматизация Delay Time.
static void testDelayProcessorRegression()
{
    std::printf ("\n--- 19. Delay Time через AudioProcessor и MIDI ---\n");
    OrogenProcessor changed, reference;
    for (auto* pr : { &changed, &reference })
    {
        pr->setPlayConfigDetails (0, 2, 48000.0, 128);
        pr->prepareToPlay (48000.0, 128);
        pr->setCurrentProgram (0);
        soloSlot (*pr, 0, 0);                     // Wavetable, один активный слот
        setP (*pr, "gl_mode", 0.0f); setP (*pr, "gl_gain", 0.7f); setP (*pr, "gl_master", 0.8f);
        setP (*pr, "arpOn", 0.0f);
        setP (*pr, "lfo1_amount", 0.0f); setP (*pr, "lfo2_amount", 0.0f);
        for (int k = 1; k <= 8; ++k) setP (*pr, "mod" + juce::String (k) + "_amt", 0.0f);
        setP (*pr, "sA_level", 0.8f); setP (*pr, "sA_attack", 0.001f);
        setP (*pr, "sA_sustain", 1.0f); setP (*pr, "sA_tail", 0.0f);
        setP (*pr, "sA_cutoff", 1.0f); setP (*pr, "sA_res", 0.0f);
        for (const char* id : { "fx_drive", "fx_chorusMix", "fx_reverbMix", "fx_plateMix",
                                "fx_materialMix", "fx_spectreMix", "fx_glitchMix", "fx_robinMix", "fx_combMix" })
            setP (*pr, id, 0.0f);
        setP (*pr, "fx_delayMix", 1.0f); setP (*pr, "fx_delayTime", 0.02f); setP (*pr, "fx_delayFb", 0.0f);
        setP (*pr, "fx_compRatio", 1.0f); setP (*pr, "fx_compMakeup", 0.0f);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (160);
    juce::AudioBuffer<float> a (2, 128), b (2, 128);
    juce::MidiBuffer note, empty;
    note.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    float priorDiff = 0.0f; double referenceEnergy = 0.0;
    for (int block = 0; block < 150; ++block)
    {
        auto& midi = block == 0 ? note : empty;
        changed.processBlock (a, midi); reference.processBlock (b, midi);
        for (int i = 0; i < 128; ++i)
        {
            const float x = a.getSample (0, i), y = b.getSample (0, i);
            priorDiff = std::max (priorDiff, std::fabs (x - y));
            if (block >= 100) referenceEnergy += (double) y * y;
        }
    }
    const int latency = changed.getLatencySamples();
    setP (changed, "fx_delayTime", 0.080f);
    float first = -1.0f; double laterDiff = 0.0; int count = 0, bad = 0;
    for (int block = 0; block < 60; ++block)
    {
        changed.processBlock (a, empty); reference.processBlock (b, empty);
        for (int i = 0; i < 128; ++i)
        {
            const int at = block * 128 + i;
            const float x = a.getSample (0, i), y = b.getSample (0, i);
            if (! std::isfinite (x) || ! std::isfinite (y)) ++bad;
            if (at == latency) first = std::fabs (x - y);
            if (at >= latency + 2400) { laterDiff += (double) (x - y) * (x - y); ++count; }
        }
    }
    const double refRms = std::sqrt (referenceEnergy / (50.0 * 128.0));
    const double diffRms = std::sqrt (laterDiff / std::max (1, count));
    std::printf ("   latency %d, до смены max Δ %.8f; первый после неё Δ %.8f; позднее ΔRMS %.6f, ref RMS %.6f, NaN %d\n",
                 latency, priorDiff, first, diffRms, refRms, bad);
    check (latency == 1024 && priorDiff < 1.0e-6f && first >= 0.0f && first < 1.0e-5f && bad == 0,
           "Time не перескакивает на первом слышимом отсчёте", num (first, 8));
    check (refRms > 0.001 && count > 0 && diffRms > 1.0e-4,
           "новое значение Delay Time действительно меняет звук", num (diffRms, 6));
}

// Снимаем реальные clip bounds paint() нативного окна. Обычный снимок
// компонента всегда перерисовывает весь экран и не обнаруживает лишний repaint.
class MeterPaintProbe : public OrogenEditor
{
public:
    explicit MeterPaintProbe (OrogenProcessor& p) : OrogenEditor (p) {}
    void paint (juce::Graphics& g) override
    {
        if (recording)
        {
            const auto r = g.getClipBounds();
            ++calls;
            pixels += (long long) r.getWidth() * (long long) r.getHeight();
            maxArea = std::max (maxArea, r.getWidth() * r.getHeight());
            if (r.getWidth() >= getWidth() && r.getHeight() >= getHeight()) ++full;
        }
        OrogenEditor::paint (g);
    }
    bool recording = false;
    int calls = 0, full = 0, maxArea = 0;
    long long pixels = 0;
};

static void testNativeMeterPaint()
{
   #if JUCE_LINUX || JUCE_BSD
    if (std::getenv ("OROGEN_GUI_NATIVE") == nullptr) return;
    std::printf ("\n--- Нативный GUI: обновление метров во время ноты ---\n");
    OrogenProcessor p;
    p.setCurrentProgram (0);
    p.setPlayConfigDetails (0, 2, 48000.0, 512);
    p.prepareToPlay (48000.0, 512);
    soloSlot (p, 0, (int) EngineType::Granular);
    setP (p, "sA_attack", 0.005f); setP (p, "sA_decay", 0.1f);
    setP (p, "sA_sustain", 1.0f); setP (p, "sA_release", 0.05f); setP (p, "sA_tail", 0.0f);
    MeterPaintProbe editor (p);
    editor.setAnimationsEnabled (false);
    editor.showPage (0);
    editor.addToDesktop (0); editor.setTopLeftPosition (0, 0); editor.setVisible (true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (1800);
    auto native = [&] {
        return editor.getPeer() != nullptr
            ? juce::createSnapshotOfNativeWindow (editor.getPeer()->getNativeHandle()) : juce::Image();
    };
    const auto before = native();
    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi, empty;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
    editor.recording = true;
    for (int k = 0; k < 25; ++k)
    {
        for (int j = 0; j < 4; ++j)
        {
            block.clear();
            p.processBlock (block, k == 0 && j == 0 ? midi : empty);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (45);
    }
    editor.recording = false;
    const auto during = native();
    const auto delta = [&] (const juce::Image& a, const juce::Image& b) {
        if (! a.isValid() || ! b.isValid()) return -1;
        int n = 0;
        for (int y = 90; y < 104; ++y) for (int x = 21; x < 36; ++x)
            if (a.getPixelAt (x, y) != b.getPixelAt (x, y)) ++n;
        return n;
    };
    const int changedPixels = delta (before, during);
    // Хост может остановить аудиоколбэк вообще, даже пока GUI открыт.
    // Должно погаснуть без вызовов processBlock и снова загореться после них.
    const auto revisionBeforeStall = p.engine().meterRevision();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (1300);
    const auto stalled = native();
    const auto revisionAfterStall = p.engine().meterRevision();
    for (int k = 0; k < 12; ++k)
    {
        for (int j = 0; j < 4; ++j) { block.clear(); p.processBlock (block, empty); }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (45);
    }
    const auto resumed = native();
    const auto revisionAfterResume = p.engine().meterRevision();
    const int stallPixels = delta (during, stalled), resumePixels = delta (stalled, resumed);
    juce::MidiBuffer noteOff;
    noteOff.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    for (int k = 0; k < 54; ++k)
    {
        for (int j = 0; j < 4; ++j)
        {
            block.clear();
            p.processBlock (block, k == 0 && j == 0 ? noteOff : empty);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
    }
    const auto after = native();
    juce::Image fresh (juce::Image::ARGB, 1280, 800, true);
    { juce::Graphics g (fresh); editor.paintEntireComponent (g, true); }
    int stalePixels = 0;
    if (after.isValid())
        for (int y = 84; y < 110; ++y) for (int x = 16; x < 41; ++x)
            if (after.getPixelAt (x, y) != fresh.getPixelAt (x, y)) ++stalePixels;
    const int fadePixels = delta (during, after);
    std::printf ("   paint: %d вызовов, %d полных, средний clip %.0f px, max %d; LED вход/спад Δ %d/%d px, stale %d px\n",
                 editor.calls, editor.full, editor.calls ? (double) editor.pixels / editor.calls : 0.0,
                 editor.maxArea, changedPixels, fadePixels, stalePixels);
    std::printf ("   остановка/возобновление processBlock: Δ %d/%d px, ревизия %u → %u → %u\n",
                 stallPixels, resumePixels, revisionBeforeStall, revisionAfterStall, revisionAfterResume);
    check (editor.calls > 3 && editor.full == 0 && editor.maxArea <= 500,
           "метры обновляют только свечение индикатора, не всё окно",
           juce::String (editor.full) + " полных кадров, max " + juce::String (editor.maxArea) + " px");
    check (changedPixels > 3 && fadePixels > 3,
           "индикатор загорается и затухает в нативном окне",
           juce::String (changedPixels) + "/" + juce::String (fadePixels) + " px");
    check (after.isValid() && stalePixels == 0,
           "частичный repaint не оставляет следов вокруг индикатора",
           juce::String (stalePixels) + " px");
    check (revisionBeforeStall == revisionAfterStall && revisionAfterResume != revisionAfterStall
           && stallPixels > 3 && resumePixels > 3,
           "при остановке аудиоколбэка метр гаснет, а при возобновлении снова работает",
           juce::String (stallPixels) + "/" + juce::String (resumePixels) + " px");
    editor.removeFromDesktop();
    p.releaseResources();
   #endif
}

int main (int argc, char** argv)
{
    // Голова без X11: GUI-инициализация здесь упала бы (нет дисплея), а
    // аудио-плагину нужен только менеджер сообщений — он же крутит AsyncUpdater,
    // который сообщает хосту новую задержку.
    std::signal (SIGSEGV, crashTrace);
    std::signal (SIGABRT, crashTrace);
    auto* mm = juce::MessageManager::getInstance();
    std::printf ("OROGEN test_plugin: старт\n");
    juce::ignoreUnused (mm);
    if (argc > 1 && juce::String (argv[1]) == "--delay-only")
    {
        testDelayProcessorRegression();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        std::printf ("\nDelay AudioProcessor: провалов %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "--lifetime-only")
    {
        testEditorControlLifetime();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        std::printf ("\nВремя жизни GUI: провалов %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "--pitch-only")
    {
        testGranularPitchProcessorRegression();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        std::printf ("\nGranular pitch AudioProcessor: провалов %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "--granular-only")
    {
        testGranularProcessorRegression();
        testGranularReverseProcessor();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        std::printf ("\nGranular AudioProcessor: провалов %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "--meter-ui-only")
    {
        testNativeMeterPaint();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        std::printf ("\nНативные метры GUI: провалов %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "--gui-only")
    {
        testGuiRegressions();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        std::printf ("\nGUI-регрессии: провалов %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    const double sr = 48000.0;

    // ---------------------------------------------------------------- 1
    std::printf ("\n--- 1. Параметры хоста ---\n");
    {
        OrogenProcessor p;
        const int count = p.getParameters().size();
        std::printf ("   параметров: %d, программ: %d\n", count, p.getNumPrograms());
        check (count == 306, "все параметры объявлены", num (count, 0));

        // Снимок последовательности ID rev21 до добавления Plate. Хэшируем
        // первые 294 ID с '\\n' между ними: даже перестановка не пройдёт.
        std::uint64_t legacyHash = 14695981039346656037ull;
        bool legacyIdsPresent = count >= 294;
        for (int i = 0; i < 294 && i < count; ++i)
        {
            auto* prm = dynamic_cast<juce::AudioProcessorParameterWithID*> (p.getParameters()[i]);
            if (prm == nullptr) { legacyIdsPresent = false; break; }
            const auto legacyId = prm->paramID + "\n";
            const auto utf8 = legacyId.toRawUTF8();
            for (const unsigned char* b = (const unsigned char*) utf8; *b != 0; ++b)
                legacyHash = (legacyHash ^ *b) * 1099511628211ull;
        }
        check (legacyIdsPresent && legacyHash == 0x92b97459d55b24e1ull,
               "порядок и ID первых 294 параметров сохранены");
        // Следующие 4 ID Plate тоже существовали до этого изменения.
        for (int i = 294; i < 298 && i < count; ++i)
        {
            auto* prm = dynamic_cast<juce::AudioProcessorParameterWithID*> (p.getParameters()[i]);
            if (prm == nullptr) { legacyIdsPresent = false; break; }
            const auto id = prm->paramID + "\n";
            for (const unsigned char* b = (const unsigned char*) id.toRawUTF8(); *b != 0; ++b)
                legacyHash = (legacyHash ^ *b) * 1099511628211ull;
        }
        check (count >= 298 && legacyIdsPresent && legacyHash == 0x93f40b47085118c4ull,
               "порядок и ID всех 298 старых параметров сохранены");
        bool reverseIds = count == 306;
        for (int s = 0; s < 8 && count == 306; ++s)
        {
            const auto expected = slotPrefix (s) + "grainReverse";
            auto* prm = dynamic_cast<juce::RangedAudioParameter*> (p.getParameters()[298 + s]);
            reverseIds = reverseIds && prm != nullptr && prm->getParameterID() == expected
                         && std::fabs (prm->getDefaultValue()) < 1.0e-7f;
        }
        check (reverseIds, "8 ID Grain Reverse добавлены в конец с default 0");

        juce::StringArray ids;
        bool allHaveId = true;
        for (auto* param : p.getParameters())
        {
            const auto id = param->getName (64);
            if (id.isEmpty()) allHaveId = false;
            ids.add (id);
        }
        ids.removeDuplicates (false);
        check (ids.size() == count, "имена параметров уникальны",
               num ((double) ids.size(), 0) + " из " + num ((double) count, 0));
        check (allHaveId, "у каждого параметра есть имя");

        // каждый слот и ключевые эффекты на месте
        const char* must[] = { "sA_type", "sH_tail", "fx_reverbMix", "fx_combTune",
                               "gl_gain", "lfo2_shape", "fx_spectreModel",
                               "fx_platePreFilter", "fx_plateInputDiff1", "fx_plateInputDiff2", "fx_plateOutput",
                               "fx_robinMix", "fx_robinSemis",           // эффект 15/16
                               "arpOn", "arpMode", "arpDiv", "arpOctaves", "arpGate", "arpLatch",
                               "mod1_src", "mod1_dst", "mod1_amt",        // мод-матрица
                               "mod8_src", "mod8_dst", "mod8_amt" };
        bool ok = true;
        for (const char* m : must) if (p.apvts.getParameter (m) == nullptr) ok = false;
        check (ok, "обязательные id присутствуют");
        check (p.getNumPrograms() >= 6, "фабричные пресеты", num (p.getNumPrograms(), 0));

        // --- фильтр слота: 16 моделей, старые id 0–4 сохранили названия ---
        {
            auto* fp = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter ("sA_filter"));
            const int n = fp != nullptr ? fp->choices.size() : 0;
            std::printf ("   моделей фильтра: %d\n", n);
            check (n == 16, "у фильтра слота 16 моделей", num (n, 0));
            const char* old[5] = { "LP12", "LP24", "BP12", "HP24", "Notch12" };
            bool oldOk = true;
            for (int i = 0; i < 5 && fp != nullptr; ++i)
                if (fp->choices[i] != old[i]) oldOk = false;
            check (oldOk, "старые id 0–4 не переименованы");
            bool cycleOk = fp != nullptr;
            for (int i = 0; i < n && fp != nullptr; ++i)
            {
                fp->setValueNotifyingHost (fp->convertTo0to1 ((float) i));
                if (fp->getIndex() != i) cycleOk = false;
            }
            check (cycleOk, "все 16 моделей проходят через параметр хоста", num (n, 0));
        }

        // --- движки: 9 типов, включая слайсер Chop (итерация 12) ---
        {
            auto* tp = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter ("sA_type"));
            const int n = tp != nullptr ? tp->choices.size() : 0;
            std::printf ("   типов движка: %d\n", n);
            check (n == 9, "в списке TYPE девять движков", num (n, 0));
            check (n > 0 && tp->choices[n - 1] == "Chop", "девятый движок — Chop",
                   n > 0 ? tp->choices[n - 1] : juce::String());
        }

        // --- фильтр переживает круг patch → параметры → patch на всех 16 id ---
        {
            Patch p3 = p.currentPatch();
                int bad = 0;
            for (int m = 0; m < 16; ++m)
            {
                p3.slots[3].filterType = m;
                p3.slots[3].res = 0.1f + 0.05f * (float) m;
                FxChain::Settings fx;
                p.applyToParams (p3, fx);
                Patch back;
                FxChain::Settings fx2;
                p.collectPatch (back, fx2);
                if (back.slots[3].filterType != m) ++bad;
                if (std::fabs (back.slots[3].res - p3.slots[3].res) > 1.0e-4f) ++bad;
            }
            std::printf ("   фильтр: %d расхождений на 16 моделях\n", bad);
            check (bad == 0, "все 16 id фильтра переживают круг patch→params→patch",
                   std::to_string (bad));
        }
    }

    // ---------------------------------------------------------------- 2
    std::printf ("\n--- 2. Patch -> параметры -> Patch ---\n");
    {
        OrogenProcessor p;
        int worstIndex = -1;
        double worst = 0.0;
        for (int prog = 0; prog < p.getNumPrograms(); ++prog)
        {
            p.setCurrentProgram (prog);
            Patch round;
            FxChain::Settings fx;
            p.collectPatch (round, fx);
            const Patch& orig = p.currentPatch();

            auto d = [] (float a, float b) { return (double) std::fabs (a - b); };
            double err = 0.0;
            err = std::max (err, d (round.gain, orig.gain));
            err = std::max (err, d (round.glide, orig.glide));
            err = std::max (err, d (round.width, orig.width));
            for (int s = 0; s < 8; ++s)
            {
                if (round.slots[s].type != orig.slots[s].type) err = 1.0;
                if (round.slots[s].on != orig.slots[s].on) err = 1.0;
                for (int k = 0; k < 4; ++k)
                    err = std::max (err, d (round.slots[s].params.p[k], orig.slots[s].params.p[k]));
                for (float e : { round.slots[s].attack, round.slots[s].decay,
                                 round.slots[s].sustain, round.slots[s].release,
                                 round.slots[s].cutoff, round.slots[s].res, round.slots[s].tail })
                    (void) e;
                err = std::max (err, d (round.slots[s].attack, orig.slots[s].attack));
                err = std::max (err, d (round.slots[s].release, orig.slots[s].release));
                err = std::max (err, d (round.slots[s].params.level, orig.slots[s].params.level));
                err = std::max (err, d (round.slots[s].params.pan, orig.slots[s].params.pan));
                err = std::max (err, d (round.slots[s].params.tuneSemis, orig.slots[s].params.tuneSemis));
                err = std::max (err, d (round.slots[s].params.detune, orig.slots[s].params.detune));
            }
            if (err > worst) { worst = err; worstIndex = prog; }
        }
        std::printf ("   худшее расхождение %.6f у пресета %d\n", worst, worstIndex);
        check (worst < 1.0e-3, "круг patch->params->patch точен", num (worst, 6));

        // мод-матрица: 8 маршрутов должны пережить круг patch → параметры → patch
        {
            Patch p2 = p.currentPatch();
            for (int r = 0; r < kModRoutes; ++r)
            {
                p2.routes[r].source = (ModSource) (r % (int) ModSource::Count);
                p2.routes[r].dest   = (ModDest) (r % (int) ModDest::Count);
                p2.routes[r].amount = -0.9f + 0.2f * (float) r;
            }
            FxChain::Settings fxs;
            p.applyToParams (p2, fxs);             // patch -> APVTS
            Patch back;
            p.collectPatch (back, fxs);            // APVTS -> patch
            double worstRoute = 0.0;
            int wrongSrc = 0, wrongDst = 0;
            for (int r = 0; r < kModRoutes; ++r)
            {
                worstRoute = std::max (worstRoute, (double) std::fabs (back.routes[r].amount - p2.routes[r].amount));
                if (back.routes[r].source != p2.routes[r].source) ++wrongSrc;
                if (back.routes[r].dest   != p2.routes[r].dest)   ++wrongDst;
            }
            std::printf ("   мод-матрица: 8 маршрутов, худшая ошибка величины %.6f, "
                         "источников не совпало %d, назначений %d\n", worstRoute, wrongSrc, wrongDst);
            check (worstRoute < 1.0e-3 && wrongSrc == 0 && wrongDst == 0,
                   "мод-матрица переживает круг patch->params->patch", num (worstRoute, 6));
        }

        // эффекты: drive включён у 4-го пресета — проверим, что доехал
        p.setCurrentProgram (3);
        Patch pp; FxChain::Settings fx;
        p.collectPatch (pp, fx);
        std::printf ("   пресет 3: drive %.3f, reverbMix %.3f, delayMix %.3f\n",
                     fx.drive, fx.reverbMix, fx.delayMix);
        check (fx.drive > 0.2f && fx.reverbMix > 0.3f, "эффекты пресета доехали");
    }

    // ---------------------------------------------------------------- 3

    // ---------------------------------------------------------------- 2b
    std::printf ("\n--- 2b. FX: параметры -> JSON-пресет -> параметры ---\n");
    {
        OrogenProcessor p;
        p.setPlayConfigDetails (0, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        p.setCurrentProgram (2);

        // Каждой ручке каждого эффекта даём нехарактерное значение, чтобы
        // забытый параметр сразу дал расхождение после круга через файл.
        const std::pair<const char*, float> set[] = {
            { "fx_drive", 0.37f }, { "fx_driveModel", 7.0f }, { "fx_driveTone", -0.4f },
            { "fx_chorusModel", 1.0f }, { "fx_chorusMix", 0.61f }, { "fx_chorusDepth", 0.23f },
            { "fx_chorusRate", 1.37f }, { "fx_chorusDelay", 111.0f }, { "fx_chorusFeedback", 77.0f },
            { "fx_chorusWidth", 21.0f }, { "fx_chorusLowpass", 33.0f }, { "fx_chorusInput", 88.0f },
            { "fx_delayMix", 0.29f }, { "fx_delayTime", 0.77f }, { "fx_delayFb", 0.66f },
            { "fx_reverbMix", 0.41f }, { "fx_reverbSize", 0.83f }, { "fx_reverbDamp", 0.17f },
            { "fx_plateMix", 0.53f }, { "fx_plateDecay", 0.61f }, { "fx_plateDamp", 0.29f },
            { "fx_plateDiffusion", 0.87f }, { "fx_platePredelay", 0.173f },
            { "fx_platePreFilter", 0.23f }, { "fx_plateInputDiff1", 0.42f },
            { "fx_plateInputDiff2", 0.71f }, { "fx_plateOutput", 1.24f },
            { "fx_materialMix", 0.47f }, { "fx_materialMode", 3.0f }, { "fx_materialExcite", 0.71f },
            { "fx_materialTone", 0.13f }, { "fx_materialDamp", 0.57f }, { "fx_materialSize", 0.79f },
            { "fx_materialFeedback", 0.31f },
            { "fx_spectreMix", 0.63f }, { "fx_spectreWindow", 5.0f }, { "fx_spectreProminence", 17.0f },
            { "fx_spectreShift", -7.0f }, { "fx_spectrePeaks", 12.0f }, { "fx_spectreWidth", 4.0f },
            { "fx_spectreLow", 1200.0f }, { "fx_spectreHigh", 7300.0f },
            { "fx_robinMix", 0.44f }, { "fx_robinSemis", 5.0f }, { "fx_robinSize", 0.62f },
            { "fx_robinSpread", 0.28f },
            { "fx_glitchMix", 0.58f }, { "fx_glitchChance", 0.73f }, { "fx_glitchLengthMs", 137.0f },
            { "fx_glitchRepeats", 5.0f }, { "fx_glitchReverse", 0.62f }, { "fx_glitchPitch", -3.0f },
            { "fx_glitchFeedback", 0.55f }, { "fx_glitchFold", 3.3f }, { "fx_glitchTone", 5300.0f },
            { "fx_combMix", 0.36f }, { "fx_combModel", 1.0f }, { "fx_combDriveDb", 6.0f },
            { "fx_combOutputDb", -4.0f }, { "fx_combDepth", 0.77f }, { "fx_combRate", 1.3f },
            { "fx_combEnvelope", 0.42f }, { "fx_combFreqA", 174.0f }, { "fx_combFreqB", 261.0f },
            { "fx_combResA", 88.0f }, { "fx_combResB", 61.0f }, { "fx_combBalance", 0.38f },
            { "fx_combDelayMs", 63.0f }, { "fx_combFeedback", 44.0f }, { "fx_combHighpass", 21.0f },
            { "fx_combTune", 3.0f }, { "fx_eqLow", 4.5f }, { "fx_eqHigh", -3.5f },
            { "fx_compThreshold", -17.0f }, { "fx_compRatio", 5.5f }, { "fx_compMakeup", 4.0f },
            { "fx_width", 1.23f } };
        for (const auto& kv : set) setP (p, kv.first, kv.second);

        FxChain::Settings a;
        Patch dummy;
        p.collectPatch (dummy, a);

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("orogen_fx_roundtrip.json");
        check (p.savePresetFile (file, "FX roundtrip"), "пресет с FX сохраняется на диск");

        const auto text = file.loadFileAsString();
        int missingKeys = 0;
        // DEL/DEP/SPD/MIX/FB/WID/LP/INP лежат одним массивом "chorusFw"
        for (const char* key : { "chorusFw", "glitchFeedback", "glitchFold", "glitchTone",
                                 "platePreFilter", "plateInputDiff1", "plateInputDiff2", "plateOutput",
                                 "spectreLow", "spectreHigh",
                                 "robinMix", "robinSemis", "robinSize", "robinSpread" })
            if (! text.contains (key)) ++missingKeys;
        check (missingKeys == 0, "в JSON есть все новые ключи FX", num ((double) missingKeys, 0));

        // сбиваем значения, затем грузим файл и сверяем все поля настроек
        for (const auto& kv : set) setP (p, kv.first, kv.second * 0.37f);
        check (p.loadPresetFile (file), "пресет с FX загружается обратно");

        FxChain::Settings b;
        Patch dummy2;
        p.collectPatch (dummy2, b);

        auto fxd = [] (float x, float y)
        {
            return std::fabs (x - y) / std::max (1.0f, std::max (std::fabs (x), std::fabs (y)));
        };
        double err = 0.0;
        int badField = 0;
        const char* worstField = "-";
        auto cmp = [&] (const char* name, double d)
        {
            if (d > err) { err = d; worstField = name; }
            if (d > 1.0e-3) ++badField;
        };
        cmp ("drive", fxd ((float) a.drive, (float) b.drive));
        cmp ("chorusMix", fxd ((float) a.chorusMix, (float) b.chorusMix));
        cmp ("chorusDepth", fxd ((float) a.chorusDepth, (float) b.chorusDepth));
        cmp ("chorusRate", fxd ((float) a.chorusRate, (float) b.chorusRate));
        cmp ("chorusModel", fxd ((float) a.chorusModel, (float) b.chorusModel));
        cmp ("delayMix", fxd ((float) a.delayMix, (float) b.delayMix));
        cmp ("delayTime", fxd ((float) a.delayTime, (float) b.delayTime));
        cmp ("delayFb", fxd ((float) a.delayFb, (float) b.delayFb));
        cmp ("reverbMix", fxd ((float) a.reverbMix, (float) b.reverbMix));
        cmp ("reverbSize", fxd ((float) a.reverbSize, (float) b.reverbSize));
        cmp ("reverbDamp", fxd ((float) a.reverbDamp, (float) b.reverbDamp));
        cmp ("plateMix", fxd ((float) a.plateMix, (float) b.plateMix));
        cmp ("plateDecay", fxd ((float) a.plateDecay, (float) b.plateDecay));
        cmp ("plateDamp", fxd ((float) a.plateDamp, (float) b.plateDamp));
        cmp ("plateDiffusion", fxd ((float) a.plateDiffusion, (float) b.plateDiffusion));
        cmp ("platePredelay", fxd ((float) a.platePredelay, (float) b.platePredelay));
        cmp ("platePreFilter", fxd (a.platePreFilter, b.platePreFilter));
        cmp ("plateInputDiff1", fxd (a.plateInputDiff1, b.plateInputDiff1));
        cmp ("plateInputDiff2", fxd (a.plateInputDiff2, b.plateInputDiff2));
        cmp ("plateOutput", fxd (a.plateOutput, b.plateOutput));
        cmp ("driveModel", fxd ((float) a.driveModel, (float) b.driveModel));
        cmp ("driveTone", fxd ((float) a.driveTone, (float) b.driveTone));
        cmp ("combMix", fxd ((float) a.combMix, (float) b.combMix));
        cmp ("combModel", fxd ((float) a.combModel, (float) b.combModel));
        cmp ("combDriveDb", fxd ((float) a.combDriveDb, (float) b.combDriveDb));
        cmp ("combOutputDb", fxd ((float) a.combOutputDb, (float) b.combOutputDb));
        cmp ("combDepth", fxd ((float) a.combDepth, (float) b.combDepth));
        cmp ("combRate", fxd ((float) a.combRate, (float) b.combRate));
        cmp ("combEnvelope", fxd ((float) a.combEnvelope, (float) b.combEnvelope));
        cmp ("combFreqA", fxd ((float) a.combFreqA, (float) b.combFreqA));
        cmp ("combFreqB", fxd ((float) a.combFreqB, (float) b.combFreqB));
        cmp ("combResA", fxd ((float) a.combResA, (float) b.combResA));
        cmp ("combResB", fxd ((float) a.combResB, (float) b.combResB));
        cmp ("combBalance", fxd ((float) a.combBalance, (float) b.combBalance));
        cmp ("combRouting", fxd ((float) a.combRouting, (float) b.combRouting));
        cmp ("combNegative", fxd ((float) a.combNegative, (float) b.combNegative));
        cmp ("combDelayMs", fxd ((float) a.combDelayMs, (float) b.combDelayMs));
        cmp ("combFeedback", fxd ((float) a.combFeedback, (float) b.combFeedback));
        cmp ("combHighpass", fxd ((float) a.combHighpass, (float) b.combHighpass));
        cmp ("combTune", fxd ((float) a.combTune, (float) b.combTune));
        cmp ("combOversample", fxd ((float) a.combOversample, (float) b.combOversample));
        cmp ("combMidSide", fxd ((float) a.combMidSide, (float) b.combMidSide));
        cmp ("materialMix", fxd ((float) a.materialMix, (float) b.materialMix));
        cmp ("materialMode", fxd ((float) a.materialMode, (float) b.materialMode));
        cmp ("materialExcite", fxd ((float) a.materialExcite, (float) b.materialExcite));
        cmp ("materialTone", fxd ((float) a.materialTone, (float) b.materialTone));
        cmp ("materialDamp", fxd ((float) a.materialDamp, (float) b.materialDamp));
        cmp ("materialSize", fxd ((float) a.materialSize, (float) b.materialSize));
        cmp ("materialFeedback", fxd ((float) a.materialFeedback, (float) b.materialFeedback));
        cmp ("materialFollow", fxd ((float) a.materialFollow, (float) b.materialFollow));
        cmp ("spectreModel", fxd ((float) a.spectreModel, (float) b.spectreModel));
        cmp ("spectreMix", fxd ((float) a.spectreMix, (float) b.spectreMix));
        cmp ("spectreWindow", fxd ((float) a.spectreWindow, (float) b.spectreWindow));
        cmp ("spectreProminence", fxd ((float) a.spectreProminence, (float) b.spectreProminence));
        cmp ("spectreShift", fxd ((float) a.spectreShift, (float) b.spectreShift));
        cmp ("spectrePeaks", fxd ((float) a.spectrePeaks, (float) b.spectrePeaks));
        cmp ("spectreWidth", fxd ((float) a.spectreWidth, (float) b.spectreWidth));
        cmp ("spectreLow", fxd ((float) a.spectreLow, (float) b.spectreLow));
        cmp ("spectreHigh", fxd ((float) a.spectreHigh, (float) b.spectreHigh));
        cmp ("spectreFreeze", fxd ((float) a.spectreFreeze, (float) b.spectreFreeze));
        cmp ("robinMix", fxd ((float) a.robinMix, (float) b.robinMix));
        cmp ("robinSemis", fxd ((float) a.robinSemis, (float) b.robinSemis));
        cmp ("robinSize", fxd ((float) a.robinSize, (float) b.robinSize));
        cmp ("robinSpread", fxd ((float) a.robinSpread, (float) b.robinSpread));
        cmp ("glitchMix", fxd ((float) a.glitchMix, (float) b.glitchMix));
        cmp ("glitchChance", fxd ((float) a.glitchChance, (float) b.glitchChance));
        cmp ("glitchLengthMs", fxd ((float) a.glitchLengthMs, (float) b.glitchLengthMs));
        cmp ("glitchRepeats", fxd ((float) a.glitchRepeats, (float) b.glitchRepeats));
        cmp ("glitchReverse", fxd ((float) a.glitchReverse, (float) b.glitchReverse));
        cmp ("glitchPitch", fxd ((float) a.glitchPitch, (float) b.glitchPitch));
        cmp ("glitchFeedback", fxd ((float) a.glitchFeedback, (float) b.glitchFeedback));
        cmp ("glitchFold", fxd ((float) a.glitchFold, (float) b.glitchFold));
        cmp ("glitchTone", fxd ((float) a.glitchTone, (float) b.glitchTone));
        cmp ("eqLow", fxd ((float) a.eqLow, (float) b.eqLow));
        cmp ("eqHigh", fxd ((float) a.eqHigh, (float) b.eqHigh));
        cmp ("compThreshold", fxd ((float) a.compThreshold, (float) b.compThreshold));
        cmp ("compRatio", fxd ((float) a.compRatio, (float) b.compRatio));
        cmp ("compMakeup", fxd ((float) a.compMakeup, (float) b.compMakeup));
        cmp ("width", fxd ((float) a.width, (float) b.width));
        cmp ("chorusFw[0]", fxd ((float) a.chorusFw[(size_t) 0], (float) b.chorusFw[(size_t) 0]));
        cmp ("chorusFw[1]", fxd ((float) a.chorusFw[(size_t) 1], (float) b.chorusFw[(size_t) 1]));
        cmp ("chorusFw[2]", fxd ((float) a.chorusFw[(size_t) 2], (float) b.chorusFw[(size_t) 2]));
        cmp ("chorusFw[3]", fxd ((float) a.chorusFw[(size_t) 3], (float) b.chorusFw[(size_t) 3]));
        cmp ("chorusFw[4]", fxd ((float) a.chorusFw[(size_t) 4], (float) b.chorusFw[(size_t) 4]));
        cmp ("chorusFw[5]", fxd ((float) a.chorusFw[(size_t) 5], (float) b.chorusFw[(size_t) 5]));
        cmp ("chorusFw[6]", fxd ((float) a.chorusFw[(size_t) 6], (float) b.chorusFw[(size_t) 6]));
        cmp ("chorusFw[7]", fxd ((float) a.chorusFw[(size_t) 7], (float) b.chorusFw[(size_t) 7]));

        std::printf ("   сверено полей: 79 + chorusFw[8], худшее расхождение %.6f у %s\n",
                     err, worstField);
        check (badField == 0, "все поля FX переживают пресет", num ((double) badField, 0) + " расхождений");
        check (err < 1.0e-3, "круг FX -> JSON -> FX точен", num (err, 6));
        file.deleteFile();
    }
    std::printf ("\n--- 3. JSON-пресет: файл и состояние ---\n");
    {
        OrogenProcessor p;
        p.setCurrentProgram (1);
        for (int s = 0; s < 8; ++s)
            setP (p, slotPrefix (s) + "grainReverse", 0.1f * (float) (s + 1));

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("orogen_test.orogenpreset");
        check (p.savePresetFile (file, "Round Trip"), "пресет сохранён", file.getFileName());
        const auto json = file.loadFileAsString();
        check (json.startsWith ("{") && json.contains ("\"patch\"") && json.contains ("\"fx\""),
               "JSON содержит patch и fx", num ((double) json.length(), 0) + " байт");

        OrogenProcessor q;
        check (q.loadPresetFile (file), "пресет прочитан");
        Patch a, b; FxChain::Settings fa, fb;
        p.collectPatch (a, fa);
        q.collectPatch (b, fb);
        bool same = a.slots[0].type == b.slots[0].type;
        for (int s = 0; s < 8; ++s)
        {
            same = same && (a.slots[s].type == b.slots[s].type)
                        && (std::fabs (a.slots[s].attack - b.slots[s].attack) < 1.0e-4f)
                        && (std::fabs (a.slots[s].params.p[0] - b.slots[s].params.p[0]) < 1.0e-4f)
                        && (std::fabs (a.slots[s].params.grainReverse - b.slots[s].params.grainReverse) < 1.0e-4f);
        }
        check (same, "патч и Grain Reverse всех 8 слотов идентичны после чтения из JSON");
        check (std::fabs (fa.reverbMix - fb.reverbMix) < 1.0e-4f, "эффекты идентичны",
               num (fb.reverbMix, 4));

        // состояние сессии: все новые поля проходят именно через хостовый XML
        setP (p, "fx_platePreFilter", 0.21f);
        setP (p, "fx_plateInputDiff1", 0.37f);
        setP (p, "fx_plateInputDiff2", 0.82f);
        setP (p, "fx_plateOutput", 1.36f);
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        OrogenProcessor r;
        r.setStateInformation (mb.getData(), (int) mb.getSize());
        Patch c; FxChain::Settings fc;
        r.collectPatch (c, fc);
        check (std::fabs (c.gain - a.gain) < 1.0e-3f, "состояние сессии сохраняет патч",
               num ((double) mb.getSize(), 0) + " байт");
        check (std::fabs (fc.platePreFilter - 0.21f) < 1.0e-4f
               && std::fabs (fc.plateInputDiff1 - 0.37f) < 1.0e-4f
               && std::fabs (fc.plateInputDiff2 - 0.82f) < 1.0e-4f
               && std::fabs (fc.plateOutput - 1.36f) < 1.0e-4f,
               "четыре новых регулятора Plate переживают состояние сессии");
        bool stateReverse = true;
        for (int s = 0; s < 8; ++s)
            stateReverse = stateReverse && std::fabs (c.slots[s].params.grainReverse - 0.1f * (float) (s + 1)) < 1.0e-4f;
        check (stateReverse, "Grain Reverse всех 8 слотов переживает состояние сессии");

        // Эмуляция состояния старой версии: удалить четыре PARAM-узла и
        // загрузить ПОВЕРХ не-дефолтных значений (новый пустой инстанс не
        // выявил бы сохранение старого текущего значения в replaceState).
        auto legacy = p.apvts.copyState();
        int removed = 0;
        for (const char* id : { "fx_platePreFilter", "fx_plateInputDiff1", "fx_plateInputDiff2", "fx_plateOutput" })
        {
            auto node = legacy.getChildWithProperty ("id", id);
            if (node.isValid()) { legacy.removeChild (node, nullptr); ++removed; }
        }
        int removedReverse = 0;
        for (int s = 0; s < 8; ++s)
        {
            auto node = legacy.getChildWithProperty ("id", slotPrefix (s) + "grainReverse");
            if (node.isValid()) { legacy.removeChild (node, nullptr); ++removedReverse; }
        }
        juce::MemoryBlock oldBlob;
        if (auto xml = legacy.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, oldBlob);
        r.setStateInformation (oldBlob.getData(), (int) oldBlob.getSize());
        r.collectPatch (c, fc);
        check (removed == 4 && std::fabs (fc.platePreFilter - 0.85f) < 1.0e-4f
               && std::fabs (fc.plateInputDiff1 - 0.75f) < 1.0e-4f
               && std::fabs (fc.plateInputDiff2 - 0.625f) < 1.0e-4f
               && std::fabs (fc.plateOutput - 0.60f) < 1.0e-4f,
               "старая сессия получает прежние фиксированные значения Plate");
        bool oldSessionReverse = removedReverse == 8;
        for (const auto& s : c.slots) oldSessionReverse = oldSessionReverse && s.params.grainReverse == 0.0f;
        check (oldSessionReverse, "старая сессия сбрасывает все 8 Reverse в 0 поверх изменённого состояния");

        // То же правило для JSON-пресета старой версии.
        auto oldPreset = presetToVar ("old", patchInit(), FxChain::Settings());
        auto* fxObject = oldPreset.getDynamicObject()->getProperty ("fx").getDynamicObject();
        for (const char* id : { "platePreFilter", "plateInputDiff1", "plateInputDiff2", "plateOutput" })
            fxObject->removeProperty (id);
        auto* patchObject = oldPreset.getDynamicObject()->getProperty ("patch").getDynamicObject();
        auto* oldSlots = patchObject != nullptr ? patchObject->getProperty ("slots").getArray() : nullptr;
        if (oldSlots != nullptr)
            for (auto& s : *oldSlots)
                if (auto* object = s.getDynamicObject()) object->removeProperty ("grainReverse");
        Patch oldPatch; FxChain::Settings oldFx; juce::String oldName;
        for (auto& s : oldPatch.slots) s.params.grainReverse = 1.0f;
        const bool loaded = presetFromVar (oldPreset, oldPatch, oldFx, oldName);
        check (loaded && std::fabs (oldFx.platePreFilter - 0.85f) < 1.0e-4f
               && std::fabs (oldFx.plateInputDiff1 - 0.75f) < 1.0e-4f
               && std::fabs (oldFx.plateInputDiff2 - 0.625f) < 1.0e-4f
               && std::fabs (oldFx.plateOutput - 0.60f) < 1.0e-4f,
               "старый JSON-пресет получает прежние коэффициенты Plate");
        bool oldJsonReverse = loaded && oldSlots != nullptr && oldSlots->size() == 8;
        for (const auto& s : oldPatch.slots) oldJsonReverse = oldJsonReverse && s.params.grainReverse == 0.0f;
        check (oldJsonReverse, "старый JSON без Reverse получает 0 во всех слотах");
    }

    // ---------------------------------------------------------------- 4

    // ---------------------------------------------------------------- 3b
    std::printf ("\n--- 3b. Банк программ ---\n");
    {
        OrogenProcessor p;
        p.setPlayConfigDetails (0, 2, sr, 512);
        p.prepareToPlay (sr, 512);

        const int base = p.getNumPrograms();
        // программа 1 (Terra Pad) + правки + новая программа из текущих значений
        p.setCurrentProgram (1);
        setP (p, "fx_chorusDelay", 91.0f);
        setP (p, "fx_glitchFold", 2.5f);
        setP (p, "gl_glide", 0.42f);
        FxChain::Settings fxEdited;
        Patch patchEdited;
        p.collectPatch (patchEdited, fxEdited);
        const int added = p.addProgram ("User Wave");
        check (added == base, "программа добавляется в конец", num ((double) added, 0));
        check (p.getNumPrograms() == base + 1, "число программ выросло",
               num ((double) p.getNumPrograms(), 0));

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("orogen_bank.json");
        check (p.saveBankFile (file), "банк сохраняется на диск");
        const auto text = file.loadFileAsString();
        check (text.contains ("OROGEN bank"), "в файле есть заголовок банка");
        check (text.contains ("User Wave"), "имя новой программы в банке");
        std::printf ("   банк: %d программ, %d байт\n", p.getNumPrograms(), (int) text.length());

        // сбиваем состояние, потом грузим банк
        p.setCurrentProgram (0);
        setP (p, "fx_chorusDelay", 5.0f);
        setP (p, "fx_glitchFold", 0.0f);
        setP (p, "gl_glide", 0.0f);
        check (p.loadBankFile (file), "банк загружается обратно");
        check (p.getNumPrograms() == base + 1, "после загрузки то же число программ",
               num ((double) p.getNumPrograms(), 0));

        // правки лежат в добавленной программе (addProgram снял текущее состояние),
        // а фабричная 1-я остаётся как была — так и работает банк
        p.setCurrentProgram (added);
        FxChain::Settings fxBack;
        Patch patchBack;
        p.collectPatch (patchBack, fxBack);
        const double dDelay = std::fabs ((double) fxBack.chorusFw[0] - 91.0);
        const double dFold  = std::fabs ((double) fxBack.glitchFold - 2.5);
        const double dGlide = std::fabs ((double) patchBack.glide - 0.42);
        std::printf ("   после круга: DEL %.1f, FOLD %.2f, glide %.2f\n",
                     (double) fxBack.chorusFw[0], (double) fxBack.glitchFold, (double) patchBack.glide);
        check (dDelay < 0.5 && dFold < 0.01 && dGlide < 0.01, "правки переживают банк",
               num (std::max (dDelay, std::max (dFold, dGlide)), 3));

        // имя программы тоже сохраняется (changeProgramName раньше жило только в памяти)
        p.changeProgramName (0, "Renamed Init");
        check (p.saveBankFile (file) && p.loadBankFile (file), "банк с переименованием");
        check (p.getProgramName (0) == "Renamed Init", "переименование переживает банк",
               p.getProgramName (0));

        // битый файл не должен ломать набор программ
        const auto bad = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("orogen_bank_bad.json");
        bad.replaceWithText ("{ \"format\": \"OROGEN bank\", \"programs\": [ {} ] }");
        const int before = p.getNumPrograms();
        check (! p.loadBankFile (bad), "битый банк отвергается");
        check (p.getNumPrograms() == before, "после битого банка программы целы",
               num ((double) p.getNumPrograms(), 0));

        // одиночный пресет принимается как банк из одной программы
        const auto one = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("orogen_single.json");
        p.savePresetFile (one, "Single");
        check (p.loadBankFile (one), "одиночный пресет грузится как банк");
        check (p.getNumPrograms() == 1 && p.getProgramName (0) == "Single",
               "банк из одного пресета", p.getProgramName (0));

        file.deleteFile();
        bad.deleteFile();
        one.deleteFile();
    }
    std::printf ("\n--- 4. Задержка ---\n");
    {
        OrogenProcessor p;
        p.setPlayConfigDetails (0, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        // ROBIN стоит в цепочке всегда: его сухой путь задержан на 1024,
        // иначе mix складывал бы сигнал с самим собой со сдвигом 21 мс.
        check (p.reportedLatency() == 1024, "базовая задержка = ROBIN 1024",
               num (p.reportedLatency(), 0));

        auto* sw = p.apvts.getParameter ("fx_spectreMix");
        sw->setValueNotifyingHost (1.0f);
        p.apvts.getParameter ("fx_spectreWindow")->setValueNotifyingHost (
            p.apvts.getParameter ("fx_spectreWindow")->convertTo0to1 (3.0f));
        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;
        block.clear(); p.processBlock (block, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);   // дать сработать AsyncUpdater
        std::printf ("   Spectre mix=1, окно 3: задержка %d сэмплов\n", p.reportedLatency());
        check (p.reportedLatency() == 1024 + (512 << 3), "задержка = ROBIN + окно Spectre",
               num (p.reportedLatency(), 0));

        sw->setValueNotifyingHost (0.0f);
        block.clear(); p.processBlock (block, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (p.reportedLatency() == 1024, "нулевой Spectre оставляет только ROBIN",
               num (p.reportedLatency(), 0));
    }

    // ---------------------------------------------------------------- 5
    std::printf ("\n--- 5. Нулевой тест: плагин == стенд ---\n");
    {
        const int total = (int) (sr * 2.0);
        OrogenProcessor p;
        auto pluginOut = renderProcessor (p, total, sr, 1);

        // тот же патч напрямую в Instrument (как в tools/render_demo_instrument)
        Patch patch; FxChain::Settings fx;
        p.setCurrentProgram (1);
        p.collectPatch (patch, fx);
        Instrument inst;
        inst.prepare (sr, 16);
        inst.setPatch (patch);
        inst.setFx (fx);
        inst.setMasterGain (0.8f);

        juce::AudioBuffer<float> ref (2, total);
        ref.clear();
        int pos = 0;
        const int onAt = (int) (sr * 0.05), offAt = (int) (sr * 0.45);
        const float vel = 102.0f / 127.0f;
        const int notes[3] = { 48, 55, 60 };
        while (pos < total)
        {
            const int n = juce::jmin (512, total - pos);
            for (int i = 0; i < n; ++i)
            {
                const int abs = pos + i;
                if (abs == onAt)
                    for (int k = 0; k < 3; ++k) inst.noteOn (notes[k], vel);
                if (abs == offAt)
                    for (int k = 0; k < 3; ++k) inst.noteOff (notes[k]);
                float l = 0.0f, r = 0.0f;
                inst.render (l, r);
                ref.setSample (0, abs, l);
                ref.setSample (1, abs, r);
            }
            pos += n;
        }

        double maxDiff = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < total; ++i)
                maxDiff = std::max (maxDiff, (double) std::fabs (
                    pluginOut.getReadPointer (ch)[i] - ref.getReadPointer (ch)[i]));
        const double rp = rmsOf (pluginOut), rr = rmsOf (ref);
        std::printf ("   плагин RMS %.6f, стенд RMS %.6f, макс|разница| %.3e\n", rp, rr, maxDiff);
        check (maxDiff < 1.0e-6, "выход плагина совпадает со стендом", num (maxDiff, 9));
        check (rp > 0.01, "плагин действительно звучит", num (rp, 4));
    }

    // ---------------------------------------------------------------- 6
    std::printf ("\n--- 6. Устойчивость на всех пресетах ---\n");
    {
        OrogenProcessor p;
        double worstPeak = 0.0;
        long long nan = 0;
        for (int prog = 0; prog < p.getNumPrograms(); ++prog)
        {
            auto out = renderProcessor (p, (int) (sr * 1.5), sr, prog);
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    const float v = out.getReadPointer (ch)[i];
                    if (! std::isfinite (v)) ++nan;
                    peak = std::max (peak, std::fabs (v));
                }
            worstPeak = std::max (worstPeak, (double) peak);
            std::printf ("   пресет %d «%s»: RMS %.6f, пик %.6f\n", prog,
                         p.getProgramName (prog).toRawUTF8(), rmsOf (out), peak);
        }
        check (nan == 0, "нет NaN/Inf", num ((double) nan, 0));
        check (worstPeak < 1.05, "нет клиппинга на всех пресетах", num (worstPeak, 3));
    }

    // --- регресс: каждая глобальная ручка должна доезжать до структуры Patch ---
    // Этот блок поймал реальную ошибку: gl_morph читался как globRefs[6] в
    // массиве из 6 указателей (выход за границу) — MORPH не влиял на звук.
    {
        std::printf ("\n-- параметры -> Patch (глобальные и слот)\n");
        OrogenProcessor p;

        auto setP = [&p] (const char* id, float value)
        {
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id)))
                rp->setValueNotifyingHost (rp->convertTo0to1 (value));
            else
                std::printf ("   нет параметра %s\n", id);
        };

        setP ("gl_morph",    0.75f);
        setP ("gl_gain",     1.50f);
        setP ("gl_width",    0.40f);
        setP ("gl_glide",    0.80f);
        setP ("gl_modWheel", 0.60f);
        setP ("gl_mode",     2.0f);
        setP ("sC_level",    0.42f);
        setP ("sC_type",     5.0f);       // Modal
        setP ("sC_cutoff",   0.31f);
        setP ("sC_filter",   13.0f);      // Ladder

        Patch pt; FxChain::Settings fxs;
        p.collectPatch (pt, fxs);

        check (std::abs (pt.morph - 0.75f) < 0.002f, "gl_morph -> patch.morph", num (pt.morph));
        check (std::abs (pt.gain  - 1.50f) < 0.002f, "gl_gain -> patch.gain",   num (pt.gain));
        check (std::abs (pt.width - 0.40f) < 0.002f, "gl_width -> patch.width", num (pt.width));
        check (std::abs (pt.glide - 0.80f) < 0.002f, "gl_glide -> patch.glide", num (pt.glide));
        check (std::abs (pt.modWheel - 0.60f) < 0.002f, "gl_modWheel -> patch.modWheel", num (pt.modWheel));
        check ((int) pt.mode == 2, "gl_mode -> patch.mode", num ((double) (int) pt.mode, 0));
        check (std::abs (pt.slots[2].params.level - 0.42f) < 0.002f, "sC_level -> slot.level",
               num (pt.slots[2].params.level));
        check ((int) pt.slots[2].type == 5, "sC_type -> slot.type", num ((double) (int) pt.slots[2].type, 0));
        check (pt.slots[2].filterType == 13, "sC_filter -> slot.filterType",
               num ((double) pt.slots[2].filterType, 0));
    }

    // --- MORPH реально меняет звук (кроссфейд слотов) ---
    {
        std::printf ("\n-- MORPH слышен в сигнале\n");
        OrogenProcessor p;
        const double sr2 = 48000.0;

        auto renderMorph = [&] (float morph)
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter ("gl_morph"));
            rp->setValueNotifyingHost (rp->convertTo0to1 (morph));
            return renderProcessor (p, (int) (sr2 * 1.0), sr2, 4);   // пресет «Morph Eight»
        };

        const auto a = renderMorph (0.0f);
        const auto b = renderMorph (1.0f);

        double diff = 0.0, energy = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                const double va = a.getReadPointer (ch)[i], vb = b.getReadPointer (ch)[i];
                diff += (va - vb) * (va - vb);
                energy += va * va;
            }
        const double rel = std::sqrt (diff / juce::jmax (1e-12, energy));

        check (rel > 0.30, "morph 0 -> 1 меняет сигнал (относительная разница)",
               num (rel) + " при RMS " + num (rmsOf (a), 4) + " / " + num (rmsOf (b), 4));
    }

    // --- покрытие интерфейсом: у каждого параметра есть место в UI ---
    {
        std::printf ("\n-- карта параметров (UiParamMap)\n");
        OrogenProcessor p;

        const auto tabs = buildUiTabs();

        juce::StringArray mapIds;
        int dupes = 0;
        for (const auto& t : tabs)
            for (const auto& sec : t.sections)
                for (const auto& id : sec.ids)
                {
                    if (mapIds.contains (id)) { ++dupes; std::printf ("   повтор в карте: %s\n", id.toRawUTF8()); }
                    else mapIds.add (id);
                }

        juce::StringArray apvtsIds;
        for (auto* base : p.apvts.processor.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (base))
                apvtsIds.add (rp->getParameterID());

        juce::StringArray missing, extra;
        for (const auto& id : apvtsIds) if (! mapIds.contains (id)) missing.add (id);
        for (const auto& id : mapIds)   if (! apvtsIds.contains (id)) extra.add (id);

        if (! missing.isEmpty())
        {
            std::printf ("   нет в карте (%d):", missing.size());
            for (const auto& id : missing) std::printf (" %s", id.toRawUTF8());
            std::printf ("\n");
        }
        if (! extra.isEmpty())
        {
            std::printf ("   лишние в карте (%d):", extra.size());
            for (const auto& id : extra) std::printf (" %s", id.toRawUTF8());
            std::printf ("\n");
        }

        std::printf ("   вкладок: %d, секций: %d, id в карте: %d, в APVTS: %d\n",
                     (int) tabs.size(),
                     [&] { int n = 0; for (const auto& t : tabs) n += (int) t.sections.size(); return n; }(),
                     mapIds.size(), apvtsIds.size());

        check (dupes == 0, "в карте нет повторов", num ((double) dupes, 0));
        check (missing.isEmpty(), "каждый параметр APVTS есть в карте", num ((double) missing.size(), 0));
        check (extra.isEmpty(), "в карте нет несуществующих параметров", num ((double) extra.size(), 0));
        check (mapIds.size() == apvtsIds.size() && apvtsIds.size() == 306,
               "покрытие 306/306", juce::String (mapIds.size()) + " / " + juce::String (apvtsIds.size()));

        // --- управление в оверлее реально создаётся для каждого параметра ---
        juce::AudioProcessorEditor* ed = p.createEditor();
        check (ed != nullptr, "редактор создаётся");
        if (ed != nullptr)
        {
            auto* ore = static_cast<OrogenEditor*> (ed);
            ore->setSize (1280, 800);

            // главный экран: селекторы движка и фильтра выбраны мышью, без
            // оверлея PARAMS — иначе смену движка видно только в оверлее
            int mainBoxes = 0;
            for (auto* child : ore->getChildren())
                if (dynamic_cast<juce::ComboBox*> (child) != nullptr && child->isVisible())
                    ++mainBoxes;
            check (mainBoxes >= 2, "на главном экране есть выбор движка и фильтра",
                   num ((double) mainBoxes, 0) + " ComboBox");

            // Оверлей теперь постраничный: каждая секция вкладки — отдельная
            // страница (плюс ALL). Считаем контролы по всем парам «вкладка ×
            // секция» и проверяем, что вместе они дают все параметры.
            int seen = 0;
            int pages = 0, wrongIds = 0;
            int sectionsTotal = 0;
            juce::StringArray pageTitles;
            for (int t = 0; t < (int) tabs.size(); ++t)
            {
                ore->showPage (100 + t);           // вкладка, все секции
                const int nSec = ore->paramsSectionCount();
                sectionsTotal += nSec;
                check ((int) tabs[(size_t) t].sections.size() == nSec,
                       (juce::String ("секции вкладки ") + tabs[(size_t) t].title).toRawUTF8(),
                       juce::String (nSec));

                for (int sIdx = 0; sIdx < nSec; ++sIdx)
                {
                    ore->showPage (200 + t * 100 + (sIdx + 1));
                    ++pages;
                    if (auto* overlay = ore->paramsOverlay())
                    {
                        const int c = countBoundControls (*overlay, tabs[(size_t) t]);
                        const auto& expectedIds = tabs[(size_t) t].sections[(size_t) sIdx].ids;
                        const int expected = expectedIds.size();
                        const auto actualIds = visibleBoundIds (*overlay);
                        if (actualIds.size() != expected) ++wrongIds;
                        for (const auto& id : expectedIds)
                        {
                            int occurrences = 0;
                            for (const auto& got : actualIds) if (got == id) ++occurrences;
                            if (occurrences != 1) ++wrongIds;
                        }
                        std::printf ("   %-10s map:%-14s view:%-14s %d / %d\n",
                                     tabs[(size_t) t].title.toRawUTF8(),
                                     tabs[(size_t) t].sections[(size_t) sIdx].title.toRawUTF8(),
                                     ore->paramsSectionName (sIdx).toRawUTF8(), c, expected);
                        seen += c;
                    }
                }
            }
            std::printf ("   страниц секций: %d (секций в API-карте: %d)\n", pages, sectionsTotal);
            check (pages == 33, "страниц ровно столько, сколько секций в карте",
                   num ((double) pages, 0));
            check (seen == apvtsIds.size(), "на все параметры есть контрол в постраничном оверлее",
                   juce::String (seen) + " / " + juce::String (apvtsIds.size()));
            check (wrongIds == 0, "каждый видимый контрол привязан к нужному ID своей секции",
                   juce::String (wrongIds) + " ошибок");

            // навигация клавишами: ↑/↓ листают секции, PageDown-подобный сброс — ALL
            {
                ore->showPage (200 + 1 * 100 + 1);           // FX → первая секция
                const auto first = ore->paramsSectionName (0);
                juce::Component* ov = ore->paramsOverlay();
                const auto key = [ov] (int code)
                {
                    if (ov == nullptr) return;
                    ov->keyPressed (juce::KeyPress (code));
                };
                key (juce::KeyPress::downKey);
                const auto second = ore->paramsSectionName (1);
                key (juce::KeyPress::upKey);
                key (juce::KeyPress::upKey);
                std::printf ("   клавиши: %s → %s → ALL\n", first.toRawUTF8(), second.toRawUTF8());
                check (! first.isEmpty() && first != second, "↓ переключает на следующую секцию", second);
            }

            // и обратно: страница ALL показывает все параметры вкладки целиком
            ore->showPage (100);
            int seenAll = 0;
            if (auto* overlay = ore->paramsOverlay())
                seenAll = countBoundControls (*overlay, tabs[0]);
            // вкладка SLOT: 8 слотов × 22 параметра
            check (seenAll == 176, "страница ALL вкладки SLOT показывает все 176",
                   num ((double) seenAll, 0));

            // снимок оверлея — заодно проверяем, что он отрисовывается
            juce::Image img (juce::Image::ARGB, 1280, 800, true);
            juce::Graphics g (img);
            ore->paintEntireComponent (g, true);
            std::printf ("   оверлей отрисован: 1280×800\n");

            delete ed;
        }
    }

    // ---------------------------------------------------------------- 7
    std::printf ("\n--- 7. Смена движка и ручки эффектов ---\n");
    {
        // 7.1 каждый из девяти типов реально меняет звук слота.
        // setup выполняется ПОСЛЕ setCurrentProgram (иначе пресет перезапишет
        // тип), затем крутится message loop — как в хосте с UI.
        const char* names[9] = { "Wavetable", "FM", "Harmonic", "Sample",
                                 "Granular", "Modal", "Resynth", "Organics", "Chop" };
        double rms[9] = {};
        for (int t = 0; t < 9; ++t)
        {
            OrogenProcessor p;
            auto b = renderWithSetup (p, 24000, sr, [t] (OrogenProcessor& pr) { soloSlot (pr, 0, t); });
            rms[t] = rmsOf (b);
            std::printf ("   тип %d %-9s RMS %.4f\n", t, names[t], rms[t]);
            check (rms[t] > 0.0015, (juce::String ("движок звучит: ") + names[t]).toRawUTF8(), num (rms[t], 4));
        }

        // 7.2 типы отличимы друг от друга
        int distinct = 0, pairs = 0;
        for (int a = 0; a < 9; ++a)
            for (int b = a + 1; b < 9; ++b)
            {
                ++pairs;
                if (std::fabs (rms[a] - rms[b]) > 0.02 * std::max (rms[a], rms[b])) ++distinct;
            }
        std::printf ("   различаются по RMS: %d из %d пар\n", distinct, pairs);
        check (distinct >= 24, "движки не подменяются друг другом",
               juce::String (distinct) + " / " + juce::String (pairs));

        // 7.3 смена типа на лету у одного процессора
        {
            OrogenProcessor p;
            auto b1 = renderWithSetup (p, 24000, sr, [] (OrogenProcessor& pr) { soloSlot (pr, 0, 0); });
            soloSlot (p, 0, 5);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (80);
            auto b2 = renderBlocks (p, 24000, sr);
            double acc = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < b1.getNumSamples(); ++i)
                {
                    const double d = b1.getReadPointer (ch)[i] - b2.getReadPointer (ch)[i];
                    acc += d * d;
                }
            const double diff = std::sqrt (acc / (double) (b1.getNumSamples() * 2));
            std::printf ("   на лету: wavetable %.4f -> modal %.4f, разница формы %.4f\n",
                         rmsOf (b1), rmsOf (b2), diff);
            check (diff > 0.01, "тип движка меняется на лету", num (diff, 4));
        }

        auto delta = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            double acc = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                {
                    const double d = a.getReadPointer (ch)[i] - b.getReadPointer (ch)[i];
                    acc += d * d;
                }
            return std::sqrt (acc / (double) (a.getNumSamples() * 2));
        };

        // 7.4 все восемь ручек хоруса прошивки доходят до DSP
        auto chorusRender = [&] (float delay, float fb, float lp, float in)
        {
            OrogenProcessor p;
            return renderWithSetup (p, 24000, sr, [=] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "fx_chorusModel", 1.0f);      // Firmware OS 1.32B
                setP (pr, "fx_chorusMix", 1.0f);
                setP (pr, "fx_chorusDepth", 0.8f);
                setP (pr, "fx_chorusRate", 0.6f);
                setP (pr, "fx_chorusDelay", delay);
                setP (pr, "fx_chorusFeedback", fb);
                setP (pr, "fx_chorusLowpass", lp);
                setP (pr, "fx_chorusInput", in);
            });
        };
        // LP слышен только там, где wet заметно отличается от dry: берём
        // DEL 100 / FB 60 (на малой задержке и нулевом фидбэке wet≈dry)
        const auto c1 = chorusRender (100.0f, 60.0f, 127.0f, 100.0f);
        const auto c2 = chorusRender (120.0f, 90.0f, 127.0f, 100.0f);
        const auto c3 = chorusRender (100.0f, 60.0f, 0.0f, 100.0f);
        const auto c4 = chorusRender (100.0f, 60.0f, 127.0f, 20.0f);
        const double dDelayFb = delta (c1, c2);
        const double dLowpass = delta (c1, c3);
        const double dInput   = delta (c1, c4);
        std::printf ("   хорус DEL/FB %.4f, LP %.4f, INP %.4f\n", dDelayFb, dLowpass, dInput);
        check (dDelayFb > 0.002, "хорус: DEL и FB слышны", num (dDelayFb, 4));
        check (dLowpass > 0.002, "хорус: LP слышен", num (dLowpass, 4));
        check (dInput > 0.002, "хорус: INP слышен", num (dInput, 4));

        // 7.5 глитч: FB/FOLD/TONE
        auto glitchRender = [&] (float fb, float fold, float tone)
        {
            OrogenProcessor p;
            return renderWithSetup (p, 24000, sr, [=] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "fx_glitchMix", 1.0f);
                setP (pr, "fx_glitchChance", 1.0f);
                setP (pr, "fx_glitchFeedback", fb);
                setP (pr, "fx_glitchFold", fold);
                setP (pr, "fx_glitchTone", tone);
            });
        };
        const double dGlitch = delta (glitchRender (0.0f, 0.0f, 24000.0f),
                                      glitchRender (0.6f, 4.0f, 800.0f));
        std::printf ("   GLITCH FB/FOLD/TONE меняют сигнал на %.4f\n", dGlitch);
        check (dGlitch > 0.002, "глитч: FB/FOLD/TONE слышны", num (dGlitch, 4));

        // 7.6 спектр: полоса анализа
        auto spectreRender = [&] (float lo, float hi)
        {
            OrogenProcessor p;
            return renderWithSetup (p, 24000, sr, [=] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "fx_spectreMix", 1.0f);
                setP (pr, "fx_spectreLow", lo);
                setP (pr, "fx_spectreHigh", hi);
            });
        };
        const double dSpectre = delta (spectreRender (0.0f, 0.0f), spectreRender (800.0f, 3000.0f));
        std::printf ("   SPECTRE LO/HI меняют сигнал на %.4f\n", dSpectre);
        check (dSpectre > 0.002, "спектр: полоса LO/HI слышна", num (dSpectre, 4));
    }


    // ---------------------------------------------------------------- 8
    std::printf ("\n--- 8. Все 16 эффектов: звук и присутствие в GUI ---\n");
    {
        struct FxCase { const char* name; const char* id; float value; const char* prefix; };

        // по одному «включающему» параметру на каждую конфигурацию FX
        const FxCase cases[] = {
            { "DRIVE",            "fx_drive",           0.55f, "fx_drive"    },
            { "CHORUS Simple",    "fx_chorusMix",        1.00f, "fx_chorus"   },
            { "CHORUS Firmware",  "fx_chorusModel",      1.00f, "fx_chorus"   },
            { "DELAY",            "fx_delayMix",         0.80f, "fx_delay"    },
            { "REVERB",           "fx_reverbMix",        0.80f, "fx_reverb"   },
            { "PLATE",            "fx_plateMix",         0.80f, "fx_plate"    },
            { "EQ",               "fx_eqLow",            9.00f, "fx_eq"       },
            { "DYNAMICS",         "fx_compThreshold",  -30.00f, "fx_comp"     },
            { "WIDTH",            "fx_width",            1.50f, "fx_width"    },
            { "MATERIAL",         "fx_materialMix",      0.80f, "fx_material" },
            { "SPECTRE Extended", "fx_spectreMix",       0.90f, "fx_spectre"  },
            { "SPECTRE Classic",  "fx_spectreModel",     1.00f, "fx_spectre"  },
            { "GLITCH",           "fx_glitchMix",        1.00f, "fx_glitch"   },
            { "ROBIN",            "fx_robinMix",         1.00f, "fx_robin"    },
            { "COMB Odin",        "fx_combMix",          0.80f, "fx_comb"     },
            { "COMB Parallel",    "fx_combModel",        1.00f, "fx_comb"     }
        };
        constexpr int nCases = (int) (sizeof (cases) / sizeof (cases[0]));

        // сухой сигнал: все миксы в ноль
        auto dryRender = [&]
        {
            OrogenProcessor p;
            return renderWithSetup (p, 24000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                for (const char* id : { "fx_drive", "fx_chorusMix", "fx_delayMix", "fx_reverbMix",
                                        "fx_plateMix", "fx_materialMix", "fx_spectreMix",
                                        "fx_glitchMix", "fx_robinMix", "fx_combMix",
                                        "fx_eqLow", "fx_eqHigh", "fx_compThreshold", "fx_compRatio" })
                    setP (pr, id, 0.0f);
                setP (pr, "fx_compMakeup", 0.0f);
                setP (pr, "fx_width", 1.0f);
            });
        };
        const auto dry = dryRender();
        const double dryRms = rmsOf (dry);

        auto deltaTo = [&] (const juce::AudioBuffer<float>& b)
        {
            double acc = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < dry.getNumSamples(); ++i)
                {
                    const double d = dry.getReadPointer (ch)[i] - b.getReadPointer (ch)[i];
                    acc += d * d;
                }
            return std::sqrt (acc / (double) (dry.getNumSamples() * 2));
        };

        // параметры, которые тоже надо поднять, чтобы эффект был слышен
        auto prepareFx = [] (OrogenProcessor& pr, const juce::String& name)
        {
            if (name == "DELAY")       setP (pr, "fx_delayTime", 0.18f);
            if (name == "DYNAMICS")    { setP (pr, "fx_compRatio", 8.0f); setP (pr, "fx_compMakeup", 6.0f); }
            if (name == "EQ")          setP (pr, "fx_eqHigh", -9.0f);
            if (name == "GLITCH")      setP (pr, "fx_glitchChance", 1.0f);
            if (name == "ROBIN")       setP (pr, "fx_robinSemis", 7.0f);
            if (name == "MATERIAL")    setP (pr, "fx_materialExcite", 0.9f);
            if (name == "SPECTRE Classic") setP (pr, "fx_spectreMix", 0.9f);
            if (name == "COMB Parallel")   setP (pr, "fx_combMix", 0.8f);
            if (name == "CHORUS Firmware") setP (pr, "fx_chorusMix", 1.0f);
        };

        juce::StringArray quiet;                       // эффекты, которые не слышны
        juce::StringArray broken;                      // эффекты, сломавшие звук
        for (int i = 0; i < nCases; ++i)
        {
            OrogenProcessor p;
            auto b = renderWithSetup (p, 24000, sr, [&] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                for (const char* id : { "fx_drive", "fx_chorusMix", "fx_delayMix", "fx_reverbMix",
                                        "fx_plateMix", "fx_materialMix", "fx_spectreMix",
                                        "fx_glitchMix", "fx_robinMix", "fx_combMix",
                                        "fx_eqLow", "fx_eqHigh", "fx_compThreshold" })
                    setP (pr, id, 0.0f);
                setP (pr, "fx_width", 1.0f);
                setP (pr, cases[i].id, cases[i].value);
                prepareFx (pr, cases[i].name);
            });

            const double d = deltaTo (b);
            const double r = rmsOf (b);
            int nan = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int j = 0; j < b.getNumSamples(); ++j)
                    if (! std::isfinite (b.getReadPointer (ch)[j])) ++nan;

            std::printf ("   %-16s Δ %.4f (RMS %.4f, сухой %.4f, NaN %d)\n",
                         cases[i].name, d, r, dryRms, nan);
            if (d <= 0.002) quiet.add (cases[i].name);
            if (r <= 0.0015 || nan != 0) broken.add (cases[i].name);
        }
        check (quiet.isEmpty(), "каждый из 16 эффектов слышен",
               quiet.isEmpty() ? juce::String ("16 / 16") : quiet.joinIntoString (", "));
        check (broken.isEmpty(), "ни один эффект не глушит и не ломает сигнал",
               broken.isEmpty() ? juce::String ("0 дефектов") : broken.joinIntoString (", "));

        // --- присутствие в GUI: главный экран и постраничный оверлей --------
        OrogenProcessor p;
        juce::AudioProcessorEditor* ed = p.createEditor();
        check (ed != nullptr, "редактор создаётся для проверки GUI");
        if (ed != nullptr)
        {
            auto* ore = static_cast<OrogenEditor*> (ed);
            ore->setSize (1280, 800);

            const auto mainIds = ore->mainScreenParamIds();
            std::printf ("   на главном экране привязано параметров: %d\n", mainIds.size());

            int onMain = 0;
            for (int i = 0; i < nCases; ++i)
            {
                bool found = false;
                for (const auto& id : mainIds)
                    if (id.startsWith (cases[i].prefix)) { found = true; break; }
                if (found) ++onMain;
            }

            // в оверлее каждая конфигурация тоже должна иметь свою секцию с контролами
            const auto tabs = buildUiTabs();
            int inOverlay = 0;
            juce::StringArray missingOverlay;
            for (int i = 0; i < nCases; ++i)
            {
                bool found = false;
                for (const auto& t : tabs)
                    for (const auto& sec : t.sections)
                        for (const auto& id : sec.ids)
                            if (id.startsWith (cases[i].prefix)) found = true;
                if (found) ++inOverlay; else missingOverlay.add (cases[i].name);
            }

            std::printf ("   в GUI: главный экран %d / 16, оверлей %d / 16\n", onMain, inOverlay);
            check (inOverlay == nCases, "все 16 эффектов есть в оверлее параметров",
                   missingOverlay.isEmpty() ? juce::String ("16 / 16") : missingOverlay.joinIntoString (", "));
            check (onMain >= 8, "эффекты доступны и с главного экрана", num ((double) onMain, 0) + " / 16");

            // страницы FX-колонки: 3 страницы, листание не теряет параметры
            {
                const int pagesCount = ore->fxColumnPageCount();
                std::printf ("   FX-колонка: страниц %d\n", pagesCount);
                check (pagesCount == 3, "FX-колонка разложена на 3 страницы",
                       num ((double) pagesCount, 0));
                // после листания набор доступных параметров не уменьшается
                const int n0 = ore->mainScreenParamIds().size();
                ore->nextFxColumnPage();
                const int n1 = ore->mainScreenParamIds().size();
                check (n1 == n0, "листание страниц не теряет параметры",
                       juce::String (n0) + " → " + juce::String (n1));
                ore->nextFxColumnPage();
                ore->nextFxColumnPage();          // вернулись на первую (по кругу)
            }

            // движки: список в списке выбора главного экрана = 9 типов, фильтров 16
            int engineItems = 0, filterItems = 0;
            for (auto* child : ore->getChildren())
                if (auto* box = dynamic_cast<juce::ComboBox*> (child))
                {
                    if (box->getNumItems() == 9)  engineItems = box->getNumItems();
                    if (box->getNumItems() == 16) filterItems = box->getNumItems();
                }
            std::printf ("   селекторы главного экрана: движков %d, фильтров %d\n",
                         engineItems, filterItems);
            check (engineItems == 9, "в списке движков все 9 типов", num ((double) engineItems, 0));
            check (filterItems == 16, "в списке фильтров все 16 моделей", num ((double) filterItems, 0));

            delete ed;
        }
    }


    // ---------------------------------------------------------------- 9
    std::printf ("\n--- 9. Параметры движков, 16 фильтров, мод-матрица, LFO, ARP ---\n");
    {
        const char* eng[9] = { "Wavetable", "FM", "Harmonic", "Sample",
                               "Granular", "Modal", "Resynth", "Organics", "Chop" };

        auto deltaTo = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            double acc = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                {
                    const double d = a.getReadPointer (ch)[i] - b.getReadPointer (ch)[i];
                    acc += d * d;
                }
            return std::sqrt (acc / (double) (a.getNumSamples() * 2));
        };

        // 9.1 каждый из четырёх макропараметров движка (P1…P4) слышен
        int deadParams = 0, checked = 0;
        for (int t = 0; t < 9; ++t)
        {
            OrogenProcessor p;
            auto base = renderWithSetup (p, 20000, sr, [t] (OrogenProcessor& pr) { soloSlot (pr, 0, t); });
            juce::String dead;
            for (int k = 0; k < 4; ++k)
            {
                OrogenProcessor q;
                const auto variant = renderWithSetup (q, 20000, sr, [t, k] (OrogenProcessor& pr)
                {
                    soloSlot (pr, 0, t);
                    setP (pr, "sA_p" + juce::String (k), 0.92f);
                });
                const double d = deltaTo (base, variant);
                ++checked;
                if (d <= 0.0015) { ++deadParams; dead << " P" << (k + 1) << "(" << num (d, 4) << ")"; }
            }
            std::printf ("   %-9s P1..P4:%s\n", eng[t], dead.isEmpty() ? " все слышны" : dead.toRawUTF8());
        }
        check (deadParams == 0, "все макропараметры всех 9 движков влияют на звук",
               juce::String (checked - deadParams) + " / " + juce::String (checked));

        // 9.2 модели фильтра: каждая отличается от LP12
        {
            OrogenProcessor p;
            auto lp12 = renderWithSetup (p, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "sA_filter", 0.0f);      // LP12
                setP (pr, "sA_cutoff", 0.45f);
                setP (pr, "sA_res", 0.35f);
            });
            int same = 0;
            juce::String sameNames;
            for (int f = 1; f < 16; ++f)
            {
                OrogenProcessor q;
                auto v = renderWithSetup (q, 20000, sr, [f] (OrogenProcessor& pr)
                {
                    soloSlot (pr, 0, 0);
                    setP (pr, "sA_filter", (float) f);
                    setP (pr, "sA_cutoff", 0.45f);
                    setP (pr, "sA_res", 0.35f);
                });
                const double d = deltaTo (lp12, v);
                if (d <= 0.0015) { ++same; sameNames << " " << f; }
            }
            std::printf ("   модели фильтра: отличны от LP12 %d из 15\n", 15 - same);
            check (same == 0, "все 16 моделей фильтра дают разный звук",
                   same == 0 ? juce::String ("16 / 16") : sameNames);
        }

        // 9.3 мод-матрица: маршрут LFO1 -> Cutoff меняет звук
        {
            OrogenProcessor p;
            auto off = renderWithSetup (p, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "sA_cutoff", 0.5f);
                setP (pr, "mod1_src", 0.0f);       // LFO 1
                setP (pr, "mod1_dst", 0.0f);       // Cutoff
                setP (pr, "mod1_amt", 0.0f);
            });
            OrogenProcessor q;
            auto on = renderWithSetup (q, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "sA_cutoff", 0.5f);
                setP (pr, "mod1_src", 0.0f);
                setP (pr, "mod1_dst", 0.0f);
                setP (pr, "mod1_amt", 1.0f);
            });
            const double d = deltaTo (off, on);
            std::printf ("   мод-маршрут LFO1 -> Cutoff: Δ %.4f\n", d);
            check (d > 0.002, "мод-матрица влияет на звук", num (d, 4));
        }

        // 9.4 быстрый LFO (amount/dest) и его форма
        {
            OrogenProcessor p;
            auto off = renderWithSetup (p, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "lfo1_amount", 0.0f);
            });
            OrogenProcessor q;
            auto on = renderWithSetup (q, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "lfo1_amount", 1.0f);
                setP (pr, "lfo1_rate", 3.0f);
                setP (pr, "lfo1_dest", 0.0f);
            });
            const double d = deltaTo (off, on);
            std::printf ("   LFO 1 (amount 1, 3 Гц): Δ %.4f\n", d);
            check (d > 0.002, "LFO слышен", num (d, 4));
        }

        // 9.5 арпеджиатор: включает ноты — меняется огибающая
        {
            OrogenProcessor p;
            auto off = renderWithSetup (p, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "arpOn", 0.0f);
            });
            OrogenProcessor q;
            auto on = renderWithSetup (q, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "arpOn", 1.0f);
                setP (pr, "arpMode", 0.0f);
                setP (pr, "arpDiv", 1.0f);
                setP (pr, "arpOctaves", 1.0f);
                setP (pr, "arpGate", 0.6f);
            });
            const double d = deltaTo (off, on);
            std::printf ("   ARP on/off: Δ %.4f (RMS %.4f / %.4f)\n", d, rmsOf (off), rmsOf (on));
            check (d > 0.002, "арпеджиатор слышен", num (d, 4));
        }

        // 9.6 наложение слоёв: второй слот добавляет сигнал
        {
            OrogenProcessor p;
            auto one = renderWithSetup (p, 20000, sr, [] (OrogenProcessor& pr)
            {
                for (int s = 0; s < 8; ++s)
                    setP (pr, "s" + juce::String::charToString ((juce::juce_wchar) ('A' + s)) + "_on",
                          s == 0 ? 1.0f : 0.0f);
            });
            OrogenProcessor q;
            auto two = renderWithSetup (q, 20000, sr, [] (OrogenProcessor& pr)
            {
                for (int s = 0; s < 8; ++s)
                    setP (pr, "s" + juce::String::charToString ((juce::juce_wchar) ('A' + s)) + "_on",
                          s <= 1 ? 1.0f : 0.0f);
                setP (pr, "sB_type", 5.0f);        // Modal во втором слоте
            });
            const double d = deltaTo (one, two);
            std::printf ("   слой B поверх A: Δ %.4f (RMS %.4f -> %.4f)\n", d, rmsOf (one), rmsOf (two));
            check (d > 0.002, "слои складываются", num (d, 4));
        }

        // 9.7 диапазон клавиш слота: нота вне зоны не звучит
        {
            OrogenProcessor p;
            auto inRange = renderWithSetup (p, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "sA_lo", 0.0f);
                setP (pr, "sA_hi", 127.0f);
            });
            OrogenProcessor q;
            auto outRange = renderWithSetup (q, 20000, sr, [] (OrogenProcessor& pr)
            {
                soloSlot (pr, 0, 0);
                setP (pr, "sA_lo", 100.0f);
                setP (pr, "sA_hi", 110.0f);
            });
            std::printf ("   диапазон клавиш: RMS %.4f (в зоне) / %.4f (вне зоны)\n",
                         rmsOf (inRange), rmsOf (outRange));
            check (rmsOf (inRange) > 0.01 && rmsOf (outRange) < 0.01,
                   "слот молчит вне своего диапазона клавиш", num (rmsOf (outRange), 4));
        }

        // 9.8 всё из блока 9 доступно в интерфейсе: макросы слота, фильтр,
        //     диапазон клавиш, мод-матрица, LFO и арпеджиатор
        {
            struct Group { const char* name; juce::StringArray ids; };
            juce::Array<Group> groups;
            auto g = [&groups] (const char* name, juce::StringArray ids)
            {
                Group gr; gr.name = name; gr.ids = std::move (ids); groups.add (gr);
            };

            juce::StringArray macros;
            for (int k = 0; k < 4; ++k) macros.add ("sA_p" + juce::String (k));
            g ("макросы слота P1..P4", macros);
            g ("фильтр слота", juce::StringArray { "sA_filter", "sA_cutoff", "sA_res" });
            g ("диапазон клавиш", juce::StringArray { "sA_lo", "sA_hi" });
            g ("мод-матрица", juce::StringArray { "mod1_src", "mod1_dst", "mod1_amt" });
            g ("LFO 1", juce::StringArray { "lfo1_amount", "lfo1_rate", "lfo1_dest" });
            g ("арпеджиатор", juce::StringArray { "arpOn", "arpMode", "arpDiv", "arpOctaves", "arpGate" });

            OrogenProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            check (ed != nullptr, "редактор создаётся для проверки GUI блока 9");
            if (ed != nullptr)
            {
                auto* ore = static_cast<OrogenEditor*> (ed.get());
                ore->setSize (1280, 800);
                const auto mainIds = ore->mainScreenParamIds();
                const auto tabs = buildUiTabs();

                auto onMain = [&mainIds] (const juce::String& id)
                {
                    for (const auto& m : mainIds) if (m == id) return true;
                    return false;
                };
                auto inOverlay = [&tabs] (const juce::String& id)
                {
                    for (const auto& t : tabs)
                        for (const auto& sec : t.sections)
                            for (const auto& x : sec.ids) if (x == id) return true;
                    return false;
                };

                int totalIds = 0, overlayOk = 0, mainOk = 0;
                juce::StringArray missOverlay, missMain;
                for (const auto& gr : groups)
                {
                    int o = 0, mn = 0;
                    for (const auto& id : gr.ids)
                    {
                        ++totalIds;
                        if (inOverlay (id)) { ++overlayOk; ++o; } else missOverlay.add (id);
                        if (onMain (id))    { ++mainOk;    ++mn; } else missMain.add (id);
                    }
                    std::printf ("   GUI «%s»: оверлей %d / %d, главный экран %d / %d\n",
                                 gr.name, o, gr.ids.size(), mn, gr.ids.size());
                }
                std::printf ("   GUI блока 9 всего: оверлей %d / %d, главный экран %d / %d\n",
                             overlayOk, totalIds, mainOk, totalIds);

                check (overlayOk == totalIds, "все параметры блока 9 есть в оверлее параметров",
                       missOverlay.isEmpty() ? juce::String (totalIds) + " / " + juce::String (totalIds)
                                             : missOverlay.joinIntoString (", "));
                // Главный экран по макету несёт шесть ручек выбранного слота
                // (cutoff/p0/level + res/p1/detune); остальное — в оверлее PARAMS.
                juce::StringArray mustMain { "sA_cutoff", "sA_res", "sA_p0", "sA_p1",
                                             "sA_level", "sA_detune" };
                juce::StringArray missMust;
                for (const auto& id : mustMain) if (! onMain (id)) missMust.add (id);
                check (missMust.isEmpty(), "шесть ручек выбранного слота на главном экране",
                       missMust.isEmpty() ? juce::String (mustMain.size()) + " / "
                                            + juce::String (mustMain.size())
                                          : missMust.joinIntoString (", "));

                // оверлей обязан содержать весь набор слота (21 параметр × 8 слотов)
                int slotInOverlay = 0, slotTotal = 0;
                for (int s = 0; s < 8; ++s)
                    for (const auto& sp : { "level", "pan", "tune", "detune", "p0", "p1", "p2", "p3",
                                            "table", "attack", "decay", "sustain", "release",
                                            "cutoff", "res", "lo", "hi", "tail", "on", "type", "filter" })
                    {
                        ++slotTotal;
                        const juce::String id = juce::String ("s") + juce::String::charToString ((juce::juce_wchar) ('A' + s)) + "_" + sp;
                        if (inOverlay (id)) ++slotInOverlay;
                    }
                std::printf ("   оверлей: параметров слотов %d / %d\n", slotInOverlay, slotTotal);
                check (slotInOverlay == slotTotal, "в оверлее весь набор параметров всех 8 слотов",
                       juce::String (slotInOverlay) + " / " + juce::String (slotTotal));
            }
        }

        // ---- 10. Главный экран: интерактивные зоны (мышь ведёт к параметрам) ----
        {
            OrogenProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            check (ed != nullptr, "редактор создаётся для блока 10");
            if (ed != nullptr)
            {
                auto* ore = static_cast<OrogenEditor*> (ed.get());
                ore->setSize (1280, 800);

                auto norm = [&p] (const juce::String& id)
                {
                    if (auto* prm = p.apvts.getParameter (id)) return prm->getValue();
                    return -1.0f;
                };

                // 10.1 чипы FX: клик включает эффект, повторный — выключает
                {
                    int ok = 0;
                    juce::StringArray bad;
                    for (int i = 0; i < 5; ++i)
                    {
                        const juce::String idS = ore->chipParamId (i);
                        const char* ids[1] = { idS.toRawUTF8() };
                        const juce::Point<float> chip (254.0f + 146.0f * (float) i + 60.0f, 615.0f);
                        // приводим смесь к нулю: тест проверяет именно жест, а не патч
                        if (auto* prm = p.apvts.getParameter (ids[0])) prm->setValueNotifyingHost (0.0f);
                        const float before = norm (ids[0]);
                        ore->clickMain (chip);
                        const float on = norm (ids[0]);
                        ore->clickMain (chip);
                        const float off = norm (ids[0]);
                        std::printf ("   чип %-16s: %.3f -> %.3f -> %.3f\n",
                                     ids[0], (double) before, (double) on, (double) off);
                        if (on > before + 0.01f && off < 0.01f) ++ok; else bad.add (ids[0]);
                    }
                    std::printf ("   чипы FX: %d / 5 включаются кликом\n", ok);
                    check (ok == 5, "клик по чипу FX включает и выключает эффект",
                           bad.isEmpty() ? juce::String ("5 / 5") : bad.joinIntoString (", "));
                }

                // 10.1b страницы чипов: все 10 эффектов с ручкой доли достижимы
                {
                    const int pages = ore->chipPageCount();
                    juce::StringArray seen;
                    for (int pg = 0; pg < pages; ++pg)
                    {
                        for (int i = 0; i < 5; ++i)
                        {
                            const int idx = pg * 5 + i;
                            if (idx < ore->chipCount()) seen.addIfNotAlreadyThere (ore->chipParamId (idx));
                        }
                        ore->clickMain ({ 886.0f + 39.0f, 586.0f });     // кнопка «‹ 1 / 2 ›»
                    }
                    std::printf ("   чипы: страниц %d, эффектов всего %d, собрано по страницам %d\n",
                                 pages, ore->chipCount(), seen.size());
                    check (pages == 2 && ore->chipCount() == 10 && seen.size() == 10,
                           "чипы FX листаются, все 10 эффектов с ручкой доли доступны",
                           juce::String (seen.size()) + " / 10 на " + juce::String (pages) + " стр.");

                    // чип второй страницы (GLITCH) обязан работать
                    ore->clickMain ({ 886.0f + 39.0f, 586.0f });         // на страницу 2
                    if (auto* prm = p.apvts.getParameter ("fx_glitchMix"))
                        prm->setValueNotifyingHost (0.0f);
                    ore->clickMain ({ 254.0f + 146.0f * 2.0f + 60.0f, 615.0f });   // третий чип 2-й стр.
                    const float gOn = norm ("fx_glitchMix");
                    ore->clickMain ({ 254.0f + 146.0f * 2.0f + 60.0f, 615.0f });
                    const float gOff = norm ("fx_glitchMix");
                    std::printf ("   чип GLITCH со 2-й страницы: %.3f -> %.3f\n", (double) gOn, (double) gOff);
                    check (gOn > 0.01f && gOff < 0.01f, "чип со второй страницы включает эффект",
                           num ((double) gOn, 3));

                    // возвращаемся на первую страницу
                    ore->clickMain ({ 886.0f + 39.0f, 586.0f });
                }

                // 10.1c дисплей: горизонталь — позиция таблицы (P1), вертикаль — варп (P2)
                {
                    const float p0before = norm ("sA_p0"), p1before = norm ("sA_p1");
                    ore->beginMainDrag ({ 500.0f, 170.0f });
                    ore->dragMain ({ 560.0f, 150.0f });
                    ore->releaseMain();
                    const float p0after = norm ("sA_p0"), p1after = norm ("sA_p1");
                    std::printf ("   дисплей: P1 %.3f -> %.3f, P2 %.3f -> %.3f\n",
                                 (double) p0before, (double) p0after,
                                 (double) p1before, (double) p1after);
                    check (p0after > p0before && p1after > p1before,
                           "перетаскивание по дисплею меняет позицию и варп таблицы",
                           num ((double) p0after, 3) + " / " + num ((double) p1after, 3));
                }

                // 10.2 узлы мод-матрицы: тянем вверх — количество растёт
                {
                    int ok = 0;
                    juce::StringArray bad;
                    juce::StringArray rows;
                    for (int i = 0; i < 8; ++i)
                    {
                        const juce::String id = "mod" + juce::String (i + 1) + "_amt";
                        // старт — из середины полосы маршрута, выше узла
                        const float x = 262.0f + (736.0f - 28.0f) / 8.0f * ((float) i + 0.5f);
                        ore->beginMainDrag ({ x, 320.0f });
                        ore->dragMain ({ x, 312.0f });
                        ore->releaseMain();
                        const float up = norm (id);
                        ore->beginMainDrag ({ x, 320.0f });
                        ore->dragMain ({ x, 328.0f });
                        ore->releaseMain();
                        const float down = norm (id);
                        rows.add (id + " " + num ((double) up, 3) + "/" + num ((double) down, 3));
                        if (up > 0.5f && down < up) ++ok; else bad.add (id);
                    }
                    std::printf ("   мод-матрица: %d / 8 узлов тянутся (%s)\n", ok,
                                 rows.joinIntoString (", ").toRawUTF8());
                    check (ok == 8, "узлы мод-матрицы на главном экране меняют количество",
                           bad.isEmpty() ? juce::String ("8 / 8") : bad.joinIntoString (", "));
                }

                // поиск узла конверта по текущей геометрии (нужен и в 10.3, и в 10.5):
                // положения узлов зависят от самих значений A/D/S, поэтому ищем заново
                auto findNode = [ore] (OrogenEditor::MainHit want) -> juce::Point<float>
                {
                    for (float x = 262.0f; x < 960.0f; x += 1.0f)
                        for (float y = 410.0f; y < 462.0f; y += 1.0f)
                        {
                            int idx = -1;
                            if (ore->hitMain ({ x, y }, idx) == want) return { x, y };
                        }
                    return { -1.0f, -1.0f };
                };

                // 10.3 конверт: узлы attack / decay / sustain / release
                {
                    auto dragNode = [ore] (juce::Point<float> from, juce::Point<float> to)
                    {
                        ore->beginMainDrag (from);
                        ore->dragMain (to);
                        ore->releaseMain();
                    };

                    const float a0 = norm ("sA_attack"), d0 = norm ("sA_decay");
                    const float s0 = norm ("sA_sustain"), r0 = norm ("sA_release");

                    // каждый раз ищем узел заново: после правки attack геометрия сдвигается
                    auto atk = findNode (OrogenEditor::MainHit::envAttack);
                    if (atk.x > 0) dragNode (atk, atk + juce::Point<float> (24.0f, 0.0f));
                    const float a1 = norm ("sA_attack");

                    auto dec = findNode (OrogenEditor::MainHit::envDecay);
                    if (dec.x > 0) dragNode (dec, dec + juce::Point<float> (24.0f, 0.0f));
                    const float d1 = norm ("sA_decay");

                    auto sus = findNode (OrogenEditor::MainHit::envSustain);
                    if (sus.x > 0) dragNode (sus, sus + juce::Point<float> (0.0f, 8.0f));   // вниз — тише
                    const float s1 = norm ("sA_sustain");

                    sus = findNode (OrogenEditor::MainHit::envSustain);
                    if (sus.x > 0) dragNode (sus, sus + juce::Point<float> (24.0f, 0.0f));
                    const float r1 = norm ("sA_release");

                    std::printf ("   конверт: attack %.3f->%.3f, decay %.3f->%.3f, "
                                 "sustain %.3f->%.3f, release %.3f->%.3f\n",
                                 (double) a0, (double) a1, (double) d0, (double) d1,
                                 (double) s0, (double) s1, (double) r0, (double) r1);
                    check (a1 > a0 && d1 > d0 && s1 < s0 && r1 > r0,
                           "узлы конверта на главном экране правят A/D/S/R",
                           juce::String (a1 > a0 ? "A+" : "A-") + juce::String (d1 > d0 ? " D+" : " D-")
                           + juce::String (s1 < s0 ? " S-" : " S+") + juce::String (r1 > r0 ? " R+" : " R-"));
                }

                // 10.4 дорожка LFO: горизонталь — частота, вертикаль — глубина
                {
                    const float r0 = norm ("lfo1_rate"), am0 = norm ("lfo1_amount");
                    ore->beginMainDrag ({ 600.0f, 372.0f });
                    ore->dragMain ({ 640.0f, 364.0f });
                    ore->releaseMain();
                    const float r1 = norm ("lfo1_rate"), am1 = norm ("lfo1_amount");
                    std::printf ("   LFO 1: частота %.3f->%.3f, глубина %.3f->%.3f\n",
                                 (double) r0, (double) r1, (double) am0, (double) am1);
                    check (r1 > r0 && am1 > am0, "дорожка LFO 1 правит частоту и глубину",
                           num ((double) r1, 3) + " / " + num ((double) am1, 3));
                }

                // 10.5 сводка: сколько целей на главном экране и все ли отвечают
                {
                    int targets = 0, alive = 0;
                    juce::StringArray dead;

                    auto probe = [&] (juce::Point<float> pt)
                    {
                        int idx = -1;
                        const auto hit = ore->hitMain (pt, idx);
                        if (hit != OrogenEditor::MainHit::none) { ++alive; }
                        else dead.add (juce::String ((int) pt.x) + ":" + juce::String ((int) pt.y));
                        juce::ignoreUnused (idx);
                    };

                    for (int i = 0; i < 5; ++i)      { ++targets; probe ({ 254.0f + 146.0f * (float) i + 60.0f, 615.0f }); }
                    ++targets; probe ({ 886.0f + 39.0f, 586.0f });   // кнопка страниц чипов
                    ++targets; probe ({ 500.0f, 170.0f });           // дисплей
                    for (int i = 0; i < 8; ++i)
                    {
                        ++targets;
                        const float x = 262.0f + 88.5f * ((float) i + 0.5f);
                        probe ({ x, 320.0f });
                    }
                    ++targets; probe ({ 600.0f, 372.0f });      // LFO
                    for (auto want : { OrogenEditor::MainHit::envAttack,
                                       OrogenEditor::MainHit::envDecay,
                                       OrogenEditor::MainHit::envSustain })
                    {
                        ++targets;
                        const auto pt = findNode (want);
                        if (pt.x > 0) probe (pt); else dead.add ("узел конверта не найден");
                    }

                    std::printf ("   целей на главном экране: %d (объявлено %d), отозвались %d\n",
                                 targets, ore->mainScreenHitCount(), alive);
                    check (alive == targets && ore->mainScreenHitCount() >= targets,
                           "все интерактивные зоны главного экрана отвечают",
                           juce::String (alive) + " / " + juce::String (targets));
                }

                // 10.6 банк программ в шапке: селектор со всеми программами хоста
                {
                    juce::ComboBox* prog = nullptr;
                    for (auto* child : ore->getChildren())
                        if (auto* box = dynamic_cast<juce::ComboBox*> (child))
                            if (box->getTooltip() == "PROGRAM") prog = box;

                    check (prog != nullptr, "в шапке есть селектор программ");
                    if (prog != nullptr)
                    {
                        const int nPrograms = p.getNumPrograms();
                        std::printf ("   программ в банке: %d, пунктов в селекторе: %d\n",
                                     nPrograms, prog->getNumItems());
                        check (prog->getNumItems() == nPrograms,
                               "селектор содержит все программы банка",
                               juce::String (prog->getNumItems()) + " / " + juce::String (nPrograms));

                        // каждая программа должна отличаться от первой хотя бы одним параметром
                        auto snapshot = [&p]
                        {
                            juce::Array<float> v;
                            static const char* ids[] = { "gl_master", "gl_gain", "gl_morph",
                                                         "sA_level", "sA_cutoff", "sA_p0",
                                                         "sA_type", "sA_filter", "fx_reverbMix" };
                            for (const auto* id : ids)
                                v.add (p.apvts.getParameter (id)->getValue());
                            return v;
                        };

                        prog->setSelectedItemIndex (0, juce::sendNotificationSync);
                        const auto base = snapshot();
                        int distinct = 0;
                        juce::StringArray rows;
                        for (int i = 1; i < nPrograms; ++i)
                        {
                            prog->setSelectedItemIndex (i, juce::sendNotificationSync);
                            const auto cur = snapshot();
                            int diff = 0;
                            for (int k = 0; k < base.size(); ++k)
                                if (std::abs (cur[k] - base[k]) > 1.0e-4f) ++diff;
                            rows.add (p.getProgramName (i).toUpperCase() + " " + juce::String (diff));
                            if (diff > 0 && p.getCurrentProgram() == i) ++distinct;
                        }
                        prog->setSelectedItemIndex (0, juce::sendNotificationSync);

                        std::printf ("   программ отличаются от первой: %d / %d (%s)\n",
                                     distinct, nPrograms - 1, rows.joinIntoString (", ").toRawUTF8());
                        check (distinct == nPrograms - 1,
                               "выбор программы в шапке меняет патч",
                               juce::String (distinct) + " / " + juce::String (nPrograms - 1));
                    }
                }

                // 10.7 мод-матрица на главном экране: все 8 маршрутов с приёмником
                {
                    ore->showPage (9);                    // секция MOD MATRIX
                    const int pagesInMatrix = ore->columnPageCount (1);
                    const auto ids = ore->allColumnIds (1);
                    int found = 0;
                    juce::StringArray missing;
                    for (int r = 1; r <= 8; ++r)
                        for (const auto* field : { "src", "dst", "amt" })
                        {
                            const juce::String id = "mod" + juce::String (r) + "_" + field;
                            if (ids.contains (id)) ++found; else missing.add (id);
                        }
                    std::printf ("   мод-матрица: страниц %d, параметров маршрутов %d / 24\n",
                                 pagesInMatrix, found);
                    check (found == 24, "все 8 маршрутов мод-матрицы с приёмником на главном экране",
                           missing.isEmpty() ? juce::String ("24 / 24") : missing.joinIntoString (", "));
                    check (pagesInMatrix >= 2, "маршруты 5…8 на второй странице колонки",
                           num ((double) pagesInMatrix, 0));
                }

                // 10.8 банк программ через GUI: добавить, сохранить, загрузить
                {
                    const int before = p.getNumPrograms();
                    const int added = ore->addProgramFromCurrent ("GUI TEST");
                    const int afterAdd = p.getNumPrograms();
                    std::printf ("   банк: программ %d -> %d (индекс %d), в селекторе %d\n",
                                 before, afterAdd, added, ore->programItemCount());
                    check (afterAdd == before + 1 && added == afterAdd - 1,
                           "добавление программы из GUI", juce::String (before) + " -> "
                                                          + juce::String (afterAdd));
                    check (ore->programItemCount() == afterAdd,
                           "список в шапке обновился", juce::String (ore->programItemCount()));

                    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                          .getChildFile ("orogen-bank-gui-test.json");
                    file.deleteFile();
                    const bool saved = ore->saveBankTo (file);
                    const auto size = file.getSize();
                    std::printf ("   банк сохранён: %s, %lld байт\n", saved ? "да" : "нет",
                                 (long long) size);
                    check (saved && size > 1000, "сохранение банка из GUI",
                           juce::String ((long long) size) + " байт");

                    // меняем параметр и загружаем банк обратно
                    auto* prm = p.apvts.getParameter ("sA_cutoff");
                    const float savedMaster = prm->getValue();
                    prm->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, savedMaster > 0.5f ? 0.05f : 0.95f));
                    const float changed = prm->getValue();

                    const bool loaded = ore->loadBankFrom (file);
                    const float restored = prm->getValue();
                    std::printf ("   банк загружен: %s, sA_cutoff %.3f -> %.3f -> %.3f, программ %d, в селекторе %d\n",
                                 loaded ? "да" : "нет", (double) savedMaster, (double) changed,
                                 (double) restored, p.getNumPrograms(), ore->programItemCount());
                    check (loaded && std::abs (restored - changed) > 1.0e-4f
                             && std::abs (restored - savedMaster) < 0.02f,
                           "загрузка банка из GUI возвращает параметры",
                           num ((double) restored, 3));
                    check (ore->programItemCount() == p.getNumPrograms(),
                           "после загрузки банка список программ совпадает",
                           juce::String (ore->programItemCount()) + " / "
                               + juce::String (p.getNumPrograms()));
                    file.deleteFile();
                }

                // 10.9 быстрый банк (пункт меню LOAD/SAVE, без диалога)
                {
                    // банк — это список программ, поэтому проверяем через программу:
                    // сохраняем текущий патч как программу, затем возим банк туда-обратно
                    const auto quick = ore->quickBankFile();
                    quick.deleteFile();

                    if (auto* pm = p.apvts.getParameter ("sB_cutoff"))
                        pm->setValueNotifyingHost (0.8f);
                    const int idx = ore->addProgramFromCurrent ("QUICK TEST");

                    const bool saved = ore->saveBankQuick();
                    const auto size = quick.getSize();

                    // портим патч и загружаем банк «одним пунктом меню»
                    if (auto* pm = p.apvts.getParameter ("sB_cutoff"))
                        pm->setValueNotifyingHost (0.1f);
                    const float broken = p.apvts.getParameter ("sB_cutoff")->getValue();

                    const bool loaded = ore->loadBankQuick();
                    const int found = p.presetNames().indexOf ("QUICK TEST");
                    if (found >= 0) p.setCurrentProgram (found);
                    const float back = p.apvts.getParameter ("sB_cutoff")->getValue();

                    std::printf ("   быстрый банк: %s, %lld байт, программа #%d, sB_cutoff %.3f -> %.3f\n",
                                 quick.getFullPathName().toRawUTF8(), (long long) size, found,
                                 (double) broken, (double) back);
                    check (saved && loaded && size > 1000 && found == idx
                             && std::abs (back - 0.8f) < 0.02f,
                           "быстрое сохранение и загрузка банка",
                           num ((double) back, 3));
                    quick.deleteFile();
                }

                // ---- 11. Анимации: фаза воспроизводима, кадры различаются ----
                {
                    auto snapshot = [ore] (double phase)
                    {
                        ore->setAnimationPhase (phase);
                        juce::Image img (juce::Image::ARGB, 1280, 800, true);
                        juce::Graphics g (img);
                        ore->paintEntireComponent (g, true);
                        return img;
                    };

                    auto diffPixels = [] (const juce::Image& a, const juce::Image& b)
                    {
                        int n = 0;
                        juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly);
                        juce::Image::BitmapData db (b, juce::Image::BitmapData::readOnly);
                        for (int y = 0; y < a.getHeight(); ++y)
                            for (int x = 0; x < a.getWidth(); ++x)
                                if (da.getPixelColour (x, y) != db.getPixelColour (x, y)) ++n;
                        return n;
                    };

                    // 11.1 фаза: 0…1 и повторяемость кадра
                    ore->setAnimationPhase (0.35);
                    const float ph1 = ore->animationPhase();
                    const auto frameA1 = snapshot (0.35);
                    const auto frameA2 = snapshot (0.35);
                    std::printf ("   фаза после установки 0.35 с: %.3f, кадр воспроизводим: %s\n",
                                 (double) ph1, diffPixels (frameA1, frameA2) == 0 ? "да" : "нет");
                    check (diffPixels (frameA1, frameA2) == 0,
                           "кадр при одной и той же фазе совпадает побитово", "0 пикселей");
                    check (ph1 > 0.15f && ph1 < 0.20f, "фаза считается как доля периода 2 с",
                           num ((double) ph1, 3));

                    // 11.2 разные фазы → разные пиксели (анимация реально рисуется)
                    const auto frameB = snapshot (1.35);
                    const int changedPx = diffPixels (frameA1, frameB);
                    std::printf ("   0.35 с против 1.35 с: различается %d пикселей (%.3f%%)\n",
                                 changedPx, 100.0 * (double) changedPx / (1280.0 * 800.0));
                    check (changedPx > 300, "анимация меняет изображение",
                           juce::String (changedPx) + " пикселей");

                    // 11.3 выключенные анимации → кадр замирает
                    ore->setAnimationsEnabled (false);
                    const auto still1 = snapshot (0.35);
                    const auto still2 = snapshot (1.35);
                    const int stillDiff = diffPixels (still1, still2);
                    std::printf ("   анимации выключены: разница кадров %d пикселей\n", stillDiff);
                    check (stillDiff == 0, "при выключенных анимациях кадры идентичны",
                           juce::String (stillDiff));
                    ore->setAnimationsEnabled (true);

                    // 11.4 анимация не трогает параметры
                    {
                        static const char* ids[] = { "sA_cutoff", "sA_p0", "gl_master",
                                                     "fx_reverbMix", "mod1_amt" };
                        juce::Array<float> before;
                        for (const auto* id : ids) before.add (p.apvts.getParameter (id)->getValue());
                        for (int i = 0; i < 40; ++i) ore->setAnimationPhase (i * 0.05);
                        int touched = 0;
                        for (int i = 0; i < before.size(); ++i)
                            if (std::abs (p.apvts.getParameter (ids[i])->getValue() - before[i]) > 1.0e-6f)
                                ++touched;
                        std::printf ("   параметров изменено анимацией: %d из %d\n", touched, before.size());
                        check (touched == 0, "анимация не меняет ни одного параметра", "0");
                    }

                    // 11.5 бюджет времени: 24 кадра в секунду не должны съедать ядро
                    {
                        juce::Image img (juce::Image::ARGB, 1280, 800, true);
                        const int frames = 20;
                        const double t0 = juce::Time::getMillisecondCounterHiRes();
                        for (int i = 0; i < frames; ++i)
                        {
                            ore->setAnimationPhase (i * 0.1);
                            juce::Graphics g (img);
                            ore->paintEntireComponent (g, true);
                        }
                        const double perFrame = (juce::Time::getMillisecondCounterHiRes() - t0) / frames;
                        std::printf ("   отрисовка кадра 1280×800: %.2f мс, при 20 кадрах/с — %.0f%% ядра\n",
                                     perFrame, perFrame * 20.0 / 10.0);
                        check (perFrame < 16.6, "полный кадр укладывается в бюджет 20 кадров/с",
                               num (perFrame, 2) + " мс");

                        // но полный кадр — не то, что считается на каждый тик:
                        // перерисовываются только «живые» зоны
                        const int animated = ore->animatedAreaPercent();
                        std::printf ("   зон под анимацией: %d%% площади экрана\n", animated);
                        check (animated > 0 && animated < 60,
                               "перерисовывается только часть экрана",
                               juce::String (animated) + "%");

                        // 11.6 реальная цена тика: рисуем только «живые» зоны
                        {
                            juce::Image img (juce::Image::ARGB, 1280, 800, true);
                            const int frames2 = 20;
                            const double t1 = juce::Time::getMillisecondCounterHiRes();
                            for (int i = 0; i < frames2; ++i)
                            {
                                juce::Graphics g (img);
                                // объединение, а не пересечение: четыре отдельных
                                // reduceClipRegion дали бы пустую область
                                juce::RectangleList<int> dirty;
                                dirty.add (248, 348, 736, 62);
                                dirty.add (248, 570, 736, 70);
                                dirty.add (16, 66, 216, 62);
                                dirty.add (988, 24, 100, 16);
                                g.reduceClipRegion (dirty);
                                ore->setAnimationPhase (i * 0.1);
                                ore->paintEntireComponent (g, true);
                            }
                            const double perTick = (juce::Time::getMillisecondCounterHiRes() - t1) / frames2;
                            std::printf ("   цена тика (только живые зоны): %.2f мс, при 20 кадрах/с — %.0f%% ядра\n",
                                         perTick, perTick * 20.0 / 10.0);
                            check (perTick < perFrame, "тик дешевле полного кадра",
                                   num (perTick, 2) + " < " + num (perFrame, 2) + " мс");
                        }
                    }

                    // ---- 12. Движки: переключение В GUI обязано менять звук ----
                    {
                        // отдельная пара процессор+редактор: в GUI жмём селектор,
                        // из процессора читаем параметр и рендерим звук
                        OrogenProcessor q;
                        std::unique_ptr<juce::AudioProcessorEditor> ed2 (q.createEditor());
                        check (ed2 != nullptr, "редактор создаётся для проверки движков");
                        if (ed2 != nullptr)
                        {
                            auto* ore2 = static_cast<OrogenEditor*> (ed2.get());
                            ore2->setSize (1280, 800);

                            juce::ComboBox* eng = nullptr;
                            for (auto* child : ore2->getChildren())
                                if (auto* box = dynamic_cast<juce::ComboBox*> (child))
                                    if (box->getTooltip().startsWith ("Engine type")) eng = box;

                            check (eng != nullptr, "в центре есть селектор движка");
                            if (eng != nullptr)
                            {
                                std::printf ("   селектор движка: %d пунктов\n", eng->getNumItems());
                                check (eng->getNumItems() == 9, "в селекторе все 9 движков",
                                       juce::String (eng->getNumItems()) + " / 9");

                                // звук каждого движка, выбранного мышью в селекторе
                                juce::Array<juce::AudioBuffer<float>> audio;
                                juce::Array<double> rms;
                                for (int t = 0; t < 9; ++t)
                                {
                                    q.setPlayConfigDetails (0, 2, sr, 512);
                                    q.prepareToPlay (sr, 512);
                                    q.setCurrentProgram (0);
                                    soloSlot (q, 0, t);
                                    eng->setSelectedItemIndex (t, juce::sendNotificationSync);
                                    const int typeFromParam = (int) std::lround (
                                        q.apvts.getParameter ("sA_type")->convertFrom0to1 (
                                            q.apvts.getParameter ("sA_type")->getValue()));
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (80);
                                    auto buf = renderBlocks (q, 20000, sr);
                                    audio.add (buf);
                                    rms.add (rmsOf (buf));
                                    if (typeFromParam != t)
                                        std::printf ("   [внимание] селектор %d, параметр %d\n", t, typeFromParam);
                                }

                                // все 36 пар должны отличаться (как в блоке 7, но через GUI)
                                int pairs = 0, distinct = 0;
                                double minDelta = 1.0;
                                for (int i = 0; i < 9; ++i)
                                    for (int k = i + 1; k < 9; ++k)
                                    {
                                        ++pairs;
                                        const double d = deltaTo (audio[i], audio[k]);
                                        minDelta = std::min (minDelta, d);
                                        if (d > 0.0015) ++distinct;
                                    }
                                std::printf ("   через GUI: пар различимо %d / %d, минимальная Δ %.4f\n",
                                             distinct, pairs, minDelta);
                                for (int i = 0; i < 9; ++i)
                                    std::printf ("     %-9s RMS %.4f\n",
                                                 q.getProgramName (0).isEmpty() ? "" : "",
                                                 (double) rms[i]);
                                check (distinct == pairs,
                                       "переключение движка в селекторе слышно",
                                       juce::String (distinct) + " / " + juce::String (pairs));

                                // селектор привязан к выбранному слоту: слот B → sB_type
                                const auto vA = q.apvts.getParameter ("sA_type");
                                const int typeABefore = (int) std::lround (vA->convertFrom0to1 (vA->getValue()));

                                ore2->showPage (1);            // выбрать слот B
                                q.apvts.getParameter ("sB_type")->setValueNotifyingHost (
                                    q.apvts.getParameter ("sB_type")->convertTo0to1 (4.0f));
                                juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
                                const int boxOnB = eng->getSelectedItemIndex();
                                eng->setSelectedItemIndex (6, juce::sendNotificationSync);
                                const auto vB = q.apvts.getParameter ("sB_type");
                                const int typeB = (int) std::lround (vB->convertFrom0to1 (vB->getValue()));
                                const int typeA = (int) std::lround (vA->convertFrom0to1 (vA->getValue()));
                                std::printf ("   слот B: селектор показывает %d, после выбора sB_type=%d, "
                                             "sA_type не тронут (%d -> %d)\n",
                                             boxOnB, typeB, typeABefore, typeA);
                                check (boxOnB == 4 && typeB == 6 && typeA == typeABefore,
                                       "селектор переключает движок именно выбранного слота",
                                       juce::String (typeB));

                                // и сам интерфейс обязан перерисоваться: имя, иконка в рейле,
                                // форма волны на дисплее
                                auto shot = [ore2]
                                {
                                    ore2->rebuildPreview (true);
                                    if (! ore2->waitForPreview (10000)) ++failures;
                                    juce::Image img (juce::Image::ARGB, 1280, 800, true);
                                    juce::Graphics g (img);
                                    ore2->setAnimationPhase (0.0);
                                    ore2->paintEntireComponent (g, true);
                                    return img;
                                };

                                ore2->showPage (0);                   // слот A
                                q.apvts.getParameter ("sA_type")->setValueNotifyingHost (
                                    q.apvts.getParameter ("sA_type")->convertTo0to1 (0.0f));
                                juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
                                const auto uiWave = shot();
                                q.apvts.getParameter ("sA_type")->setValueNotifyingHost (
                                    q.apvts.getParameter ("sA_type")->convertTo0to1 (4.0f));
                                juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
                                const auto uiGran = shot();

                                int uiDiff = 0;
                                juce::Image::BitmapData d1 (uiWave, juce::Image::BitmapData::readOnly);
                                juce::Image::BitmapData d2 (uiGran, juce::Image::BitmapData::readOnly);
                                for (int y = 0; y < 800; ++y)
                                    for (int x = 0; x < 1280; ++x)
                                        if (d1.getPixelColour (x, y) != d2.getPixelColour (x, y)) ++uiDiff;
                                std::printf ("   интерфейс при смене движка Wavetable -> Granular: %d пикселей\n",
                                             uiDiff);
                                check (uiDiff > 2000, "смена движка видна в интерфейсе",
                                       juce::String (uiDiff) + " пикселей");

                                // ---- 13. Фильтр слота виден в превью дисплея ----
                                {
                                    auto shotWith = [ore2, &q] (float cutoff, int model)
                                    {
                                        if (auto* prm = q.apvts.getParameter ("sA_cutoff"))
                                            prm->setValueNotifyingHost (prm->convertTo0to1 (cutoff));
                                        if (auto* prm = q.apvts.getParameter ("sA_filter"))
                                            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) model));
                                        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
                                        ore2->rebuildPreview (true);
                                        if (! ore2->waitForPreview (10000)) ++failures;
                                        ore2->setAnimationPhase (0.0);
                                        juce::Image img (juce::Image::ARGB, 1280, 800, true);
                                        juce::Graphics g (img);
                                        ore2->paintEntireComponent (g, true);
                                        return img;
                                    };

                                    auto diff = [] (const juce::Image& a, const juce::Image& b)
                                    {
                                        int n = 0;
                                        juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly);
                                        juce::Image::BitmapData db (b, juce::Image::BitmapData::readOnly);
                                        for (int y = 0; y < a.getHeight(); ++y)
                                            for (int x = 0; x < a.getWidth(); ++x)
                                                if (da.getPixelColour (x, y) != db.getPixelColour (x, y)) ++n;
                                        return n;
                                    };

                                    // один и тот же движок, разные модели фильтра при низкой отсечке
                                    const auto lp = shotWith (0.30f, 0);    // LP12
                                    const auto hp = shotWith (0.30f, 3);    // HP24
                                    const auto notch = shotWith (0.30f, 4); // Notch12
                                    const int dLpHp = diff (lp, hp);
                                    const int dLpNotch = diff (lp, notch);
                                    std::printf ("   превью: LP12 против HP24 — %d пикселей, против Notch12 — %d\n",
                                                 dLpHp, dLpNotch);
                                    check (dLpHp > 2000 && dLpNotch > 2000,
                                           "модель фильтра видна в превью дисплея",
                                           juce::String (dLpHp) + " / " + juce::String (dLpNotch));

                                    // отсечка: открытый фильтр и закрытый должны отличаться
                                    const auto open = shotWith (0.95f, 0);
                                    const auto shut = shotWith (0.15f, 0);
                                    const int dCut = diff (open, shut);
                                    std::printf ("   превью: cutoff 0.95 против 0.15 — %d пикселей\n", dCut);
                                    check (dCut > 2000, "отсечка видна в превью дисплея",
                                           juce::String (dCut) + " пикселей");

                                    // возвращаем нейтральные настройки
                                    if (auto* prm = q.apvts.getParameter ("sA_cutoff"))
                                        prm->setValueNotifyingHost (1.0f);
                                }

                                // ---- 14. Режим патча в превью, плавное листание, hover ----
                                {
                                    // 14.1 Morph: превью показывает смесь слотов, а не один слот
                                    auto shotNow = [ore2]
                                    {
                                        ore2->rebuildPreview (true);
                                        if (! ore2->waitForPreview (10000)) ++failures;
                                        ore2->setAnimationPhase (0.0);
                                        juce::Image img (juce::Image::ARGB, 1280, 800, true);
                                        juce::Graphics g (img);
                                        ore2->paintEntireComponent (g, true);
                                        return img;
                                    };
                                    auto diff2 = [] (const juce::Image& a, const juce::Image& b)
                                    {
                                        int n = 0;
                                        juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly);
                                        juce::Image::BitmapData db (b, juce::Image::BitmapData::readOnly);
                                        for (int y = 0; y < a.getHeight(); ++y)
                                            for (int x = 0; x < a.getWidth(); ++x)
                                                if (da.getPixelColour (x, y) != db.getPixelColour (x, y)) ++n;
                                        return n;
                                    };

                                    ore2->showPage (0);
                                    setP (q, "sA_on", 1.0f);
                                    setP (q, "sB_on", 1.0f);
                                    setP (q, "sB_type", 5.0f);       // Modal во втором слоте
                                    setP (q, "sA_cutoff", 1.0f);
                                    setP (q, "gl_mode", 0.0f);       // Stack
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
                                    const auto stackShot = shotNow();

                                    setP (q, "gl_mode", 1.0f);       // Morph
                                    setP (q, "gl_morph", 0.071f);
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
                                    const auto morphShot = shotNow();

                                    const int dMode = diff2 (stackShot, morphShot);
                                    std::printf ("   превью: Stack против Morph (A/B 50%%) — %d пикселей\n", dMode);
                                    check (dMode > 2000, "режим Morph виден в превью дисплея",
                                           juce::String (dMode) + " пикселей");

                                    setP (q, "gl_mode", 0.0f);
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);

                                    // 14.2 плавное листание: сдвиг появляется и уходит
                                    ore2->showPage (8);              // FX CHAIN
                                    ore2->nextColumnPage (0);
                                    const int shiftStart = std::abs (ore2->columnPageShift (0));
                                    const bool movingStart = ore2->columnTransitioning (0);
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (800);
                                    const int shiftEnd = std::abs (ore2->columnPageShift (0));
                                    const bool movingEnd = ore2->columnTransitioning (0);
                                    std::printf ("   листание FX: сдвиг %d px -> %d px, переход %s -> %s\n",
                                                 shiftStart, shiftEnd,
                                                 movingStart ? "идёт" : "нет", movingEnd ? "идёт" : "нет");
                                    check (shiftStart > 0 && shiftEnd == 0 && ! movingEnd,
                                           "страницы колонки листаются плавно и доезжают",
                                           juce::String (shiftStart) + " -> 0");

                                    // 14.3 hover: значение плавно 0 -> 1 и обратно
                                    ore2->hoverKnob (0, true);
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
                                    const double hOn = ore2->knobHover (0);
                                    ore2->hoverKnob (0, false);
                                    juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
                                    const double hOff = ore2->knobHover (0);
                                    std::printf ("   hover ручки: %.2f (наведение) -> %.2f (ушли)\n", hOn, hOff);
                                    check (hOn > 0.99 && hOff < 0.01, "hover-подсветка плавно включается и гаснет",
                                           num (hOn, 2) + " / " + num (hOff, 2));
                                }
                            }
                        }
                    }
                }

            }
        }
    }

    testGuiRegressions();
    testNativeMeterPaint();
    testEditorControlLifetime();
    testGranularProcessorRegression();
    testGranularReverseProcessor();
    testGranularPitchProcessorRegression();
    testDelayProcessorRegression();
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("\n=================================================\n");
    if (failures == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else               std::printf ("ПРОВАЛОВ: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
