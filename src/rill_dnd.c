/* XDND drag-and-drop for Rill's own X windows.
 *
 * The desktop and panel windows are created through Kryon's X backend, so
 * ClientMessage events addressed to them arrive on that connection. The XDND
 * proxy mechanism solves this cleanly: the target window carries an XdndProxy
 * property pointing at a small window owned by this module, and every XDND
 * message is then delivered here for handling. The source side drives the
 * protocol from the pointer state that Rill already polls.
 */
#if defined(__linux__) && !defined(KRYON_NATIVE_PLAN9)

#define Font X11Font
#define Screen X11Screen
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#undef Font
#undef Screen

#include "rill_dnd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    Display *display;
    Window proxy;
    Window target;
    Window source;
    int version;
    int offers_uris;
    int accepting;
    unsigned long action;
    unsigned long timestamp;
    int waiting_selection;
    int pending_finish;
    Window pending_source;
    RillDndDrop pending;
    int pending_ready;
    Atom xa_xdnd_aware, xa_xdnd_proxy, xa_xdnd_enter, xa_xdnd_position;
    Atom xa_xdnd_status, xa_xdnd_leave, xa_xdnd_drop, xa_xdnd_finished;
    Atom xa_xdnd_selection, xa_xdnd_type_list, xa_uri_list;
    Atom xa_xdnd_action_copy, xa_xdnd_action_move, xa_xdnd_action_ask;
    Atom xa_incr, xa_targets, xa_property;
} DndTarget;

static DndTarget dnd;

static unsigned long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000);
}

static unsigned long
fake_timestamp(void)
{
    static unsigned long stamp;
    stamp += 16;
    return stamp & 0xffffffffUL;
}

static void
send_client_message(Display *display, Window destination, Atom message,
                    unsigned long a, unsigned long b, unsigned long c,
                    unsigned long d, unsigned long e)
{
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = destination;
    event.xclient.message_type = message;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)a;
    event.xclient.data.l[1] = (long)b;
    event.xclient.data.l[2] = (long)c;
    event.xclient.data.l[3] = (long)d;
    event.xclient.data.l[4] = (long)e;
    XSendEvent(display, destination, False, NoEventMask, &event);
}

static int
window_has_atom(Display *display, Window window, Atom property, Atom expected)
{
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    int found = 0;
    if(XGetWindowProperty(display, window, property, 0, 8, False, XA_ATOM,
                          &actual, &format, &count, &remaining, &data) == Success &&
       data != NULL) {
        if(count > 0 && format == 32)
            found = expected == None || ((Atom *)data)[0] == expected;
        XFree(data);
    }
    return found;
}

int
RillDndTargetInit(unsigned long window)
{
    if(window == 0 || window == (unsigned long)None)
        return 0;
    if(dnd.display != NULL && dnd.target == (Window)window)
        return 1;
    RillDndTargetShutdown();
    dnd.display = XOpenDisplay(NULL);
    if(dnd.display == NULL)
        return 0;
    dnd.xa_xdnd_aware = XInternAtom(dnd.display, "XdndAware", False);
    dnd.xa_xdnd_proxy = XInternAtom(dnd.display, "XdndProxy", False);
    dnd.xa_xdnd_enter = XInternAtom(dnd.display, "XdndEnter", False);
    dnd.xa_xdnd_position = XInternAtom(dnd.display, "XdndPosition", False);
    dnd.xa_xdnd_status = XInternAtom(dnd.display, "XdndStatus", False);
    dnd.xa_xdnd_leave = XInternAtom(dnd.display, "XdndLeave", False);
    dnd.xa_xdnd_drop = XInternAtom(dnd.display, "XdndDrop", False);
    dnd.xa_xdnd_finished = XInternAtom(dnd.display, "XdndFinished", False);
    dnd.xa_xdnd_selection = XInternAtom(dnd.display, "XdndSelection", False);
    dnd.xa_xdnd_type_list = XInternAtom(dnd.display, "XdndTypeList", False);
    dnd.xa_uri_list = XInternAtom(dnd.display, "text/uri-list", False);
    dnd.xa_xdnd_action_copy = XInternAtom(dnd.display, "XdndActionCopy", False);
    dnd.xa_xdnd_action_move = XInternAtom(dnd.display, "XdndActionMove", False);
    dnd.xa_xdnd_action_ask = XInternAtom(dnd.display, "XdndActionAsk", False);
    dnd.xa_incr = XInternAtom(dnd.display, "INCR", False);
    dnd.xa_targets = XInternAtom(dnd.display, "TARGETS", False);
    dnd.xa_property = XInternAtom(dnd.display, "RILL_DND_DATA", False);
    dnd.proxy = XCreateSimpleWindow(dnd.display, DefaultRootWindow(dnd.display),
                                    -1, -1, 1, 1, 0, 0, 0);
    if(dnd.proxy == None) {
        RillDndTargetShutdown();
        return 0;
    }
    XSetWindowAttributes attributes;
    attributes.override_redirect = True;
    XChangeWindowAttributes(dnd.display, dnd.proxy, CWOverrideRedirect, &attributes);
    XSelectInput(dnd.display, dnd.proxy, PropertyChangeMask);
    Atom version = 5;
    XChangeProperty(dnd.display, dnd.proxy, dnd.xa_xdnd_aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&version, 1);
    XChangeProperty(dnd.display, dnd.proxy, dnd.xa_xdnd_proxy, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&dnd.proxy, 1);
    XMapWindow(dnd.display, dnd.proxy);
    XChangeProperty(dnd.display, (Window)window, dnd.xa_xdnd_aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&version, 1);
    XChangeProperty(dnd.display, (Window)window, dnd.xa_xdnd_proxy, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&dnd.proxy, 1);
    XFlush(dnd.display);
    dnd.target = (Window)window;
    return 1;
}

