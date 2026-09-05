#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/shape.h>
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
    cleanup();
    pump();
    check(parent(w) == root && attrs(w).map_state == IsViewable,
          "graceful WM exit reparents live apps");
    start(argv[1]);
    check(parent(w) != root, "new WM adopts live app");
    XDestroyWindow(d, w);
    XDestroyWindow(d, dock);
    pump();
    cleanup();
    XCloseDisplay(d);
    puts("Rill WM protocol, input, restoration and compositing tests passed");
    return 0;
}
