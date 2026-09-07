/* Clipboard manager test: capture on selection change, history ordering,
 * duplicate suppression, re-selection and re-serving after the original
 * owner exits. */
#include "rill_platform.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
check(const char *name, int ok, int *failures)
{
    if(!ok) {
        fprintf(stderr, "rill clipboard test failed: %s\n", name);
        (*failures)++;
    }
}

static Atom utf8_atom_for(Display *display)
{
    return XInternAtom(display, "UTF8_STRING", False);
}

/* Act like a real selection owner: answer pending SelectionRequests from
   the RILL_TEST_VALUE property this test publishes. */
static void
pump(Display *display)
{
    XSync(display, False);
    usleep(150000);
    XSync(display, False);
    while(XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if(event.type != SelectionRequest)
            continue;
        XEvent reply;
        Atom property = event.xselectionrequest.property;
        memset(&reply, 0, sizeof(reply));
        reply.xselection.type = SelectionNotify;
        reply.xselection.requestor = event.xselectionrequest.requestor;
        reply.xselection.selection = event.xselectionrequest.selection;
        reply.xselection.target = event.xselectionrequest.target;
        reply.xselection.time = event.xselectionrequest.time;
        reply.xselection.property = property;
        {
            Atom type;
            int format;
            unsigned long items, remaining;
            unsigned char *data = NULL;
            if(XGetWindowProperty(display, event.xselectionrequest.owner,
                                  XInternAtom(display, "RILL_TEST_VALUE",
                                              False),
                                  0, 4096, False, AnyPropertyType, &type,
                                  &format, &items, &remaining, &data) ==
                   Success &&
               data != NULL && format == 8) {
                XChangeProperty(display, event.xselectionrequest.requestor,
                                property, utf8_atom_for(display), 8,
                                PropModeReplace, data, (int)items);
            } else
                reply.xselection.property = None;
            if(data != NULL)
                XFree(data);
        }
        XSendEvent(display, event.xselectionrequest.requestor, False,
                   NoEventMask, &reply);
    }
    XFlush(display);
}

/* Capture is asynchronous: the platform converts, the owner answers on the
   next pump, the store lands on the poll after that. */
static int
poll_history(const RillPlatformServices *platform, Display *display,
             const char **texts, int cap, const char *expect)
{
    for(int i = 0; i < 20; i++) {
        int count = platform->clipboard_history(texts, cap);
        if(count > 0 && expect != NULL && strstr(texts[0], expect) != NULL)
            return count;
        pump(display);
    }
    return platform->clipboard_history(texts, cap);
}

/* A minimal selection owner serving UTF8_STRING from a window property. */
static Window
own_clipboard(Display *display, const char *text)
{
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                        0, 0, 10, 10, 0, 0, 0);
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    XSetSelectionOwner(display, clipboard, window, CurrentTime);
    XChangeProperty(display, window, XInternAtom(display, "RILL_TEST_VALUE",
                                                  False),
                    utf8, 8, PropModeReplace, (const unsigned char *)text,
                    (int)strlen(text));
    XFlush(display);
    return window;
}

int
main(int argc, char **argv)
{
    Display *display;
    const RillPlatformServices *platform = RillPlatformCurrent();
    const char *history[8];
    int failures = 0;
    int count;
    Window first, second;

    (void)argc;
    (void)argv;
    display = XOpenDisplay(NULL);
    if(display == NULL) {
        puts("rill clipboard test skipped: no display");
        return 0;
    }
    check("platform exposes clipboard history",
          platform->clipboard_history != NULL &&
          platform->clipboard_select != NULL, &failures);

    /* Selection-owner notifications only cover future changes, so the
       watcher must be running before anything owns a selection. A real
       call is required: a NULL poll returns before starting the watcher. */
    {
        const char *warm[1];
        platform->clipboard_history(warm, 1);
    }
    pump(display);

    first = own_clipboard(display, "first copy");
    count = poll_history(platform, display, history, 8, "first copy");
    check("first copy captured", count >= 1 &&
          strncmp(history[0], "first copy", 10) == 0, &failures);

    second = own_clipboard(display, "second copy");
    count = poll_history(platform, display, history, 8, "second copy");
    check("second copy captured newest-first", count >= 2 &&
          strncmp(history[0], "second copy", 11) == 0 &&
          strncmp(history[1], "first copy", 10) == 0, &failures);

    own_clipboard(display, "second copy");
    /* Drain the duplicate's in-flight conversion: its store would otherwise
       land after the reselect below and re-promote the duplicate. */
    count = poll_history(platform, display, history, 8, "second copy");
    check("duplicates suppressed", count == 2, &failures);

    /* Re-select the older entry: it must move to the front and become the
       served CLIPBOARD. */
    check("reselect older entry", platform->clipboard_select(1), &failures);
    pump(display);
    pump(display);
    count = platform->clipboard_history(history, 8);
    check("reselected entry moves first", count >= 2 &&
          strncmp(history[0], "first copy", 10) == 0, &failures);

    /* Kill every original owner; the platform must still serve requests. */
    XDestroyWindow(display, first);
    XDestroyWindow(display, second);
    pump(display);
    {
        Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
        Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
        Window requestor = XCreateSimpleWindow(
            display, DefaultRootWindow(display), 0, 0, 10, 10, 0, 0, 0);
        XConvertSelection(display, clipboard, utf8, utf8, requestor,
                          CurrentTime);
        pump(display);
        /* The platform serves the request during its own pump; a history
           poll drives it. */
        platform->clipboard_history(NULL, 0);
        {
            const char *drive[1];
            platform->clipboard_history(drive, 1);
        }
        {
            Atom type;
            int format;
            unsigned long items, remaining;
            unsigned char *data = NULL;
            int got = XGetWindowProperty(display, requestor, utf8, 0, 1024,
                                         True, AnyPropertyType, &type, &format,
                                         &items, &remaining, &data) == Success &&
                      data != NULL && items >= 10 &&
                      memcmp(data, "first copy", 10) == 0;
            check("selection survives owner exit", got, &failures);
            if(data != NULL)
                XFree(data);
        }
        XDestroyWindow(display, requestor);
    }

    XCloseDisplay(display);
    if(failures != 0)
        return 1;
    puts("Rill clipboard manager tests passed");
    return 0;
}
