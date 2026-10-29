// Отрисовка редактора в PNG без дисплея: нужен для документации и для проверки
// раскладки на машинах без X11/Wayland (в песочнице их нет).
//
//   render_ui <out.png> [view] [width] [height] [phase] [program]
//             [cutoff] [filter] [morph] [mode] [scene] [elapsed-ms]
// scene: fx-before | fx-change | fx-hover | hover (необязательно)
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

    // Необязательные сцены для GIF: используем тот же таймер редактора,
    // что и в реальном окне, и фиксируем заданный момент перехода.
    if (scene.isNotEmpty())
    {
        if (scene.startsWith ("fx-")) ore->showPage (8);
        mm->runDispatchLoopUntil (65);       // установить время первого тика
        if (scene == "fx-change" || scene == "fx-hover")
        {
            ore->nextColumnPage (0);
            if (scene == "fx-hover") mm->runDispatchLoopUntil (220);
        }
        if (scene == "hover" || scene == "fx-hover") ore->hoverKnob (0, true);
        if (elapsedMs > 0) mm->runDispatchLoopUntil (elapsedMs);
        ore->setAnimationPhase (phase);
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
