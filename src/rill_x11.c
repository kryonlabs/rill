#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>

#define Font X11Font
#define Screen X11Screen
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/XTest.h>
#undef Font
#undef Screen

#include "rill_x11.h"

static int wm_select_failed;
static Display *manager_display;
static int manager_damage_error;
static int (*previous_error_handler)(Display *, XErrorEvent *);

static int
manager_error_handler(Display *display, XErrorEvent *event)
{
    /* A client can disappear between receiving an event and querying it. */
    if(display == manager_display &&
       (event->error_code == BadWindow || event->error_code == BadDrawable ||
        event->error_code == manager_damage_error))
        return 0;
    return previous_error_handler != NULL ? previous_error_handler(display, event) : 0;
}

static void
set_parent_death_signal(void)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
}

static int
x11_error_handler(Display *display, XErrorEvent *event)
{
    (void)display;
    if(event != NULL && event->error_code == BadAccess)
        wm_select_failed = 1;
    return 0;
}

void
RillX11Init(RillX11Manager *wm)
{
    if(wm == NULL)
        return;
    memset(wm, 0, sizeof(*wm));
    wm->desktop_count = 4;
    wm->drag_index = -1;
    wm->focused_index = -1;
}

static void switch_desktop(RillX11Manager *wm, int desktop);
static void set_client_desktop(RillX11Manager *wm, int index, int desktop);

static Atom
atom(RillX11Manager *wm, const char *name)
{
    return XInternAtom(wm->display, name, False);
}

static void
init_atoms(RillX11Manager *wm)
{
    wm->wm_protocols = atom(wm, "WM_PROTOCOLS");
    wm->wm_delete_window = atom(wm, "WM_DELETE_WINDOW");
    wm->net_client_list = atom(wm, "_NET_CLIENT_LIST");
    wm->net_active_window = atom(wm, "_NET_ACTIVE_WINDOW");
    wm->net_close_window = atom(wm, "_NET_CLOSE_WINDOW");
    wm->net_wm_name = atom(wm, "_NET_WM_NAME");
    wm->utf8_string = atom(wm, "UTF8_STRING");
    wm->wm_name = XA_WM_NAME;
    wm->wm_state = atom(wm, "WM_STATE");
    wm->net_wm_pid = atom(wm, "_NET_WM_PID");
    wm->net_wm_window_type = atom(wm, "_NET_WM_WINDOW_TYPE");
    wm->net_wm_window_type_desktop = atom(wm, "_NET_WM_WINDOW_TYPE_DESKTOP");
    wm->net_wm_window_type_dock = atom(wm, "_NET_WM_WINDOW_TYPE_DOCK");
    wm->net_wm_state = atom(wm, "_NET_WM_STATE");
    wm->net_wm_state_skip_taskbar =
        atom(wm, "_NET_WM_STATE_SKIP_TASKBAR");
}

static int
read_text_property(RillX11Manager *wm, Window window, Atom property,
                   char *out, int out_size)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    if(out == NULL || out_size <= 0)
        return 0;
    out[0] = '\0';
    if(XGetWindowProperty(wm->display, window, property, 0, 1024, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) != Success ||
       data == NULL)
        return 0;
    if((actual_type == wm->utf8_string || actual_type == XA_STRING) &&
       actual_format == 8 && item_count > 0) {
        int len = (int)item_count;

        if(len >= out_size)
            len = out_size - 1;
        memcpy(out, data, (size_t)len);
        out[len] = '\0';
        XFree(data);
        return 1;
    }
    XFree(data);
    return 0;
}

static void
update_title(RillX11Manager *wm, RillX11Client *client)
{
    if(!read_text_property(wm, client->window, wm->net_wm_name,
                           client->title, (int)sizeof(client->title)) &&
       !read_text_property(wm, client->window, wm->wm_name,
                           client->title, (int)sizeof(client->title))) {
        snprintf(client->title, sizeof(client->title), "Window 0x%lx",
                 (unsigned long)client->window);
    }
}

static int
atom_list_contains(RillX11Manager *wm, Window window, Atom property, Atom needle)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data = NULL;
    int found = 0;

    if(property == None || needle == None)
        return 0;
    if(XGetWindowProperty(wm->display, window, property, 0, 64, False,
                          XA_ATOM, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) == Success &&
       data != NULL && actual_type == XA_ATOM && actual_format == 32) {
        Atom *atoms = (Atom *)data;

        for(unsigned long i = 0; i < item_count; i++) {
            if(atoms[i] == needle) {
                found = 1;
                break;
            }
        }
    }
    if(data != NULL)
        XFree(data);
    return found;
}

static int
cardinal_property(RillX11Manager *wm, Window window, Atom property,
                  unsigned long *out)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data = NULL;
    int ok = 0;

    if(property == None || out == NULL)
        return 0;
    if(XGetWindowProperty(wm->display, window, property, 0, 1, False,
                          XA_CARDINAL, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) == Success &&
       data != NULL && actual_type == XA_CARDINAL && actual_format == 32 &&
       item_count > 0) {
        *out = ((unsigned long *)data)[0];
        ok = 1;
    }
    if(data != NULL)
        XFree(data);
    return ok;
}

