#include "rill_x11.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define Font X11Font
#define Screen X11Screen
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#undef Font
#undef Screen

/* Protocol tests never create renderer textures. */
void UnloadTexture(Texture2D texture) { assert(texture.id == 0); }

static void poll_manager(RillX11Manager *wm, Display *client)
{
    XSync(client, False);
    for(int i = 0; i < 10; i++) {
        RillX11Poll(wm);
        XSync(wm->display, False);
        usleep(1000);
    }
}

int main(void)
{
    RillX11Manager wm;
    Display *client;
    Window window;
    Atom protocols, delete_window;
    XEvent event = {0};
    RillX11Init(&wm);
    assert(RillX11StartRoot(&wm));
    client = XOpenDisplay(NULL);
    assert(client != NULL);
    window = XCreateSimpleWindow(client, DefaultRootWindow(client), 20, 20, 320, 240, 0, 0, 0);
    protocols = XInternAtom(client, "WM_PROTOCOLS", False);
    delete_window = XInternAtom(client, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(client, window, &delete_window, 1);
    XStoreName(client, window, "Protocol test");
    /* A taskbar hint must not stop the WM from mapping a window. */
    Atom skip = XInternAtom(client, "_NET_WM_STATE_SKIP_TASKBAR", False);
    XChangeProperty(client, window, XInternAtom(client, "_NET_WM_STATE", False),
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)&skip, 1);
    XMapWindow(client, window);
    poll_manager(&wm, client);
    assert(wm.client_count == 1);
    assert(wm.clients[0].window == window);
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(client, "_NET_CURRENT_DESKTOP", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    XSendEvent(client, DefaultRootWindow(client), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    poll_manager(&wm, client);
    assert(wm.current_desktop == 1 && wm.client_count == 1);
    XWindowAttributes attrs;
    XGetWindowAttributes(client, window, &attrs);
    assert(attrs.map_state == IsUnmapped);
    event.xclient.data.l[0] = 0;
    XSendEvent(client, DefaultRootWindow(client), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    poll_manager(&wm, client);
    assert(wm.current_desktop == 0 && wm.client_count == 1);
    XGetWindowAttributes(client, window, &attrs);
    assert(attrs.map_state == IsViewable);
    event.xclient.message_type = XInternAtom(client, "_NET_CLOSE_WINDOW", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1234;
    XSendEvent(client, DefaultRootWindow(client), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    poll_manager(&wm, client);
    int received = 0;
    while(XPending(client)) {
        XNextEvent(client, &event);
        if(event.type == ClientMessage && event.xclient.message_type == protocols &&
           (Atom)event.xclient.data.l[0] == delete_window) {
            assert(event.xclient.data.l[1] == 1234);
            received = 1;
        }
    }
    assert(received);
    XDestroyWindow(client, window);
    poll_manager(&wm, client);
    assert(wm.client_count == 0);
    /* Exercise capacity growth and preserve focus when an earlier client exits. */
    Window many[48];
    for(int i = 0; i < 48; i++) {
        many[i] = XCreateSimpleWindow(client, DefaultRootWindow(client),
                                      20, 20, 80, 60, 0, 0, 0);
        XMapWindow(client, many[i]);
    }
    poll_manager(&wm, client);
    assert(wm.client_count == 48);
    Window focused = wm.clients[wm.focused_index].window;
    XDestroyWindow(client, many[0]);
    poll_manager(&wm, client);
    assert(wm.client_count == 47);
    assert(wm.focused_index >= 0 && wm.clients[wm.focused_index].window == focused);
    for(int i = 1; i < 48; i++) XDestroyWindow(client, many[i]);
    poll_manager(&wm, client);
    assert(wm.client_count == 0 && wm.focused_index == -1);
    XCloseDisplay(client);
    RillX11Shutdown(&wm);
    return 0;
}
