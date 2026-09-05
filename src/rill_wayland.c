#include "rill_wayland.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

int RillWaylandSession(void)
{
    const char *display = getenv("WAYLAND_DISPLAY");
    return getenv("RILL_CONTAINED_X11") == NULL && display != NULL && display[0] != '\0';
}

#ifdef RILL_HAS_WAYLAND
#include <wayland-client.h>
#include <poll.h>
#include <errno.h>
#include "toplevel-client.h"

typedef struct Task {
    struct Task *next;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    RillTask current;
    RillTask pending;
    int ready;
} Task;

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_seat *seat;
static struct zwlr_foreign_toplevel_manager_v1 *manager;
static unsigned int seat_name, manager_name;
static Task *tasks;
static int next_id = 1;

static void title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *value)
{
    Task *task = data;
    (void)handle;
    snprintf(task->pending.title, sizeof(task->pending.title), "%s", value);
}

static void app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *value)
{
    (void)data; (void)handle; (void)value;
}

static void output(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *value)
{
    (void)data; (void)handle; (void)value;
}

static void state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_array *values)
{
    Task *task = data;
    uint32_t *value;
    (void)handle;
    task->pending.focused = 0;
    wl_array_for_each(value, values)
        if(*value == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            task->pending.focused = 1;
}

static void done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    Task *task = data;
    (void)handle;
    task->current = task->pending;
    task->ready = 1;
}

static void closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    Task *task = data, **link = &tasks;
    while(*link != NULL && *link != task) link = &(*link)->next;
    if(*link != NULL) *link = task->next;
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    free(task);
}

static void parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                   struct zwlr_foreign_toplevel_handle_v1 *value)
{
    (void)data; (void)handle; (void)value;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener task_listener = {
    title, app_id, output, output, state, done, closed, parent
};

static void toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *object,
                     struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    Task *task = calloc(1, sizeof(*task));
    (void)data; (void)object;
    if(task == NULL || next_id == INT_MAX) {
        free(task);
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    task->handle = handle;
    task->pending.id = next_id++;
    task->pending.platform_owned = 1;
    task->next = tasks;
    tasks = task;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &task_listener, task);
}

static void finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *object)
{
    (void)data;
    zwlr_foreign_toplevel_manager_v1_destroy(object);
    manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    toplevel, finished
};

static void capabilities(void *data, struct wl_seat *object, uint32_t caps)
{
    (void)data; (void)object; (void)caps;
}

static const struct wl_seat_listener seat_listener = {capabilities, NULL};

static void global(void *data, struct wl_registry *object, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    if(strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 && manager == NULL) {
        manager = wl_registry_bind(object, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                    version < 3 ? version : 3);
        manager_name = name;
        zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener, NULL);
    } else if(strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL) {
        seat = wl_registry_bind(object, name, &wl_seat_interface, 1);
        seat_name = name;
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void removed(void *data, struct wl_registry *object, uint32_t name)
{
    (void)data; (void)object;
    if(name == seat_name && seat != NULL) { wl_seat_destroy(seat); seat = NULL; }
    if(name == manager_name && manager != NULL) {
        zwlr_foreign_toplevel_manager_v1_destroy(manager);
        manager = NULL;
    }
}

static const struct wl_registry_listener registry_listener = {global, removed};

void RillWaylandShutdown(void)
{
    while(tasks != NULL) closed(tasks, tasks->handle);
    if(manager != NULL) zwlr_foreign_toplevel_manager_v1_destroy(manager);
    if(seat != NULL) wl_seat_destroy(seat);
    if(registry != NULL) wl_registry_destroy(registry);
    if(display != NULL) wl_display_disconnect(display);
    display = NULL; registry = NULL; manager = NULL; seat = NULL;
    seat_name = manager_name = 0;
}

static int refresh(void)
{
    struct pollfd fd;
    if(!RillWaylandSession()) return 0;
    if(display == NULL) {
        display = wl_display_connect(NULL);
        if(display == NULL) return 0;
        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registry_listener, NULL);
        if(wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) goto failed;
    }
    while(wl_display_prepare_read(display) != 0)
        if(wl_display_dispatch_pending(display) < 0) goto failed;
    fd.fd = wl_display_get_fd(display); fd.events = POLLIN; fd.revents = 0;
    if(wl_display_flush(display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        goto failed;
    }
    if(poll(&fd, 1, 0) > 0) {
        if(wl_display_read_events(display) < 0) goto failed;
    } else {
        wl_display_cancel_read(display);
    }
    if(wl_display_dispatch_pending(display) < 0) goto failed;
    return manager != NULL;
failed:
    RillWaylandShutdown();
    return 0;
}

int RillWaylandAvailable(void) { return refresh(); }

int RillWaylandListTasks(RillTask *out, int cap)
{
    int count = 0;
    if(out == NULL || cap <= 0 || !refresh()) return 0;
    for(Task *task = tasks; task != NULL && count < cap; task = task->next)
        if(task->ready) out[count++] = task->current;
    return count;
}

static int action(int id, int focus)
{
    if(!refresh() || (focus && seat == NULL)) return 0;
    for(Task *task = tasks; task != NULL; task = task->next) {
        if(task->current.id != id || !task->ready) continue;
        if(focus) {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(task->handle);
            zwlr_foreign_toplevel_handle_v1_activate(task->handle, seat);
        } else {
            zwlr_foreign_toplevel_handle_v1_close(task->handle);
        }
        return wl_display_flush(display) >= 0 || errno == EAGAIN;
    }
    return 0;
}

int RillWaylandFocusTask(int id) { return action(id, 1); }
int RillWaylandCloseTask(int id) { return action(id, 0); }
#else
int RillWaylandAvailable(void) { return 0; }
int RillWaylandListTasks(RillTask *out, int cap) { (void)out; (void)cap; return 0; }
int RillWaylandFocusTask(int id) { (void)id; return 0; }
int RillWaylandCloseTask(int id) { (void)id; return 0; }
void RillWaylandShutdown(void) {}
#endif
