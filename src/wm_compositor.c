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
#include <string.h>

/* Shadows are layered translucent frames; each ring darkens toward the window. */
enum { ShadowExtent = 12, ShadowLayers = 4 };

typedef struct Surface {
    struct Surface *next;
    Window window;
    Damage damage;
    int seen, shadow;
    int x, y, w, h;
} Surface;
static Display *connection;
static Window root_window, overlay, owner_window;
static Atom selection, opacity_atom, type_atom;
static Surface *surfaces;
static Picture output, buffer_picture;
static Pixmap buffer;
static int damage_event, width, height, dirty;
static XserverRegion damage_region;
static int shadows = 1;

static void damage_add_rectangle(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (!damage_region)
        damage_region = XFixesCreateRegion(connection, NULL, 0);
    XRectangle rectangle = {(short)x, (short)y, (unsigned short)w, (unsigned short)h};
    XserverRegion part = XFixesCreateRegion(connection, &rectangle, 1);
    XFixesUnionRegion(connection, damage_region, damage_region, part);
    XFixesDestroyRegion(connection, part);
    dirty = 1;
}
static void damage_add_surface(Surface *s)
{
    int extent = shadows ? ShadowExtent : 0;
    damage_add_rectangle(s->x - extent, s->y - extent, s->w + 2 * extent, s->h + 2 * extent);
}
static void damage_discard(void)
{
    if (damage_region) {
        XFixesDestroyRegion(connection, damage_region);
        damage_region = None;
    }
    dirty = 0;
}
static void draw_shadow(int x, int y, int w, int h)
{
    /* Extent 12 pixels in four rings; the innermost band is darkest. */
    static const unsigned short alphas[ShadowLayers] = {0x0600, 0x0a00, 0x0e00, 0x1200};
    for (int i = 0; i < ShadowLayers; i++) {
        XRenderColor color = {0, 0, 0, alphas[i]};
        int inset = ShadowExtent - (i + 1) * (ShadowExtent / ShadowLayers);
        XRenderFillRectangle(connection, PictOpOver, buffer_picture, &color,
                             x - ShadowExtent + inset, y - ShadowExtent + inset,
                             w + 2 * (ShadowExtent - inset), h + 2 * (ShadowExtent - inset));
    }
}
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
    s->shadow = 1;
    s->x = a.x;
    s->y = a.y;
    s->w = a.width + 2 * a.border_width;
    s->h = a.height + 2 * a.border_width;
    s->damage = XDamageCreate(connection, window, XDamageReportNonEmpty);
    s->next = surfaces;
    surfaces = s;
    /* Docks and desktop surfaces sit flat against the screen edge. */
    Atom actual;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(connection, window, type_atom, 0, 64, False, XA_ATOM, &actual, &format,
                           &count, &after, &data) == Success &&
        data && format == 32) {
        Atom dock = XInternAtom(connection, "_NET_WM_WINDOW_TYPE_DOCK", False),
             desk = XInternAtom(connection, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        for (unsigned long i = 0; i < count; i++)
            if (((Atom *)data)[i] == dock || ((Atom *)data)[i] == desk)
                s->shadow = 0;
    }
    if (data)
        XFree(data);
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
    type_atom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const char *shadow_setting = getenv("RILL_WM_SHADOWS");
    if (shadow_setting && strcmp(shadow_setting, "0") == 0)
        shadows = 0;
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
        if (!damage_region)
            damage_region = XFixesCreateRegion(connection, NULL, 0);
        /* Damage rectangles are window-relative; the notify carries the root
           geometry of the window at damage time. */
        XserverRegion part = XFixesCreateRegion(connection, NULL, 0);
        XDamageSubtract(connection, damage->damage, None, part);
        XFixesTranslateRegion(connection, part, damage->geometry.x, damage->geometry.y);
        XFixesUnionRegion(connection, damage_region, damage_region, part);
        XFixesDestroyRegion(connection, part);
        dirty = 1;
        return;
    }
    /* Structure events arrive both directly on watched windows and through the
       root substructure mask; the child window is the target either way. */
    Window window = event->xany.window;
    switch (event->type) {
    case MapNotify:
        window = event->xmap.window;
        break;
    case UnmapNotify:
        window = event->xunmap.window;
        break;
    case DestroyNotify:
        window = event->xdestroywindow.window;
        break;
    case ConfigureNotify:
        window = event->xconfigure.window;
        break;
    case CirculateNotify:
        window = event->xcirculate.window;
        break;
    default:
        break;
    }
    Surface *s = NULL;
    for (Surface *scan = surfaces; scan; scan = scan->next)
        if (scan->window == window) {
            s = scan;
            break;
        }
    if (event->type == Expose) {
        /* Expose rectangles are window-relative; damage is in root space. */
        int x = s ? s->x : 0, y = s ? s->y : 0;
        damage_add_rectangle(x + event->xexpose.x, y + event->xexpose.y, event->xexpose.width,
                             event->xexpose.height);
        return;
    }
    if (!s) {
        /* Newly mapped windows are not watched yet; paint where they appeared. */
        if (event->type == MapNotify && window != owner_window && window != overlay) {
            XWindowAttributes a;
            if (XGetWindowAttributes(connection, window, &a) && a.map_state != IsUnmapped &&
                a.class != InputOnly) {
                int extent = shadows ? ShadowExtent : 0;
                damage_add_rectangle(a.x - a.border_width - extent, a.y - a.border_width - extent,
                                     a.width + 2 * a.border_width + 2 * extent,
                                     a.height + 2 * a.border_width + 2 * extent);
            }
        }
        return;
    }
    if (event->type == MapNotify || event->type == CirculateNotify)
        damage_add_surface(s);
    else if (event->type == UnmapNotify || event->type == DestroyNotify)
        damage_add_surface(s);
    else if (event->type == ConfigureNotify) {
        damage_add_surface(s);
        s->x = event->xconfigure.x;
        s->y = event->xconfigure.y;
        s->w = event->xconfigure.width + 2 * event->xconfigure.border_width;
        s->h = event->xconfigure.height + 2 * event->xconfigure.border_width;
        damage_add_surface(s);
    } else if (event->type == PropertyNotify)
        damage_add_surface(s);
}
void CompositorPaint(void)
{
    if (!connection || !dirty)
        return;
    XWindowAttributes root_attr;
    if (!XGetWindowAttributes(connection, root_window, &root_attr))
        return;
    int full = !damage_region;
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
        full = 1;
    }
    if (full)
        damage_discard();
    else
        XFixesSetPictureClipRegion(connection, buffer_picture, 0, 0, damage_region);
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
            if (shadows) {
                Surface *s = NULL;
                for (Surface *scan = surfaces; scan; scan = scan->next)
                    if (scan->window == window) {
                        s = scan;
                        break;
                    }
                if (s && s->shadow)
                    draw_shadow(a.x - a.border_width, a.y - a.border_width,
                                a.width + 2 * a.border_width, a.height + 2 * a.border_width);
            }
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
        } else {
            XWindowAttributes a;
            if (XGetWindowAttributes(connection, s->window, &a)) {
                s->x = a.x;
                s->y = a.y;
                s->w = a.width + 2 * a.border_width;
                s->h = a.height + 2 * a.border_width;
            }
            link = &s->next;
        }
    }
    if (!full)
        XFixesSetPictureClipRegion(connection, output, 0, 0, damage_region);
    XRenderComposite(connection, PictOpSrc, buffer_picture, None, output, 0, 0, 0, 0, 0, 0, width,
                     height);
    if (!full) {
        XFixesSetPictureClipRegion(connection, output, 0, 0, None);
        XFixesSetPictureClipRegion(connection, buffer_picture, 0, 0, None);
    }
    damage_discard();
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
    damage_discard();
    connection = NULL;
    buffer_picture = output = buffer = 0;
    width = height = 0;
}
