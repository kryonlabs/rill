/* Rill's X11 window manager. Client geometry is always in root coordinates. */
#include "session.h"
#include "wm_compositor.h"
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/sync.h>
#include <X11/keysym.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Geometry {
    int x, y, w, h;
} Geometry;
typedef struct Client {
    struct Client *next;
    Window window, frame, transient, hidden_by_parent;
    Geometry geometry, normal;
    int border, title, old_border, desktop, ignore_unmap;
    int special, dock, minimized, fullscreen, max_h, max_v, above, below, modal;
    int skip_taskbar, skip_pager, shaded, urgent, input, decorated, visible;
    int sync_request, hung, ping_pending;
    XID sync_counter;
    unsigned long focus_order, ping_time, sync_serial;
    long ping_sent_ms, sync_sent_ms;
    XSizeHints hints;
    XftDraw *draw;
    GC paint;
    char name[512];
} Client;

static Display *display;
static Window root, support;
static Client *clients, *focused, *dragged;
static int screen_number, screen_w, screen_h, desktop, desktops = 4, showing_desktop;
static int randr_event, have_randr, shape_event, have_shape, claiming, claim_failed;
static int have_sync;
static long ping_timeout_ms = 5000;
static unsigned int numlock_mask;
static unsigned long focus_order;
static Time last_time, last_click_time;
static Window last_click_window;
static int drag_mode, drag_root_x, drag_root_y;
static Geometry drag_start;
static int drag_max_h, drag_max_v;
static GC gc;
static XftFont *font;
static XftColor text_color;
static Cursor move_cursor, resize_cursor;
static volatile sig_atomic_t stopping;
static Atom wm_selection;
static int composite_enabled = 1;
static Window menu_window;
static XftDraw *menu_draw;
static Client *menu_client;
static int menu_item;
static void menu_close(void);
static void menu_paint(void);
static void menu_open(Client *client, int x, int y, Time time);
static void menu_activate(void);
static int menu_items(Client *c);
static void switcher_drop(Client *c);
static void send_ping(Client *c);

enum { Border = 3, Title = 28, Button = 25 };
/* Decoration buttons, focus model and title double-click imported from the
   xfwm4 xfconf channel when present; RILL_WM_BUTTON_LAYOUT and
   RILL_WM_FOCUS_MODE override for testing. Letters: O menu, T stick, S shade,
   H hide, M maximize, C close, split left|right of the title. */
static char button_left[8], button_right[8];
static int follow_mouse;
enum { DoubleClickMaximize, DoubleClickShade, DoubleClickNone };
static int double_click = DoubleClickMaximize;
static Atom atom(const char *name) { return XInternAtom(display, name, False); }
static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }
static void stop_signal(int sig)
{
    (void)sig;
    stopping = 1;
}
static int xerror(Display *d, XErrorEvent *e)
{
    (void)d;
    if (claiming && e->error_code == BadAccess)
        claim_failed = 1;
    /* Windows may disappear between an event and a property/geometry request. */
    if (getenv("RILL_WM_DEBUG")) {
        char error[128];
        XGetErrorText(d, e->error_code, error, sizeof(error));
        fprintf(stderr, "rill-wm: %s request=%u.%u resource=%lx\n", error, e->request_code,
                e->minor_code, e->resourceid);
    }
    return 0;
}
static unsigned long *property(Window w, const char *name, Atom type, unsigned long *count)
{
    Atom actual;
    int format;
    unsigned long remaining;
    unsigned char *data = NULL;
    *count = 0;
    if (XGetWindowProperty(display, w, atom(name), 0, 4096, False, type, &actual, &format, count,
                           &remaining, &data) != Success ||
        actual != type || format != 32) {
        if (data)
            XFree(data);
        *count = 0;
        return NULL;
    }
    return (unsigned long *)data;
}
static unsigned long cardinal(Window w, const char *name, unsigned long fallback)
{
    unsigned long n, *data = property(w, name, XA_CARDINAL, &n);
    unsigned long value = n ? data[0] : fallback;
    if (data)
        XFree(data);
    return value;
}
static int has(Window w, const char *name, const char *value)
{
    unsigned long n, *data = property(w, name, XA_ATOM, &n);
    int found = 0;
    Atom needle = atom(value);
    for (unsigned long i = 0; i < n; i++)
        if (data[i] == needle)
            found = 1;
    if (data)
        XFree(data);
    return found;
}
static void set_cardinals(Window w, const char *name, const unsigned long *values, int n)
{
    XChangeProperty(display, w, atom(name), XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)values, n);
}
static Client *find(Window w)
{
    if (w == None)
        return NULL;
    for (Client *c = clients; c; c = c->next)
        if (c->window == w || c->frame == w)
            return c;
    return NULL;
}
static Window surface(Client *c) { return c->frame ? c->frame : c->window; }
static int on_desktop(Client *c) { return c->desktop < 0 || c->desktop == desktop; }
static int visible(Client *c)
{
    return !c->minimized && on_desktop(c) && (!showing_desktop || c->special);
}
static void publish_clients(void);
static void focus(Client *c, Time time);
static void restack(void);
static void configure(Client *c);
static void update_workarea(void);
static void finish_drag(int cancel);
static void close_client(Client *c, Time time);
static void maximize(Client *c);
static void minimize(Client *c);
static void move_desktop(Client *c, int value);
static void state(Client *c);

