// Test-only synthetic external Xdnd file source. Include at the very end of a
// test translation unit: Xlib defines global macros such as KeyPress.
#if JUCE_LINUX
#include <X11/Xlib.h>
#include <X11/Xatom.h>

bool nativeFileDrop (juce::Component& editor, juce::Point<int> localPoint,
                            const juce::File& file, const std::function<bool()>& hovering,
                            bool& sawHover, int& selections)
{
    juce::DynamicLibrary library;
    if (! library.open ("libX11.so.6")) return false;
    #define ORO_XFN(n) auto fn##n = reinterpret_cast<decltype (&n)> (library.getFunction (#n))
    ORO_XFN (XOpenDisplay); ORO_XFN (XCloseDisplay); ORO_XFN (XDefaultRootWindow);
    ORO_XFN (XInternAtom); ORO_XFN (XCreateSimpleWindow); ORO_XFN (XDestroyWindow);
    ORO_XFN (XSetSelectionOwner); ORO_XFN (XGetSelectionOwner); ORO_XFN (XWarpPointer);
    ORO_XFN (XQueryPointer); ORO_XFN (XSendEvent); ORO_XFN (XPending); ORO_XFN (XNextEvent);
    ORO_XFN (XChangeProperty); ORO_XFN (XSelectInput); ORO_XFN (XFlush); ORO_XFN (XSync);
    #undef ORO_XFN
    if (! fnXOpenDisplay || ! fnXCloseDisplay || ! fnXDefaultRootWindow || ! fnXInternAtom
        || ! fnXCreateSimpleWindow || ! fnXDestroyWindow || ! fnXSetSelectionOwner || ! fnXGetSelectionOwner
        || ! fnXWarpPointer || ! fnXQueryPointer || ! fnXSendEvent || ! fnXPending || ! fnXNextEvent
        || ! fnXChangeProperty || ! fnXSelectInput || ! fnXFlush || ! fnXSync) return false;
    auto* display = fnXOpenDisplay (nullptr);
    if (! display) return false;
    const auto root = fnXDefaultRootWindow (display);
    const auto atom = [&] (const char* name) { return fnXInternAtom (display, name, False); };
    const Atom selection = atom ("XdndSelection"), mime = atom ("text/uri-list");
    const Atom enter = atom ("XdndEnter"), position = atom ("XdndPosition"), drop = atom ("XdndDrop");
    const Atom finished = atom ("XdndFinished"), action = atom ("XdndActionCopy");
    const Window source = fnXCreateSimpleWindow (display, root, 0, 0, 1, 1, 0, 0, 0);
    // Obtain an actual X server event timestamp, as a real file manager does.
    // A strict selection owner must reject requests for versions from before
    // ownership or from the future rather than accepting bogus timestamps.
    fnXSelectInput (display, source, PropertyChangeMask);
    const auto marker = atom ("OROGEN_XDND_CLOCK");
    const unsigned char token = 1;
    fnXChangeProperty (display, source, marker, XA_STRING, 8, PropModeReplace, &token, 1);
    fnXFlush (display);
    XEvent clockEvent {};
    do { fnXNextEvent (display, &clockEvent); } while (clockEvent.type != PropertyNotify);
    const auto ownerTime = (std::uint32_t) clockEvent.xproperty.time;
    const auto localTime = juce::Time::getMillisecondCounter();
    const auto serverTime = [&] { return (std::uint32_t) (ownerTime + (juce::Time::getMillisecondCounter() - localTime)); };
    const bool strict = std::getenv ("OROGEN_XDND_STRICT") != nullptr;
    fnXSetSelectionOwner (display, selection, source, ownerTime);
    fnXSync (display, False);
    if (fnXGetSelectionOwner (display, selection) != source)
    { fnXDestroyWindow (display, source); fnXCloseDisplay (display); return false; }
    auto* mm = juce::MessageManager::getInstance();
    const auto screenPoint = editor.localPointToGlobal (localPoint);
    fnXWarpPointer (display, None, root, 0, 0, 0, 0, screenPoint.x, screenPoint.y);
    fnXSync (display, False); mm->runDispatchLoopUntil (80);
    // Descend to the innermost native child. A hosted VST3 has its own XEmbed
    // child under the JUCE test host's editor peer.
    Window target = root;
    for (int i = 0; i < 16; ++i)
    {
        Window outRoot = 0, child = 0; int rx = 0, ry = 0, wx = 0, wy = 0; unsigned int mask = 0;
        if (! fnXQueryPointer (display, target, &outRoot, &child, &rx, &ry, &wx, &wy, &mask) || child == 0) break;
        target = child;
    }
    if (std::getenv ("OROGEN_XDND_DEBUG"))
    {
        std::printf ("   Xdnd point %d,%d target=0x%lx root=0x%lx peer=0x%lx editor=%d,%d %dx%d\n",
                     screenPoint.x, screenPoint.y, target, root,
                     editor.getPeer() ? (unsigned long) editor.getPeer()->getNativeHandle() : 0ul,
                     editor.getScreenX(), editor.getScreenY(), editor.getWidth(), editor.getHeight());
        Window w = target;
        while (w != None && w != root)
        {
            Atom actual = None; int format = 0; unsigned long count = 0, left = 0; unsigned char* data = nullptr;
            XGetWindowProperty (display, w, atom ("XdndAware"), 0, 1, False, AnyPropertyType, &actual, &format, &count, &left, &data);
            std::printf ("   window 0x%lx XdndAware type=%lu format=%d count=%lu value=%lu\n", w, actual, format, count,
                         data && count ? *(unsigned long*) data : 0ul);
            if (data) XFree (data);
            Window rr = None, parent = None, *children = nullptr; unsigned int n = 0;
            if (! XQueryTree (display, w, &rr, &parent, &children, &n)) break;
            if (children) XFree (children);
            w = parent;
        }
        std::fflush (stdout);
    }
    if (target == root)
    { fnXDestroyWindow (display, source); fnXCloseDisplay (display); return false; }
    // A real file manager sends a percent-escaped URI, not a raw path. This
    // also covers spaces, percent signs and non-ASCII filename components.
    const auto uriString = juce::URL (file).toString (false) + "\r\n";
    const auto uri = uriString.toUTF8();
    const auto message = [&] (Atom type, long v1, long v2, long v3, long v4)
    {
        XEvent e {};
        e.xclient.type = ClientMessage; e.xclient.display = display; e.xclient.window = target;
        e.xclient.message_type = type; e.xclient.format = 32;
        e.xclient.data.l[0] = (long) source; e.xclient.data.l[1] = v1;
        e.xclient.data.l[2] = v2; e.xclient.data.l[3] = v3; e.xclient.data.l[4] = v4;
        fnXSendEvent (display, target, False, NoEventMask, &e); fnXFlush (display);
    };
    bool complete = false;
    const auto pump = [&] (int milliseconds)
    {
        const double end = juce::Time::getMillisecondCounterHiRes() + milliseconds;
        do
        {
            mm->runDispatchLoopUntil (20);
            while (fnXPending (display) > 0)
            {
                XEvent event {}; fnXNextEvent (display, &event);
                if (event.type == SelectionRequest)
                {
                    ++selections;
                    const auto& request = event.xselectionrequest;
                    const auto requested = (std::uint32_t) request.time;
                    const bool validTime = requested == CurrentTime
                        || ((std::int32_t) (requested - ownerTime) >= 0 && (std::int32_t) (requested - serverTime()) <= 0);
                    const Atom property = (! strict || validTime)
                        ? (request.property != None ? request.property : request.target) : None;
                    if (std::getenv ("OROGEN_XDND_DEBUG"))
                        std::printf ("   selection timestamp=%u owner=%u now=%u accepted=%d\n",
                                     requested, ownerTime, serverTime(), (int) (property != None));
                    if (property != None)
                        fnXChangeProperty (display, request.requestor, property, request.target, 8, PropModeReplace,
                                           reinterpret_cast<const unsigned char*> (uri.getAddress()),
                                           (int) std::strlen (uri.getAddress()));
                    XEvent reply {};
                    reply.xselection.type = SelectionNotify;
                    reply.xselection.display = display;
                    reply.xselection.requestor = request.requestor;
                    reply.xselection.selection = request.selection;
                    reply.xselection.target = request.target;
                    reply.xselection.property = property;
                    reply.xselection.time = request.time;
                    fnXSendEvent (display, request.requestor, False, NoEventMask, &reply);
                    fnXFlush (display);
                }
                else if (event.type == ClientMessage && event.xclient.message_type == finished)
                    complete = true;
            }
            if (hovering && hovering()) sawHover = true;
        } while (juce::Time::getMillisecondCounterHiRes() < end);
    };
    // JUCE advertises Xdnd version 3; a conforming source must negotiate
    // down to the target's version rather than sending its own maximum (5).
    message (enter, 3L << 24, (long) mime, 0, 0);
    const auto xy = [] (int x, int y) { return (long) (((unsigned long) x << 16) | ((unsigned long) y & 0xffffu)); };
    message (position, 0, xy (screenPoint.x, screenPoint.y), serverTime(), (long) action);
    pump (220);
    // A changed position makes the receiver update its target after selection.
    message (position, 0, xy (screenPoint.x + 1, screenPoint.y), serverTime(), (long) action);
    pump (250);
    message (drop, 0, serverTime(), 0, 0);
    for (int i = 0; i < 15 && ! complete; ++i) pump (100);
    pump (80);
    fnXDestroyWindow (display, source); fnXCloseDisplay (display);
    return complete;
}
#else
static bool nativeFileDrop (juce::Component&, juce::Point<int>, const juce::File&,
                            const std::function<bool()>&, bool&, int&) { return false; }
#endif