static int
skip_window(RillX11Manager *wm, Window window, const XWindowAttributes *attrs)
{
    unsigned long pid;
    char title[128];

    if(window == wm->support_window || attrs == NULL || attrs->override_redirect)
        return 1;
    if(cardinal_property(wm, window, wm->net_wm_pid, &pid) &&
       pid == (unsigned long)getpid())
        return 1;
    if(read_text_property(wm, window, wm->net_wm_name, title,
                          (int)sizeof(title)) &&
       strcmp(title, "Rill") == 0)
        return 1;
    if(atom_list_contains(wm, window, wm->net_wm_window_type,
                          wm->net_wm_window_type_desktop))
        return 1;
    if(atom_list_contains(wm, window, wm->net_wm_window_type,
                          wm->net_wm_window_type_dock))
        return 1;
    return 0;
}

static int
client_index(RillX11Manager *wm, Window window)
{
    for(int i = 0; i < wm->client_count; i++)
        if(wm->clients[i].window == window)
            return i;
    return -1;
}

static void
publish_client_list(RillX11Manager *wm)
{
    Window *windows;
    int count = 0;

    if(!wm->active)
        return;
    windows = wm->client_count > 0 ? malloc((size_t)wm->client_count * sizeof(*windows)) : NULL;
    if(wm->client_count > 0 && windows == NULL) return;
    for(int i = 0; i < wm->client_count; i++)
        if(wm->clients[i].mapped)
            windows[count++] = wm->clients[i].window;
    XChangeProperty(wm->display, wm->root, wm->net_client_list, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)windows, count);
    free(windows);
    {
        Window active = wm->focused_index >= 0 && wm->focused_index < wm->client_count ?
                        wm->clients[wm->focused_index].window : None;

        XChangeProperty(wm->display, wm->root, wm->net_active_window,
                        XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)&active, 1);
    }
}

static void
focus_client(RillX11Manager *wm, int index)
{
    if(index < 0 || index >= wm->client_count)
        return;
    if(wm->clients[index].desktop >= 0 && wm->clients[index].desktop != wm->current_desktop)
        switch_desktop(wm, wm->clients[index].desktop);
    for(int i = 0; i < wm->client_count; i++)
        wm->clients[i].focused = i == index;
    wm->focused_index = index;
    XRaiseWindow(wm->display, wm->clients[index].window);
    XSetInputFocus(wm->display, wm->clients[index].window,
                   RevertToPointerRoot, CurrentTime);
    publish_client_list(wm);
    XFlush(wm->display);
}

static void
restack_client(RillX11Manager *wm, int index)
{
    RillX11Client client;

    if(index < 0 || index >= wm->client_count)
        return;
    client = wm->clients[index];
    if(index < wm->client_count - 1) {
        memmove(&wm->clients[index], &wm->clients[index + 1],
                (size_t)(wm->client_count - index - 1) *
                    sizeof(wm->clients[0]));
        wm->clients[wm->client_count - 1] = client;
    }
    focus_client(wm, wm->client_count - 1);
}

static void
free_client(RillX11Manager *wm, int index)
{
    RillX11Client *client;

    if(index < 0 || index >= wm->client_count)
        return;
    client = &wm->clients[index];
    if(client->damage != None)
        XDamageDestroy(wm->display, client->damage);
    if(client->texture.id != 0)
        UnloadTexture(client->texture);
    free(client->pixels);
    memmove(&wm->clients[index], &wm->clients[index + 1],
            (size_t)(wm->client_count - index - 1) * sizeof(wm->clients[0]));
    wm->client_count--;
    if(wm->focused_index == index)
        wm->focused_index = -1;
    else if(wm->focused_index > index)
        wm->focused_index--;
    if(wm->drag_index == index)
        wm->drag_index = -1;
    else if(wm->drag_index > index)
        wm->drag_index--;
    publish_client_list(wm);
}

