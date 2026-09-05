/* XRender compositor for root children, including menus and override-redirect windows. */
#include "wm_compositor.h"
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Surface {
    struct Surface *next;
    Window window;
    Damage damage;
    int seen;
} Surface;
static Display *connection;
static Window root_window, overlay, owner_window;
static Atom selection, opacity_atom;
static Surface *surfaces;
static Picture output, buffer_picture;
static Pixmap buffer;
static int damage_event, width, height, dirty;

static void watch(Window window)
{
    for (Surface *s = surfaces; s; s = s->next)
        if (s->window == window) {
            s->seen = 1;
            return;
        }
    XWindowAttributes a;
    if (!XGetWindowAttributes(connection, window, &a) || a.class == InputOnly)
        return;
    Surface *s = calloc(1, sizeof(*s));
    if (!s)
        return;
    s->window = window;
    s->seen = 1;
    s->damage = XDamageCreate(connection, window, XDamageReportNonEmpty);
    s->next = surfaces;
    surfaces = s;
    XSelectInput(connection, window, a.your_event_mask | StructureNotifyMask | PropertyChangeMask);
}
int CompositorStart(Display *display, Window root, Window owner)
{
    int event, error, major = 0, minor = 2;
    if (!XCompositeQueryExtension(display, &event, &error) ||
        !XCompositeQueryVersion(display, &major, &minor) || (major == 0 && minor < 2) ||
        !XRenderQueryExtension(display, &event, &error) ||
        !XDamageQueryExtension(display, &damage_event, &error) ||
        !XFixesQueryExtension(display, &event, &error))
        return 0;
    char name[40];
    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", DefaultScreen(display));
    selection = XInternAtom(display, name, False);
    if (XGetSelectionOwner(display, selection) != None)
        return 0;
    connection = display;
    root_window = root;
    owner_window = owner;
    opacity_atom = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);
    XSetSelectionOwner(display, selection, owner, CurrentTime);
    if (XGetSelectionOwner(display, selection) != owner) {
        connection = NULL;
        return 0;
    }
    XEvent notice = {0};
    notice.xclient.type = ClientMessage;
    notice.xclient.window = root;
    notice.xclient.message_type = XInternAtom(display, "MANAGER", False);
    notice.xclient.format = 32;
    notice.xclient.data.l[1] = selection;
    notice.xclient.data.l[2] = owner;
    XSendEvent(display, root, False, StructureNotifyMask, &notice);
    XCompositeRedirectSubwindows(display, root, CompositeRedirectManual);
    overlay = XCompositeGetOverlayWindow(display, root);
    XserverRegion empty = XFixesCreateRegion(display, NULL, 0);
    XFixesSetWindowShapeRegion(display, overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(display, empty);
    XWindowAttributes a;
    XGetWindowAttributes(display, overlay, &a);
    output =
        XRenderCreatePicture(display, overlay, XRenderFindVisualFormat(display, a.visual), 0, NULL);
    dirty = 1;
    return 1;
}
void CompositorEvent(XEvent *event)
{
    if (!connection)
        return;
    if (event->type == SelectionClear && event->xselectionclear.selection == selection) {
        CompositorStop();
        return;
    }
    if (event->type == damage_event + XDamageNotify) {
        XDamageNotifyEvent *damage = (XDamageNotifyEvent *)event;
        XDamageSubtract(connection, damage->damage, None, None);
        dirty = 1;
    } else if (event->type == MapNotify || event->type == UnmapNotify ||
               event->type == DestroyNotify || event->type == ConfigureNotify ||
               event->type == CirculateNotify || event->type == PropertyNotify ||
               event->type == Expose)
        dirty = 1;
}
void CompositorPaint(void)
{
    if (!connection || !dirty)
        return;
    dirty = 0;
    XWindowAttributes root_attr;
    if (!XGetWindowAttributes(connection, root_window, &root_attr))
        return;
    if (width != root_attr.width || height != root_attr.height || !buffer) {
        if (buffer_picture)
            XRenderFreePicture(connection, buffer_picture);
        if (buffer)
            XFreePixmap(connection, buffer);
        width = root_attr.width;
        height = root_attr.height;
        buffer = XCreatePixmap(connection, root_window, width, height, root_attr.depth);
        buffer_picture = XRenderCreatePicture(
            connection, buffer, XRenderFindVisualFormat(connection, root_attr.visual), 0, NULL);
    }
    XRenderColor background = {0x2020, 0x2424, 0x2c2c, 0xffff};
    XRenderFillRectangle(connection, PictOpSrc, buffer_picture, &background, 0, 0, width, height);
    Window r, p, *children = NULL;
    unsigned n = 0;
    for (Surface *s = surfaces; s; s = s->next)
        s->seen = 0;
    if (XQueryTree(connection, root_window, &r, &p, &children, &n)) {
        for (unsigned i = 0; i < n; i++) {
            Window window = children[i];
            XWindowAttributes a;
            if (window == overlay || window == owner_window)
                continue;
            watch(window);
            if (!XGetWindowAttributes(connection, window, &a) || a.map_state != IsViewable ||
                a.class == InputOnly || a.width <= 0 || a.height <= 0)
                continue;
            XRenderPictFormat *format = XRenderFindVisualFormat(connection, a.visual);
            if (!format)
                continue;
            Pixmap pixmap = XCompositeNameWindowPixmap(connection, window);
            Picture picture = XRenderCreatePicture(connection, pixmap, format, 0, NULL),
                    mask = None;
            Atom type;
            int bits;
            unsigned long count, after;
            unsigned char *data = NULL;
            if (XGetWindowProperty(connection, window, opacity_atom, 0, 1, False, XA_CARDINAL,
                                   &type, &bits, &count, &after, &data) == Success &&
                data && bits == 32 && count) {
                unsigned long alpha = *(unsigned long *)data;
                if (alpha != 0xffffffffUL) {
                    XRenderColor color = {0, 0, 0, (unsigned short)(alpha >> 16)};
                    mask = XRenderCreateSolidFill(connection, &color);
                }
            }
            if (data)
                XFree(data);
            XserverRegion shape =
                XFixesCreateRegionFromWindow(connection, window, WindowRegionBounding);
            XFixesSetPictureClipRegion(connection, picture, 0, 0, shape);
            XFixesDestroyRegion(connection, shape);
            XRenderComposite(connection, PictOpOver, picture, mask, buffer_picture, 0, 0, 0, 0, a.x,
                             a.y, a.width + 2 * a.border_width, a.height + 2 * a.border_width);
            if (mask)
                XRenderFreePicture(connection, mask);
            XRenderFreePicture(connection, picture);
            XFreePixmap(connection, pixmap);
        }
        if (children)
            XFree(children);
    }
    Surface **link = &surfaces;
    while (*link) {
        Surface *s = *link;
        if (!s->seen) {
            *link = s->next;
            XDamageDestroy(connection, s->damage);
            free(s);
        } else
            link = &s->next;
    }
    XRenderComposite(connection, PictOpSrc, buffer_picture, None, output, 0, 0, 0, 0, 0, 0, width,
                     height);
}
void CompositorStop(void)
{
    if (!connection)
        return;
    while (surfaces) {
        Surface *s = surfaces;
        surfaces = s->next;
        XDamageDestroy(connection, s->damage);
        free(s);
    }
    if (buffer_picture)
        XRenderFreePicture(connection, buffer_picture);
    if (buffer)
        XFreePixmap(connection, buffer);
    if (output)
        XRenderFreePicture(connection, output);
    XCompositeUnredirectSubwindows(connection, root_window, CompositeRedirectManual);
    XCompositeReleaseOverlayWindow(connection, root_window);
    if (XGetSelectionOwner(connection, selection) == owner_window)
        XSetSelectionOwner(connection, selection, None, CurrentTime);
    connection = NULL;
    buffer_picture = output = buffer = 0;
    width = height = 0;
}
