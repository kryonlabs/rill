#include "rill_platform.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
check(const char *name, int ok, int *failures)
{
    if(!ok) {
        fprintf(stderr, "rill xembed test failed: %s\n", name);
        (*failures)++;
    }
}

static Window
parent_of(Display *display, Window window)
{
    Window root, parent, *children = NULL;
    unsigned count;
    parent = None;
    if(XQueryTree(display, window, &root, &parent, &children, &count)) {
        if(children != NULL)
            XFree(children);
    }
    return parent;
}

static void
pump(Display *display)
{
    XSync(display, False);
    usleep(150000);
    XSync(display, False);
}

int
main(void)
{
    Display *display;
    Window root, icon, second, owner;
    Atom selection, message;
    XEvent dock;
    const RillPlatformServices *platform = RillPlatformCurrent();
    int failures = 0;

    display = XOpenDisplay(NULL);
    if(display == NULL) {
        puts("rill xembed test skipped: no display");
        return 0;
    }
    root = DefaultRootWindow(display);
    check("platform hosts the legacy tray",
          platform->xembed_tray_count != NULL &&
          platform->xembed_tray_layout != NULL, &failures);
    check("tray starts empty", platform->xembed_tray_count() == 0, &failures);

    char name[40];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", DefaultScreen(display));
    selection = XInternAtom(display, name, False);
    owner = XGetSelectionOwner(display, selection);
    check("selection owner published", owner != None, &failures);

    /* Dock a tray icon by sending the legacy request to the host window. */
    icon = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, 0, 0x3355ff);
    XSelectInput(display, icon, StructureNotifyMask);
    message = XInternAtom(display, "_NET_SYSTEM_TRAY_MESSAGE", False);
    memset(&dock, 0, sizeof(dock));
    dock.xclient.type = ClientMessage;
    dock.xclient.window = owner;
    dock.xclient.message_type = message;
    dock.xclient.format = 32;
    dock.xclient.data.l[0] = 0; /* SYSTEM_TRAY_REQUEST_DOCK */
    dock.xclient.data.l[1] = icon;
    XSendEvent(display, owner, False, NoEventMask, &dock);
    XFlush(display);
    pump(display);
    check("docked icon counted", platform->xembed_tray_count() == 1, &failures);
    {
        XWindowAttributes attributes;
        check("icon reparented into the host",
              parent_of(display, icon) == owner, &failures);
        check("icon mapped",
              XGetWindowAttributes(display, icon, &attributes) &&
              attributes.map_state != IsUnmapped, &failures);
    }

    /* A second icon takes the next slot and widens the host window. */
    second = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, 0, 0xff5533);
    dock.xclient.data.l[1] = second;
    XSendEvent(display, owner, False, NoEventMask, &dock);
    XFlush(display);
    pump(display);
    check("second icon counted", platform->xembed_tray_count() == 2, &failures);
    platform->xembed_tray_layout(400, 700, 26, 1);
    pump(display);
    {
        XWindowAttributes host_attributes, icon_attributes;
        check("host window placed",
              XGetWindowAttributes(display, owner, &host_attributes) &&
              host_attributes.x == 400 && host_attributes.y == 700 &&
              host_attributes.width >= 2 * 22, &failures);
        check("icon slotted",
              XGetWindowAttributes(display, icon, &icon_attributes) &&
              icon_attributes.x == 3, &failures);
    }
    platform->xembed_tray_layout(0, 0, 26, 0);
    pump(display);
    {
        XWindowAttributes host_attributes;
        check("hidden host moves offscreen",
              XGetWindowAttributes(display, owner, &host_attributes) &&
              host_attributes.x < 0 && host_attributes.y < 0, &failures);
    }

    XDestroyWindow(display, icon);
    pump(display);
    check("destroyed icon leaves the tray",
          platform->xembed_tray_count() == 1, &failures);
    XDestroyWindow(display, second);
    pump(display);
    check("last icon leaves the tray",
          platform->xembed_tray_count() == 0, &failures);

    XCloseDisplay(display);
    if(failures != 0)
        return 1;
    puts("Rill legacy XEmbed tray tests passed");
    return 0;
}