static void
manage_window(RillX11Manager *wm, Window window)
{
    XWindowAttributes attrs;
    RillX11Client *client;
    int index;

    if(!wm->active || window == None || client_index(wm, window) >= 0)
        return;
    if(!XGetWindowAttributes(wm->display, window, &attrs))
        return;
    if(skip_window(wm, window, &attrs)) {
        /* Docks and desktop surfaces still need their MapRequest honored. */
        if(!attrs.override_redirect && attrs.map_state == IsUnmapped)
            XMapWindow(wm->display, window);
        return;
    }

    if(wm->client_count == wm->client_capacity) {
        int capacity;
        RillX11Client *clients;
        if(wm->client_capacity > INT_MAX / 2) return;
        capacity = wm->client_capacity != 0 ? wm->client_capacity * 2 : RILL_X11_INITIAL_WINDOWS;
        if((size_t)capacity > (size_t)-1 / sizeof(*clients)) return;
        clients = realloc(wm->clients, (size_t)capacity * sizeof(*clients));
        if(clients == NULL) {
            fprintf(stderr, "rill: not enough memory to manage another X11 window\n");
            return;
        }
        wm->clients = clients;
        wm->client_capacity = capacity;
    }
    index = wm->client_count++;
    client = &wm->clients[index];
    memset(client, 0, sizeof(*client));
    client->window = window;
    client->desktop = wm->current_desktop;
    {
        unsigned long desktop;
        if(cardinal_property(wm, window, atom(wm, "_NET_WM_DESKTOP"), &desktop)) {
            if(desktop == 0xffffffffUL) client->desktop = -1;
            else if(desktop < (unsigned long)wm->desktop_count) client->desktop = desktop;
        }
        desktop = client->desktop < 0 ? 0xffffffffUL : (unsigned long)client->desktop;
        XChangeProperty(wm->display, window, atom(wm, "_NET_WM_DESKTOP"), XA_CARDINAL,
                        32, PropModeReplace, (unsigned char *)&desktop, 1);
    }
    client->x = attrs.x;
    client->y = attrs.y;
    client->w = attrs.width > 1 ? attrs.width : 420;
    client->h = attrs.height > 1 ? attrs.height : 260;
    client->frame_x = 150 + index * 28;
    client->frame_y = 86 + index * 28;
    client->frame_w = client->w + 2;
    client->frame_h = client->h + 32;
    if(client->frame_w < 320)
        client->frame_w = 320;
    if(client->frame_h < 220)
        client->frame_h = 220;
    client->mapped = attrs.map_state == IsViewable;
    client->dirty = 1;
    update_title(wm, client);
    XSelectInput(wm->display, window,
                 StructureNotifyMask | PropertyChangeMask | FocusChangeMask);
    XCompositeRedirectWindow(wm->display, window, CompositeRedirectManual);
    client->damage = XDamageCreate(wm->display, window,
                                   XDamageReportNonEmpty);
    if(!client->mapped)
        XMapWindow(wm->display, window);
    client->mapped = 1;
    restack_client(wm, index);
    publish_client_list(wm);
}

static void
scan_existing_windows(RillX11Manager *wm)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;

    if(!XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                   &children, &count))
        return;
    for(unsigned int i = 0; i < count; i++) {
        XWindowAttributes attrs;
        if(XGetWindowAttributes(wm->display, children[i], &attrs) && attrs.map_state == IsViewable)
            manage_window(wm, children[i]);
    }
    if(children != NULL)
        XFree(children);
}

static int
open_manager_display(RillX11Manager *wm, const char *display_name)
{
    wm->display = XOpenDisplay(display_name);
    if(wm->display == NULL)
        return 0;
    wm->screen = DefaultScreen(wm->display);
    wm->root = RootWindow(wm->display, wm->screen);
    init_atoms(wm);
    int extension_event, extension_error, major, minor;
    if(!XCompositeQueryExtension(wm->display, &extension_event, &extension_error) ||
       !XTestQueryExtension(wm->display, &extension_event, &extension_error, &major, &minor) ||
       !XDamageQueryExtension(wm->display, &wm->damage_event,
                              &wm->damage_error)) {
        XCloseDisplay(wm->display);
        wm->display = NULL;
        return 0;
    }
    wm->support_window = XCreateSimpleWindow(wm->display, wm->root, 0, 0,
                                             1, 1, 0, 0, 0);
    return 1;
}

static int
claim_wm(RillX11Manager *wm)
{
    char selection_name[32];
    Atom wm_selection;
    int (*old_handler)(Display *, XErrorEvent *);

    snprintf(selection_name, sizeof(selection_name), "WM_S%d", wm->screen);
    wm_selection = atom(wm, selection_name);
    wm_select_failed = 0;
    old_handler = XSetErrorHandler(x11_error_handler);
    XSelectInput(wm->display, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 PropertyChangeMask);
    XSync(wm->display, False);
    XSetErrorHandler(old_handler);
    if(wm_select_failed)
        return 0;
    XSetSelectionOwner(wm->display, wm_selection, wm->support_window,
                       CurrentTime);
    if(XGetSelectionOwner(wm->display, wm_selection) != wm->support_window)
        return 0;
    return 1;
}

static int
start_on_display(RillX11Manager *wm, const char *display_name, int owns_server)
{
    if(!open_manager_display(wm, display_name))
        return 0;
    if(!claim_wm(wm)) {
        XCloseDisplay(wm->display);
        wm->display = NULL;
        return 0;
    }
    manager_display = wm->display;
    manager_damage_error = wm->damage_error + BadDamage;
    previous_error_handler = XSetErrorHandler(manager_error_handler);
    wm->active = 1;
    wm->owns_server = owns_server;
    snprintf(wm->display_name, sizeof(wm->display_name), "%s",
             display_name != NULL ? display_name : DisplayString(wm->display));
    {
        Atom check = atom(wm, "_NET_SUPPORTING_WM_CHECK");
        Atom supported[] = {check, wm->net_client_list, wm->net_active_window,
                            wm->net_close_window, wm->net_wm_name,
                            atom(wm, "_NET_NUMBER_OF_DESKTOPS"),
                            atom(wm, "_NET_CURRENT_DESKTOP"), atom(wm, "_NET_WM_DESKTOP")};
        unsigned long count = wm->desktop_count, current = wm->current_desktop;
        XChangeProperty(wm->display, wm->root, atom(wm, "_NET_NUMBER_OF_DESKTOPS"),
                        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&count, 1);
        XChangeProperty(wm->display, wm->root, atom(wm, "_NET_CURRENT_DESKTOP"),
                        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&current, 1);
        XChangeProperty(wm->display, wm->root, check, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&wm->support_window, 1);
        XChangeProperty(wm->display, wm->support_window, check, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&wm->support_window, 1);
        XChangeProperty(wm->display, wm->support_window, wm->net_wm_name,
                        wm->utf8_string, 8, PropModeReplace, (unsigned char *)"Rill", 4);
        XChangeProperty(wm->display, wm->root, atom(wm, "_NET_SUPPORTED"), XA_ATOM,
                        32, PropModeReplace, (unsigned char *)supported,
                        sizeof(supported) / sizeof(supported[0]));
    }
    scan_existing_windows(wm);
    publish_client_list(wm);
    return 1;
}

