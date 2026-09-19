#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static Display *d;
static Window root;
static pid_t manager;
static void cleanup(void)
{
    if (manager > 0) {
        kill(manager, SIGTERM);
        waitpid(manager, NULL, 0);
        manager = 0;
    }
}
static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "wm test failed: %s\n", message);
        cleanup();
        exit(1);
    }
}
static Atom atom(const char *name) { return XInternAtom(d, name, False); }
static void pump(void)
{
    XSync(d, False);
    usleep(140000);
    XSync(d, False);
}
static unsigned long value(Window w, const char *name, Atom type, int index)
{
    Atom actual;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL, result = 0;
    unsigned long answer = 0;
    (void)result;
    if (XGetWindowProperty(d, w, atom(name), 0, 4096, False, type, &actual, &format, &n, &after,
                           &data) == Success &&
        data && format == 32 && n > (unsigned)index)
        answer = ((unsigned long *)data)[index];
    if (data)
        XFree(data);
    return answer;
}
static int contains(Window w, const char *name, Atom type, unsigned long item)
{
    Atom actual;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    int found = 0;
    if (XGetWindowProperty(d, w, atom(name), 0, 4096, False, type, &actual, &format, &n, &after,
                           &data) == Success &&
        data && format == 32)
        for (unsigned long i = 0; i < n; i++)
            if (((unsigned long *)data)[i] == item)
                found = 1;
    if (data)
        XFree(data);
    return found;
}
static void send(Window w, const char *name, long a, long b, long c, long f, long g)
{
    XEvent e = {0};
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = atom(name);
    e.xclient.format = 32;
    e.xclient.data.l[0] = a;
    e.xclient.data.l[1] = b;
    e.xclient.data.l[2] = c;
    e.xclient.data.l[3] = f;
    e.xclient.data.l[4] = g;
    XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    pump();
}
static Window parent(Window w)
{
    Window r, p, *children = NULL;
    unsigned n;
    XQueryTree(d, w, &r, &p, &children, &n);
    if (children)
        XFree(children);
    return p;
}
static XWindowAttributes attrs(Window w)
{
    XWindowAttributes a;
    check(XGetWindowAttributes(d, w, &a), "window exists");
    return a;
}
static void position(Window w, int *x, int *y)
{
    Window child;
    XTranslateCoordinates(d, w, root, 0, 0, x, y, &child);
}
static void key(KeySym modifier, KeySym symbol)
{
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, modifier), True, CurrentTime);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, symbol), True, CurrentTime);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, symbol), False, CurrentTime);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, modifier), False, CurrentTime);
    pump();
}
static void start(const char *binary)
{
    manager = fork();
    check(manager >= 0, "fork manager");
    if (manager == 0) {
        unsetenv("SESSION_MANAGER");
        execl(binary, binary, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100 && !value(root, "_NET_SUPPORTING_WM_CHECK", XA_WINDOW, 0); i++)
        usleep(20000);
    check(value(root, "_NET_SUPPORTING_WM_CHECK", XA_WINDOW, 0) != 0, "manager registered");
    pump();
}
int main(int argc, char **argv)
{
    check(argc == 2, "binary argument");
    d = XOpenDisplay(NULL);
    check(d != NULL, "display");
    root = DefaultRootWindow(d);
    atexit(cleanup);
    setenv("RILL_WM_PING_MS", "300", 1);
    setenv("RILL_WM_BUTTON_LAYOUT", "O|HC", 1);
    start(argv[1]);
    Window dock = XCreateSimpleWindow(d, root, 0, 0, 1280, 40, 0, 0, 0x222222);
    Atom type = atom("_NET_WM_WINDOW_TYPE_DOCK");
    XChangeProperty(d, dock, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type, 1);
    unsigned long strut[12] = {0, 0, 40, 0, 0, 0, 0, 0, 0, 1279, 0, 0};
    XChangeProperty(d, dock, atom("_NET_WM_STRUT_PARTIAL"), XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut, 12);
    XMapWindow(d, dock);
    pump();
    check(parent(dock) == root, "dock stays unframed");
    check(value(root, "_NET_WORKAREA", XA_CARDINAL, 1) == 40, "panel workarea");
    Window w = XCreateSimpleWindow(d, root, 100, 100, 320, 200, 0, 0, 0xff0000);
    XSelectInput(d, w, StructureNotifyMask | KeyPressMask | ButtonPressMask);
    Atom protocols[2] = {atom("WM_DELETE_WINDOW"), atom("WM_TAKE_FOCUS")};
    XSetWMProtocols(d, w, protocols, 2);
    XStoreName(d, w, "Normal test window");
    XMapWindow(d, w);
    pump();
    check(parent(w) != root, "client reparented into decoration frame");
    check(value(w, "_NET_FRAME_EXTENTS", XA_CARDINAL, 2) == 31, "title extents");
    unsigned long motif[5] = {2, 0, 0, 0, 0};
    XChangeProperty(d, w, atom("_MOTIF_WM_HINTS"), atom("_MOTIF_WM_HINTS"), 32,
                    PropModeReplace, (unsigned char *)motif, 5);
    pump();
    check(value(w, "_NET_FRAME_EXTENTS", XA_CARDINAL, 2) == 0,
          "client can remove decorations after mapping");
    XDeleteProperty(d, w, atom("_MOTIF_WM_HINTS"));
    pump();
    check(value(w, "_NET_FRAME_EXTENTS", XA_CARDINAL, 2) == 31,
          "deleting decoration hint restores frame");
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == w, "new client focused");
    check(contains(root, "_NET_CLIENT_LIST", XA_WINDOW, w), "client list contains app");
    int x, y;
    position(w, &x, &y);
    int original_x = x, original_y = y;
    send(w, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    check(attrs(w).width == 1274 && attrs(w).height == 726,
          "maximized size reserves panel and frame");
    position(w, &x, &y);
    check(x == 3 && y == 71, "maximized position below panel");
    send(w, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_FULLSCREEN"), 0, 0, 0);
    check(attrs(w).width == 1280 && attrs(w).height == 800, "fullscreen covers screen");
    check(value(w, "_NET_FRAME_EXTENTS", XA_CARDINAL, 2) == 0, "fullscreen removes decoration");
    send(w, "_NET_WM_STATE", 0, atom("_NET_WM_STATE_FULLSCREEN"), 0, 0, 0);
    check(attrs(w).width == 1274, "fullscreen restores maximize");
    send(w, "_NET_WM_STATE", 0, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    position(w, &x, &y);
    check(attrs(w).width == 320 && attrs(w).height == 200 && x == original_x && y == original_y,
          "normal geometry restored");
    send(w, "WM_CHANGE_STATE", IconicState, 0, 0, 0, 0);
    check(attrs(parent(w)).map_state == IsUnmapped, "minimize unmaps frame");
    check(contains(root, "_NET_CLIENT_LIST", XA_WINDOW, w),
          "minimized window retained in task list");
    check(contains(w, "_NET_WM_STATE", XA_ATOM, atom("_NET_WM_STATE_HIDDEN")),
          "hidden hint published");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    check(attrs(w).map_state == IsViewable, "taskbar activation restores");
    send(root, "_NET_CURRENT_DESKTOP", 1, 0, 0, 0, 0);
    check(attrs(parent(w)).map_state == IsUnmapped && attrs(dock).map_state == IsViewable,
          "workspace switch preserves panel");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    check(value(root, "_NET_CURRENT_DESKTOP", XA_CARDINAL, 0) == 0,
          "activation switches workspace");
    send(w, "_NET_WM_DESKTOP", 0xffffffffUL, 0, 0, 0, 0);
    send(root, "_NET_CURRENT_DESKTOP", 2, 0, 0, 0, 0);
    check(attrs(w).map_state == IsViewable, "sticky app visible on another workspace");
    send(w, "_NET_WM_DESKTOP", 2, 0, 0, 0, 0);
    XSizeHints hints = {0};
    hints.flags = PMinSize | PMaxSize | PResizeInc | PBaseSize;
    hints.min_width = 100;
    hints.min_height = 80;
    hints.max_width = 600;
    hints.max_height = 500;
    hints.base_width = 100;
    hints.base_height = 80;
    hints.width_inc = 10;
    hints.height_inc = 5;
    XSetWMNormalHints(d, w, &hints);
    pump();
    XResizeWindow(d, w, 337, 218);
    pump();
    check(attrs(w).width == 330 && attrs(w).height == 215, "resize increments respected");
    XResizeWindow(d, w, 30, 20);
    pump();
    check(attrs(w).width == 100 && attrs(w).height == 80, "minimum size respected");
    XResizeWindow(d, w, 900, 900);
    pump();
    check(attrs(w).width == 600 && attrs(w).height == 500, "maximum size respected");
    Window dialog = XCreateSimpleWindow(d, root, 0, 0, 180, 100, 0, 0, 0x00ff00);
    XSetTransientForHint(d, dialog, w);
    Atom modal = atom("_NET_WM_STATE_MODAL");
    XChangeProperty(d, dialog, atom("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&modal, 1);
    XMapWindow(d, dialog);
    pump();
    check(value(dialog, "_NET_WM_DESKTOP", XA_CARDINAL, 0) == 2, "dialog inherits workspace");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == dialog, "modal prevents parent focus");
    send(w, "WM_CHANGE_STATE", IconicState, 0, 0, 0, 0);
    check(attrs(parent(dialog)).map_state == IsUnmapped, "parent minimizes its modal dialog");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    check(attrs(dialog).map_state == IsViewable &&
              value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == dialog,
          "parent restoration restores modal dialog and focus");
    XDestroyWindow(d, dialog);
    pump();
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == w,
          "dialog close restores parent focus");
    key(XK_Alt_L, XK_F9);
    check(attrs(parent(w)).map_state == IsUnmapped, "Alt-F9 minimizes");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    key(XK_Alt_L, XK_F10);
    check(attrs(w).width == 1274, "Alt-F10 maximizes");
    key(XK_Alt_L, XK_F10);
    key(XK_Alt_L, XK_space);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Return), True, CurrentTime);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Return), False, CurrentTime);
    pump();
    check(attrs(w).width == 1274, "Alt-Space menu activates maximize");
    key(XK_Alt_L, XK_F10);
    send(root, "_NET_NUMBER_OF_DESKTOPS", 6, 0, 0, 0, 0);
    check(value(root, "_NET_NUMBER_OF_DESKTOPS", XA_CARDINAL, 0) == 6, "workspace count expands");
    send(w, "_NET_WM_DESKTOP", 5, 0, 0, 0, 0);
    send(root, "_NET_CURRENT_DESKTOP", 5, 0, 0, 0, 0);
    send(root, "_NET_NUMBER_OF_DESKTOPS", 3, 0, 0, 0, 0);
    check(value(root, "_NET_CURRENT_DESKTOP", XA_CARDINAL, 0) == 2 &&
              value(w, "_NET_WM_DESKTOP", XA_CARDINAL, 0) == 2 && attrs(w).map_state == IsViewable,
          "workspace removal relocates clients and active workspace");
    /* Alt+Tab opens the MRU switcher overlay; releasing Alt commits the focus. */
    Window second = XCreateSimpleWindow(d, root, 400, 300, 240, 160, 0, 0, 0x0000ff);
    XStoreName(d, second, "Second test window");
    XMapWindow(d, second);
    pump();
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == second, "new window takes focus");
    KeyCode alt = XKeysymToKeycode(d, XK_Alt_L);
    KeyCode tab = XKeysymToKeycode(d, XK_Tab);
    XTestFakeKeyEvent(d, alt, True, CurrentTime);
    XTestFakeKeyEvent(d, tab, True, CurrentTime);
    XTestFakeKeyEvent(d, tab, False, CurrentTime);
    pump();
    Window switcher_overlay = XCompositeGetOverlayWindow(d, root);
    int highlighted = 0;
    XImage *switcher_shot = XGetImage(d, switcher_overlay, 480, 300, 320, 200, AllPlanes, ZPixmap);
    if (switcher_shot) {
        for (int py = 0; py < switcher_shot->height && !highlighted; py++)
            for (int px = 0; px < switcher_shot->width; px++)
                if ((XGetPixel(switcher_shot, px, py) & 0xffffff) == 0x344c6b) {
                    highlighted = 1;
                    break;
                }
        XDestroyImage(switcher_shot);
    }
    XCompositeReleaseOverlayWindow(d, root);
    check(highlighted, "switcher overlay paints a highlighted tile");
    XTestFakeKeyEvent(d, alt, False, CurrentTime);
    pump();
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == w,
          "Alt-Tab selects the previously focused window");
    XDestroyWindow(d, second);
    pump();
    /* Imported button layout "O|HC": close at the far right, hide beside it. */
    position(w, &x, &y);
    XTestFakeMotionEvent(d, DefaultScreen(d), x + attrs(w).width - 10, y - 16, CurrentTime);
    pump();
    XTestFakeButtonEvent(d, 1, True, CurrentTime);
    XTestFakeButtonEvent(d, 1, False, CurrentTime);
    pump();
    int button_close = 0;
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ClientMessage && e.xclient.message_type == atom("WM_PROTOCOLS") &&
            (Atom)e.xclient.data.l[0] == atom("WM_DELETE_WINDOW"))
            button_close = 1;
    }
    check(button_close, "rightmost configured button closes");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    XTestFakeMotionEvent(d, DefaultScreen(d), x + attrs(w).width - 32, y - 16, CurrentTime);
    pump();
    XTestFakeButtonEvent(d, 1, True, CurrentTime);
    XTestFakeButtonEvent(d, 1, False, CurrentTime);
    pump();
    check(attrs(parent(w)).map_state == IsUnmapped, "second configured button hides");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    /* A RandR fake monitor drives monitor-aware maximize and tiling. */
    XRRMonitorInfo fake = {0};
    fake.name = atom("RillRight");
    fake.x = 640;
    fake.y = 0;
    fake.width = 640;
    fake.height = 800;
    fake.mwidth = 640;
    fake.mheight = 800;
    XRRSetMonitor(d, root, &fake);
    pump();
    Window mono = XCreateSimpleWindow(d, root, 700, 300, 200, 150, 0, 0, 0x0f0f0f);
    XStoreName(d, mono, "Right monitor window");
    XMapWindow(d, mono);
    pump();
    send(mono, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    position(mono, &x, &y);
    check(x == 643 && attrs(mono).width == 634, "maximize follows the right monitor");
    key(XK_Super_L, XK_Right);
    position(mono, &x, &y);
    check(x == 963 && attrs(mono).width == 314, "tiling uses the right monitor workarea");
    XRRDeleteMonitor(d, root, atom("RillRight"));
    pump();
    send(mono, "_NET_WM_STATE", 0, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    send(mono, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    position(mono, &x, &y);
    check(x == 3 && attrs(mono).width == 1274, "monitor removal returns to full-screen maximize");
    XDestroyWindow(d, mono);
    pump();
    /* Moving through EWMH must use native pointer input, not UI emulation. */
    position(w, &x, &y);
    original_x = x;
    original_y = y;
    XTestFakeMotionEvent(d, DefaultScreen(d), x + 40, y + 40, CurrentTime);
    pump();
    send(w, "_NET_WM_MOVERESIZE", x + 40, y + 40, 8, 1, 2);
    XTestFakeMotionEvent(d, DefaultScreen(d), x + 100, y + 90, CurrentTime);
    pump();
    position(w, &x, &y);
    check(x == original_x + 60 && y == original_y + 50, "pointer drag moves client");
    send(w, "_NET_WM_MOVERESIZE", 0, 0, 11, 0, 0);
    position(w, &x, &y);
    check(x == original_x && y == original_y, "cancel drag restores position");
    send(root, "_NET_SHOWING_DESKTOP", 1, 0, 0, 0, 0);
    check(attrs(parent(w)).map_state == IsUnmapped, "show desktop hides apps");
    send(root, "_NET_SHOWING_DESKTOP", 0, 0, 0, 0, 0);
    check(attrs(w).map_state == IsViewable, "show desktop restores apps");
    /* Verify the compositor renders client content and an override-redirect popup. */
    Window overlay = XCompositeGetOverlayWindow(d, root);
    pump();
    position(w, &x, &y);
    XImage *shot = XGetImage(d, overlay, x + 20, y + 20, 1, 1, AllPlanes, ZPixmap);
    check(shot && ((XGetPixel(shot, 0, 0) & 0xffffff) == 0xff0000),
          "compositor paints client content");
    if (shot)
        XDestroyImage(shot);
    /* The drop shadow darkens the background band around the frame. */
    shot = XGetImage(d, overlay, x - 7, y - 35, 1, 1, AllPlanes, ZPixmap);
    check(shot && (XGetPixel(shot, 0, 0) & 0xff) < 0x2c && (XGetPixel(shot, 0, 0) & 0xff) > 4,
          "compositor draws a shadow around frames");
    if (shot)
        XDestroyImage(shot);
    XSetWindowAttributes popup_attrs = {0};
    popup_attrs.override_redirect = True;
    popup_attrs.background_pixel = 0x0000ff;
    Window popup = XCreateWindow(d, root, x + 10, y + 10, 60, 60, 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWOverrideRedirect | CWBackPixel, &popup_attrs);
    XMapRaised(d, popup);
    pump();
    shot = XGetImage(d, overlay, x + 20, y + 20, 1, 1, AllPlanes, ZPixmap);
    check(shot && ((XGetPixel(shot, 0, 0) & 0xffffff) == 0x0000ff),
          "compositor paints popup above app");
    if (shot)
        XDestroyImage(shot);
    XDestroyWindow(d, popup);
    pump();
    XRectangle input_region = {0, 0, 40, 40};
    XShapeCombineRectangles(d, w, ShapeInput, 0, 0, &input_region, 1, ShapeSet, Unsorted);
    pump();
    int rectangle_count, ordering, has_input_hole = 1, title_interactive = 0;
    XRectangle *rectangles = XShapeGetRectangles(d, parent(w), ShapeInput, &rectangle_count, &ordering);
    int border = value(w, "_NET_FRAME_EXTENTS", XA_CARDINAL, 0);
    int title_height = value(w, "_NET_FRAME_EXTENTS", XA_CARDINAL, 2);
    for (int i = 0; i < rectangle_count; i++) {
        XRectangle r = rectangles[i];
        if (border + 60 >= r.x && border + 60 < r.x + r.width &&
            title_height + 60 >= r.y && title_height + 60 < r.y + r.height)
            has_input_hole = 0;
        if (20 >= r.x && 20 < r.x + r.width && 10 >= r.y && 10 < r.y + r.height)
            title_interactive = 1;
    }
    XFree(rectangles);
    check(has_input_hole && title_interactive, "frame preserves client input holes and title input");
    XShapeCombineMask(d, w, ShapeInput, 0, 0, None, ShapeSet);
    pump();
    XRectangle shape = {0, 0, 40, 40};
    XShapeCombineRectangles(d, w, ShapeBounding, 0, 0, &shape, 1, ShapeSet, Unsorted);
    pump();
    shot = XGetImage(d, overlay, x + 60, y + 60, 1, 1, AllPlanes, ZPixmap);
    check(shot && ((XGetPixel(shot, 0, 0) & 0xffffff) != 0xff0000),
          "shaped client exposes desktop outside bounding region");
    if (shot)
        XDestroyImage(shot);
    XShapeCombineMask(d, w, ShapeBounding, 0, 0, None, ShapeSet);
    pump();
    XVisualInfo visual;
    if (XMatchVisualInfo(d, DefaultScreen(d), 32, TrueColor, &visual)) {
        XSetWindowAttributes alpha = {0};
        alpha.colormap = XCreateColormap(d, root, visual.visual, AllocNone);
        alpha.background_pixel = 0xff00ff00;
        alpha.border_pixel = 0;
        Window argb = XCreateWindow(d, root, 900, 500, 100, 100, 0, 32, InputOutput, visual.visual,
                                    CWColormap | CWBackPixel | CWBorderPixel, &alpha);
        XMapWindow(d, argb);
        pump();
        check(parent(argb) != root && attrs(argb).map_state == IsViewable,
              "ARGB client receives compatible frame visual");
        int ax, ay;
        position(argb, &ax, &ay);
        shot = XGetImage(d, overlay, ax + 20, ay + 20, 1, 1, AllPlanes, ZPixmap);
        check(shot && ((XGetPixel(shot, 0, 0) & 0xffffff) == 0x00ff00),
              "compositor paints ARGB client");
        if (shot)
            XDestroyImage(shot);
        XDestroyWindow(d, argb);
        XFreeColormap(d, alpha.colormap);
        pump();
    }
    XCompositeReleaseOverlayWindow(d, root);
    send(w, "_NET_CLOSE_WINDOW", 1234, 0, 0, 0, 0);
    int closed = 0, take_focus = 0;
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ClientMessage && e.xclient.message_type == atom("WM_PROTOCOLS")) {
            if ((Atom)e.xclient.data.l[0] == atom("WM_DELETE_WINDOW") &&
                e.xclient.data.l[1] == 1234)
                closed = 1;
            if ((Atom)e.xclient.data.l[0] == atom("WM_TAKE_FOCUS")) {
                check(e.xclient.data.l[1] != CurrentTime,
                      "take-focus carries real server timestamp");
                take_focus = 1;
            }
        }
    }
    check(closed && take_focus, "ICCCM close and take-focus protocols");
    send(root, "_NET_CURRENT_DESKTOP", 2, 0, 0, 0, 0);
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        check(!(e.type == ClientMessage && e.xclient.message_type == atom("WM_PROTOCOLS") &&
                (Atom)e.xclient.data.l[0] == atom("WM_TAKE_FOCUS")),
              "same-workspace request must not queue a stale focus transfer");
    }
    /* Focus stealing: an application activation with a stale timestamp must
       only raise the demands-attention hint. */
    Window stealer = XCreateSimpleWindow(d, root, 500, 80, 200, 120, 0, 0, 0x00ff00);
    XStoreName(d, stealer, "Stealer test window");
    XMapWindow(d, stealer);
    pump();
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == stealer,
          "late-mapped window without a user time takes focus");
    send(w, "_NET_ACTIVE_WINDOW", 1, 1, 0, 0, 0);
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == stealer,
          "stale application activation does not steal focus");
    check(contains(w, "_NET_WM_STATE", XA_ATOM, atom("_NET_WM_STATE_DEMANDS_ATTENTION")),
          "stale activation raises the urgent hint instead");
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == w,
          "pager activation still focuses directly");
    XDestroyWindow(d, stealer);
    pump();
    /* _NET_WM_PING flags unresponsive clients; the window menu force-closes.
       The unresponsive app runs in a child process so XKillClient only severs
       that connection. */
    pid_t hung_pid = fork();
    check(hung_pid >= 0, "fork unresponsive client");
    if (hung_pid == 0) {
        manager = 0; /* the child must not kill the WM through atexit(cleanup) */
        Display *cd = XOpenDisplay(NULL);
        if (!cd)
            _exit(2);
        Window hung = XCreateSimpleWindow(cd, DefaultRootWindow(cd), 300, 400, 220, 140, 0, 0,
                                          0xff00ff);
        Atom ping[3] = {XInternAtom(cd, "WM_DELETE_WINDOW", False),
                        XInternAtom(cd, "WM_TAKE_FOCUS", False),
                        XInternAtom(cd, "_NET_WM_PING", False)};
        XSetWMProtocols(cd, hung, ping, 3);
        XStoreName(cd, hung, "Unresponsive test window");
        XSelectInput(cd, hung, StructureNotifyMask);
        XMapWindow(cd, hung);
        XFlush(cd);
        for (;;) {
            XEvent wait;
            XNextEvent(cd, &wait);
        }
    }
    pump();
    Atom list_type;
    int list_format;
    unsigned long list_count, list_after;
    unsigned char *list_data = NULL;
    Window hung_app = None;
    if (XGetWindowProperty(d, root, atom("_NET_CLIENT_LIST"), 0, 4096, False, XA_WINDOW,
                           &list_type, &list_format, &list_count, &list_after, &list_data) ==
            Success &&
        list_data && list_count)
        hung_app = ((Window *)list_data)[list_count - 1];
    if (list_data)
        XFree(list_data);
    check(hung_app != None, "unresponsive client joins the client list");
    send(hung_app, "_NET_CLOSE_WINDOW", 4321, 0, 0, 0, 0);
    usleep(450000);
    pump();
    key(XK_Alt_L, XK_space);
    KeyCode down = XKeysymToKeycode(d, XK_Down);
    for (int i = 0; i < 9; i++) {
        XTestFakeKeyEvent(d, down, True, CurrentTime);
        XTestFakeKeyEvent(d, down, False, CurrentTime);
        pump();
    }
    KeyCode enter = XKeysymToKeycode(d, XK_Return);
    XTestFakeKeyEvent(d, enter, True, CurrentTime);
    XTestFakeKeyEvent(d, enter, False, CurrentTime);
    pump();
    int killed = 0, hung_status;
    for (int i = 0; i < 20 && !killed; i++) {
        if (waitpid(hung_pid, &hung_status, WNOHANG) == hung_pid)
            killed = 1;
        else
            usleep(100000);
    }
    check(killed, "window menu force-closes an unresponsive client");
    /* Sync resize advances the client's frame counter and waits for it. */
    Window synced = XCreateSimpleWindow(d, root, 700, 200, 260, 180, 0, 0, 0xffff00);
    XSyncValue initial;
    XSyncIntToValue(&initial, 0);
    XSyncCounter counter = XSyncCreateCounter(d, initial);
    unsigned long counter_id = counter;
    XChangeProperty(d, synced, atom("_NET_WM_SYNC_REQUEST_COUNTER"), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&counter_id, 1);
    Atom sync_protocols[1] = {atom("_NET_WM_SYNC_REQUEST")};
    XSetWMProtocols(d, synced, sync_protocols, 1);
    XStoreName(d, synced, "Sync resize test window");
    XMapWindow(d, synced);
    pump();
    position(synced, &x, &y);
    XTestFakeMotionEvent(d, DefaultScreen(d), x + 259, y + 90, CurrentTime);
    send(synced, "_NET_WM_MOVERESIZE", x + 259, y + 90, 3, 1, 2);
    XTestFakeMotionEvent(d, DefaultScreen(d), x + 319, y + 90, CurrentTime);
    pump();
    XSyncValue current;
    check(XSyncQueryCounter(d, counter, &current) && (XSyncValueLow32(current) & 1) == 1,
          "sync resize advances the client counter to an odd value");
    XSyncIntToValue(&current, XSyncValueLow32(current) + 1);
    XSyncSetCounter(d, counter, current);
    pump();
    XTestFakeMotionEvent(d, DefaultScreen(d), x + 349, y + 90, CurrentTime);
    pump();
    check(attrs(synced).width == 350, "sync resize applies after acknowledgement");
    send(synced, "_NET_WM_MOVERESIZE", 0, 0, 11, 0, 0);
    XSyncDestroyCounter(d, counter);
    XDestroyWindow(d, synced);
    pump();
    cleanup();
    pump();
    check(parent(w) == root && attrs(w).map_state == IsViewable,
          "graceful WM exit reparents live apps");
    FILE *keys = fopen("/tmp/rill-wm-test-keys", "w");
    check(keys != NULL, "write key configuration");
    if (keys) {
        fputs("# rill test bindings\nnot a binding\nclose = Ctrl+Alt+q\ncycle = Alt+Tab\n", keys);
        fclose(keys);
    }
    setenv("RILL_WM_KEYS", "/tmp/rill-wm-test-keys", 1);
    setenv("RILL_WM_FOCUS_MODE", "follows-mouse", 1);
    start(argv[1]);
    check(parent(w) != root, "new WM adopts live app");
    /* The adopted app lives on workspace 3; activate it so key bindings have a target. */
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    while (XPending(d)) {
        XEvent discard;
        XNextEvent(d, &discard);
    }
    key(XK_Alt_L, XK_F4);
    int default_close = 0;
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ClientMessage && e.xclient.message_type == atom("WM_PROTOCOLS") &&
            (Atom)e.xclient.data.l[0] == atom("WM_DELETE_WINDOW"))
            default_close = 1;
    }
    check(!default_close, "rebound close stops firing on Alt-F4");
    KeyCode ctrl = XKeysymToKeycode(d, XK_Control_L);
    KeyCode q = XKeysymToKeycode(d, XK_q);
    XTestFakeKeyEvent(d, ctrl, True, CurrentTime);
    XTestFakeKeyEvent(d, alt, True, CurrentTime);
    XTestFakeKeyEvent(d, q, True, CurrentTime);
    XTestFakeKeyEvent(d, q, False, CurrentTime);
    XTestFakeKeyEvent(d, alt, False, CurrentTime);
    XTestFakeKeyEvent(d, ctrl, False, CurrentTime);
    pump();
    int configured_close = 0;
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ClientMessage && e.xclient.message_type == atom("WM_PROTOCOLS") &&
            (Atom)e.xclient.data.l[0] == atom("WM_DELETE_WINDOW"))
            configured_close = 1;
    }
    check(configured_close, "configured Ctrl-Alt-q closes the focused window");
    /* Focus-follows-mouse moves focus without clicks in this mode. */
    position(w, &x, &y);
    XTestFakeMotionEvent(d, DefaultScreen(d), x + 160, y + 100, CurrentTime);
    pump();
    check(value(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 0) == w,
          "pointer entry focuses a window in follows-mouse mode");
    position(w, &original_x, &original_y);
    int normal_width = attrs(w).width, normal_height = attrs(w).height;
    send(w, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    kill(manager, SIGKILL);
    waitpid(manager, NULL, 0);
    manager = 0;
    pump();
    start(argv[1]);
    send(w, "_NET_ACTIVE_WINDOW", 2, 0, 0, 0, 0);
    send(w, "_NET_WM_STATE", 0, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
         atom("_NET_WM_STATE_MAXIMIZED_VERT"), 0, 0);
    position(w, &x, &y);
    check(x == original_x && y == original_y && attrs(w).width == normal_width &&
          attrs(w).height == normal_height, "WM crash preserves pre-maximize geometry");
    XDestroyWindow(d, w);
    XDestroyWindow(d, dock);
    pump();
    cleanup();
    XCloseDisplay(d);
    puts("Rill WM protocol, input, restoration and compositing tests passed");
    return 0;
}