/* Store one completed drop for the UI; the XdndFinished reply waits for it. */
static void
queue_drop(int x, int y, int move, const char *data, int length)
{
    if(dnd.pending_ready || dnd.pending_finish)
        return;
    memset(&dnd.pending, 0, sizeof(dnd.pending));
    dnd.pending.x = x;
    dnd.pending.y = y;
    dnd.pending.move = move;
    int count = 0;
    const char *start = data;
    const char *scan = data;
    const char *end = data + length;
    while(scan <= end && count < RILL_DND_MAX_URIS) {
        if(scan == end || *scan == '\n') {
            size_t entry = (size_t)(scan - start);
            if(entry > 0 && start[entry - 1] == '\r')
                entry--;
            if(entry > 0 && entry < RILL_DND_URI_MAX - 1 && start[0] != '#') {
                memcpy(dnd.pending.uris[count], start, entry);
                dnd.pending.uris[count][entry] = '\0';
                count++;
            }
            start = scan + 1;
        }
        scan++;
    }
    dnd.pending.count = count;
    if(count > 0) {
        dnd.pending_ready = 1;
        dnd.pending_finish = 1;
        dnd.pending_source = dnd.source;
    } else {
        /* Nothing usable: finish the transaction right away. */
        send_client_message(dnd.display, dnd.source, dnd.xa_xdnd_finished,
                            dnd.proxy, 0, 0, 0, 0);
        XFlush(dnd.display);
        dnd.source = None;
    }
}

/* Assemble an INCR transfer chunk by chunk with a bounded wait. */
static char *
read_incr(unsigned long *assembled_length)
{
    unsigned long deadline = now_ms() + 2000;
    char *assembled = NULL;
    unsigned long received = 0;
    while(now_ms() < deadline) {
        XEvent event;
        if(!XCheckWindowEvent(dnd.display, dnd.proxy, PropertyChangeMask, &event)) {
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
            continue;
        }
        if(event.xproperty.state != PropertyNewValue)
            continue;
        Atom actual;
        int format;
        unsigned long count, remaining;
        unsigned char *data = NULL;
        if(XGetWindowProperty(dnd.display, dnd.proxy, dnd.xa_property, 0,
                              8L * 1024 * 1024 / 4, True, AnyPropertyType,
                              &actual, &format, &count, &remaining,
                              &data) != Success || data == NULL)
            continue;
        if(count == 0) {
            XFree(data);
            break;
        }
        char *grown = realloc(assembled, received + count + 1);
        if(grown == NULL) {
            XFree(data);
            break;
        }
        assembled = grown;
        memcpy(assembled + received, data, count);
        received += count;
        assembled[received] = '\0';
        XFree(data);
    }
    *assembled_length = received;
    return assembled;
}