int
RillX11StartRoot(RillX11Manager *wm)
{
    const char *display_name;

    if(wm == NULL)
        return 0;
    display_name = getenv("DISPLAY");
    return start_on_display(wm, display_name, 0);
}

static int
display_lock_exists(int number)
{
    char path[64];

    snprintf(path, sizeof(path), "/tmp/.X%d-lock", number);
    return access(path, F_OK) == 0;
}

static int
wait_for_display(const char *display_name)
{
    for(int i = 0; i < 100; i++) {
        Display *display = XOpenDisplay(display_name);

        if(display != NULL) {
            XCloseDisplay(display);
            return 1;
        }
        usleep(30000);
    }
    return 0;
}

int
RillX11StartWindowed(RillX11Manager *wm, int width, int height)
{
    char display_name[32];
    char screen_arg[64];
    pid_t pid;
    int display_number = -1;

    if(wm == NULL)
        return 0;
    for(int i = 90; i < 140; i++) {
        if(!display_lock_exists(i)) {
            display_number = i;
            break;
        }
    }
    if(display_number < 0)
        return 0;
    snprintf(display_name, sizeof(display_name), ":%d", display_number);
    snprintf(screen_arg, sizeof(screen_arg), "%dx%dx24", width, height);
    pid = fork();
    if(pid < 0)
        return 0;
    if(pid == 0) {
        set_parent_death_signal();
        execlp("Xvfb", "Xvfb", display_name, "-screen", "0", screen_arg,
               "-nolisten", "tcp", (char *)NULL);
        _exit(127);
    }
    wm->server_pid = pid;
    if(!wait_for_display(display_name)) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        wm->server_pid = 0;
        return 0;
    }
    if(!start_on_display(wm, display_name, 1)) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        wm->server_pid = 0;
        return 0;
    }
    setenv("RILL_CLIENT_DISPLAY", display_name, 1);
    setenv("RILL_CONTAINED_X11", "1", 1);
    {
        const char *display_file = getenv("RILL_X11_DISPLAY_FILE");

        if(display_file != NULL && display_file[0] != '\0') {
            FILE *file = fopen(display_file, "w");

            if(file != NULL) {
                fprintf(file, "%s\n", display_name);
                fclose(file);
            }
        }
    }
    return 1;
}

const char *
RillX11DisplayName(const RillX11Manager *wm)
{
    return wm != NULL && wm->display_name[0] != '\0' ? wm->display_name : NULL;
}

int
RillX11Launch(RillX11Manager *wm, const char *command)
{
    pid_t pid;

    if(wm == NULL || !wm->active || command == NULL || command[0] == '\0')
        return 0;
    pid = fork();
    if(pid < 0)
        return 0;
    if(pid == 0) {
        if(wm->owns_server) set_parent_death_signal();
        setsid();
        setenv("DISPLAY", wm->display_name, 1);
        if(wm->owns_server) {
        setenv("GDK_BACKEND", "x11", 1);
        setenv("QT_QPA_PLATFORM", "xcb", 1);
        setenv("SDL_VIDEODRIVER", "x11", 1);
        setenv("CLUTTER_BACKEND", "x11", 1);
        setenv("MOZ_ENABLE_WAYLAND", "0", 1);
        unsetenv("WAYLAND_DISPLAY");
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        unsetenv("DESKTOP_STARTUP_ID");
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    return 1;
}

static void
configure_client(RillX11Manager *wm, XConfigureRequestEvent *event)
{
    XWindowChanges changes;
    int index;

    if(event == NULL)
        return;
    changes.x = event->x;
    changes.y = event->y;
    changes.width = event->width;
    changes.height = event->height;
    changes.border_width = event->border_width;
    changes.sibling = event->above;
    changes.stack_mode = event->detail;
    XConfigureWindow(wm->display, event->window,
                     (unsigned int)event->value_mask, &changes);
    index = client_index(wm, event->window);
    if(index >= 0) {
        RillX11Client *client = &wm->clients[index];

        if(event->value_mask & CWWidth)
            client->w = event->width;
        if(event->value_mask & CWHeight)
            client->h = event->height;
        client->dirty = 1;
    }
}

static int
client_visible(RillX11Manager *wm, RillX11Client *client)
{
    return client->desktop < 0 || client->desktop == wm->current_desktop;
}

static void
switch_desktop(RillX11Manager *wm, int desktop)
{
    unsigned long value;
    if(desktop < 0 || desktop >= wm->desktop_count || desktop == wm->current_desktop) return;
    for(int i = 0; i < wm->client_count; i++) {
        RillX11Client *client = &wm->clients[i];
        if(client->desktop == wm->current_desktop) {
            client->ignore_unmap++;
            XUnmapWindow(wm->display, client->window);
        }
        client->focused = 0;
    }
    wm->current_desktop = desktop;
    wm->focused_index = -1;
    wm->drag_index = -1;
    for(int i = 0; i < wm->client_count; i++) {
        if(client_visible(wm, &wm->clients[i])) {
            XMapWindow(wm->display, wm->clients[i].window);
            wm->clients[i].dirty = 1;
            wm->focused_index = i;
        }
    }
    if(wm->focused_index >= 0) {
        wm->clients[wm->focused_index].focused = 1;
        XSetInputFocus(wm->display, wm->clients[wm->focused_index].window,
                       RevertToPointerRoot, CurrentTime);
    } else {
        XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, CurrentTime);
    }
    value = desktop;
    XChangeProperty(wm->display, wm->root, atom(wm, "_NET_CURRENT_DESKTOP"),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&value, 1);
    publish_client_list(wm);
    XFlush(wm->display);
}

static void
set_client_desktop(RillX11Manager *wm, int index, int desktop)
{
    RillX11Client *client;
    unsigned long value;
    int was_visible;
    if(index < 0 || desktop < -1 || desktop >= wm->desktop_count) return;
    client = &wm->clients[index];
    was_visible = client_visible(wm, client);
    client->desktop = desktop;
    value = desktop < 0 ? 0xffffffffUL : (unsigned long)desktop;
    XChangeProperty(wm->display, client->window, atom(wm, "_NET_WM_DESKTOP"),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&value, 1);
    if(was_visible && !client_visible(wm, client)) {
        client->ignore_unmap++;
        XUnmapWindow(wm->display, client->window);
        if(wm->focused_index == index) {
            client->focused = 0;
            wm->focused_index = -1;
            XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, CurrentTime);
        }
    } else if(!was_visible && client_visible(wm, client)) {
        XMapWindow(wm->display, client->window);
        client->dirty = 1;
    }
    publish_client_list(wm);
}

