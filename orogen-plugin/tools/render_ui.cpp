// Отрисовка редактора в PNG без дисплея: нужен для документации и для проверки
// раскладки на машинах без X11/Wayland (в песочнице их нет).
//
//   render_ui <out.png> [view] [width] [height] [phase] [program]
//             [cutoff] [filter] [morph] [mode] [scene] [elapsed-ms]
// scene: fx-before | fx-change | fx-hover | hover | lfo-wrap | granular |
//        anim-frames | transition-frames | lfo-wrap-frames (необязательно).
// Для *-frames <out> — каталог из 40 PNG; GIF собирается отдельно.
//
// view: 0…7 = слоты A–H, 8…11 = секции патч-колонки (FX/MATRIX/ARP/LFO),
//       100…104 = оверлей параметров, вкладки SLOT/FX/MOD/ARP-LFO/GLOBAL
#include "../src/plugin/PluginProcessor.h"
#include "../src/plugin/PluginEditor.h"

#include <cstdio>
#include <csignal>
#if ! defined (_WIN32)
#include <execinfo.h>
#endif

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

using namespace orogen;

int main (int argc, char** argv)
{
    std::signal (SIGSEGV, crashTrace);
    std::signal (SIGABRT, crashTrace);
    auto* mm = juce::MessageManager::getInstance();
    juce::ignoreUnused (mm);

    const juce::File out (argc > 1 ? argv[1] : "ui.png");
    const int page = argc > 2 ? std::atoi (argv[2]) : 0;
    const int w = argc > 3 ? std::atoi (argv[3]) : 1280;
    const int h = argc > 4 ? std::atoi (argv[4]) : 800;
    // фаза анимации в секундах: один и тот же кадр воспроизводим побитово
    const double phase = argc > 5 ? std::atof (argv[5]) : 0.0;
    const int program = argc > 6 ? std::atoi (argv[6]) : 1;
    const float cutoff = argc > 7 ? (float) std::atof (argv[7]) : -1.0f;   // <0 — как в пресете
    const int filterModel = argc > 8 ? std::atoi (argv[8]) : -1;
    const float morph = argc > 9 ? (float) std::atof (argv[9]) : -1.0f;
    const int patchMode = argc > 10 ? std::atoi (argv[10]) : -1;
    const juce::String scene = argc > 11 ? argv[11] : "";
    const int elapsedMs = argc > 12 ? juce::jmax (0, std::atoi (argv[12])) : 0;

    OrogenProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    proc.setCurrentProgram (program);                   // 1 = Terra Pad

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    if (editor == nullptr) { std::printf ("нет редактора\n"); return 1; }

    editor->setSize (w, h);

    // переключить страницу — через клавишу, как это делает пользователь
    // 0…11 — слоты/секции колонки, 100…104 — вкладки оверлея,
    // 200+t*100+s — конкретная секция оверлея (страницы по секциям)
    const int idx = (page >= 200) ? page
                  : (page >= 100) ? juce::jlimit (100, 104, page)
                                  : juce::jlimit (0, 11, page);
    static_cast<OrogenEditor*> (editor.get())->showPage (idx);
    if (cutoff >= 0.0f)
        if (auto* prm = proc.apvts.getParameter ("sA_cutoff"))
            prm->setValueNotifyingHost (prm->convertTo0to1 (cutoff));
    if (filterModel >= 0)
        if (auto* prm = proc.apvts.getParameter ("sA_filter"))
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) filterModel));

    if (morph >= 0.0f)
        if (auto* prm = proc.apvts.getParameter ("gl_morph"))
            prm->setValueNotifyingHost (prm->convertTo0to1 (morph));
    if (patchMode >= 0)
        if (auto* prm = proc.apvts.getParameter ("gl_mode"))
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) patchMode));

    auto* ore = static_cast<OrogenEditor*> (editor.get());
    if (scene == "granular")
    {
        const auto set = [&proc] (const char* id, float value) {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (value));
        };
        set ("sA_on", 1.0f);
        set ("sA_type", 4.0f);
        set ("sA_table", 0.0f);
        set ("sA_grainReverse", 0.5f);
    }
    // Статические скриншоты оверлея должны показывать финальное состояние,
    // а не первый кадр перехода при открытии вкладки.
    if (page >= 100) { ore->setAnimationsEnabled (false); ore->setAnimationsEnabled (true); }
    ore->setAnimationPhase (phase);
    ore->rebuildPreview (true);
    if (! ore->waitForPreview (30000))
    {
        std::fprintf (stderr, "превью не готово за 30 секунд\n");
        return 2;
    }

    // Серии кадров: один редактор, одна реальная очередь сообщений и
    // тот же таймер, что в плагине. Для периодического кадра фазу задаём
    // явно; для перехода и LFO сохраняем фактический ход времени таймера.
    if (scene == "anim-frames" || scene == "transition-frames" || scene == "lfo-wrap-frames")
    {
        if (out.createDirectory().failed()) { std::fprintf (stderr, "не удалось создать каталог кадров\n"); return 3; }
        ore->setAnimationsEnabled (true);
        if (scene == "transition-frames")
        {
            ore->showPage (8);
            mm->runDispatchLoopUntil (220);
        }
        if (scene == "lfo-wrap-frames")
        {
            const auto set = [&proc] (const char* id, float value) {
                if (auto* p = proc.apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (value));
            };
            ore->setAnimationsEnabled (false);
            set ("lfo1_shape", 2.0f); set ("lfo1_amount", 0.8f); set ("lfo1_rate", 5.0f);
            ore->setAnimationPhase (0.792);
            set ("lfo1_rate", 0.05f);
            ore->setAnimationsEnabled (true);
        }
        for (int i = 0; i < 40; ++i)
        {
            if (scene == "anim-frames") ore->setAnimationPhase (0.05 * (double) i);
            else
            {
                if (scene == "transition-frames" && i == 4) ore->nextColumnPage (0);
                if (scene == "transition-frames" && i == 14) ore->hoverKnob (0, true);
                if (scene == "transition-frames" && i == 29) ore->hoverKnob (0, false);
                mm->runDispatchLoopUntil (50);
            }
            juce::Image frame (juce::Image::ARGB, w, h, true);
            { juce::Graphics g (frame); editor->paintEntireComponent (g, true); }
            const auto file = out.getChildFile ("frame" + juce::String (i).paddedLeft ('0', 3) + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;
            if (! stream.openedOk() || ! png.writeImageToStream (frame, stream))
            { std::fprintf (stderr, "не удалось записать кадр %d\n", i); return 4; }
        }
        std::printf ("записано 40 кадров: %s\n", out.getFullPathName().toRawUTF8());
        editor.reset();
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        return 0;
    }

    // Необязательные сцены для GIF: используем тот же таймер редактора,
    // что и в реальном окне, и фиксируем заданный момент перехода.
    if (scene.isNotEmpty())
    {
        if (scene.startsWith ("fx-")) ore->showPage (8);
        if (scene == "lfo-wrap")
        {
            // Реальный таймер редактора: масштаб 4→1, затем playhead
            // пересекает правый край внутри ступени S&H. elapsedMs задаёт
            // кадр анимации; состояние/фаза не подменяются после таймера.
            const auto set = [&proc] (const char* id, float value) {
                if (auto* p = proc.apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (value));
            };
            ore->setAnimationsEnabled (false);
            set ("lfo1_shape", 2.0f); set ("lfo1_amount", 0.8f); set ("lfo1_rate", 5.0f);
            ore->setAnimationPhase (0.792);
            set ("lfo1_rate", 0.05f);
            ore->setAnimationsEnabled (true);
        }
        mm->runDispatchLoopUntil (65);       // установить время первого тика
        if (scene == "fx-change" || scene == "fx-hover")
        {
            ore->nextColumnPage (0);
            if (scene == "fx-hover") mm->runDispatchLoopUntil (220);
        }
        if (scene == "hover" || scene == "fx-hover") ore->hoverKnob (0, true);
        if (elapsedMs > 0) mm->runDispatchLoopUntil (elapsedMs);
        if (scene != "lfo-wrap") ore->setAnimationPhase (phase);
    }

    // заставляем редактор пересчитать раскладку и отрисовать себя в картинку
    editor->resized();
    std::printf ("страница после переключения: %d, фаза %.3f\n",
                 static_cast<OrogenEditor*> (editor.get())->visiblePage(), phase);

    juce::Image image (juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g (image);
        editor->paintEntireComponent (g, true);
    }

    out.deleteFile();
    juce::FileOutputStream stream (out);
    if (stream.openedOk())
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (image, stream);
    }
    std::printf ("записано: %s (%d×%d, страница %d)\n", out.getFullPathName().toRawUTF8(), w, h, idx);

    editor.reset();
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return 0;
}