static void protocol(Client *c, const char *name, Time time)
{
    Atom *list = NULL;
    int count = 0;
    if (!XGetWMProtocols(display, c->window, &list, &count))
        return;
    for (int i = 0; i < count; i++)
        if (list[i] == atom(name)) {
            XEvent e = {0};
            e.xclient.type = ClientMessage;
            e.xclient.window = c->window;
            e.xclient.message_type = atom("WM_PROTOCOLS");
            e.xclient.format = 32;
            e.xclient.data.l[0] = list[i];
            e.xclient.data.l[1] = time;
            XSendEvent(display, c->window, False, NoEventMask, &e);
            break;
        }
    XFree(list);
}
static void close_client(Client *c, Time time)
{
    if (!c || c->special)
        return;
    if (has(c->window, "WM_PROTOCOLS", "WM_DELETE_WINDOW")) {
        protocol(c, "WM_DELETE_WINDOW", time);
        send_ping(c);
    } else
        XKillClient(display, c->window);
}
static void title(Client *c)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    c->name[0] = 0;
    if (XGetWindowProperty(display, c->window, atom("_NET_WM_NAME"), 0, 128, False,
                           atom("UTF8_STRING"), &type, &format, &n, &after, &data) == Success &&
        data && format == 8) {
        snprintf(c->name, sizeof(c->name), "%.*s", (int)n, data);
    }
    if (data)
        XFree(data);
    if (!c->name[0]) {
        char *name = NULL;
        if (XFetchName(display, c->window, &name) && name) {
            snprintf(c->name, sizeof(c->name), "%s", name);
            XFree(name);
        }
    }
}
static void set_button_layout(const char *layout)
{
    const char *bar = strchr(layout, '|');
    char left[8];
    int n = 0;
    snprintf(left, sizeof(left), "%.*s", bar ? (int)(bar - layout) : (int)strlen(layout), layout);
    const char *groups[2] = {left, bar ? bar + 1 : ""};
    for (int side = 0; side < 2; side++) {
        char *out = side ? button_right : button_left;
        n = 0;
        for (const char *p = groups[side]; *p && n < 7; p++)
            if (strchr("OTSHMC", *p))
                out[n++] = *p;
        out[n] = 0;
    }
}
static int xfwm4_setting(const char *property, char *out, size_t size)
{
    char command[256];
    snprintf(command, sizeof(command), "xfconf-query -c xfwm4 -p %s 2>/dev/null", property);
    FILE *stream = popen(command, "r");
    if (!stream)
        return 0;
    int ok = fgets(out, (int)size, stream) != NULL;
    pclose(stream);
    if (ok) {
        char *end = out + strlen(out);
        while (end > out && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = 0;
    }
    return ok && out[0];
}
static void load_decoration_preferences(void)
{
    char value[128];
    set_button_layout("|HMC");
    const char *override = getenv("RILL_WM_BUTTON_LAYOUT");
    if (override && *override)
        set_button_layout(override);
    else if (xfwm4_setting("/general/button_layout", value, sizeof(value)))
        set_button_layout(value);
    const char *mode = getenv("RILL_WM_FOCUS_MODE");
    if (mode && strcmp(mode, "follows-mouse") == 0)
        follow_mouse = 1;
    else if (mode && strcmp(mode, "click") == 0)
        follow_mouse = 0;
    else if (xfwm4_setting("/general/focus_follows_mouse", value, sizeof(value)))
        follow_mouse = strcmp(value, "true") == 0;
    if (xfwm4_setting("/general/double_click_action", value, sizeof(value))) {
        if (strcmp(value, "shade") == 0)
            double_click = DoubleClickShade;
        else if (strcmp(value, "none") == 0)
            double_click = DoubleClickNone;
    }
}
static void draw_button_glyph(Client *c, int x, char kind)
{
    XSetForeground(display, c->paint, 0xffe6e9ef);
    Window frame = c->frame;
    GC paint = c->paint;
    if (kind == 'C') {
        XDrawLine(display, frame, paint, x + 8, 10, x + 17, 19);
        XDrawLine(display, frame, paint, x + 17, 10, x + 8, 19);
    } else if (kind == 'M')
        XDrawRectangle(display, frame, paint, x + 8, 10, 9, 9);
    else if (kind == 'H')
        XDrawLine(display, frame, paint, x + 8, 19, x + 17, 19);
    else if (kind == 'S') {
        XDrawRectangle(display, frame, paint, x + 7, 8, 11, 13);
        XDrawLine(display, frame, paint, x + 7, 14, x + 18, 14);
    } else if (kind == 'T') {
        XDrawLine(display, frame, paint, x + 12, 8, x + 17, 14);
        XDrawLine(display, frame, paint, x + 17, 14, x + 12, 20);
        XDrawLine(display, frame, paint, x + 12, 20, x + 7, 14);
        XDrawLine(display, frame, paint, x + 7, 14, x + 12, 8);
    } else if (kind == 'O') {
        XDrawLine(display, frame, paint, x + 8, 10, x + 17, 10);
        XDrawLine(display, frame, paint, x + 8, 14, x + 17, 14);
        XDrawLine(display, frame, paint, x + 8, 18, x + 17, 18);
    }
}
static void activate_button(Client *c, char kind, int x, int y, Time time)
{
    if (kind == 'C')
        close_client(c, time);
    else if (kind == 'M')
        maximize(c);
    else if (kind == 'H')
        minimize(c);
    else if (kind == 'S' && c->decorated) {
        c->shaded = !c->shaded;
        configure(c);
        state(c);
    } else if (kind == 'T')
        move_desktop(c, c->desktop < 0 ? desktop : -1);
    else if (kind == 'O')
        menu_open(c, x, y, time);
}
static void draw_frame(Client *c)
{
    if (!c->frame || !c->title)
        return;
    int width = c->geometry.w + 2 * c->border;
    unsigned long color = c == focused ? 0x344c6b : 0x41444b;
    if (c->urgent && c != focused)
        color = 0x805328;
    if (c->hung)
        color = 0x8a3d2e;
    color |= 0xff000000UL;
    XSetForeground(display, c->paint, color);
    XFillRectangle(display, c->frame, c->paint, 0, 0, width, c->title + c->border);
    XFillRectangle(display, c->frame, c->paint, 0, c->title + c->border, c->border, c->geometry.h);
    XFillRectangle(display, c->frame, c->paint, width - c->border, c->title + c->border, c->border,
                   c->geometry.h);
    XFillRectangle(display, c->frame, c->paint, 0, c->geometry.h + c->title + c->border, width,
                   c->border);
    int n_left = (int)strlen(button_left), n_right = (int)strlen(button_right);
    for (int i = 0; i < n_right; i++)
        draw_button_glyph(c, width - (n_right - i) * Button, button_right[i]);
    for (int i = 0; i < n_left; i++)
        draw_button_glyph(c, i * Button, button_left[i]);
    if (c->draw && font) {
        XRectangle clip = {7 + n_left * Button, 0,
                           (unsigned short)max(0, width - (n_left + n_right) * Button - 14), Title};
        XftDrawSetClipRectangles(c->draw, 0, 0, &clip, 1);
        XftDrawStringUtf8(c->draw, &text_color, font, 8, (Title + font->ascent - font->descent) / 2,
                          (const FcChar8 *)c->name, strlen(c->name));
        XftDrawSetClip(c->draw, NULL);
    }
}
static void state(Client *c)
{
    Atom values[14];
    int n = 0;
#define STATE(flag, name)                                                                          \
    if (c->flag)                                                                                   \
    values[n++] = atom("_NET_WM_STATE_" name)
    STATE(minimized, "HIDDEN");
    STATE(fullscreen, "FULLSCREEN");
    STATE(max_h, "MAXIMIZED_HORZ");
    STATE(max_v, "MAXIMIZED_VERT");
    STATE(above, "ABOVE");
    STATE(below, "BELOW");
    STATE(modal, "MODAL");
    STATE(skip_taskbar, "SKIP_TASKBAR");
    STATE(skip_pager, "SKIP_PAGER");
    STATE(shaded, "SHADED");
    STATE(urgent, "DEMANDS_ATTENTION");
#undef STATE
    if (c->desktop < 0)
        values[n++] = atom("_NET_WM_STATE_STICKY");
    XChangeProperty(display, c->window, atom("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)values, n);
    unsigned long wmstate[2] = {c->minimized ? IconicState : NormalState, None};
    XChangeProperty(display, c->window, atom("WM_STATE"), atom("WM_STATE"), 32, PropModeReplace,
                    (unsigned char *)wmstate, 2);
    unsigned long d = c->desktop < 0 ? 0xffffffffUL : (unsigned long)c->desktop;
    set_cardinals(c->window, "_NET_WM_DESKTOP", &d, 1);
}
static Geometry area(Client *client, int struts)
{
    Geometry a = {0, 0, screen_w, screen_h};
    if (have_randr && client) {
        int n = 0;
        XRRMonitorInfo *monitors = XRRGetMonitors(display, root, True, &n);
        int x = client->normal.x + client->normal.w / 2,
            y = client->normal.y + client->normal.h / 2;
        for (int i = 0; i < n; i++)
            if (x >= monitors[i].x && x < monitors[i].x + monitors[i].width && y >= monitors[i].y &&
                y < monitors[i].y + monitors[i].height) {
                a = (Geometry){monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height};
                break;
            }
        if (monitors)
            XRRFreeMonitors(monitors);
    }
    if (!struts)
        return a;
    int left = a.x, top = a.y, right = a.x + a.w, bottom = a.y + a.h;
    for (Client *c = clients; c; c = c->next)
        if (c->dock && on_desktop(c)) {
            unsigned long n, *s = property(c->window, "_NET_WM_STRUT_PARTIAL", XA_CARDINAL, &n);
            if (n < 12) {
                if (s)
                    XFree(s);
                s = property(c->window, "_NET_WM_STRUT", XA_CARDINAL, &n);
            }
            if (n >= 4) {
                if (n < 12 || ((int)s[4] < a.y + a.h && (int)s[5] >= a.y))
                    left = max(left, min(screen_w, s[0]));
                if (n < 12 || ((int)s[6] < a.y + a.h && (int)s[7] >= a.y))
                    right = min(right, screen_w - min(screen_w, s[1]));
                if (n < 12 || ((int)s[8] < a.x + a.w && (int)s[9] >= a.x))
                    top = max(top, min(screen_h, s[2]));
                if (n < 12 || ((int)s[10] < a.x + a.w && (int)s[11] >= a.x))
                    bottom = min(bottom, screen_h - min(screen_h, s[3]));
            }
            if (s)
                XFree(s);
        }
    return (Geometry){left, top, max(1, right - left), max(1, bottom - top)};
}
static void constrain(Client *c, Geometry *g)
{
    XSizeHints *h = &c->hints;
    int basew = h->flags & PBaseSize ? h->base_width : h->flags & PMinSize ? h->min_width : 0;
    int baseh = h->flags & PBaseSize ? h->base_height : h->flags & PMinSize ? h->min_height : 0;
    int minw = h->flags & PMinSize ? max(1, h->min_width) : 1;
    int minh = h->flags & PMinSize ? max(1, h->min_height) : 1;
    g->w = max(minw, g->w);
    g->h = max(minh, g->h);
    if (h->flags & PMaxSize) {
        if (h->max_width > 0)
            g->w = min(g->w, h->max_width);
        if (h->max_height > 0)
            g->h = min(g->h, h->max_height);
    }
    if (h->flags & PAspect) {
        if (h->min_aspect.y > 0 && h->min_aspect.x > 0 &&
            (double)g->w / g->h < (double)h->min_aspect.x / h->min_aspect.y)
            g->w = (int)((double)g->h * h->min_aspect.x / h->min_aspect.y);
        if (h->max_aspect.y > 0 && h->max_aspect.x > 0 &&
            (double)g->w / g->h > (double)h->max_aspect.x / h->max_aspect.y)
            g->h = (int)((double)g->w * h->max_aspect.y / h->max_aspect.x);
    }
    if (h->flags & PResizeInc) {
        if (h->width_inc > 0)
            g->w = basew + max(0, g->w - basew) / h->width_inc * h->width_inc;
        if (h->height_inc > 0)
            g->h = baseh + max(0, g->h - baseh) / h->height_inc * h->height_inc;
    }
    g->w = max(minw, min(32760, g->w));
    g->h = max(minh, min(32760, g->h));
}
static void notify_geometry(Client *c)
{
    XEvent e = {0};
    e.xconfigure.type = ConfigureNotify;
    e.xconfigure.display = display;
    e.xconfigure.event = c->window;
    e.xconfigure.window = c->window;
    e.xconfigure.x = c->geometry.x;
    e.xconfigure.y = c->geometry.y;
    e.xconfigure.width = c->geometry.w;
    e.xconfigure.height = c->geometry.h;
    e.xconfigure.border_width = 0;
    e.xconfigure.above = None;
    XSendEvent(display, c->window, False, StructureNotifyMask, &e);
}
static void shape(Client *c)
{
    if (!have_shape || !c->frame)
        return;
    XShapeCombineShape(display, c->frame, ShapeBounding, c->border, c->title + c->border, c->window,
                       ShapeBounding, ShapeSet);
    if (c->border || c->title) {
        int width = c->geometry.w + 2 * c->border,
            height = c->geometry.h + c->title + 2 * c->border;
        XRectangle pieces[4] = {{0, 0, width, c->title + c->border},
                                {0, c->title + c->border, c->border, c->geometry.h},
                                {width - c->border, c->title + c->border, c->border, c->geometry.h},
                                {0, height - c->border, width, c->border}};
        XShapeCombineRectangles(display, c->frame, ShapeBounding, 0, 0, pieces, 4, ShapeUnion,
                                Unsorted);
    }
}
static void configure(Client *c)
{
    Geometry g = c->normal;
    c->border = c->decorated && !c->fullscreen ? Border : 0;
    c->title = c->decorated && !c->fullscreen ? Title : 0;
    if (c->fullscreen)
        g = area(c, 0);
    else {
        Geometry a = area(c, 1);
        if (c->max_h) {
            g.x = a.x + c->border;
            g.w = a.w - 2 * c->border;
        }
        if (c->max_v) {
            g.y = a.y + c->title + c->border;
            g.h = a.h - c->title - 2 * c->border;
        }
        if (!c->max_h && !c->max_v)
            constrain(c, &g);
    }
    g.w = max(1, g.w);
    g.h = max(1, g.h);
    c->geometry = g;
    unsigned long extents[4] = {c->border, c->border, c->title + c->border, c->border};
    set_cardinals(c->window, "_NET_FRAME_EXTENTS", extents, 4);
    if (c->frame) {
        XMoveResizeWindow(display, c->frame, g.x - c->border, g.y - c->title - c->border,
                          g.w + 2 * c->border,
                          c->shaded && !c->fullscreen ? c->title + 2 * c->border
                                                      : g.h + c->title + 2 * c->border);
        XMoveResizeWindow(display, c->window, c->border, c->title + c->border, g.w, g.h);
    } else
        XMoveResizeWindow(display, c->window, g.x, g.y, g.w, g.h);
    shape(c);
    notify_geometry(c);
    draw_frame(c);
}
static void visibility(Client *c)
{
    int show = visible(c);
    if (show != c->visible) {
        if (show) {
            XMapWindow(display, c->window);
            if (c->frame)
                XMapWindow(display, c->frame);
        } else {
            if (!c->frame)
                c->ignore_unmap++;
            XUnmapWindow(display, surface(c));
        }
        c->visible = show;
    }
    state(c);
}
static void restack(void)
{
    /* Raising each layer in order keeps panels above ordinary windows. */
    for (int layer = 0; layer < 5; layer++)
        for (Client *c = clients; c; c = c->next) {
            int l = c->special      ? (c->dock ? 3 : 0)
                    : c->below      ? 0
                    : c->fullscreen ? 4
                    : c->above      ? 3
                                    : 1;
            if (c->visible && l == layer)
                XRaiseWindow(display, surface(c));
        }
    if (focused && focused->visible) {
        XRaiseWindow(display, surface(focused));
        if (!focused->fullscreen)
            for (Client *c = clients; c; c = c->next)
                if (c != focused && c->visible && (c->dock || c->above))
                    XRaiseWindow(display, surface(c));
    }
    publish_clients();
}
static void publish_clients(void)
{
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        n++;
    Window *values = calloc((size_t)max(1, n), sizeof(Window));
    if (!values)
        return;
    int i = 0;
    for (Client *c = clients; c; c = c->next)
        values[i++] = c->window;
    XChangeProperty(display, root, atom("_NET_CLIENT_LIST"), XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)values, n);
    Window r, p, *children = NULL;
    unsigned count;
    i = 0;
    if (XQueryTree(display, root, &r, &p, &children, &count)) {
        for (unsigned j = 0; j < count; j++) {
            Client *c = find(children[j]);
            if (c && i < n)
                values[i++] = c->window;
        }
        if (children)
            XFree(children);
    }
    XChangeProperty(display, root, atom("_NET_CLIENT_LIST_STACKING"), XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)values, i);
    free(values);
    Window active = focused ? focused->window : None;
    XChangeProperty(display, root, atom("_NET_ACTIVE_WINDOW"), XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&active, 1);
}
static Client *modal_for(Client *c)
{
    for (int depth = 0; c && depth < 16; depth++) {
        Client *next = NULL;
        for (Client *t = clients; t; t = t->next)
            if (t != c && t->transient == c->window && t->modal && visible(t)) {
                next = t;
                break;
            }
        if (!next)
            break;
        c = next;
    }
    return c;
}
/* WM_TAKE_FOCUS requires an actual server timestamp (CurrentTime is invalid). */
static Bool timestamp_event(Display *connection, XEvent *event, XPointer argument)
{
    (void)connection;
    Atom requested = *(Atom *)argument;
    return event->type == PropertyNotify && event->xproperty.window == support &&
           event->xproperty.atom == requested;
}
static Time server_time(void)
{
    XEvent event;
    Atom requested = atom("_RILL_TIMESTAMP");
    XChangeProperty(display, support, requested, XA_INTEGER, 8, PropModeAppend, NULL, 0);
    XIfEvent(display, &event, timestamp_event, (XPointer)&requested);
    return event.xproperty.time;
}
/* X timestamps wrap; "after" means within half a cycle ahead. */
static int time_after(Time a, Time b)
{
    return a != b && ((a - b) & 0xffffffffUL) < 0x80000000UL;
}
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
/* _NET_WM_PING: ask a client to echo a timestamp; no answer marks it hung so
   the window menu can offer a forced close. */
static void send_ping(Client *c)
{
    if (!c || !has(c->window, "WM_PROTOCOLS", "_NET_WM_PING"))
        return;
    Time t = server_time();
    XEvent e = {0};
    e.xclient.type = ClientMessage;
    e.xclient.window = c->window;
    e.xclient.message_type = atom("WM_PROTOCOLS");
    e.xclient.format = 32;
    e.xclient.data.l[0] = atom("_NET_WM_PING");
    e.xclient.data.l[1] = t;
    e.xclient.data.l[2] = root;
    XSendEvent(display, c->window, False, NoEventMask, &e);
    c->ping_time = t;
    c->ping_pending = 1;
    c->ping_sent_ms = now_ms();
}
static void ping_check(void)
{
    long now = now_ms();
    for (Client *c = clients; c; c = c->next)
        if (c->ping_pending && !c->hung && now - c->ping_sent_ms >= ping_timeout_ms) {
            c->hung = 1;
            draw_frame(c);
        }
}
static void focus(Client *c, Time time)
{
    if (time == CurrentTime)
        time = server_time();
    if (c && c->special)
        return;
    if (c) {
        c->minimized = 0;
        c->hidden_by_parent = None;
        visibility(c);
        for (Client *t = clients; t; t = t->next) {
            if (t->hidden_by_parent == c->window) {
                t->hidden_by_parent = None;
                t->minimized = 0;
                visibility(t);
            }
        }
    }
    c = modal_for(c);
    if (c && !on_desktop(c))
        return;
    Client *old = focused;
    focused = c;
    if (c) {
        c->minimized = 0;
        c->urgent = 0;
        c->focus_order = ++focus_order;
        visibility(c);
        if (c->input)
            XSetInputFocus(display, c->window, RevertToPointerRoot, time);
        protocol(c, "WM_TAKE_FOCUS", time);
        if (getenv("RILL_WM_DEBUG")) {
            Window actual;
            int revert;
            XGetInputFocus(display, &actual, &revert);
            fprintf(stderr, "rill-wm: focus %lx input=%d time=%lu actual=%lx name=%s\n", c->window,
                    c->input, time, actual, c->name);
        }
        draw_frame(c);
    } else
        XSetInputFocus(display, root, RevertToPointerRoot, time);
    if (old && old != c)
        draw_frame(old);
    restack();
}
static void focus_fallback(void)
{
    Client *best = NULL;
    for (Client *c = clients; c; c = c->next)
        if (!c->special && visible(c) && (!best || c->focus_order > best->focus_order))
            best = c;
    focus(best, CurrentTime);
}
static void switch_desktop(int value)
{
    if (value < 0 || value >= desktops || (value == desktop && !showing_desktop))
        return;
    desktop = value;
    showing_desktop = 0;
    unsigned long v = value, zero = 0;
    set_cardinals(root, "_NET_CURRENT_DESKTOP", &v, 1);
    set_cardinals(root, "_NET_SHOWING_DESKTOP", &zero, 1);
    for (Client *c = clients; c; c = c->next)
        visibility(c);
    update_workarea();
    focus_fallback();
}
static void desktop_count(int count)
{
    if (count < 1 || count > 32)
        return;
    desktops = count;
    for (Client *c = clients; c; c = c->next)
        if (c->desktop >= desktops)
            c->desktop = desktops - 1;
    unsigned long number = desktops, viewport[64] = {0};
    set_cardinals(root, "_NET_NUMBER_OF_DESKTOPS", &number, 1);
    set_cardinals(root, "_NET_DESKTOP_VIEWPORT", viewport, desktops * 2);
    char names[1024];
    int length = 0;
    for (int i = 0; i < desktops; i++)
        length += snprintf(names + length, sizeof(names) - length, "Workspace %d", i + 1) + 1;
    XChangeProperty(display, root, atom("_NET_DESKTOP_NAMES"), atom("UTF8_STRING"), 8,
                    PropModeReplace, (unsigned char *)names, length);
    switch_desktop(min(desktop, desktops - 1));
}
static void move_desktop(Client *c, int value)
{
    if (!c || value >= desktops || value < -1)
        return;
    c->desktop = value;
    for (Client *t = clients; t; t = t->next)
        if (t->transient == c->window) {
            t->desktop = value;
            visibility(t);
        }
    visibility(c);
    if (focused == c && !visible(c))
        focus_fallback();
}
static void minimize(Client *c)
{
    if (!c || c->special)
        return;
    c->minimized = 1;
    visibility(c);
    for (Client *t = clients; t; t = t->next)
        if (t->transient == c->window && !t->minimized) {
            t->hidden_by_parent = c->window;
            t->minimized = 1;
            visibility(t);
        }
    if (focused && !visible(focused))
        focus_fallback();
}
static void show_desktop(int show)
{
    showing_desktop = show;
    unsigned long v = show;
    set_cardinals(root, "_NET_SHOWING_DESKTOP", &v, 1);
    for (Client *c = clients; c; c = c->next)
        visibility(c);
    focus_fallback();
}
static void update_workarea(void)
{
    Geometry a = area(NULL, 1);
    unsigned long *data = calloc((size_t)desktops * 4, sizeof(*data));
    if (!data)
        return;
    for (int i = 0; i < desktops; i++) {
        data[i * 4] = a.x;
        data[i * 4 + 1] = a.y;
        data[i * 4 + 2] = a.w;
        data[i * 4 + 3] = a.h;
    }
    set_cardinals(root, "_NET_WORKAREA", data, desktops * 4);
    free(data);
    for (Client *c = clients; c; c = c->next)
        if (c->fullscreen || c->max_h || c->max_v)
            configure(c);
}
static void hints(Client *c)
{
    long supplied;
    memset(&c->hints, 0, sizeof(c->hints));
    XGetWMNormalHints(display, c->window, &c->hints, &supplied);
    XWMHints *h = XGetWMHints(display, c->window);
    c->input = 1;
    if (h) {
        c->input = !(h->flags & InputHint) || h->input;
        c->urgent = (h->flags & XUrgencyHint) != 0;
        XFree(h);
    }
    c->transient = None;
    XGetTransientForHint(display, c->window, &c->transient);
}
static void manage(Window window)
{
    XWindowAttributes a;
    if (window == support || find(window) || !XGetWindowAttributes(display, window, &a) ||
        a.override_redirect || a.class == InputOnly)
        return;
    Client *c = calloc(1, sizeof(*c));
    if (!c)
        return;
    c->window = window;
    c->old_border = a.border_width;
    c->dock = has(window, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK");
    c->special = c->dock || has(window, "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DESKTOP");
    c->decorated = !c->special;
    unsigned long n, *motif = property(window, "_MOTIF_WM_HINTS", atom("_MOTIF_WM_HINTS"), &n);
    if (n >= 3 && (motif[0] & 2) && motif[2] == 0)
        c->decorated = 0;
    if (motif)
        XFree(motif);
    c->desktop = c->special ? -1 : (int)cardinal(window, "_NET_WM_DESKTOP", desktop);
    if (c->desktop >= desktops)
        c->desktop = desktop;
    hints(c);
    title(c);
    Client *parent = find(c->transient);
    if (parent && !c->special)
        c->desktop = parent->desktop;
    c->normal = (Geometry){a.x, a.y, max(1, a.width), max(1, a.height)};
    if (c->decorated) {
        Geometry area_ = area(c, 1);
        if (parent && !(c->hints.flags & (USPosition | PPosition))) {
            c->normal.x = parent->geometry.x + (parent->geometry.w - a.width) / 2;
            c->normal.y = parent->geometry.y + (parent->geometry.h - a.height) / 2;
        }
        c->normal.x = max(area_.x + Border, min(c->normal.x, area_.x + area_.w - 80));
        c->normal.y = max(area_.y + Title + Border, min(c->normal.y, area_.y + area_.h - 40));
    }
#define INITIAL(field, name) c->field = has(window, "_NET_WM_STATE", "_NET_WM_STATE_" name)
    INITIAL(fullscreen, "FULLSCREEN");
    INITIAL(max_h, "MAXIMIZED_HORZ");
    INITIAL(max_v, "MAXIMIZED_VERT");
    INITIAL(above, "ABOVE");
    INITIAL(below, "BELOW");
    INITIAL(modal, "MODAL");
    INITIAL(skip_taskbar, "SKIP_TASKBAR");
    INITIAL(skip_pager, "SKIP_PAGER");
    INITIAL(shaded, "SHADED");
#undef INITIAL
    XWMHints *wh = XGetWMHints(display, window);
    if (wh) {
        if ((wh->flags & StateHint) && wh->initial_state == IconicState)
            c->minimized = 1;
        XFree(wh);
    }
    c->sync_request = has(window, "WM_PROTOCOLS", "_NET_WM_SYNC_REQUEST");
    c->sync_counter = (XID)cardinal(window, "_NET_WM_SYNC_REQUEST_COUNTER", 0);
    if (getenv("RILL_WM_DEBUG"))
        fprintf(stderr, "rill-wm: manage %lx sync=%d counter=%lu\n", window, c->sync_request,
                (unsigned long)c->sync_counter);
    Client **tail = &clients;
    while (*tail)
        tail = &(*tail)->next;
    *tail = c;
    XSelectInput(display, window,
                 PropertyChangeMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask);
    if (have_shape)
        XShapeSelectInput(display, window, ShapeNotifyMask);
    if (!c->special) {
        XSetWindowAttributes frame_attributes = {0};
        frame_attributes.colormap = a.colormap;
        frame_attributes.background_pixel = a.depth == 32 ? 0 : 0x41444b;
        frame_attributes.border_pixel = 0;
        c->frame = XCreateWindow(display, root, c->normal.x, c->normal.y, a.width, a.height, 0,
                                 a.depth, InputOutput, a.visual,
                                 CWColormap | CWBackPixel | CWBorderPixel, &frame_attributes);
        c->paint = XCreateGC(display, c->frame, 0, NULL);
        XSelectInput(display, c->frame,
                     SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask |
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
        XAddToSaveSet(display, window);
        XSetWindowBorderWidth(display, window, 0);
        if (a.map_state != IsUnmapped)
            c->ignore_unmap++;
        XReparentWindow(display, window, c->frame, Border, Title + Border);
        c->draw = XftDrawCreate(display, c->frame, a.visual, a.colormap);
        XGrabButton(display, AnyButton, AnyModifier, window, False, ButtonPressMask, GrabModeSync,
                    GrabModeAsync, None, None);
    }
    if (c->frame) {
        unsigned long opacity = cardinal(c->window, "_NET_WM_WINDOW_OPACITY", 0xffffffffUL);
        set_cardinals(c->frame, "_NET_WM_WINDOW_OPACITY", &opacity, 1);
    }
    configure(c);
    visibility(c);
    update_workarea();
    static const char *actions[] = {"_NET_WM_ACTION_MOVE",           "_NET_WM_ACTION_RESIZE",
                                    "_NET_WM_ACTION_MINIMIZE",       "_NET_WM_ACTION_MAXIMIZE_HORZ",
                                    "_NET_WM_ACTION_MAXIMIZE_VERT",  "_NET_WM_ACTION_FULLSCREEN",
                                    "_NET_WM_ACTION_CHANGE_DESKTOP", "_NET_WM_ACTION_CLOSE",
                                    "_NET_WM_ACTION_ABOVE",          "_NET_WM_ACTION_BELOW",
                                    "_NET_WM_ACTION_SHADE"};
    Atom allowed[sizeof(actions) / sizeof(actions[0])];
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        allowed[i] = atom(actions[i]);
    XChangeProperty(display, window, atom("_NET_WM_ALLOWED_ACTIONS"), XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)allowed,
                    c->special ? 0 : sizeof(allowed) / sizeof(allowed[0]));
    /* Focus new clients only when they claim a user action: no user-time
       property (legacy clients) or a timestamp at least as recent as the last
       user input. Stale or zero timestamps leave them in the background. */
    unsigned long *time_window = property(window, "_NET_WM_USER_TIME_WINDOW", XA_WINDOW, &n);
    Window user_window = n >= 1 ? (Window)time_window[0] : None;
    if (time_window)
        XFree(time_window);
    unsigned long *user_time = property(user_window ? user_window : window, "_NET_WM_USER_TIME",
                                        XA_CARDINAL, &n);
    int has_user_time = n >= 1;
    Time stamp = has_user_time ? user_time[0] : 0;
    if (user_time)
        XFree(user_time);
    if (!c->special && !c->minimized && on_desktop(c) &&
        (!has_user_time || (stamp != 0 && !time_after(last_time, stamp))))
        focus(c, CurrentTime);
    else
        restack();
}
static void unmanage(Client *c, int destroyed)
{
    if (!c)
        return;
    if (menu_client == c)
        menu_close();
    switcher_drop(c);
    if (dragged == c)
        finish_drag(0);
    int had_focus = focused == c;
    if (had_focus)
        focused = NULL;
    Client **p = &clients;
    while (*p && *p != c)
        p = &(*p)->next;
    if (*p)
        *p = c->next;
    if (!destroyed) {
        XUnmapWindow(display, surface(c));
        if (c->frame) {
            XReparentWindow(display, c->window, root, c->geometry.x, c->geometry.y);
            XRemoveFromSaveSet(display, c->window);
            XSetWindowBorderWidth(display, c->window, c->old_border);
        }
        XDeleteProperty(display, c->window, atom("WM_STATE"));
        XDeleteProperty(display, c->window, atom("_NET_FRAME_EXTENTS"));
        if (stopping)
            XMapWindow(display, c->window);
    }
    for (Client *t = clients; t; t = t->next) {
        if (t->hidden_by_parent == c->window) {
            t->hidden_by_parent = None;
            t->minimized = 0;
            visibility(t);
        }
    }
    if (c->draw)
        XftDrawDestroy(c->draw);
    if (c->paint)
        XFreeGC(display, c->paint);
    if (c->frame)
        XDestroyWindow(display, c->frame);
    free(c);
    update_workarea();
    if (had_focus && !stopping)
        focus_fallback();
    else
        publish_clients();
}
static void maximize(Client *c)
{
    if (!c || c->special)
        return;
    c->max_h = c->max_v = !(c->max_h && c->max_v);
    c->shaded = 0;
    configure(c);
    state(c);
}
static void begin_drag(Client *c, int mode, int x, int y, Time time)
{
    if (!c || c->special || c->fullscreen)
        return;
    drag_max_h = c->max_h;
    drag_max_v = c->max_v;
    if (c->max_h || c->max_v) {
        c->max_h = c->max_v = 0;
        configure(c);
        state(c);
    }
    dragged = c;
    drag_mode = mode;
    drag_root_x = x;
    drag_root_y = y;
    drag_start = c->normal;
    XGrabPointer(display, root, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                 GrabModeAsync, None, mode == 8 || mode == 10 ? move_cursor : resize_cursor, time);
    XGrabKeyboard(display, root, False, GrabModeAsync, GrabModeAsync, time);
}
static void finish_drag(int cancel)
{
    if (!dragged)
        return;
    if (cancel) {
        dragged->normal = drag_start;
        dragged->max_h = drag_max_h;
        dragged->max_v = drag_max_v;
        configure(dragged);
        state(dragged);
    }
    dragged = NULL;
    XUngrabPointer(display, CurrentTime);
    XUngrabKeyboard(display, CurrentTime);
}
static int motion(int x, int y)
{
    if (!dragged)
        return 0;
    /* Sync resize: wait for the client to acknowledge the previous resize by
       settling its counter, with a short timeout so non-acking clients still
       resize. */
    if (drag_mode != 8 && drag_mode != 10 && dragged->sync_request && dragged->sync_counter &&
        have_sync && dragged->sync_serial) {
        XSyncValue value;
        if (XSyncQueryCounter(display, dragged->sync_counter, &value) &&
            (unsigned long)XSyncValueLow32(value) < dragged->sync_serial &&
            now_ms() - dragged->sync_sent_ms < 100)
            return 1;
    }
    int dx = x - drag_root_x, dy = y - drag_root_y;
    Geometry g = drag_start;
    if (drag_mode == 8 || drag_mode == 10) {
        g.x += dx;
        g.y += dy;
        Geometry a = area(dragged, 1);
        if (abs(g.x - dragged->border - a.x) < 12)
            g.x = a.x + dragged->border;
        if (abs(g.y - dragged->title - dragged->border - a.y) < 12)
            g.y = a.y + dragged->title + dragged->border;
        if (abs(g.x + g.w + dragged->border - a.x - a.w) < 12)
            g.x = a.x + a.w - g.w - dragged->border;
        if (abs(g.y + g.h + dragged->border - a.y - a.h) < 12)
            g.y = a.y + a.h - g.h - dragged->border;
    } else {
        int left = drag_mode == 0 || drag_mode == 6 || drag_mode == 7;
        int top = drag_mode == 0 || drag_mode == 1 || drag_mode == 2;
        if (left) {
            g.x += dx;
            g.w -= dx;
        } else if (drag_mode != 1 && drag_mode != 5)
            g.w += dx;
        if (top) {
            g.y += dy;
            g.h -= dy;
        } else if (drag_mode != 3 && drag_mode != 7)
            g.h += dy;
        constrain(dragged, &g);
        if (left)
            g.x = drag_start.x + drag_start.w - g.w;
        if (top)
            g.y = drag_start.y + drag_start.h - g.h;
    }
    dragged->normal = g;
    configure(dragged);
    if (drag_mode != 8 && drag_mode != 10 && dragged->sync_request && dragged->sync_counter &&
        have_sync) {
        dragged->sync_serial = (dragged->sync_serial + 2) | 1;
        XSyncValue value;
        XSyncIntsToValue(&value, (int)dragged->sync_serial, 0);
        XSyncSetCounter(display, dragged->sync_counter, value);
        dragged->sync_sent_ms = now_ms();
        if (getenv("RILL_WM_DEBUG"))
            fprintf(stderr, "rill-wm: sync resize counter=%lu serial=%lu\n",
                    (unsigned long)dragged->sync_counter, dragged->sync_serial);
    }
    return 0;
}
/* MRU Alt+Tab switcher: a centered override-redirect panel painted in software
   (background and icons through one XImage, titles through Xft) that commits
   focus when Alt is released. */
enum { SwitcherTileW = 96, SwitcherTileH = 84, SwitcherIcon = 36, SwitcherColumns = 8 };
#define SwitcherMax 64
static Window switcher_window;
static XftDraw *switcher_draw;
static XImage *switcher_image;
static Client *switcher_items[SwitcherMax];
static int switcher_count, switcher_index, switcher_active;
static KeyCode alt_keys[16];
static int alt_key_count;

static void switcher_paint(void);
static int switcher_collect(Client **items, int max)
{
    int n = 0;
    for (Client *c = clients; c && n < max; c = c->next) {
        if (c->special || c->skip_taskbar || !on_desktop(c))
            continue;
        int i = n++;
        while (i > 0 && c->focus_order > items[i - 1]->focus_order) {
            items[i] = items[i - 1];
            i--;
        }
        items[i] = c;
    }
    return n;
}
static unsigned long *icon_data(Window w, unsigned long *count)
{
    Atom actual;
    int format;
    unsigned long remaining;
    unsigned char *data = NULL;
    *count = 0;
    long length = 4096;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (XGetWindowProperty(display, w, atom("_NET_WM_ICON"), 0, length, False, XA_CARDINAL,
                               &actual, &format, count, &remaining, &data) != Success ||
            actual != XA_CARDINAL || format != 32) {
            if (data)
                XFree(data);
            *count = 0;
            return NULL;
        }
        if (!remaining)
            return (unsigned long *)data;
        XFree(data);
        data = NULL;
        length = 262160; /* 256x256 ARGB icons fit after the cheap first read. */
    }
    *count = 0;
    return NULL;
}
static const unsigned long *icon_best(const unsigned long *data, unsigned long count, int size,
                                      unsigned long *icon_w, unsigned long *icon_h)
{
    const unsigned long *best = NULL;
    unsigned long best_score = ~0UL, i = 0;
    while (i + 1 < count) {
        unsigned long w = data[i], h = data[i + 1];
        if (w == 0 || h == 0 || w > 512 || h > 512 || w * h > count - i - 2)
            break;
        unsigned long score = w >= (unsigned)size && h >= (unsigned)size
                                  ? (w - (unsigned)size) + (h - (unsigned)size)
                                  : ((unsigned)size - w) + ((unsigned)size - h) + (1UL << 20);
        if (score < best_score) {
            best_score = score;
            *icon_w = w;
            *icon_h = h;
            best = data + i;
        }
        i += 2 + w * h;
    }
    return best;
}
static unsigned long blend(unsigned long pixel, unsigned long argb)
{
    unsigned a = (argb >> 24) & 0xff;
    if (a == 0)
        return pixel;
    unsigned r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    unsigned pr = (pixel >> 16) & 0xff, pg = (pixel >> 8) & 0xff, pb = pixel & 0xff;
    r = (r * a + pr * (255 - a) + 127) / 255;
    g = (g * a + pg * (255 - a) + 127) / 255;
    b = (b * a + pb * (255 - a) + 127) / 255;
    return (r << 16) | (g << 8) | b;
}
static void switcher_paint(void)
{
    if (!switcher_active || !switcher_window)
        return;
    int cols = min(max(1, switcher_count), SwitcherColumns);
    int rows = max(1, (switcher_count + cols - 1) / cols);
    int w = cols * SwitcherTileW + 16, h = rows * SwitcherTileH + 16;
    if (!switcher_image || switcher_image->width != w || switcher_image->height != h) {
        if (switcher_image)
            XDestroyImage(switcher_image);
        char *buffer = calloc((size_t)w * h, 4);
        if (!buffer)
            return;
        switcher_image = XCreateImage(display, DefaultVisual(display, screen_number),
                                      DefaultDepth(display, screen_number), ZPixmap, 0, buffer, w, h,
                                      32, 0);
        if (!switcher_image) {
            free(buffer);
            return;
        }
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            XPutPixel(switcher_image, x, y, 0x26292f);
    for (int i = 0; i < switcher_count; i++) {
        int tx = 8 + (i % cols) * SwitcherTileW, ty = 8 + (i / cols) * SwitcherTileH;
        unsigned long tile = i == switcher_index ? 0x344c6b : 0x30343c;
        for (int y = 0; y < SwitcherTileH; y++)
            for (int x = 0; x < SwitcherTileW; x++)
                XPutPixel(switcher_image, tx + x, ty + y, tile);
        unsigned long count, iw = 0, ih = 0;
        unsigned long *icons = icon_data(switcher_items[i]->window, &count);
        const unsigned long *best = icons ? icon_best(icons, count, SwitcherIcon, &iw, &ih) : NULL;
        int ix = tx + (SwitcherTileW - SwitcherIcon) / 2, iy = ty + 8;
        for (int y = 0; y < SwitcherIcon; y++)
            for (int x = 0; x < SwitcherIcon; x++) {
                unsigned long argb = 0;
                if (best)
                    argb = best[2 + (unsigned long)(y * ih / SwitcherIcon) * iw +
                                (unsigned long)x * iw / SwitcherIcon];
                XPutPixel(switcher_image, ix + x, iy + y,
                          blend(tile, best ? argb : 0));
            }
        if (!best) {
            /* Generic window glyph for clients without an icon. */
            for (int x = 0; x < 26; x++) {
                XPutPixel(switcher_image, ix + 5 + x, iy + 6, blend(tile, 0xffb8bcc8));
                XPutPixel(switcher_image, ix + 5 + x, iy + 30, blend(tile, 0xffb8bcc8));
            }
            for (int y = 0; y < 25; y++) {
                XPutPixel(switcher_image, ix + 5, iy + 6 + y, blend(tile, 0xffb8bcc8));
                XPutPixel(switcher_image, ix + 30, iy + 6 + y, blend(tile, 0xffb8bcc8));
            }
            for (int x = 0; x < 19; x++)
                XPutPixel(switcher_image, ix + 8 + x, iy + 10, blend(tile, 0xffb8bcc8));
        }
        if (icons)
            XFree(icons);
    }
    XPutImage(display, switcher_window, gc, switcher_image, 0, 0, 0, 0, w, h);
    if (!switcher_draw || !font)
        return;
    for (int i = 0; i < switcher_count; i++) {
        int tx = 8 + (i % cols) * SwitcherTileW, ty = 8 + (i / cols) * SwitcherTileH;
        const char *name = switcher_items[i]->name;
        int length = (int)strlen(name);
        while (length > 1) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(display, font, (const FcChar8 *)name, length, &extents);
            if (extents.xOff <= SwitcherTileW - 10)
                break;
            length--;
        }
        XRectangle clip = {(short)(tx + 2), (short)(ty + SwitcherTileH - 20),
                           (unsigned short)(SwitcherTileW - 4), 16};
        XftDrawSetClipRectangles(switcher_draw, 0, 0, &clip, 1);
        XftDrawStringUtf8(switcher_draw, &text_color, font, tx + 5, ty + SwitcherTileH - 8,
                          (const FcChar8 *)name, length);
        XftDrawSetClip(switcher_draw, NULL);
    }
}
static void switcher_close_overlay(void)
{
    if (!switcher_active)
        return;
    switcher_active = 0;
    XUnmapWindow(display, switcher_window);
    XUngrabKeyboard(display, CurrentTime);
}
static void switcher_step(int delta)
{
    if (!switcher_active || switcher_count < 2)
        return;
    switcher_index = ((switcher_index + delta) % switcher_count + switcher_count) % switcher_count;
    switcher_paint();
}
static void switcher_activate(void)
{
    Client *target = switcher_items[switcher_index];
    switcher_close_overlay();
    if (target)
        focus(target, last_time);
}
static void switcher_cancel(void)
{
    switcher_close_overlay();
}
static void switcher_drop(Client *c)
{
    if (!switcher_active)
        return;
    int shift = 0;
    for (int i = 0; i < switcher_count; i++) {
        if (switcher_items[i] == c)
            shift = 1;
        else if (shift)
            switcher_items[i - 1] = switcher_items[i];
    }
    if (!shift)
        return;
    switcher_count--;
    if (!switcher_count)
        switcher_cancel();
    else {
        if (switcher_index >= switcher_count)
            switcher_index = switcher_count - 1;
        switcher_paint();
    }
}
static void switcher_open(int backwards, Time time)
{
    if (switcher_active) {
        switcher_step(backwards ? -1 : 1);
        return;
    }
    switcher_count = switcher_collect(switcher_items, SwitcherMax);
    if (!switcher_count)
        return;
    switcher_index = switcher_count == 1 ? 0 : (backwards ? switcher_count - 1 : 1);
    if (!switcher_window) {
        XSetWindowAttributes a = {0};
        a.override_redirect = True;
        a.background_pixel = 0x26292f;
        a.event_mask = ExposureMask;
        switcher_window = XCreateWindow(display, root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                                        CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask,
                                        &a);
        switcher_draw = XftDrawCreate(display, switcher_window, DefaultVisual(display, screen_number),
                                      DefaultColormap(display, screen_number));
    }
    switcher_active = 1;
    int cols = min(max(1, switcher_count), SwitcherColumns);
    int rows = max(1, (switcher_count + cols - 1) / cols);
    int w = cols * SwitcherTileW + 16, h = rows * SwitcherTileH + 16;
    Geometry a = focused && !focused->special ? area(focused, 0) : area(NULL, 0);
    XMoveResizeWindow(display, switcher_window, a.x + (a.w - w) / 2, a.y + (a.h - h) / 2, w, h);
    XMapRaised(display, switcher_window);
    XGrabKeyboard(display, root, False, GrabModeAsync, GrabModeAsync, time);
    switcher_paint();
}
static void switcher_key(KeySym k, unsigned m)
{
    if (k == XK_Tab && (m & Mod1Mask)) {
        switcher_step((m & ShiftMask) ? -1 : 1);
        return;
    }
    if (k == XK_Left || k == XK_Up)
        switcher_step(-1);
    else if (k == XK_Right || k == XK_Down)
        switcher_step(1);
    else if (k == XK_Return || k == XK_space)
        switcher_activate();
    else if (k == XK_Escape)
        switcher_cancel();
}
static void tile(Client *c, KeySym key)
{
    if (!c || c->special)
        return;
    c->fullscreen = c->max_h = c->max_v = 0;
    c->shaded = 0;
    Geometry a = area(c, 1);
    if (key == XK_Left || key == XK_Right) {
        a.w /= 2;
        if (key == XK_Right)
            a.x += a.w;
    } else {
        a.h /= 2;
        if (key == XK_Down)
            a.y += a.h;
    }
    c->normal =
        (Geometry){a.x + Border, a.y + Title + Border, a.w - 2 * Border, a.h - Title - 2 * Border};
    configure(c);
    state(c);
}
/* Key bindings: "action = modifiers+key" lines in $XDG_CONFIG_HOME/rill/wm-keys
   (or RILL_WM_KEYS) override the defaults; unlisted actions keep theirs. */
enum {
    KeyClose,
    KeyMinimize,
    KeyMaximize,
    KeyFullscreen,
    KeyMove,
    KeyResize,
    KeyCycle,
    KeyCycleBack,
    KeyWindowMenu,
    KeyWorkspacePrev,
    KeyWorkspaceNext,
    KeyWindowWorkspacePrev,
    KeyWindowWorkspaceNext,
    KeyTileLeft,
    KeyTileRight,
    KeyTileUp,
    KeyTileDown,
    KeyShowDesktop,
    KeyActionCount
};
typedef struct {
    int action;
    KeySym keysym;
    unsigned modifiers;
} Binding;
static const struct {
    int action;
    const char *name;
    KeySym keysym;
    unsigned modifiers;
} default_bindings[] = {
    {KeyClose, "close", XK_F4, Mod1Mask},
    {KeyMinimize, "minimize", XK_F9, Mod1Mask},
    {KeyMaximize, "maximize", XK_F10, Mod1Mask},
    {KeyFullscreen, "fullscreen", XK_F11, Mod1Mask},
    {KeyMove, "move", XK_F7, Mod1Mask},
    {KeyResize, "resize", XK_F8, Mod1Mask},
    {KeyCycle, "cycle", XK_Tab, Mod1Mask},
    {KeyCycleBack, "cycle-back", XK_Tab, Mod1Mask | ShiftMask},
    {KeyWindowMenu, "window-menu", XK_space, Mod1Mask},
    {KeyWorkspacePrev, "workspace-prev", XK_Left, ControlMask | Mod1Mask},
    {KeyWorkspaceNext, "workspace-next", XK_Right, ControlMask | Mod1Mask},
    {KeyWindowWorkspacePrev, "window-workspace-prev", XK_Left, ControlMask | Mod1Mask | ShiftMask},
    {KeyWindowWorkspaceNext, "window-workspace-next", XK_Right, ControlMask | Mod1Mask | ShiftMask},
    {KeyTileLeft, "tile-left", XK_Left, Mod4Mask},
    {KeyTileRight, "tile-right", XK_Right, Mod4Mask},
    {KeyTileUp, "tile-up", XK_Up, Mod4Mask},
    {KeyTileDown, "tile-down", XK_Down, Mod4Mask},
    {KeyShowDesktop, "show-desktop", XK_d, Mod4Mask},
    {KeyShowDesktop, "show-desktop", XK_d, ControlMask | Mod1Mask},
};
static Binding bindings[sizeof(default_bindings) / sizeof(default_bindings[0])];
static int binding_count;
static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                          end[-1] == '\r'))
        *--end = 0;
    return text;
}
static int parse_binding(char *line, Binding *out)
{
    char *equals = strchr(line, '=');
    if (!equals)
        return 0;
    *equals = 0;
    char *name = trim(line), *combo = trim(equals + 1);
    for (unsigned i = 0; i < sizeof(default_bindings) / sizeof(default_bindings[0]); i++)
        if (strcmp(default_bindings[i].name, name) == 0)
            out->action = default_bindings[i].action;
    if (out->action < 0)
        return 0;
    out->modifiers = 0;
    out->keysym = NoSymbol;
    char *save = NULL;
    for (char *token = strtok_r(combo, "+", &save); token; token = strtok_r(NULL, "+", &save)) {
        char *part = trim(token);
        if (strcasecmp(part, "ctrl") == 0 || strcasecmp(part, "control") == 0)
            out->modifiers |= ControlMask;
        else if (strcasecmp(part, "shift") == 0)
            out->modifiers |= ShiftMask;
        else if (strcasecmp(part, "alt") == 0 || strcasecmp(part, "meta") == 0)
            out->modifiers |= Mod1Mask;
        else if (strcasecmp(part, "super") == 0 || strcasecmp(part, "mod4") == 0)
            out->modifiers |= Mod4Mask;
        else {
            KeySym symbol = XStringToKeysym(part);
            if (symbol == NoSymbol)
                return 0;
            out->keysym = symbol;
        }
    }
    return out->keysym != NoSymbol;
}
static void load_bindings(const char *path)
{
    binding_count = 0;
    for (unsigned i = 0; i < sizeof(default_bindings) / sizeof(default_bindings[0]); i++) {
        bindings[binding_count].action = default_bindings[i].action;
        bindings[binding_count].keysym = default_bindings[i].keysym;
        bindings[binding_count].modifiers = default_bindings[i].modifiers;
        binding_count++;
    }
    FILE *file = fopen(path, "r");
    if (!file)
        return;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *comment = strchr(line, '#');
        if (comment)
            *comment = 0;
        Binding parsed = {-1, NoSymbol, 0};
        if (!parse_binding(line, &parsed))
            continue;
        int replaced = 0;
        for (int i = 0; i < binding_count && !replaced; i++)
            if (bindings[i].action == parsed.action &&
                bindings[i].modifiers == parsed.modifiers) {
                bindings[i].keysym = parsed.keysym;
                replaced = 1;
            }
        if (!replaced)
            for (int i = 0; i < binding_count && !replaced; i++)
                if (bindings[i].action == parsed.action) {
                    bindings[i].keysym = parsed.keysym;
                    bindings[i].modifiers = parsed.modifiers;
                    replaced = 1;
                }
    }
    fclose(file);
}
static int binding_for(KeySym keysym, unsigned modifiers)
{
    for (int i = 0; i < binding_count; i++)
        if (bindings[i].keysym == keysym && bindings[i].modifiers == modifiers)
            return bindings[i].action;
    return -1;
}
static void switch_workspace(int direction)
{
    switch_desktop((desktop + (direction > 0 ? 1 : desktops - 1)) % desktops);
}
static void move_and_switch_workspace(Client *c, int direction)
{
    int d = (desktop + (direction > 0 ? 1 : desktops - 1)) % desktops;
    move_desktop(c, d);
    switch_desktop(d);
}
static void toggle_fullscreen(Client *c)
{
    if (!c || c->special)
        return;
    c->fullscreen = !c->fullscreen;
    configure(c);
    state(c);
    restack();
}
static void key(XKeyEvent *e)
{
    KeySym k = XLookupKeysym(e, 0);
    unsigned m = e->state &
                 ~(LockMask | numlock_mask | Button1Mask | Button2Mask | Button3Mask |
                   Button4Mask | Button5Mask);
    last_time = e->time;
    if (switcher_active) {
        switcher_key(k, m);
        return;
    }
    if (menu_client) {
        if (k == XK_Escape)
            menu_close();
        else if (k == XK_Return || k == XK_space)
            menu_activate();
        else if (k == XK_Up || k == XK_Down) {
            int items = menu_items(menu_client);
            menu_item = (menu_item + (k == XK_Down ? 1 : items - 1)) % items;
            menu_paint();
        }
        return;
    }
    if (dragged) {
        if (k == XK_Escape) {
            finish_drag(1);
            return;
        }
        if (k == XK_Return) {
            finish_drag(0);
            return;
        }
        int step = (m & ControlMask) ? 1 : 10;
        /* Keyboard operation works directly on the saved geometry. */
        Geometry g = dragged->normal;
        int dx = k == XK_Left    ? -step
                 : k == XK_Right ? step
                                 : 0,
            dy = k == XK_Up     ? -step
                 : k == XK_Down ? step
                                : 0;
        if (drag_mode == 8 || drag_mode == 10) {
            g.x += dx;
            g.y += dy;
        } else {
            g.w += dx;
            g.h += dy;
        }
        dragged->normal = g;
        configure(dragged);
        return;
    }
    switch (binding_for(k, m)) {
    case KeyCycle:
        switcher_open(0, e->time);
        break;
    case KeyCycleBack:
        switcher_open(1, e->time);
        break;
    case KeyClose:
        close_client(focused, e->time);
        break;
    case KeyMinimize:
        minimize(focused);
        break;
    case KeyMaximize:
        maximize(focused);
        break;
    case KeyFullscreen:
        toggle_fullscreen(focused);
        break;
    case KeyMove:
        begin_drag(focused, 10, e->x_root, e->y_root, e->time);
        break;
    case KeyResize:
        begin_drag(focused, 9, e->x_root, e->y_root, e->time);
        break;
    case KeyWindowMenu:
        if (focused)
            menu_open(focused, focused->geometry.x, focused->geometry.y, e->time);
        break;
    case KeyWorkspacePrev:
        switch_workspace(-1);
        break;
    case KeyWorkspaceNext:
        switch_workspace(1);
        break;
    case KeyWindowWorkspacePrev:
        move_and_switch_workspace(focused, -1);
        break;
    case KeyWindowWorkspaceNext:
        move_and_switch_workspace(focused, 1);
        break;
    case KeyTileLeft:
        tile(focused, XK_Left);
        break;
    case KeyTileRight:
        tile(focused, XK_Right);
        break;
    case KeyTileUp:
        tile(focused, XK_Up);
        break;
    case KeyTileDown:
        tile(focused, XK_Down);
        break;
    case KeyShowDesktop:
        show_desktop(!showing_desktop);
        break;
    }
}
static void grab_keys(void)
{
    XUngrabKey(display, AnyKey, AnyModifier, root);
    XModifierKeymap *map = XGetModifierMapping(display);
    numlock_mask = 0;
    alt_key_count = 0;
    KeyCode num = XKeysymToKeycode(display, XK_Num_Lock);
    if (map) {
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < map->max_keypermod; j++) {
                KeyCode code = map->modifiermap[i * map->max_keypermod + j];
                if (code == num)
                    numlock_mask = 1u << i;
                if (i == 3 && code && alt_key_count < (int)(sizeof(alt_keys) / sizeof(alt_keys[0])))
                    alt_keys[alt_key_count++] = code;
            }
        XFreeModifiermap(map);
    }
    unsigned locks[] = {0, LockMask, numlock_mask, LockMask | numlock_mask};
    for (int i = 0; i < binding_count; i++)
        for (unsigned j = 0; j < 4; j++)
            XGrabKey(display, XKeysymToKeycode(display, bindings[i].keysym),
                     bindings[i].modifiers | locks[j], root, True, GrabModeAsync, GrabModeAsync);
}
static void button(XButtonEvent *e)
{
    last_time = e->time;
    if (menu_client) {
        if (e->button == 1 && e->x >= 0 && e->x < 230 && e->y >= 0 &&
            e->y < menu_items(menu_client) * 28) {
            menu_item = e->y / 28;
            menu_activate();
        } else if (e->button == 1)
            menu_close();
        return;
    }
    Client *c = find(e->window);
    if (!c || c->special) {
        XAllowEvents(display, ReplayPointer, e->time);
        return;
    }
    focus(c, e->time);
    if ((e->state & Mod1Mask) && (e->button == 1 || e->button == 3)) {
        XAllowEvents(display, AsyncPointer, e->time);
        begin_drag(c, e->button == 1 ? 8 : 4, e->x_root, e->y_root, e->time);
        return;
    }
    if (e->window == c->window) {
        XAllowEvents(display, ReplayPointer, e->time);
        return;
    }
    if (e->button == 4 || e->button == 5) {
        c->shaded = e->button == 4;
        configure(c);
        state(c);
        return;
    }
    if (e->button == 3 && e->window == c->frame) {
        menu_open(c, e->x_root, e->y_root, e->time);
        return;
    }
    if (e->button != 1)
        return;
    int width = c->geometry.w + 2 * c->border;
    int n_left = (int)strlen(button_left), n_right = (int)strlen(button_right);
    if (e->y < c->title + c->border) {
        if (e->x >= width - n_right * Button) {
            int index = (width - e->x) / Button;
            if (index < n_right)
                activate_button(c, button_right[n_right - 1 - index], e->x_root, e->y_root,
                                e->time);
            return;
        }
        if (e->x < n_left * Button) {
            int index = e->x / Button;
            if (index < n_left)
                activate_button(c, button_left[index], e->x_root, e->y_root, e->time);
            return;
        }
    }
    if (e->y < c->title + c->border && e->x >= n_left * Button && e->x < width - n_right * Button) {
        if (last_click_window == c->window && e->time - last_click_time < 300) {
            if (double_click == DoubleClickShade && c->decorated) {
                c->shaded = !c->shaded;
                configure(c);
                state(c);
            } else if (double_click == DoubleClickMaximize)
                maximize(c);
            last_click_time = 0;
            return;
        }
        last_click_time = e->time;
        last_click_window = c->window;
        begin_drag(c, 8, e->x_root, e->y_root, e->time);
        return;
    }
    int left = e->x < c->border, right = e->x >= width - c->border;
    int top = e->y < c->border;
    int mode = top ? (left ? 0 : right ? 2 : 1) : left ? 7 : right ? 3 : 5;
    if (e->y >= c->geometry.h + c->title + c->border)
        mode = left ? 6 : right ? 4 : 5;
    begin_drag(c, mode, e->x_root, e->y_root, e->time);
}
static int menu_items(Client *c)
{
    return 9 + (c && c->hung ? 1 : 0);
}
static void menu_close(void)
{
    if (!menu_client)
        return;
    menu_client = NULL;
    XUnmapWindow(display, menu_window);
    XUngrabPointer(display, CurrentTime);
    XUngrabKeyboard(display, CurrentTime);
}
static void menu_paint(void)
{
    if (!menu_client)
        return;
    Client *c = menu_client;
    int items = menu_items(c);
    const char *labels[10] = {c->max_h && c->max_v ? "Restore" : "Maximize",
                              "Minimize",
                              c->fullscreen ? "Leave fullscreen" : "Fullscreen",
                              c->shaded ? "Unroll" : "Roll up",
                              c->above ? "Normal stacking" : "Always on top",
                              c->desktop < 0 ? "Only this workspace" : "All workspaces",
                              "Move to previous workspace",
                              "Move to next workspace",
                              "Close",
                              c->hung ? "Force close" : ""};
    for (int i = 0; i < items; i++) {
        XSetForeground(display, gc, i == menu_item ? 0x344c6b : 0x30343c);
        XFillRectangle(display, menu_window, gc, 0, i * 28, 230, 28);
        if (font && menu_draw)
            XftDrawStringUtf8(menu_draw, &text_color, font, 9, i * 28 + 19,
                              (const FcChar8 *)labels[i], strlen(labels[i]));
    }
}
static void menu_open(Client *c, int x, int y, Time time)
{
    if (!c || c->special)
        return;
    finish_drag(0);
    if (!menu_window) {
        XSetWindowAttributes a = {0};
        a.override_redirect = True;
        a.background_pixel = 0x30343c;
        a.event_mask =
            ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
        menu_window =
            XCreateWindow(display, root, 0, 0, 230, 9 * 28, 0, CopyFromParent, InputOutput,
                          CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
        menu_draw = XftDrawCreate(display, menu_window, DefaultVisual(display, screen_number),
                                  DefaultColormap(display, screen_number));
    }
    menu_client = c;
    menu_item = 0;
    int rows = menu_items(c);
    XMoveResizeWindow(display, menu_window, max(0, min(x, screen_w - 230)),
                      max(0, min(y, screen_h - rows * 28)), 230, rows * 28);
    XMapRaised(display, menu_window);
    XGrabPointer(display, menu_window, False,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                 GrabModeAsync, None, None, time);
    XGrabKeyboard(display, menu_window, False, GrabModeAsync, GrabModeAsync, time);
    menu_paint();
}
static void menu_activate(void)
{
    Client *c = menu_client;
    int item = menu_item;
    menu_close();
    if (!c)
        return;
    switch (item) {
    case 0:
        maximize(c);
        break;
    case 1:
        minimize(c);
        break;
    case 2:
        c->fullscreen = !c->fullscreen;
        configure(c);
        state(c);
        restack();
        break;
    case 3:
        if (c->decorated) {
            c->shaded = !c->shaded;
            configure(c);
            state(c);
        }
        break;
    case 4:
        c->above = !c->above;
        c->below = 0;
        state(c);
        restack();
        break;
    case 5:
        move_desktop(c, c->desktop < 0 ? desktop : -1);
        break;
    case 6:
        move_desktop(c, (desktop + desktops - 1) % desktops);
        break;
    case 7:
        move_desktop(c, (desktop + 1) % desktops);
        break;
    case 8:
        close_client(c, last_time);
        break;
    case 9:
        XKillClient(display, c->window);
        break;
    }
}
static void change_state(Client *c, Atom value, long action)
{
    if (!c || c->special || action < 0 || action > 2)
        return;
    int *field = NULL;
#define FIELD(member, name)                                                                        \
    if (value == atom("_NET_WM_STATE_" name))                                                      \
    field = &c->member
    FIELD(fullscreen, "FULLSCREEN");
    FIELD(max_h, "MAXIMIZED_HORZ");
    FIELD(max_v, "MAXIMIZED_VERT");
    FIELD(above, "ABOVE");
    FIELD(below, "BELOW");
    FIELD(shaded, "SHADED");
    FIELD(modal, "MODAL");
    FIELD(skip_taskbar, "SKIP_TASKBAR");
    FIELD(skip_pager, "SKIP_PAGER");
    FIELD(urgent, "DEMANDS_ATTENTION");
#undef FIELD
    if (value == atom("_NET_WM_STATE_STICKY")) {
        int sticky = c->desktop < 0;
        sticky = action == 2 ? !sticky : action == 1;
        move_desktop(c, sticky ? -1 : desktop);
    }
    if (field)
        *field = action == 2 ? !*field : action == 1;
    if (field == &c->above && c->above)
        c->below = 0;
    if (field == &c->below && c->below)
        c->above = 0;
}
static void message(XClientMessageEvent *e)
{
    if (e->format != 32)
        return;
    if (e->message_type == atom("WM_PROTOCOLS") && e->window == root &&
        (Atom)e->data.l[0] == atom("_NET_WM_PING")) {
        for (Client *c = clients; c; c = c->next)
            if (c->ping_pending && (Time)e->data.l[1] == c->ping_time) {
                c->ping_pending = 0;
                if (c->hung) {
                    c->hung = 0;
                    draw_frame(c);
                }
            }
        return;
    }
    Client *c = find(e->window);
    if (e->message_type == atom("_NET_NUMBER_OF_DESKTOPS"))
        desktop_count(e->data.l[0]);
    else if (e->message_type == atom("_NET_CURRENT_DESKTOP"))
        switch_desktop(e->data.l[0]);
    else if (e->message_type == atom("_NET_SHOWING_DESKTOP"))
        show_desktop(e->data.l[0] != 0);
    else if (e->message_type == atom("_NET_ACTIVE_WINDOW") && c) {
        Time stamp = (Time)e->data.l[1];
        /* Pagers may activate directly. Applications may only take focus with
           a current timestamp; stale or zero requests raise the urgent hint. */
        if (e->data.l[0] == 1 && c != focused && (stamp == 0 || time_after(last_time, stamp))) {
            if (!c->urgent) {
                c->urgent = 1;
                draw_frame(c);
            }
            state(c);
            return;
        }
        if (showing_desktop)
            show_desktop(0);
        if (c->desktop >= 0 && !on_desktop(c))
            switch_desktop(c->desktop);
        focus(c, stamp);
    } else if (e->message_type == atom("_NET_CLOSE_WINDOW"))
        close_client(c, e->data.l[0]);
    else if (e->message_type == atom("WM_CHANGE_STATE") && e->data.l[0] == IconicState)
        minimize(c);
    else if (e->message_type == atom("_NET_WM_DESKTOP"))
        move_desktop(c, (e->data.l[0] & 0xffffffffUL) == 0xffffffffUL ? -1 : e->data.l[0]);
    else if (e->message_type == atom("_NET_WM_STATE") && c) {
        change_state(c, e->data.l[1], e->data.l[0]);
        if (e->data.l[2] != e->data.l[1])
            change_state(c, e->data.l[2], e->data.l[0]);
        configure(c);
        state(c);
        restack();
    } else if (e->message_type == atom("_NET_REQUEST_FRAME_EXTENTS")) {
        unsigned long extents[4] = {Border, Border, Title + Border, Border};
        set_cardinals(e->window, "_NET_FRAME_EXTENTS", extents, 4);
    } else if (e->message_type == atom("_NET_WM_MOVERESIZE") && c) {
        if (e->data.l[2] == 11)
            finish_drag(1);
        else if (e->data.l[2] >= 0 && e->data.l[2] <= 10)
            begin_drag(c, e->data.l[2], e->data.l[0], e->data.l[1], last_time);
    } else if (e->message_type == atom("_NET_MOVERESIZE_WINDOW") && c && !c->special) {
        unsigned flags = e->data.l[0];
        if (flags & (1 << 8))
            c->normal.x = e->data.l[1];
        if (flags & (1 << 9))
            c->normal.y = e->data.l[2];
        if (flags & (1 << 10))
            c->normal.w = e->data.l[3];
        if (flags & (1 << 11))
            c->normal.h = e->data.l[4];
        configure(c);
    }
}
static void configuration(XConfigureRequestEvent *e)
{
    Client *c = find(e->window);
    if (!c || c->special) {
        XWindowChanges values = {e->x,     e->y,     e->width, e->height, e->border_width,
                                 e->above, e->detail};
        XConfigureWindow(display, e->window, e->value_mask, &values);
        if (c) {
            if (e->value_mask & CWX)
                c->normal.x = e->x;
            if (e->value_mask & CWY)
                c->normal.y = e->y;
            if (e->value_mask & CWWidth)
                c->normal.w = e->width;
            if (e->value_mask & CWHeight)
                c->normal.h = e->height;
            c->geometry = c->normal;
        }
        return;
    }
    if (!c->fullscreen && !c->max_h && !c->max_v) {
        if (e->value_mask & CWX)
            c->normal.x = e->x;
        if (e->value_mask & CWY)
            c->normal.y = e->y;
        if (e->value_mask & CWWidth)
            c->normal.w = e->width;
        if (e->value_mask & CWHeight)
            c->normal.h = e->height;
    }
    if ((e->value_mask & CWStackMode) && e->detail == Above && c == focused)
        restack();
    configure(c);
}
static void event(XEvent *e)
{
    Client *c = find(e->xany.window);
    switch (e->type) {
    case MapRequest:
        c = find(e->xmaprequest.window);
        if (c) {
            c->minimized = 0;
            visibility(c);
            focus(c, CurrentTime);
        } else
            manage(e->xmaprequest.window);
        break;
    case ConfigureRequest:
        configuration(&e->xconfigurerequest);
        break;
    case DestroyNotify:
        c = find(e->xdestroywindow.window);
        if (c && c->window == e->xdestroywindow.window)
            unmanage(c, 1);
        break;
    case UnmapNotify:
        c = find(e->xunmap.window);
        /* Listen to the client's own notification, avoiding duplicate parent events. */
        if (c && e->xunmap.window == c->window &&
            (e->xunmap.event == c->window || e->xunmap.send_event)) {
            if (c->ignore_unmap > 0)
                c->ignore_unmap--;
            else
                unmanage(c, 0);
        }
        break;
    case Expose:
        if (e->xexpose.window == switcher_window) {
            switcher_paint();
            break;
        }
        if (e->xexpose.window == menu_window) {
            menu_paint();
            break;
        }
        if (c && e->xexpose.count == 0)
            draw_frame(c);
        break;
    case ButtonPress:
        if (switcher_active) {
            switcher_cancel();
            break;
        }
        button(&e->xbutton);
        break;
    case ButtonRelease:
        last_time = e->xbutton.time;
        finish_drag(0);
        break;
    case MotionNotify:
        if (menu_client) {
            int item = e->xmotion.y / 28;
            int items = menu_items(menu_client);
            if (item >= 0 && item < items && item != menu_item) {
                menu_item = item;
                menu_paint();
            }
            break;
        }
        last_time = e->xmotion.time;
        motion(e->xmotion.x_root, e->xmotion.y_root);
        break;
    case KeyPress:
        key(&e->xkey);
        break;
    case EnterNotify:
        /* Focus-follows-mouse skips grabs and inferior crossings. */
        if (follow_mouse && (e->xcrossing.mode == NotifyNormal ||
                             e->xcrossing.mode == NotifyUngrab) &&
            e->xcrossing.detail != NotifyInferior) {
            Client *entered = find(e->xcrossing.window);
            if (entered && !entered->special && visible(entered) && entered != focused)
                focus(entered, e->xcrossing.time);
        }
        break;
    case KeyRelease:
        if (switcher_active)
            for (int i = 0; i < alt_key_count; i++)
                if (e->xkey.keycode == alt_keys[i]) {
                    switcher_activate();
                    break;
                }
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&e->xmapping);
        grab_keys();
        break;
    case ClientMessage:
        message(&e->xclient);
        break;
    case SelectionClear:
        if (e->xselectionclear.selection == wm_selection)
            stopping = 1;
        break;
    case PropertyNotify:
        if (c) {
            if (e->xproperty.window == c->window && c->frame &&
                e->xproperty.atom == atom("_NET_WM_WINDOW_OPACITY")) {
                unsigned long opacity = cardinal(c->window, "_NET_WM_WINDOW_OPACITY", 0xffffffffUL);
                set_cardinals(c->frame, "_NET_WM_WINDOW_OPACITY", &opacity, 1);
            }
            if (e->xproperty.atom == atom("_NET_WM_NAME") || e->xproperty.atom == XA_WM_NAME) {
                title(c);
                draw_frame(c);
            }
            if (e->xproperty.atom == XA_WM_NORMAL_HINTS || e->xproperty.atom == XA_WM_HINTS ||
                e->xproperty.atom == XA_WM_TRANSIENT_FOR) {
                hints(c);
                draw_frame(c);
            }
            if (e->xproperty.atom == atom("_NET_WM_STRUT") ||
                e->xproperty.atom == atom("_NET_WM_STRUT_PARTIAL"))
                update_workarea();
            if (e->xproperty.atom == atom("_NET_WM_SYNC_REQUEST_COUNTER"))
                c->sync_counter =
                    (XID)cardinal(c->window, "_NET_WM_SYNC_REQUEST_COUNTER", 0);
        }
        break;
    case ConfigureNotify:
        if (e->xconfigure.window == root &&
            (e->xconfigure.width != screen_w || e->xconfigure.height != screen_h)) {
            screen_w = e->xconfigure.width;
            screen_h = e->xconfigure.height;
            for (Client *window = clients; window; window = window->next) {
                if (!window->special && !window->fullscreen && !window->max_h && !window->max_v) {
                    window->normal.x = max(window->border, min(window->normal.x, screen_w - 80));
                    window->normal.y =
                        max(window->title + window->border, min(window->normal.y, screen_h - 40));
                    configure(window);
                }
            }
            update_workarea();
            unsigned long size[2] = {screen_w, screen_h};
            set_cardinals(root, "_NET_DESKTOP_GEOMETRY", size, 2);
        }
        break;
    }
    if (have_shape && e->type == shape_event + ShapeNotify) {
        Client *shaped = find(((XShapeEvent *)e)->window);
        if (shaped)
            shape(shaped);
    }
    if (have_randr && e->type == randr_event + RRScreenChangeNotify) {
        XRRUpdateConfiguration(e);
        screen_w = DisplayWidth(display, screen_number);
        screen_h = DisplayHeight(display, screen_number);
        update_workarea();
    }
}
static int initialize(void)
{
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "rill-wm: cannot open DISPLAY\n");
        return 0;
    }
    screen_number = DefaultScreen(display);
    root = RootWindow(display, screen_number);
    screen_w = DisplayWidth(display, screen_number);
    screen_h = DisplayHeight(display, screen_number);
    XSetErrorHandler(xerror);
    claiming = 1;
    XSelectInput(display, root,
                 SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask |
                     PropertyChangeMask | KeyPressMask | KeyReleaseMask);
    XSync(display, False);
    claiming = 0;
    if (claim_failed) {
        fprintf(stderr, "rill-wm: another window manager owns this display\n");
        return 0;
    }
    support = XCreateSimpleWindow(display, root, -1, -1, 1, 1, 0, 0, 0);
    XSelectInput(display, support, PropertyChangeMask);
    char name[32];
    snprintf(name, sizeof(name), "WM_S%d", screen_number);
    wm_selection = atom(name);
    XSetSelectionOwner(display, wm_selection, support, CurrentTime);
    if (XGetSelectionOwner(display, wm_selection) != support)
        return 0;
    XEvent manager = {0};
    manager.xclient.type = ClientMessage;
    manager.xclient.window = root;
    manager.xclient.message_type = atom("MANAGER");
    manager.xclient.format = 32;
    manager.xclient.data.l[1] = wm_selection;
    manager.xclient.data.l[2] = support;
    XSendEvent(display, root, False, StructureNotifyMask, &manager);
    const char *supported[] = {"_NET_SUPPORTING_WM_CHECK",
                               "_NET_CLIENT_LIST",
                               "_NET_CLIENT_LIST_STACKING",
                               "_NET_ACTIVE_WINDOW",
                               "_NET_NUMBER_OF_DESKTOPS",
                               "_NET_CURRENT_DESKTOP",
                               "_NET_DESKTOP_NAMES",
                               "_NET_DESKTOP_GEOMETRY",
                               "_NET_DESKTOP_VIEWPORT",
                               "_NET_WORKAREA",
                               "_NET_SHOWING_DESKTOP",
                               "_NET_WM_DESKTOP",
                               "_NET_WM_NAME",
                               "_NET_CLOSE_WINDOW",
                               "_NET_WM_PING",
                               "_NET_WM_STATE",
                               "_NET_WM_STATE_HIDDEN",
                               "_NET_WM_STATE_FULLSCREEN",
                               "_NET_WM_STATE_MAXIMIZED_HORZ",
                               "_NET_WM_STATE_MAXIMIZED_VERT",
                               "_NET_WM_STATE_ABOVE",
                               "_NET_WM_STATE_BELOW",
                               "_NET_WM_STATE_MODAL",
                               "_NET_WM_STATE_SKIP_TASKBAR",
                               "_NET_WM_STATE_SKIP_PAGER",
                               "_NET_WM_STATE_STICKY",
                               "_NET_WM_STATE_SHADED",
                               "_NET_WM_STATE_DEMANDS_ATTENTION",
                               "_NET_FRAME_EXTENTS",
                               "_NET_REQUEST_FRAME_EXTENTS",
                               "_NET_WM_ALLOWED_ACTIONS",
                               "_NET_WM_STRUT",
                               "_NET_WM_STRUT_PARTIAL",
                               "_NET_WM_MOVERESIZE",
                               "_NET_MOVERESIZE_WINDOW",
                               "_NET_WM_WINDOW_TYPE",
                               "_NET_WM_WINDOW_TYPE_NORMAL",
                               "_NET_WM_WINDOW_TYPE_DIALOG",
                               "_NET_WM_WINDOW_TYPE_DOCK",
                               "_NET_WM_WINDOW_TYPE_DESKTOP"};
    Atom atoms[sizeof(supported) / sizeof(supported[0])];
    for (unsigned i = 0; i < sizeof(atoms) / sizeof(atoms[0]); i++)
        atoms[i] = atom(supported[i]);
    XChangeProperty(display, root, atom("_NET_SUPPORTED"), XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)atoms, sizeof(atoms) / sizeof(atoms[0]));
    XChangeProperty(display, root, atom("_NET_SUPPORTING_WM_CHECK"), XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&support, 1);
    XChangeProperty(display, support, atom("_NET_SUPPORTING_WM_CHECK"), XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&support, 1);
    XChangeProperty(display, support, atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8, PropModeReplace,
                    (unsigned char *)"Rill WM", 7);
    unsigned long count = desktops, current = 0, size[2] = {screen_w, screen_h}, viewport[8] = {0};
    set_cardinals(root, "_NET_NUMBER_OF_DESKTOPS", &count, 1);
    set_cardinals(root, "_NET_CURRENT_DESKTOP", &current, 1);
    set_cardinals(root, "_NET_SHOWING_DESKTOP", &current, 1);
    set_cardinals(root, "_NET_DESKTOP_GEOMETRY", size, 2);
    set_cardinals(root, "_NET_DESKTOP_VIEWPORT", viewport, 8);
    const char names[] = "Workspace 1\0Workspace 2\0Workspace 3\0Workspace 4\0";
    XChangeProperty(display, root, atom("_NET_DESKTOP_NAMES"), atom("UTF8_STRING"), 8,
                    PropModeReplace, (unsigned char *)names, sizeof(names) - 1);
    int error;
    have_shape = XShapeQueryExtension(display, &shape_event, &error);
    have_randr = XRRQueryExtension(display, &randr_event, &error);
    have_sync = XSyncInitialize(display, &error, &error);
    if (have_randr)
        XRRSelectInput(display, root,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                           RROutputChangeNotifyMask);
    gc = XCreateGC(display, root, 0, NULL);
    font = XftFontOpenName(display, screen_number, "sans-10");
    XftColorAllocName(display, DefaultVisual(display, screen_number),
                      DefaultColormap(display, screen_number), "#eeeeee", &text_color);
    move_cursor = XCreateFontCursor(display, XC_fleur);
    resize_cursor = XCreateFontCursor(display, XC_bottom_right_corner);
    load_decoration_preferences();
    char keys_path[512];
    const char *keys_override = getenv("RILL_WM_KEYS");
    if (keys_override && *keys_override)
        snprintf(keys_path, sizeof(keys_path), "%s", keys_override);
    else {
        const char *base = getenv("XDG_CONFIG_HOME");
        if (base && *base)
            snprintf(keys_path, sizeof(keys_path), "%s/rill/wm-keys", base);
        else if ((base = getenv("HOME")) && *base)
            snprintf(keys_path, sizeof(keys_path), "%s/.config/rill/wm-keys", base);
        else
            keys_path[0] = 0;
    }
    if (keys_path[0])
        load_bindings(keys_path);
    grab_keys();
    Window rr, pp, *children = NULL;
    unsigned n;
    if (XQueryTree(display, root, &rr, &pp, &children, &n)) {
        for (unsigned i = 0; i < n; i++) {
            XWindowAttributes a;
            if (XGetWindowAttributes(display, children[i], &a) && a.map_state == IsViewable)
                manage(children[i]);
        }
        if (children)
            XFree(children);
    }
    update_workarea();
    publish_clients();
    if (composite_enabled && !CompositorStart(display, root, support))
        fprintf(stderr, "rill-wm: compositing unavailable; using direct X11 rendering\n");
    XSync(display, False);
    return 1;
}
int main(int argc, char **argv)
{
    const char *composite = getenv("RILL_WM_COMPOSITE");
    if (composite && strcmp(composite, "0") == 0)
        composite_enabled = 0;
    const char *ping_ms = getenv("RILL_WM_PING_MS");
    if (ping_ms) {
        long value = atol(ping_ms);
        ping_timeout_ms = value < 100 ? 100 : value;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-composite") == 0)
            composite_enabled = 0;
        else if (strcmp(argv[i], "--sm-client-id") == 0 && i + 1 < argc)
            i++;
        else if (strcmp(argv[i], "--help") == 0) {
            puts("rill-wm [--no-composite]\nX11 window manager with decorations, workspaces and "
                 "compositing.");
            return 0;
        } else {
            fprintf(stderr, "rill-wm: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    if (!initialize()) {
        if (display)
            XCloseDisplay(display);
        return 1;
    }
    SessionConnect(argc, argv);
    while (!stopping && SessionPoll()) {
        /* Bound each batch so animation cannot starve repaint or session requests. */
        for (int batch = 0; batch < 256 && XPending(display); batch++) {
            XEvent e;
            XNextEvent(display, &e);
            CompositorEvent(&e);
            event(&e);
        }
        ping_check();
        CompositorPaint();
        XFlush(display);
        struct pollfd fd = {ConnectionNumber(display), POLLIN, 0};
        poll(&fd, 1, 16);
        if (fd.revents & (POLLHUP | POLLERR))
            break;
    }
    stopping = 1;
    switcher_cancel();
    if (switcher_draw)
        XftDrawDestroy(switcher_draw);
    if (switcher_image)
        XDestroyImage(switcher_image);
    if (switcher_window)
        XDestroyWindow(display, switcher_window);
    menu_close();
    if (menu_draw)
        XftDrawDestroy(menu_draw);
    if (menu_window)
        XDestroyWindow(display, menu_window);
    CompositorStop();
    while (clients)
        unmanage(clients, 0);
    XDeleteProperty(display, root, atom("_NET_SUPPORTING_WM_CHECK"));
    XDeleteProperty(display, root, atom("_NET_SUPPORTED"));
    XDestroyWindow(display, support);
    SessionDisconnect();
    if (font)
        XftFontClose(display, font);
    XftColorFree(display, DefaultVisual(display, screen_number),
                 DefaultColormap(display, screen_number), &text_color);
    XFreeGC(display, gc);
    XFreeCursor(display, move_cursor);
    XFreeCursor(display, resize_cursor);
    XCloseDisplay(display);
    return 0;
}
