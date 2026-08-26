#include "rill_platform.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

typedef struct XLibreSession {
    Display *display;
    Window root;
    Atom active_window;
    Atom client_list;
    Atom close_window;
    Atom net_wm_name;
    Atom utf8_string;
    Atom wm_name;
} XLibreSession;

static int
xlibre_open(XLibreSession *session)
{
    if(session == NULL)
        return 0;
    memset(session, 0, sizeof(*session));
    session->display = XOpenDisplay(NULL);
    if(session->display == NULL)
        return 0;
    session->root = RootWindow(session->display,
                               DefaultScreen(session->display));
    session->active_window = XInternAtom(session->display,
                                         "_NET_ACTIVE_WINDOW", True);
    session->client_list = XInternAtom(session->display,
                                       "_NET_CLIENT_LIST", True);
    session->close_window = XInternAtom(session->display,
                                        "_NET_CLOSE_WINDOW", True);
    session->net_wm_name = XInternAtom(session->display,
                                       "_NET_WM_NAME", True);
    session->utf8_string = XInternAtom(session->display,
                                       "UTF8_STRING", True);
    session->wm_name = XA_WM_NAME;
    return 1;
}

static void
xlibre_close(XLibreSession *session)
{
    if(session != NULL && session->display != NULL)
        XCloseDisplay(session->display);
}

static Window
xlibre_window_property(XLibreSession *session, Window window, Atom property)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    Window result;

    if(session == NULL || session->display == NULL || property == None)
        return 0;
    data = NULL;
    result = 0;
    if(XGetWindowProperty(session->display, window, property, 0, 1, False,
                          XA_WINDOW, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) == Success &&
       data != NULL && actual_type == XA_WINDOW && actual_format == 32 &&
       item_count > 0)
        result = ((Window *)data)[0];
    if(data != NULL)
        XFree(data);
    return result;
}

static int
xlibre_read_text_property(XLibreSession *session, Window window, Atom property,
                          char *out, int out_size)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    size_t len;

    if(session == NULL || session->display == NULL || property == None ||
       out == NULL || out_size <= 0)
        return 0;
    data = NULL;
    if(XGetWindowProperty(session->display, window, property, 0, 1024, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) != Success ||
       data == NULL)
        return 0;
    if(property == session->net_wm_name && session->utf8_string != None &&
       actual_type != session->utf8_string) {
        XFree(data);
        return 0;
    }
    if(actual_format != 8 || item_count == 0) {
        XFree(data);
        return 0;
    }
    len = item_count;
    if(len >= (size_t)out_size)
        len = (size_t)out_size - 1;
    memcpy(out, data, len);
    out[len] = '\0';
    XFree(data);
    return out[0] != '\0';
}

static void
xlibre_window_title(XLibreSession *session, Window window, char *out,
                    int out_size)
{
    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(xlibre_read_text_property(session, window, session->net_wm_name, out,
                                 out_size))
        return;
    if(xlibre_read_text_property(session, window, session->wm_name, out,
                                 out_size))
        return;
    snprintf(out, (size_t)out_size, "Window 0x%lx", (unsigned long)window);
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
         const char *description, const char *category, const char *command,
         const char *icon, int favorite)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->description, sizeof(out->description), "%s", description);
    snprintf(out->category, sizeof(out->category), "%s", category);
    snprintf(out->command, sizeof(out->command), "%s", command);
    lookup_icon_path(icon, out->icon_path, (int)sizeof(out->icon_path));
    out->favorite = favorite;
}

static int
linux_list_launchers(RillLauncher *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    count = 0;
    if(count < cap)
        launcher(&out[count++], "terminal", "Terminal Emulator",
                 "Use the command line", "Accessories", "host:ktrem",
                 "utilities-terminal", 1);
    if(count < cap)
        launcher(&out[count++], "files", "File Manager",
                 "Browse the file system", "Accessories", "host:shelf",
                 "system-file-manager", 1);
    if(count < cap)
        launcher(&out[count++], "settings", "Settings",
                 "Configure the desktop", "Settings", "internal:settings",
                 "preferences-system", 1);
    if(count < cap)
        launcher(&out[count++], "workbook", "Workbook",
                 "Edit spreadsheets", "Office",
                 "/mnt/storage/Projects/workbook/workbook",
                 "x-office-spreadsheet", 0);
    if(count < cap)
        launcher(&out[count++], "inbe", "Inner Breeze",
                 "Breathe and focus", "Other",
                 "/mnt/storage/Projects/inbe/build/bin/linux/inbe-linux-x86_64",
                 "applications-wellness", 0);
    if(count < cap)
        launcher(&out[count++], "about", "About Rill",
                 "Desktop information", "System", "internal:about",
                 "help-about", 0);
    return count;
}

static int
linux_list_tasks(RillTask *out, int cap)
{
    XLibreSession session;
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    Window active;
    Window *windows;
    int count;

    if(out == NULL || cap <= 0)
        return 0;
    if(!xlibre_open(&session))
        return 0;

    data = NULL;
    count = 0;
    active = xlibre_window_property(&session, session.root,
                                    session.active_window);
    if(XGetWindowProperty(session.display, session.root, session.client_list,
                          0, RILL_MAX_TASKS, False, XA_WINDOW, &actual_type,
                          &actual_format, &item_count, &bytes_after,
                          &data) == Success &&
       data != NULL && actual_type == XA_WINDOW && actual_format == 32) {
        windows = (Window *)data;
        for(unsigned long i = 0; i < item_count && count < cap; i++) {
            if((unsigned long)windows[i] > INT_MAX)
                continue;
            out[count].id = (int)windows[i];
            out[count].focused = windows[i] == active;
            out[count].urgent = 0;
            xlibre_window_title(&session, windows[i], out[count].title,
                                (int)sizeof(out[count].title));
            count++;
        }
    }
    if(data != NULL)
        XFree(data);
    xlibre_close(&session);
    return count;
}

static int
linux_launch(const RillLauncher *launcher)
{
    pid_t pid;

    if(launcher == NULL || launcher->command[0] == '\0')
        return 0;
    if(strncmp(launcher->command, "internal:", 9) == 0 ||
       strncmp(launcher->command, "host:", 5) == 0)
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
    XLibreSession session;
    XEvent event;
    Window window;
    int sent;

    if(task_id <= 0 || !xlibre_open(&session))
        return 0;
    if(session.active_window == None) {
        xlibre_close(&session);
        return 0;
    }
    window = (Window)task_id;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = session.active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;
    sent = XSendEvent(session.display, session.root, False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &event);
    XFlush(session.display);
    xlibre_close(&session);
    return sent != 0;
}

static int
linux_close_task(int task_id)
{
    XLibreSession session;
    XEvent event;
    Window window;
    int sent;

    if(task_id <= 0 || !xlibre_open(&session))
        return 0;
    if(session.close_window == None) {
        xlibre_close(&session);
        return 0;
    }
    window = (Window)task_id;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = session.close_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = CurrentTime;
    event.xclient.data.l[1] = 2;
    sent = XSendEvent(session.display, session.root, False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &event);
    XFlush(session.display);
    xlibre_close(&session);
    return sent != 0;
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
    "xlibre",
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