static void
request_close(RillX11Manager *wm, Window window, Time timestamp)
{
    Atom *protocols = NULL;
    int count = 0, supports_delete = 0;
    if(XGetWMProtocols(wm->display, window, &protocols, &count)) {
        for(int i = 0; i < count; i++)
            if(protocols[i] == wm->wm_delete_window) supports_delete = 1;
        XFree(protocols);
    }
    if(supports_delete) {
        XEvent event;
        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.display = wm->display;
        event.xclient.window = window;
        event.xclient.message_type = wm->wm_protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = wm->wm_delete_window;
        event.xclient.data.l[1] = timestamp;
        XSendEvent(wm->display, window, False, NoEventMask, &event);
    } else {
        XKillClient(wm->display, window);
    }
    XFlush(wm->display);
}

static void
handle_client_message(RillX11Manager *wm, XClientMessageEvent *event)
{
    int index;

    if(event == NULL || event->format != 32)
        return;
    if(event->message_type == atom(wm, "_NET_CURRENT_DESKTOP")) {
        switch_desktop(wm, (int)event->data.l[0]);
    } else if(event->message_type == atom(wm, "_NET_WM_DESKTOP")) {
        unsigned long desktop = (unsigned long)event->data.l[0] & 0xffffffffUL;
        if(desktop == 0xffffffffUL || desktop < (unsigned long)wm->desktop_count)
            set_client_desktop(wm, client_index(wm, event->window),
                               desktop == 0xffffffffUL ? -1 : (int)desktop);
    } else if(event->message_type == wm->net_close_window &&
       client_index(wm, event->window) >= 0) {
        request_close(wm, event->window, (Time)event->data.l[0]);
    } else if(event->message_type == wm->net_active_window) {
        index = client_index(wm, event->window);
        if(index >= 0)
            restack_client(wm, index);
    }
}

void
RillX11Poll(RillX11Manager *wm)
{
    XEvent event;
    int damage_type;

    if(wm == NULL || !wm->active || wm->display == NULL)
        return;
    damage_type = wm->damage_event + XDamageNotify;
    while(XPending(wm->display) > 0) {
        XNextEvent(wm->display, &event);
        if(event.type == MapRequest) {
            manage_window(wm, event.xmaprequest.window);
        } else if(event.type == ConfigureRequest) {
            configure_client(wm, &event.xconfigurerequest);
        } else if(event.type == DestroyNotify) {
            free_client(wm, client_index(wm, event.xdestroywindow.window));
        } else if(event.type == UnmapNotify && event.xunmap.event == wm->root) {
            int index = client_index(wm, event.xunmap.window);
            if(index >= 0 && wm->clients[index].ignore_unmap > 0)
                wm->clients[index].ignore_unmap--;
            else
                free_client(wm, index);
        } else if(event.type == PropertyNotify) {
            int index = client_index(wm, event.xproperty.window);

            if(index >= 0 &&
               (event.xproperty.atom == wm->net_wm_name ||
                event.xproperty.atom == wm->wm_name))
                update_title(wm, &wm->clients[index]);
        } else if(event.type == ClientMessage) {
            handle_client_message(wm, &event.xclient);
        } else if(event.type == FocusIn) {
            int index = client_index(wm, event.xfocus.window);

            if(index >= 0 && client_visible(wm, &wm->clients[index])) {
                for(int i = 0; i < wm->client_count; i++) wm->clients[i].focused = i == index;
                wm->focused_index = index;
                publish_client_list(wm);
            }
        } else if(event.type == damage_type) {
            XDamageNotifyEvent *damage = (XDamageNotifyEvent *)&event;
            int index = client_index(wm, damage->drawable);

            if(index >= 0)
                wm->clients[index].dirty = 1;
            XDamageSubtract(wm->display, damage->damage, None, None);
        }
    }
}

