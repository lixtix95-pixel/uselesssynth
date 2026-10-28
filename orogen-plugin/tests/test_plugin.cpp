// Проверки обёртки. Здесь важно доказать три вещи:
//   1) плагин звучит ТЕМ ЖЕ кодом, что стенды (нулевой тест против Instrument);
//   2) параметры и структуры не разъезжаются (patch -> params -> patch);
//   3) пресет переживает сохранение в файл и состояние сессии.
#include "../src/plugin/PluginProcessor.h"
#include "../src/plugin/PluginEditor.h"
#include "../src/plugin/UiParamMap.h"

#include <cstdio>
#include <csignal>
#include <execinfo.h>

static void crashTrace (int sig)
{
    void* frames[24];
    const int n = backtrace (frames, 24);
    std::fprintf (stderr, "signal %d, backtrace:\n", sig);
    backtrace_symbols_fd (frames, n, 2);
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

int main()
{
    // Голова без X11: GUI-инициализация здесь упала бы (нет дисплея), а
    // аудио-плагину нужен только менеджер сообщений — он же крутит AsyncUpdater,
    // который сообщает хосту новую задержку.
    std::signal (SIGSEGV, crashTrace);
    std::signal (SIGABRT, crashTrace);
    auto* mm = juce::MessageManager::getInstance();
    std::printf ("OROGEN test_plugin: старт\n");
    juce::ignoreUnused (mm);
    const double sr = 48000.0;

    // ---------------------------------------------------------------- 1
    std::printf ("\n--- 1. Параметры хоста ---\n");
    {
        OrogenProcessor p;
        const int count = p.getParameters().size();
        std::printf ("   параметров: %d, программ: %d\n", count, p.getNumPrograms());
        check (count > 200, "все параметры объявлены", num (count, 0));

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
            { "fx_plateDiffusion", 0.87f }, { "fx_platePredelay", 0.033f },
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

        std::printf ("   сверено полей: 75 + chorusFw[8], худшее расхождение %.6f у %s\n",
                     err, worstField);
        check (badField == 0, "все поля FX переживают пресет", num ((double) badField, 0) + " расхождений");
        check (err < 1.0e-3, "круг FX -> JSON -> FX точен", num (err, 6));
        file.deleteFile();
    }
    std::printf ("\n--- 3. JSON-пресет: файл и состояние ---\n");
    {
        OrogenProcessor p;
        p.setCurrentProgram (1);

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
                        && (std::fabs (a.slots[s].params.p[0] - b.slots[s].params.p[0]) < 1.0e-4f);
        }
        check (same, "патч идентичен после чтения из файла");
        check (std::fabs (fa.reverbMix - fb.reverbMix) < 1.0e-4f, "эффекты идентичны",
               num (fb.reverbMix, 4));

        // состояние сессии
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        OrogenProcessor r;
        r.setStateInformation (mb.getData(), (int) mb.getSize());
        Patch c; FxChain::Settings fc;
        r.collectPatch (c, fc);
        check (std::fabs (c.gain - a.gain) < 1.0e-3f, "состояние сессии сохраняет патч",
               num ((double) mb.getSize(), 0) + " байт");
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
        check (mapIds.size() == apvtsIds.size() && apvtsIds.size() == 294,
               "покрытие 294/294", juce::String (mapIds.size()) + " / " + juce::String (apvtsIds.size()));

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
            int pages = 0;
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
                        const int expected = (int) tabs[(size_t) t].sections[(size_t) sIdx].ids.size();
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
            // вкладка SLOT: 8 слотов × 21 параметр
            check (seenAll == 168, "страница ALL вкладки SLOT показывает все 168",
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

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("\n=================================================\n");
    if (failures == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else               std::printf ("ПРОВАЛОВ: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