static void
read_selection_property(void)
{
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    dnd.waiting_selection = 0;
    if(XGetWindowProperty(dnd.display, dnd.proxy, dnd.xa_property, 0,
                          8L * 1024 * 1024 / 4, True, AnyPropertyType,
                          &actual, &format, &count, &remaining,
                          &data) != Success || data == NULL)
        return;
    if(actual == dnd.xa_incr && format == 32 && count >= 1) {
        XFree(data);
        unsigned long length = 0;
        char *assembled = read_incr(&length);
        if(assembled != NULL) {
            queue_drop(dnd.pending.x, dnd.pending.y, dnd.pending.move,
                       assembled, (int)length);
            free(assembled);
        }
        return;
    }
    if(actual == dnd.xa_uri_list && format == 8)
        queue_drop(dnd.pending.x, dnd.pending.y, dnd.pending.move,
                   (char *)data, (int)count);
    XFree(data);
}

static void
handle_target_event(XEvent *event)
{
    if(event->type == ClientMessage &&
       event->xclient.message_type == dnd.xa_xdnd_enter) {
        dnd.source = (Window)event->xclient.data.l[0];
        dnd.version = (int)((unsigned long)event->xclient.data.l[1] & 0xffUL);
        dnd.offers_uris = 0;
        unsigned long types[3] = {(unsigned long)event->xclient.data.l[2],
                                  (unsigned long)event->xclient.data.l[3],
                                  (unsigned long)event->xclient.data.l[4]};
        for(int i = 0; i < 3; i++)
            if(types[i] == (unsigned long)dnd.xa_uri_list)
                dnd.offers_uris = 1;
        if(!dnd.offers_uris &&
           ((unsigned long)event->xclient.data.l[1] & 0x100UL) != 0) {
            /* The complete type list lives on the source window. */
            Atom actual;
            int format;
            unsigned long count, remaining;
            unsigned char *data = NULL;
            if(XGetWindowProperty(dnd.display, dnd.source, dnd.xa_xdnd_type_list,
                                  0, 64, False, XA_ATOM, &actual, &format,
                                  &count, &remaining, &data) == Success &&
               data != NULL) {
                for(unsigned long i = 0; i < count; i++)
                    if(((Atom *)data)[i] == dnd.xa_uri_list)
                        dnd.offers_uris = 1;
                XFree(data);
            }
        }
    } else if(event->type == ClientMessage &&
              event->xclient.message_type == dnd.xa_xdnd_position) {
        if((Window)event->xclient.data.l[0] != dnd.source)
            return;
        int x = (int)(((unsigned long)event->xclient.data.l[2] >> 16) & 0xffff);
        int y = (int)((unsigned long)event->xclient.data.l[2] & 0xffff);
        dnd.action = (unsigned long)event->xclient.data.l[4];
        dnd.timestamp = (unsigned long)event->xclient.data.l[3];
        unsigned long answer = dnd.xa_xdnd_action_copy;
        if(dnd.action == (unsigned long)dnd.xa_xdnd_action_move)
            answer = dnd.xa_xdnd_action_move;
        else if(dnd.action == (unsigned long)dnd.xa_xdnd_action_ask)
            answer = dnd.xa_xdnd_action_copy;
        dnd.accepting = dnd.offers_uris;
        dnd.pending.x = x;
        dnd.pending.y = y;
        dnd.pending.move = answer == (unsigned long)dnd.xa_xdnd_action_move;
        send_client_message(dnd.display, dnd.source, dnd.xa_xdnd_status,
                            dnd.proxy, dnd.accepting ? 3UL : 2UL, 0, 0,
                            dnd.accepting ? answer : 0);
        XFlush(dnd.display);
    } else if(event->type == ClientMessage &&
              event->xclient.message_type == dnd.xa_xdnd_leave) {
        if((Window)event->xclient.data.l[0] == dnd.source) {
            dnd.source = None;
            dnd.offers_uris = 0;
            dnd.accepting = 0;
        }
    } else if(event->type == ClientMessage &&
              event->xclient.message_type == dnd.xa_xdnd_drop) {
        if((Window)event->xclient.data.l[0] != dnd.source || !dnd.offers_uris) {
            send_client_message(dnd.display, dnd.source, dnd.xa_xdnd_finished,
                                dnd.proxy, 0, 0, 0, 0);
            XFlush(dnd.display);
            return;
        }
        dnd.timestamp = (unsigned long)event->xclient.data.l[2];
        XConvertSelection(dnd.display, dnd.xa_xdnd_selection, dnd.xa_uri_list,
                          dnd.xa_property, dnd.proxy, CurrentTime);
        dnd.waiting_selection = 1;
        XFlush(dnd.display);
    } else if(event->type == SelectionNotify &&
              event->xselection.property == dnd.xa_property &&
              event->xselection.selection == dnd.xa_xdnd_selection) {
        read_selection_property();
    }
}

