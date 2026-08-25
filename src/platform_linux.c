#include "rill_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

enum {
    RILL_X11_TASK_BASE = 200000,
    RILL_X11_TASK_MAP_MAX = 128
};

typedef struct LinuxX11TaskMap {
    Window window;
    int id;
} LinuxX11TaskMap;

static Display *x11_display;
static LinuxX11TaskMap x11_tasks[RILL_X11_TASK_MAP_MAX];
static int x11_task_count;
static int x11_next_task_id = RILL_X11_TASK_BASE;

static int
x11_ignore_error(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    return 0;
}

static Display *
linux_x11_display(void)
{
    if(x11_display == NULL) {
        x11_display = XOpenDisplay(NULL);
        if(x11_display != NULL)
            XSetErrorHandler(x11_ignore_error);
    }
    return x11_display;
}

static Atom
x11_atom(Display *display, const char *name)
{
    if(display == NULL || name == NULL)
        return None;
    return XInternAtom(display, name, False);
}

static int
x11_task_id_for_window(Window window)
{
    int slot = -1;

    if(window == None)
        return 0;
    for(int i = 0; i < x11_task_count; i++)
        if(x11_tasks[i].window == window)
            return x11_tasks[i].id;
    if(x11_task_count < RILL_X11_TASK_MAP_MAX)
        slot = x11_task_count++;
    else
        slot = window % RILL_X11_TASK_MAP_MAX;
    x11_tasks[slot].window = window;
    x11_tasks[slot].id = x11_next_task_id++;
    return x11_tasks[slot].id;
}

static Window
x11_window_for_task_id(int task_id)
{
    for(int i = 0; i < x11_task_count; i++)
        if(x11_tasks[i].id == task_id)
            return x11_tasks[i].window;
    return None;
}

static int
x11_window_title(Display *display, Window window, char *out, int out_size)
{
    Atom utf8;
    Atom net_name;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;
    char *name = NULL;

    if(out == NULL || out_size <= 0)
        return 0;
    out[0] = '\0';
    if(display == NULL || window == None)
        return 0;

    utf8 = x11_atom(display, "UTF8_STRING");
    net_name = x11_atom(display, "_NET_WM_NAME");
    if(net_name != None &&
       XGetWindowProperty(display, window, net_name, 0, 1024, False, utf8,
                          &actual_type, &actual_format, &nitems, &after,
                          &data) == Success &&
       data != NULL) {
        if(actual_format == 8 && nitems > 0) {
            int len = nitems < (unsigned long)(out_size - 1) ?
                      (int)nitems : out_size - 1;
            memcpy(out, data, (size_t)len);
            out[len] = '\0';
        }
        XFree(data);
        if(out[0] != '\0')
            return 1;
    }

    if(XFetchName(display, window, &name) != 0 && name != NULL) {
        snprintf(out, (size_t)out_size, "%s", name);
        XFree(name);
        return out[0] != '\0';
    }
    return 0;
}

static int
x11_window_has_type(Display *display, Window window, Atom wanted)
{
    Atom net_type;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;
    int found = 0;

    if(display == NULL || window == None || wanted == None)
        return 0;
    net_type = x11_atom(display, "_NET_WM_WINDOW_TYPE");
    if(net_type == None)
        return 0;
    if(XGetWindowProperty(display, window, net_type, 0, 32, False, XA_ATOM,
                          &actual_type, &actual_format, &nitems, &after,
                          &data) != Success ||
       data == NULL)
        return 0;
    if(actual_type == XA_ATOM && actual_format == 32) {
        Atom *types = (Atom *)data;

        for(unsigned long i = 0; i < nitems; i++) {
            if(types[i] == wanted) {
                found = 1;
                break;
            }
        }
    }
    XFree(data);
    return found;
}