static unsigned char
channel_from_mask(unsigned long pixel, unsigned long mask)
{
    unsigned long value;
    int shift = 0;
    int bits = 0;

    if(mask == 0)
        return 0;
    while(((mask >> shift) & 1UL) == 0)
        shift++;
    value = (pixel & mask) >> shift;
    while(((mask >> (shift + bits)) & 1UL) != 0)
        bits++;
    if(bits >= 8)
        return (unsigned char)(value >> (bits - 8));
    return (unsigned char)((value * 255UL) / ((1UL << bits) - 1UL));
}

static int
refresh_client_texture(RillX11Manager *wm, RillX11Client *client)
{
    Pixmap pixmap;
    Window root;
    XWindowAttributes attrs;
    Window child;
    int x;
    int y;
    int root_x;
    int root_y;
    unsigned int width;
    unsigned int height;
    unsigned int border;
    unsigned int depth;
    XImage *image;
    size_t need;
    Image kry_image;

    if(client == NULL || !client->dirty || !client->mapped)
        return 1;
    if(!XGetWindowAttributes(wm->display, client->window, &attrs))
        return 0;
    client->w = attrs.width;
    client->h = attrs.height;
    root_x = 0;
    root_y = 0;
    XTranslateCoordinates(wm->display, client->window, wm->root, 0, 0,
                          &root_x, &root_y, &child);
    client->x = root_x;
    client->y = root_y;

    pixmap = XCompositeNameWindowPixmap(wm->display, client->window);
    if(pixmap == None)
        return 0;
    if(!XGetGeometry(wm->display, pixmap, &root, &x, &y, &width, &height,
                     &border, &depth) || width == 0 || height == 0) {
        XFreePixmap(wm->display, pixmap);
        return 0;
    }
    image = XGetImage(wm->display, pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
    XFreePixmap(wm->display, pixmap);
    if(image == NULL)
        return 0;

    need = (size_t)width * height * 4;
    if(client->pixel_cap < need) {
        unsigned char *pixels = realloc(client->pixels, need);

        if(pixels == NULL) {
            XDestroyImage(image);
            return 0;
        }
        client->pixels = pixels;
        client->pixel_cap = need;
    }
    for(unsigned int yy = 0; yy < height; yy++) {
        for(unsigned int xx = 0; xx < width; xx++) {
            unsigned long pixel = XGetPixel(image, (int)xx, (int)yy);
            unsigned char *out = client->pixels + ((size_t)yy * width + xx) * 4;

            out[0] = channel_from_mask(pixel, image->red_mask);
            out[1] = channel_from_mask(pixel, image->green_mask);
            out[2] = channel_from_mask(pixel, image->blue_mask);
            out[3] = 255;
        }
    }
    XDestroyImage(image);

    client->w = (int)width;
    client->h = (int)height;
    if(client->texture.id == 0 || client->texture.width != (int)width ||
       client->texture.height != (int)height) {
        if(client->texture.id != 0)
            UnloadTexture(client->texture);
        kry_image.data = client->pixels;
        kry_image.width = (int)width;
        kry_image.height = (int)height;
        kry_image.mipmaps = 1;
        kry_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        client->texture = LoadTextureFromImage(kry_image);
    } else {
        UpdateTexture(client->texture, client->pixels);
    }
    client->dirty = 0;
    return 1;
}

static Color
mix(Color a, Color b, float t)
{
    Color c;

    if(t < 0.0f)
        t = 0.0f;
    if(t > 1.0f)
        t = 1.0f;
    c.r = (unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t);
    c.g = (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t);
    c.b = (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t);
    c.a = 255;
    return c;
}

static int
close_button(Rectangle close)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), close);
    Color color = hover ? (Color){220, 80, 96, 255} :
                  Fade(GetThemeText(), 0.55f);

    DrawRectangleRounded(close, 0.18f, 4, Fade(color, hover ? 0.24f : 0.08f));
    DrawLine((int)close.x + 6, (int)close.y + 6,
             (int)(close.x + close.width) - 6,
             (int)(close.y + close.height) - 6, color);
    DrawLine((int)(close.x + close.width) - 6, (int)close.y + 6,
             (int)close.x + 6, (int)(close.y + close.height) - 6, color);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void