int
RillDndTargetPoll(RillDndDrop *drop)
{
    if(dnd.display == NULL)
        return 0;
    while(XPending(dnd.display) > 0) {
        XEvent event;
        XNextEvent(dnd.display, &event);
        handle_target_event(&event);
    }
    if(drop != NULL && dnd.pending_ready) {
        *drop = dnd.pending;
        dnd.pending_ready = 0;
        return 1;
    }
    if(dnd.pending_finish && !dnd.pending_ready) {
        /* Fail safe: never leave a source waiting forever. */
        RillDndTargetFinish(0);
    }
    return 0;
}

void
RillDndTargetFinish(int accepted)
{
    if(dnd.display == NULL || !dnd.pending_finish)
        return;
    send_client_message(dnd.display, dnd.pending_source, dnd.xa_xdnd_finished,
                        dnd.proxy, accepted ? 1UL : 0UL, 0, 0, 0);
    XFlush(dnd.display);
    dnd.pending_finish = 0;
    dnd.source = None;
    dnd.offers_uris = 0;
    dnd.accepting = 0;
}

void
RillDndTargetShutdown(void)
{
    if(dnd.display != NULL) {
        if(dnd.pending_finish)
            RillDndTargetFinish(0);
        if(dnd.proxy != None)
            XDestroyWindow(dnd.display, dnd.proxy);
        XCloseDisplay(dnd.display);
    }
    memset(&dnd, 0, sizeof(dnd));
    dnd.proxy = None;
    dnd.target = None;
    dnd.source = None;
}

/* ---- Source side ---- */

typedef struct {
    int active;
    int completed;
    char *payload;
    int payload_length;
    Window target;
    int accepted;
    int dropping;
    unsigned long drop_started;
    unsigned long last_position;
    Display *display;
    Window source;
    Atom aware, proxy, enter, position, status, leave, drop, finished;
    Atom selection, type_list, uri_list, action_copy;
} DndSource;

static DndSource source_state;

static int
source_open(void)
{
    if(source_state.display != NULL)
        return 1;
    source_state.display = XOpenDisplay(NULL);
    if(source_state.display == NULL)
        return 0;
    source_state.aware = XInternAtom(source_state.display, "XdndAware", False);
    source_state.proxy = XInternAtom(source_state.display, "XdndProxy", False);
    source_state.enter = XInternAtom(source_state.display, "XdndEnter", False);
    source_state.position = XInternAtom(source_state.display, "XdndPosition", False);
    source_state.status = XInternAtom(source_state.display, "XdndStatus", False);
    source_state.leave = XInternAtom(source_state.display, "XdndLeave", False);
    source_state.drop = XInternAtom(source_state.display, "XdndDrop", False);
    source_state.finished = XInternAtom(source_state.display, "XdndFinished", False);
    source_state.selection = XInternAtom(source_state.display, "XdndSelection", False);
    source_state.type_list = XInternAtom(source_state.display, "XdndTypeList", False);
    source_state.uri_list = XInternAtom(source_state.display, "text/uri-list", False);
    source_state.action_copy = XInternAtom(source_state.display, "XdndActionCopy", False);
    source_state.source = XCreateSimpleWindow(source_state.display,
                                              DefaultRootWindow(source_state.display),
                                              -1, -1, 1, 1, 0, 0, 0);
    if(source_state.source == None) {
        XCloseDisplay(source_state.display);
        source_state.display = NULL;
        return 0;
    }
    XSetWindowAttributes attributes;
    attributes.override_redirect = True;
    XChangeWindowAttributes(source_state.display, source_state.source,
                            CWOverrideRedirect, &attributes);
    XMapWindow(source_state.display, source_state.source);
    return 1;
}