static int
x11_is_task_window(Display *display, Window window, char *title, int title_size)
{
    XWindowAttributes attrs;
    Atom dock;
    Atom desktop;

    if(display == NULL || window == None)
        return 0;
    if(!XGetWindowAttributes(display, window, &attrs))
        return 0;
    if(attrs.override_redirect || attrs.map_state != IsViewable)
        return 0;
    dock = x11_atom(display, "_NET_WM_WINDOW_TYPE_DOCK");
    desktop = x11_atom(display, "_NET_WM_WINDOW_TYPE_DESKTOP");
    if(x11_window_has_type(display, window, dock) ||
       x11_window_has_type(display, window, desktop))
        return 0;
    if(!x11_window_title(display, window, title, title_size))
        return 0;
    if(strcmp(title, "Rill") == 0)
        return 0;
    return 1;
}

static Window
x11_active_window(Display *display)
{
    Atom active_atom;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;
    Window active = None;

    if(display == NULL)
        return None;
    active_atom = x11_atom(display, "_NET_ACTIVE_WINDOW");
    if(active_atom == None)
        return None;
    if(XGetWindowProperty(display, DefaultRootWindow(display), active_atom, 0, 1,
                          False, XA_WINDOW, &actual_type, &actual_format,
                          &nitems, &after, &data) == Success &&
       data != NULL) {
        if(actual_type == XA_WINDOW && actual_format == 32 && nitems >= 1)
            active = ((Window *)data)[0];
        XFree(data);
    }
    return active;
}

static int
x11_add_task(Display *display, Window window, Window active,
             RillTask *out, int cap, int *count)
{
    char title[128];

    if(out == NULL || count == NULL || *count >= cap)
        return 0;
    if(!x11_is_task_window(display, window, title, sizeof(title)))
        return 0;
    out[*count].id = x11_task_id_for_window(window);
    snprintf(out[*count].title, sizeof(out[*count].title), "%s", title);
    out[*count].focused = window == active;
    out[*count].urgent = 0;
    (*count)++;
    return 1;
}

static void
lookup_icon_path(const char *name, char *out, int out_size)
{
    static int gtk_attempted = 0;
    static int gtk_ready = 0;
    GtkIconTheme *theme;
    GdkPixbuf *pixbuf;
    GError *error = NULL;
    char path[512];
    char safe[128];
    int i;

    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(name == NULL || name[0] == '\0')
        return;
    if(!gtk_attempted) {
        gtk_attempted = 1;
        gtk_ready = gtk_init_check(NULL, NULL) ? 1 : 0;
    }
    if(!gtk_ready)
        return;
    theme = gtk_icon_theme_get_default();
    if(theme == NULL)
        return;
    pixbuf = gtk_icon_theme_load_icon(theme, name, 32, 0, &error);
    if(pixbuf == NULL) {
        if(error != NULL)
            g_error_free(error);
        return;
    }
    for(i = 0; name[i] != '\0' && i < (int)sizeof(safe) - 1; i++) {
        char c = name[i];
        safe[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    safe[i] = '\0';
    snprintf(path, sizeof(path), "/tmp/rill-icon-%s.png", safe);
    if(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL))
        snprintf(out, (size_t)out_size, "%s", path);
    if(error != NULL)
        g_error_free(error);
    g_object_unref(pixbuf);
}

static void
launcher(RillLauncher *out, const char *id, const char *name,
         const char *command, const char *icon)
{
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->command, sizeof(out->command), "%s", command);
    lookup_icon_path(icon, out->icon_path, (int)sizeof(out->icon_path));
}

static int
linux_list_launchers(RillLauncher *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    count = 0;
    if(count < cap)
        launcher(&out[count++], "terminal", "Terminal", "host:kapsule",
                 "utilities-terminal");
    if(count < cap)
        launcher(&out[count++], "files", "Files", "host:shelf",
                 "system-file-manager");
    if(count < cap)
        launcher(&out[count++], "settings", "Settings", "internal:settings",
                 "preferences-system");
    if(count < cap)
        launcher(&out[count++], "workbook", "Workbook",
                 "/mnt/storage/Projects/workbook/workbook",
                 "x-office-spreadsheet");
    if(count < cap)
        launcher(&out[count++], "inbe", "Inner Breeze",
                 "/mnt/storage/Projects/inbe/build/bin/linux/inbe-linux-x86_64",
                 "applications-wellness");
    if(count < cap)
        launcher(&out[count++], "about", "About Rill", "internal:about",
                 "help-about");
    return count;
}

