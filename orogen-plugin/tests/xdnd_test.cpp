// Standalone Xlib translation unit, kept separate from code using namespace juce
// so X11's Font, Time, Drawable and macros cannot collide with JUCE classes.
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <cstdio>
#include <cstdlib>
#if JUCE_LINUX
bool nativeFileDrop (juce::Component&, juce::Point<int>, const juce::File&,
                     const std::function<bool()>&, bool&, int&);
#include "xdnd_test_impl.h"
#endif