/* Deepest visible window below `from` containing the translated point. */
static Window
descend_to_point(Display *display, Window from, int x, int y)
{
    Window current = from;
    for(;;) {
        Window root, parent, *children = NULL;
        unsigned int count;
        if(!XQueryTree(display, current, &root, &parent, &children, &count))
            return current;
        Window next = None;
        for(int i = (int)count - 1; i >= 0; i--) {
            XWindowAttributes attributes;
            if(!XGetWindowAttributes(display, children[i], &attributes))
                continue;
            if(attributes.map_state != IsViewable || attributes.class == InputOnly)
                continue;
            int local_x, local_y;
            Window child;
            if(XTranslateCoordinates(display, DefaultRootWindow(display),
                                     children[i], x, y, &local_x, &local_y,
                                     &child) &&
               local_x >= 0 && local_y >= 0 &&
               local_x < attributes.width && local_y < attributes.height) {
                next = children[i];
                break;
            }
        }
        XFree(children);
        if(next == None)
            return current;
        current = next;
    }
}

/* The XDND target under the pointer: walk up from the deepest window until
 * an XdndAware one appears, then follow its XdndProxy when it has one. */
static Window
find_target_window(Display *display, int x, int y)
{
    Window root, child, current;
    int root_x = x, root_y = y, win_x, win_y;
    unsigned int mask;
    if(!XQueryPointer(display, DefaultRootWindow(display), &root, &child,
                      &root_x, &root_y, &win_x, &win_y, &mask))
        return None;
    current = child != None ? descend_to_point(display, child, root_x, root_y)
                            : None;
    int guard = 0;
    while(current != None && current != root && guard++ < 32) {
        if(window_has_atom(display, current, source_state.aware, None)) {
            Atom actual;
            int format;
            unsigned long count, remaining;
            unsigned char *data = NULL;
            if(XGetWindowProperty(display, current, source_state.proxy,
                                  0, 4, False, XA_WINDOW, &actual, &format,
                                  &count, &remaining, &data) == Success &&
               data != NULL && count > 0) {
                Window proxy = ((Window *)data)[0];
                XFree(data);
                if(proxy != None && proxy != current)
                    return proxy;
            }
            return current;
        }
        Window root2, parent, *children = NULL;
        unsigned int nchildren;
        if(!XQueryTree(display, current, &root2, &parent, &children, &nchildren))
            return None;
        if(children != NULL)
            XFree(children);
        if(parent == None || parent == root2)
            return None;
        current = parent;
    }
    return None;
}

static void
source_cleanup(void)
{
    if(source_state.display != NULL)
        XFlush(source_state.display);
    source_state.active = 0;
    source_state.dropping = 0;
    source_state.accepted = 0;
    source_state.target = None;
    source_state.payload_length = 0;
    free(source_state.payload);
    source_state.payload = NULL;
}

static void
serve_selection_request(XSelectionRequestEvent *request)
{
    Display *display = source_state.display;
    if(request->target == source_state.uri_list) {
        XChangeProperty(display, request->requestor, request->property,
                        source_state.uri_list, 8, PropModeReplace,
                        (unsigned char *)source_state.payload,
                        source_state.payload_length);
    } else if(request->target == XInternAtom(display, "TARGETS", False)) {
        Atom targets = source_state.uri_list;
        XChangeProperty(display, request->requestor, request->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&targets, 1);
    } else {
        if(request->property != None)
            XChangeProperty(display, request->requestor, request->property,
                            XA_ATOM, 32, PropModeReplace, NULL, 0);
    }
    XEvent notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = SelectionNotify;
    notify.xselection.requestor = request->requestor;
    notify.xselection.selection = request->selection;
    notify.xselection.target = request->target;
    notify.xselection.property = request->property;
    notify.xselection.time = request->time;
    XSendEvent(display, request->requestor, False, NoEventMask, &notify);
}