static int
linux_list_tasks(RillTask *out, int cap)
{
    Display *display;
    Window root;
    Window active;
    Atom client_list;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;
    int count = 0;

    if(out == NULL || cap <= 0)
        return 0;

    display = linux_x11_display();
    if(display == NULL)
        return 0;
    root = DefaultRootWindow(display);
    active = x11_active_window(display);
    client_list = x11_atom(display, "_NET_CLIENT_LIST");
    if(client_list != None &&
       XGetWindowProperty(display, root, client_list, 0, 1024, False, XA_WINDOW,
                          &actual_type, &actual_format, &nitems, &after,
                          &data) == Success &&
       data != NULL) {
        if(actual_type == XA_WINDOW && actual_format == 32) {
            Window *windows = (Window *)data;

            for(unsigned long i = 0; i < nitems && count < cap; i++)
                x11_add_task(display, windows[i], active, out, cap, &count);
        }
        XFree(data);
        XFlush(display);
        return count;
    }

    {
        Window root_return = None;
        Window parent_return = None;
        Window *children = NULL;
        unsigned int child_count = 0;

        if(XQueryTree(display, root, &root_return, &parent_return, &children,
                      &child_count) != 0) {
            for(unsigned int i = 0; i < child_count && count < cap; i++)
                x11_add_task(display, children[i], active, out, cap, &count);
        }
        if(children != NULL)
            XFree(children);
    }
    XFlush(display);
    return count;
}

static int
linux_launch(const RillLauncher *launcher)
{
    pid_t pid;

    if(launcher == NULL || launcher->command[0] == '\0')
        return 0;
    if(strncmp(launcher->command, "internal:", 9) == 0)
        return 0;

    pid = fork();
    if(pid < 0)
        return 0;
    if(pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", launcher->command, (char *)NULL);
        _exit(127);
    }
    return 1;
}

static int
linux_focus_task(int task_id)
{
    Display *display = linux_x11_display();
    Window window = x11_window_for_task_id(task_id);
    XEvent event;
    Atom active;

    if(display == NULL || window == None)
        return 0;
    active = x11_atom(display, "_NET_ACTIVE_WINDOW");
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = active;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XMapRaised(display, window);
    XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
    XFlush(display);
    return 1;
}

static int
linux_close_task(int task_id)
{
    Display *display = linux_x11_display();
    Window window = x11_window_for_task_id(task_id);
    Atom protocols;
    Atom delete_window;
    Atom *supported = NULL;
    int supported_count = 0;
    int can_delete = 0;

    if(display == NULL || window == None)
        return 0;
    protocols = x11_atom(display, "WM_PROTOCOLS");
    delete_window = x11_atom(display, "WM_DELETE_WINDOW");
    if(protocols != None && delete_window != None &&
       XGetWMProtocols(display, window, &supported, &supported_count) != 0) {
        for(int i = 0; i < supported_count; i++) {
            if(supported[i] == delete_window) {
                can_delete = 1;
                break;
            }
        }
    }
    if(supported != NULL)
        XFree(supported);
    if(can_delete) {
        XEvent event;

        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = delete_window;
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display, window, False, NoEventMask, &event);
    } else {
        XKillClient(display, window);
    }
    XFlush(display);
    return 1;
}

static const char *
linux_settings_root(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if(xdg != NULL && xdg[0] != '\0')
        return xdg;
    return "~/.config/rill";
}

static const RillPlatformServices services = {
    "linux",
    linux_list_launchers,
    linux_list_tasks,
    linux_launch,
    linux_focus_task,
    linux_close_task,
    linux_settings_root
};

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
