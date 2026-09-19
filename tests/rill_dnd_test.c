/* XDND protocol test: an independent raw-Xlib source and target talk to
 * Rill's implementation in both directions, including the proxy indirection
 * Rill uses for Kryon-owned windows. */
#include "../include/rill_dnd.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    Display *display;
    Window window;
    Window proxy;
    Atom enter, position, status, leave, drop, finished, selection;
    Atom type_list, uri_list, aware, proxy_atom, action_copy, action_move;
} Peer;

static void
peer_init(Peer *peer)
{
    peer->display = XOpenDisplay(NULL);
    assert(peer->display != NULL);
    peer->window = XCreateSimpleWindow(peer->display, DefaultRootWindow(peer->display),
                                       40, 40, 200, 200, 0, 0, 0);
    assert(peer->window != None);
    XSelectInput(peer->display, peer->window, 0);
    XMapWindow(peer->display, peer->window);
    peer->proxy = None;
    Atom type = XInternAtom(peer->display, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(peer->display, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(peer->display, peer->window, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&dock, 1);
    peer->enter = XInternAtom(peer->display, "XdndEnter", False);
    peer->position = XInternAtom(peer->display, "XdndPosition", False);
    peer->status = XInternAtom(peer->display, "XdndStatus", False);
    peer->leave = XInternAtom(peer->display, "XdndLeave", False);
    peer->drop = XInternAtom(peer->display, "XdndDrop", False);
    peer->finished = XInternAtom(peer->display, "XdndFinished", False);
    peer->selection = XInternAtom(peer->display, "XdndSelection", False);
    peer->type_list = XInternAtom(peer->display, "XdndTypeList", False);
    peer->uri_list = XInternAtom(peer->display, "text/uri-list", False);
    peer->aware = XInternAtom(peer->display, "XdndAware", False);
    peer->proxy_atom = XInternAtom(peer->display, "XdndProxy", False);
    peer->action_copy = XInternAtom(peer->display, "XdndActionCopy", False);
    peer->action_move = XInternAtom(peer->display, "XdndActionMove", False);
    XFlush(peer->display);
}

static void
peer_send(Peer *peer, Window destination, Atom message, unsigned long a,
          unsigned long b, unsigned long c, unsigned long d, unsigned long e)
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
    XSendEvent(peer->display, destination, False, NoEventMask, &event);
    XFlush(peer->display);
}

static void
pause_ms(int milliseconds)
{
    usleep((useconds_t)milliseconds * 1000);
}

static Window
read_proxy_property(Display *display, Window window, Atom property)
{
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    Window result = None;
    if(XGetWindowProperty(display, window, property, 0, 4, False, XA_WINDOW,
                          &actual, &format, &count, &remaining,
                          &data) == Success && data != NULL && count > 0) {
        result = ((Window *)data)[0];
        XFree(data);
    }
    return result;
}

/* ---- Direction 1: an external source drops onto Rill's target ---- */

static void
test_receive_drop(void)
{
    Peer peer;
    peer_init(&peer);
    assert(RillDndTargetInit((unsigned long)peer.window));
    /* A source resolves the proxy exactly like GTK: the window names a proxy
     * and the proxy points back to itself. */
    Window proxy = read_proxy_property(peer.display, peer.window, peer.proxy_atom);
    assert(proxy != None);
    assert(read_proxy_property(peer.display, proxy, peer.proxy_atom) == proxy);

    peer_send(&peer, proxy, peer.enter, peer.window, 5UL, peer.uri_list, 0, 0);
    peer_send(&peer, proxy, peer.position, peer.window, 0,
              (60UL << 16) | 70UL, 12345UL, peer.action_move);

    int accepted = 0;
    unsigned long action = 0;
    for(int attempt = 0; attempt < 100 && !accepted; attempt++) {
        RillDndTargetPoll(NULL); /* lets the target process and answer */
        while(XPending(peer.display) > 0) {
            XEvent event;
            XNextEvent(peer.display, &event);
            if(event.type == ClientMessage &&
               event.xclient.message_type == peer.status) {
                accepted = (event.xclient.data.l[1] & 1) != 0;
                action = (unsigned long)event.xclient.data.l[4];
            }
        }
        if(!accepted)
            pause_ms(10);
    }
    assert(accepted);
    assert(action == peer.action_move);

    XSetSelectionOwner(peer.display, peer.selection, peer.window, CurrentTime);
    peer_send(&peer, proxy, peer.drop, peer.window, 0, 12345UL, 0, 0);

    const char *payload = "file:///tmp/one.txt\r\nfile:///tmp/two%20words.txt\r\n";
    int got_selection_request = 0;
    for(int attempt = 0; attempt < 100 && !got_selection_request; attempt++) {
        RillDndTargetPoll(NULL); /* drives the conversion request */
        while(XPending(peer.display) > 0) {
            XEvent event;
            XNextEvent(peer.display, &event);
            if(event.type == SelectionRequest &&
               event.xselectionrequest.selection == peer.selection) {
                XChangeProperty(peer.display, event.xselectionrequest.requestor,
                                event.xselectionrequest.property, peer.uri_list, 8,
                                PropModeReplace, (const unsigned char *)payload,
                                (int)strlen(payload));
                XEvent notify;
                memset(&notify, 0, sizeof(notify));
                notify.type = SelectionNotify;
                notify.xselection.requestor = event.xselectionrequest.requestor;
                notify.xselection.selection = peer.selection;
                notify.xselection.target = event.xselectionrequest.target;
                notify.xselection.property = event.xselectionrequest.property;
                notify.xselection.time = event.xselectionrequest.time;
                XSendEvent(peer.display, event.xselectionrequest.requestor,
                           False, NoEventMask, &notify);
                got_selection_request = 1;
            }
        }
        if(!got_selection_request)
            pause_ms(10);
    }
    assert(got_selection_request);

    RillDndDrop drop;
    int delivered = 0;
    for(int attempt = 0; attempt < 100 && !delivered; attempt++) {
        if(RillDndTargetPoll(&drop))
            delivered = 1;
        else
            pause_ms(10);
    }
    assert(delivered);
    assert(drop.count == 2);
    assert(strcmp(drop.uris[0], "file:///tmp/one.txt") == 0);
    assert(strcmp(drop.uris[1], "file:///tmp/two%20words.txt") == 0);
    assert(drop.x == 60 && drop.y == 70);
    assert(drop.move);

    RillDndTargetFinish(1);
    int saw_finished = 0;
    for(int attempt = 0; attempt < 100 && !saw_finished; attempt++) {
        while(XPending(peer.display) > 0) {
            XEvent event;
            XNextEvent(peer.display, &event);
            if(event.type == ClientMessage &&
               event.xclient.message_type == peer.finished) {
                assert((Window)event.xclient.data.l[0] == proxy);
                saw_finished = (event.xclient.data.l[1] & 1) != 0;
            }
        }
        if(!saw_finished)
            pause_ms(10);
    }
    assert(saw_finished);
    RillDndTargetShutdown();
    XDestroyWindow(peer.display, peer.window);
    XCloseDisplay(peer.display);
    puts("XDND target received an external drop through the proxy");
}

/* ---- Direction 2: Rill's source drags onto an external target ---- */

static void
test_send_drag(void)
{
    Peer peer;
    peer_init(&peer);
    /* The external target advertises a proxy as well, so Rill's source has to
     * follow the same indirection. */
    peer.proxy = XCreateSimpleWindow(peer.display, DefaultRootWindow(peer.display),
                                     -1, -1, 1, 1, 0, 0, 0);
    Atom version = 5;
    XChangeProperty(peer.display, peer.window, peer.aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&version, 1);
    XChangeProperty(peer.display, peer.window, peer.proxy_atom, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&peer.proxy, 1);
    XChangeProperty(peer.display, peer.proxy, peer.proxy_atom, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&peer.proxy, 1);
    XChangeProperty(peer.display, peer.proxy, peer.aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&version, 1);
    XSync(peer.display, False);
    XWarpPointer(peer.display, None, DefaultRootWindow(peer.display), 0, 0, 0, 0,
                 120, 120);
    XFlush(peer.display);
    pause_ms(50);

    const char *uris[] = {"file:///tmp/rill-a.bin", "file:///tmp/rill-b.bin"};
    assert(RillDndSourceStart(uris, 2));
    char received[512] = "";
    int finished = 0;
    for(int attempt = 0; attempt < 400; attempt++) {
        int state = RillDndSourceUpdate(120, 120, attempt < 100);
        while(XPending(peer.display) > 0) {
            XEvent event;
            XNextEvent(peer.display, &event);
            if(event.type == ClientMessage &&
               event.xclient.message_type == peer.enter) {
                assert((Window)event.xclient.data.l[0] != None);
                assert(((unsigned long)event.xclient.data.l[1] & 0xffUL) == 5UL);
            } else if(event.type == ClientMessage &&
                      event.xclient.message_type == peer.position) {
                Window from = (Window)event.xclient.data.l[0];
                peer_send(&peer, from, peer.status, peer.proxy, 3UL, 0, 0,
                          peer.action_copy);
            } else if(event.type == ClientMessage &&
                      event.xclient.message_type == peer.drop) {
                Window from = (Window)event.xclient.data.l[0];
                Atom property = XInternAtom(peer.display, "PEER_DND", False);
                XConvertSelection(peer.display, peer.selection, peer.uri_list,
                                  property, peer.window, CurrentTime);
                XFlush(peer.display);
                /* The selection owner (Rill's source) must keep pumping to
                 * answer the conversion while we wait for the data. */
                for(int inner = 0; inner < 200 && !finished; inner++) {
                    RillDndSourceUpdate(120, 120, 0);
                    XEvent selection;
                    if(XCheckTypedEvent(peer.display, SelectionNotify, &selection)) {
                        Atom actual;
                        int format;
                        unsigned long count, remaining;
                        unsigned char *data = NULL;
                        if(selection.xselection.property == property &&
                           XGetWindowProperty(peer.display, peer.window, property,
                                              0, 512, True, AnyPropertyType,
                                              &actual, &format, &count, &remaining,
                                              &data) == Success && data != NULL) {
                            size_t copy = count < sizeof(received) - 1 ?
                                          count : sizeof(received) - 1;
                            memcpy(received, data, copy);
                            received[copy] = '\0';
                            XFree(data);
                        }
                        peer_send(&peer, from, peer.finished, peer.window, 1UL,
                                  0, 0, 0);
                        finished = 1;
                    }
                    pause_ms(5);
                }
            }
        }
        if(finished || state == 2)
            break;
        pause_ms(5);
    }
    assert(finished);
    assert(strstr(received, "file:///tmp/rill-a.bin") != NULL);
    assert(strstr(received, "file:///tmp/rill-b.bin") != NULL);
    RillDndSourceAbort();
    XDestroyWindow(peer.display, peer.window);
    XDestroyWindow(peer.display, peer.proxy);
    XCloseDisplay(peer.display);
    puts("XDND source delivered a drag to an external proxy target");
}

int
main(void)
{
    test_receive_drop();
    test_send_drag();
    puts("XDND protocol target and source passed");
    return 0;
}