int
RillDndSourceStart(const char *const *uris, int count)
{
    if(uris == NULL || count < 1 || count > RILL_DND_MAX_URIS || !source_open())
        return 0;
    size_t capacity = 1024;
    char *payload = malloc(capacity);
    size_t length = 0;
    if(payload == NULL)
        return 0;
    for(int i = 0; i < count; i++) {
        if(uris[i] == NULL)
            continue;
        size_t item = strlen(uris[i]);
        if(item >= RILL_DND_URI_MAX) {
            free(payload);
            return 0;
        }
        while(length + item + 3 > capacity) {
            capacity *= 2;
            char *grown = realloc(payload, capacity);
            if(grown == NULL) {
                free(payload);
                return 0;
            }
            payload = grown;
        }
        memcpy(payload + length, uris[i], item);
        length += item;
        payload[length++] = '\r';
        payload[length++] = '\n';
    }
    if(length == 0) {
        free(payload);
        return 0;
    }
    payload[length] = '\0';
    free(source_state.payload);
    source_state.payload = payload;
    source_state.payload_length = (int)length;
    source_state.target = None;
    source_state.accepted = 0;
    source_state.dropping = 0;
    source_state.completed = 0;
    source_state.active = 1;
    XSetSelectionOwner(source_state.display, source_state.selection,
                       source_state.source, CurrentTime);
    XChangeProperty(source_state.display, source_state.source,
                    source_state.type_list, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&source_state.uri_list, 1);
    XFlush(source_state.display);
    return 1;
}

int
RillDndSourceUpdate(int x, int y, int button_down)
{
    if(!source_state.active || source_state.display == NULL)
        return source_state.completed ? 2 : 0;
    while(XPending(source_state.display) > 0) {
        XEvent event;
        XNextEvent(source_state.display, &event);
        if(event.type == ClientMessage &&
           event.xclient.message_type == source_state.status) {
            if((Window)event.xclient.data.l[0] == source_state.target)
                source_state.accepted = (event.xclient.data.l[1] & 1) != 0;
        } else if(event.type == ClientMessage &&
                  event.xclient.message_type == source_state.finished) {
            source_cleanup();
            source_state.completed = 1;
            return 2;
        } else if(event.type == SelectionRequest &&
                  event.xselectionrequest.selection == source_state.selection) {
            serve_selection_request(&event.xselectionrequest);
        }
    }
    if(source_state.dropping) {
        if(now_ms() - source_state.drop_started > 3000) {
            source_cleanup();
            source_state.completed = 1;
            return 2;
        }
        return 1;
    }
    if(!button_down) {
        if(source_state.target != None && source_state.accepted)
            send_client_message(source_state.display, source_state.target,
                                source_state.drop, source_state.source, 0,
                                fake_timestamp(), 0, 0);
        else if(source_state.target != None)
            send_client_message(source_state.display, source_state.target,
                                source_state.leave, source_state.source, 0,
                                0, 0, 0);
        XFlush(source_state.display);
        if(source_state.target == None || !source_state.accepted) {
            source_cleanup();
            source_state.completed = 1;
            return 2;
        }
        source_state.dropping = 1;
        source_state.drop_started = now_ms();
        return 1;
    }
    Window target = find_target_window(source_state.display, x, y);
    if(target == source_state.source)
        target = None;
    if(target != source_state.target) {
        if(source_state.target != None)
            send_client_message(source_state.display, source_state.target,
                                source_state.leave, source_state.source, 0,
                                0, 0, 0);
        source_state.target = target;
        source_state.accepted = 0;
        if(target != None)
            send_client_message(source_state.display, target,
                                source_state.enter, source_state.source,
                                5UL, source_state.uri_list, 0, 0);
    }
    unsigned long now = now_ms();
    if(target != None && now - source_state.last_position > 30) {
        source_state.last_position = now;
        send_client_message(source_state.display, target,
                            source_state.position, source_state.source, 0,
                            ((unsigned long)x << 16) |
                                ((unsigned long)y & 0xffffUL),
                            fake_timestamp(), source_state.action_copy);
        XFlush(source_state.display);
    }
    return 1;
}

void
RillDndSourceAbort(void)
{
    if(source_state.active && source_state.display != NULL &&
       source_state.target != None)
        send_client_message(source_state.display, source_state.target,
                            source_state.leave, source_state.source, 0, 0, 0, 0);
    source_cleanup();
    source_state.completed = 0;
}

#endif /* __linux__ && !KRYON_NATIVE_PLAN9 */