RillX11Draw(RillX11Manager *wm)
{
    if(wm == NULL || !wm->active)
        return;
    for(int i = 0; i < wm->client_count; i++) {
        RillX11Client *client = &wm->clients[i];
        if(!client_visible(wm, client)) continue;
        Rectangle frame = {client->frame_x, client->frame_y,
                           client->frame_w, client->frame_h};
        Rectangle title = {client->frame_x, client->frame_y,
                           client->frame_w, 30};
        Rectangle content = {client->frame_x + 1, client->frame_y + 31,
                             client->frame_w - 2, client->frame_h - 32};
        Rectangle close = {client->frame_x + client->frame_w - 30,
                           client->frame_y + 4, 22, 22};
        Color border = client->focused ? GetThemeLink() :
                       Fade(GetThemeText(), 0.32f);

        (void)refresh_client_texture(wm, client);
        DrawRectangleRounded(frame, 0.025f, 8, GetThemeSurface());
        DrawRectangleRoundedLinesEx(frame, 0.025f, 8, 2.0f, border);
        DrawRectangleRec(title, mix(GetThemeSurface(), border, 0.18f));
        BeginScissorMode((int)title.x + 8, (int)title.y,
                         (int)title.width - 44, (int)title.height);
        Text(client->title, (int)title.x + 10, (int)title.y + 8,
             Text14, GetThemeText());
        EndScissorMode();
        if(close_button(close)) {
            request_close(wm, client->window, CurrentTime);
        }
        BeginScissorMode((int)content.x, (int)content.y,
                         (int)content.width, (int)content.height);
        DrawRectangleRec(content, GetThemeBackground());
        if(client->texture.id != 0) {
            DrawTexturePro(client->texture,
                           (Rectangle){0, 0, (float)client->texture.width,
                                       (float)client->texture.height},
                           content, (Vector2){0, 0}, 0.0f, WHITE);
        } else {
            Text("Waiting for X11 surface", (int)content.x + 14,
                 (int)content.y + 14, Text14, GetThemeIcon());
        }
        EndScissorMode();
    }
}

static void
client_root_origin(RillX11Manager *wm, RillX11Client *client, int *x, int *y)
{
    Window child;

    *x = 0;
    *y = 0;
    XTranslateCoordinates(wm->display, client->window, wm->root, 0, 0,
                          x, y, &child);
}

static void
send_button(RillX11Manager *wm, RillX11Client *client, int button, int down,
            Vector2 mouse)
{
    Rectangle content = {client->frame_x + 1, client->frame_y + 31,
                         client->frame_w - 2, client->frame_h - 32};
    int root_x;
    int root_y;
    int local_x;
    int local_y;

    if(content.width <= 0 || content.height <= 0)
        return;
    client_root_origin(wm, client, &root_x, &root_y);
    local_x = (int)((mouse.x - content.x) * client->w / content.width);
    local_y = (int)((mouse.y - content.y) * client->h / content.height);
    XTestFakeMotionEvent(wm->display, wm->screen,
                         root_x + local_x, root_y + local_y, CurrentTime);
    XTestFakeButtonEvent(wm->display, button, down, CurrentTime);
    XFlush(wm->display);
}

static void
send_key(RillX11Manager *wm, KeySym keysym, int shifted)
{
    KeyCode code;
    KeyCode shift;

    if(keysym == NoSymbol)
        return;
    code = XKeysymToKeycode(wm->display, keysym);
    if(code == 0)
        return;
    shift = XKeysymToKeycode(wm->display, XK_Shift_L);
    if(shifted && shift != 0)
        XTestFakeKeyEvent(wm->display, shift, True, CurrentTime);
    XTestFakeKeyEvent(wm->display, code, True, CurrentTime);
    XTestFakeKeyEvent(wm->display, code, False, CurrentTime);
    if(shifted && shift != 0)
        XTestFakeKeyEvent(wm->display, shift, False, CurrentTime);
    XFlush(wm->display);
}

static void
send_char(RillX11Manager *wm, int ch)
{
    if(ch >= 'A' && ch <= 'Z')
        send_key(wm, (KeySym)(ch + ('a' - 'A')), 1);
    else if(ch >= 32 && ch <= 126)
        send_key(wm, (KeySym)ch, 0);
}

static void
forward_keyboard(RillX11Manager *wm)
{
    int ch;

    if(wm->focused_index < 0 || wm->focused_index >= wm->client_count)
        return;
    while((ch = GetCharPressed()) > 0)
        send_char(wm, ch);
    if(IsKeyPressed(KEY_ENTER))
        send_key(wm, XK_Return, 0);
    if(IsKeyPressed(KEY_BACKSPACE))
        send_key(wm, XK_BackSpace, 0);
    if(IsKeyPressed(KEY_TAB))
        send_key(wm, XK_Tab, 0);
    if(IsKeyPressed(KEY_ESCAPE))
        send_key(wm, XK_Escape, 0);
    if(IsKeyPressed(KEY_DELETE))
        send_key(wm, XK_Delete, 0);
    if(IsKeyPressed(KEY_LEFT))
        send_key(wm, XK_Left, 0);
    if(IsKeyPressed(KEY_RIGHT))
        send_key(wm, XK_Right, 0);
    if(IsKeyPressed(KEY_UP))
        send_key(wm, XK_Up, 0);
    if(IsKeyPressed(KEY_DOWN))
        send_key(wm, XK_Down, 0);
}

