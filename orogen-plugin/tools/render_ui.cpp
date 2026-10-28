// Отрисовка редактора в PNG без дисплея: нужен для документации и для проверки
// раскладки на машинах без X11/Wayland (в песочнице их нет).
//
//   render_ui <out.png> [view] [width] [height]
//
// view: 0…7 = слоты A–H, 8…11 = секции патч-колонки (FX/MATRIX/ARP/LFO),
//       100…104 = оверлей параметров, вкладки SLOT/FX/MOD/ARP-LFO/GLOBAL
#include "../src/plugin/PluginProcessor.h"
#include "../src/plugin/PluginEditor.h"

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
    static_cast<OrogenEditor*> (editor.get())->setAnimationPhase (phase);

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

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return 0;
}
