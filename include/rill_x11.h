#ifndef RILL_X11_H
#define RILL_X11_H

#include "kryon.h"

#include <stddef.h>
#include <sys/types.h>

typedef struct _XDisplay RillX11Display;
typedef unsigned long RillX11Window;
typedef unsigned long RillX11Atom;
typedef unsigned long RillX11Damage;

#define RILL_X11_INITIAL_WINDOWS 32

typedef struct RillX11Client {
    RillX11Window window;
    RillX11Damage damage;
    Texture2D texture;
    unsigned char *pixels;
    size_t pixel_cap;
    int x;
    int y;
    int w;
    int h;
    int frame_x;
    int frame_y;
    int frame_w;
    int frame_h;
    int desktop;
    int ignore_unmap;
    int mapped;
    int focused;
    int dirty;
    char title[128];
} RillX11Client;

typedef struct RillX11Manager {
    RillX11Display *display;
    RillX11Window root;
    RillX11Window support_window;
    RillX11Atom wm_protocols;
    RillX11Atom wm_delete_window;
    RillX11Atom net_client_list;
    RillX11Atom net_active_window;
    RillX11Atom net_close_window;
    RillX11Atom net_wm_name;
    RillX11Atom utf8_string;
    RillX11Atom wm_name;
    RillX11Atom wm_state;
    RillX11Atom net_wm_pid;
    RillX11Atom net_wm_window_type;
    RillX11Atom net_wm_window_type_desktop;
    RillX11Atom net_wm_window_type_dock;
    RillX11Atom net_wm_state;
    RillX11Atom net_wm_state_skip_taskbar;
    int current_desktop;
    int desktop_count;
    int screen;
    int active;
    int owns_server;
    int damage_event;
    int damage_error;
    int drag_index;
    int drag_dx;
    int drag_dy;
    int focused_index;
    pid_t server_pid;
    char display_name[64];
    RillX11Client *clients;
    int client_capacity;
    int client_count;
} RillX11Manager;

void RillX11SyncDesktop(void);
int RillX11SetDesktop(const char *title);

void RillX11Init(RillX11Manager *wm);
int RillX11StartRoot(RillX11Manager *wm);
int RillX11StartWindowed(RillX11Manager *wm, int width, int height);
const char *RillX11DisplayName(const RillX11Manager *wm);
int RillX11Launch(RillX11Manager *wm, const char *command);
void RillX11Poll(RillX11Manager *wm);
void RillX11ProcessInput(RillX11Manager *wm);
void RillX11Draw(RillX11Manager *wm);
void RillX11Shutdown(RillX11Manager *wm);

#endif