void
RillX11ProcessInput(RillX11Manager *wm)
{
    Vector2 mouse;

    if(wm == NULL || !wm->active)
        return;
    mouse = GetMousePosition();
    if(IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        wm->drag_index = -1;
    if(wm->drag_index >= 0 && wm->drag_index < wm->client_count &&
       IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        RillX11Client *client = &wm->clients[wm->drag_index];

        client->frame_x = (int)mouse.x - wm->drag_dx;
        client->frame_y = (int)mouse.y - wm->drag_dy;
        if(client->frame_y < 26)
            client->frame_y = 26;
        return;
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        for(int i = wm->client_count - 1; i >= 0; i--) {
            RillX11Client *client = &wm->clients[i];
            if(!client_visible(wm, client)) continue;
            Rectangle frame = {client->frame_x, client->frame_y,
                               client->frame_w, client->frame_h};
            Rectangle title = {client->frame_x, client->frame_y,
                               client->frame_w, 30};
            Rectangle close = {client->frame_x + client->frame_w - 30,
                               client->frame_y + 4, 22, 22};
            Rectangle content = {client->frame_x + 1, client->frame_y + 31,
                                 client->frame_w - 2, client->frame_h - 32};

            if(!CheckCollisionPointRec(mouse, frame))
                continue;
            restack_client(wm, i);
            client = &wm->clients[wm->client_count - 1];
            if(CheckCollisionPointRec(mouse, title) &&
               !CheckCollisionPointRec(mouse, close)) {
                wm->drag_index = wm->client_count - 1;
                wm->drag_dx = (int)mouse.x - client->frame_x;
                wm->drag_dy = (int)mouse.y - client->frame_y;
            } else if(CheckCollisionPointRec(mouse, content)) {
                send_button(wm, client, 1, True, mouse);
            }
            break;
        }
    }
    if(IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
       wm->focused_index >= 0 && wm->focused_index < wm->client_count)
        send_button(wm, &wm->clients[wm->focused_index], 1, False, mouse);
    forward_keyboard(wm);
}

void
RillX11Shutdown(RillX11Manager *wm)
{
    if(wm == NULL)
        return;
    if(wm->display != NULL) {
        while(wm->client_count > 0)
            free_client(wm, wm->client_count - 1);
        if(wm->support_window != None)
            XDestroyWindow(wm->display, wm->support_window);
        XDeleteProperty(wm->display, wm->root, atom(wm, "_NET_SUPPORTING_WM_CHECK"));
        XDeleteProperty(wm->display, wm->root, atom(wm, "_NET_SUPPORTED"));
        XSync(wm->display, False);
        XCloseDisplay(wm->display);
        XSetErrorHandler(previous_error_handler);
        manager_display = NULL;
    }
    if(wm->owns_server && wm->server_pid > 0) {
        kill(wm->server_pid, SIGTERM);
        waitpid(wm->server_pid, NULL, 0);
    }
    if(wm->owns_server) {
        unsetenv("RILL_CONTAINED_X11");
        unsetenv("RILL_CLIENT_DISPLAY");
    }
    free(wm->clients);
    RillX11Init(wm);
}

static Window
find_named_window(Display *display, Window root, const char *title, int depth)
{
    char *name = NULL;
    Window parent, returned_root, *children = NULL, found = None;
    unsigned int count;
    if(depth > 8) return None;
    if(XFetchName(display, root, &name) && name != NULL) {
        int matches = strcmp(name, title) == 0;
        XFree(name);
        if(matches) return root;
    }
    if(!XQueryTree(display, root, &returned_root, &parent, &children, &count)) return None;
    for(unsigned int i = 0; i < count && found == None; i++)
        found = find_named_window(display, children[i], title, depth + 1);
    if(children != NULL) XFree(children);
    return found;
}

static Window desktop_window;

void
RillX11SyncDesktop(void)
{
    Display *display;
    XWindowAttributes root, window;
    if(desktop_window == None) return;
    display = XOpenDisplay(NULL);
    if(display == NULL) return;
    if(XGetWindowAttributes(display, DefaultRootWindow(display), &root) &&
       XGetWindowAttributes(display, desktop_window, &window) &&
       (window.width != root.width || window.height != root.height)) {
        XMoveResizeWindow(display, desktop_window, 0, 0, root.width, root.height);
        XFlush(display);
    }
    XCloseDisplay(display);
}

int
RillX11SetDesktop(const char *title)
{
    Display *display = XOpenDisplay(NULL);
    Window root, window;
    Atom type, states[2];
    unsigned long desktop = 0xffffffffUL;
    if(display == NULL) return 0;
    root = DefaultRootWindow(display);
    window = find_named_window(display, root, title, 0);
    if(window == None) { XCloseDisplay(display); return 0; }
    /* Remap so an existing WM applies the desktop role before management. */
    XUnmapWindow(display, window);
    type = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_WINDOW_TYPE", False),
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
    states[0] = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
    states[1] = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_STATE", False),
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 2);
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_DESKTOP", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&desktop, 1);
    desktop_window = window;
    XStoreName(display, window, "Rill");
    XMoveResizeWindow(display, window, 0, 0, DisplayWidth(display, DefaultScreen(display)),
                      DisplayHeight(display, DefaultScreen(display)));
    XMapWindow(display, window);
    XLowerWindow(display, window);
    XSync(display, False);
    XCloseDisplay(display);
    return 1;
}
