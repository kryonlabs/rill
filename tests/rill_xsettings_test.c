/* XSETTINGS provider test: selection ownership, wire-format round trip and
 * serial bumps, run against the platform services under Xvfb. */
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
        fprintf(stderr, "rill xsettings test failed: %s\n", name);
        (*failures)++;
    }
}

int
main(int argc, char **argv)
{
    Display *display;
    const RillPlatformServices *platform = RillPlatformCurrent();
    RillXSetting settings[3];
    RillXSetting readback[8];
    int failures = 0;
    int count;
    char selection[32];
    Window owner;

    (void)argc;
    (void)argv;
    display = XOpenDisplay(NULL);
    if(display == NULL) {
        puts("rill xsettings test skipped: no display");
        return 0;
    }
    memset(settings, 0, sizeof(settings));
    snprintf(settings[0].name, sizeof(settings[0].name), "Net/ThemeName");
    settings[0].type = 1;
    snprintf(settings[0].string_value, sizeof(settings[0].string_value),
             "Adwaita");
    snprintf(settings[1].name, sizeof(settings[1].name), "Net/IconThemeName");
    settings[1].type = 1;
    snprintf(settings[1].string_value, sizeof(settings[1].string_value),
             "oxygen");
    snprintf(settings[2].name, sizeof(settings[2].name), "Xft/DPI");
    settings[2].type = 0;
    settings[2].integer_value = 96;

    check("platform publishes xsettings",
          platform->xsettings_publish != NULL &&
          platform->xsettings_publish(settings, 3), &failures);
    XSync(display, False);

    snprintf(selection, sizeof(selection), "_XSETTINGS_S%d",
             DefaultScreen(display));
    owner = XGetSelectionOwner(display,
                               XInternAtom(display, selection, False));
    check("selection owner taken", owner != None, &failures);

    {
        Atom type;
        int format;
        unsigned long items, remaining;
        unsigned char *data = NULL;
        check("root property present",
              XGetWindowProperty(display, DefaultRootWindow(display),
                                 XInternAtom(display, "_XSETTINGS_SETTINGS",
                                             False),
                                 0, 64, False,
                                 XInternAtom(display, "_XSETTINGS_SETTINGS",
                                             False),
                                 &type, &format, &items, &remaining,
                                 &data) == Success &&
              data != NULL && items >= 12, &failures);
        if(data != NULL) {
            int entries = data[8] | (data[9] << 8) | (data[10] << 16) |
                          (data[11] << 24);
            check("entry count on the wire", entries == 3, &failures);
            XFree(data);
        }
    }

    count = platform->xsettings_read(readback, 8);
    check("read returns all settings", count == 3, &failures);
    {
        int theme = -1, icon = -1, dpi = -1;
        for(int i = 0; i < count; i++) {
            if(strcmp(readback[i].name, "Net/ThemeName") == 0)
                theme = i;
            if(strcmp(readback[i].name, "Net/IconThemeName") == 0)
                icon = i;
            if(strcmp(readback[i].name, "Xft/DPI") == 0)
                dpi = i;
        }
        check("all names round trip", theme >= 0 && icon >= 0 && dpi >= 0,
              &failures);
        if(theme >= 0)
            check("theme value round trip",
                  readback[theme].type == 1 &&
                  strcmp(readback[theme].string_value, "Adwaita") == 0,
                  &failures);
        if(icon >= 0)
            check("icon theme round trip",
                  readback[icon].type == 1 &&
                  strcmp(readback[icon].string_value, "oxygen") == 0,
                  &failures);
        if(dpi >= 0)
            check("dpi round trip",
                  readback[dpi].type == 0 && readback[dpi].integer_value == 96,
                  &failures);
    }

    settings[2].integer_value = 192;
    check("republish succeeds",
          platform->xsettings_publish(settings, 3), &failures);
    XSync(display, False);
    count = platform->xsettings_read(readback, 8);
    {
        int dpi = -1;
        for(int i = 0; i < count; i++)
            if(strcmp(readback[i].name, "Xft/DPI") == 0)
                dpi = i;
        check("republished value visible",
              dpi >= 0 && readback[dpi].integer_value == 192, &failures);
    }

    XCloseDisplay(display);
    if(failures != 0)
        return 1;
    puts("Rill XSETTINGS provider tests passed");
    return 0;
}
