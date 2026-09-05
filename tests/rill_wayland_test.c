#include "rill_wayland.h"
#include "toplevel-server.h"
#include <wayland-server.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void destroy_resource(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void unset_minimized(struct wl_client *client, struct wl_resource *resource)
{
    (void)client; (void)resource;
}

static void activate(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat)
{
    struct wl_array values;
    uint32_t *value;
    (void)client;
    assert(seat != NULL);
    wl_array_init(&values);
    value = wl_array_add(&values, sizeof(*value));
    *value = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
    zwlr_foreign_toplevel_handle_v1_send_state(resource, &values);
    zwlr_foreign_toplevel_handle_v1_send_title(resource, "Focused native window");
    zwlr_foreign_toplevel_handle_v1_send_done(resource);
    wl_array_release(&values);
}

static void close_window(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    zwlr_foreign_toplevel_handle_v1_send_closed(resource);
}

static const struct zwlr_foreign_toplevel_handle_v1_interface handle_impl = {
    .unset_minimized = unset_minimized,
    .activate = activate,
    .close = close_window,
    .destroy = destroy_resource
};

static void stop(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    zwlr_foreign_toplevel_manager_v1_send_finished(resource);
}

static const struct zwlr_foreign_toplevel_manager_v1_interface manager_impl = {.stop = stop};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *manager = wl_resource_create(client, &zwlr_foreign_toplevel_manager_v1_interface, version, id);
    struct wl_resource *handle = wl_resource_create(client, &zwlr_foreign_toplevel_handle_v1_interface, version, 0);
    struct wl_array state;
    (void)data;
    wl_resource_set_implementation(manager, &manager_impl, NULL, NULL);
    wl_resource_set_implementation(handle, &handle_impl, NULL, NULL);
    zwlr_foreign_toplevel_manager_v1_send_toplevel(manager, handle);
    zwlr_foreign_toplevel_handle_v1_send_title(handle, "Native window");
    zwlr_foreign_toplevel_handle_v1_send_app_id(handle, "test.application");
    wl_array_init(&state);
    zwlr_foreign_toplevel_handle_v1_send_state(handle, &state);
    zwlr_foreign_toplevel_handle_v1_send_done(handle);
    wl_array_release(&state);
}

static void bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *seat = wl_resource_create(client, &wl_seat_interface, version, id);
    (void)data;
    wl_seat_send_capabilities(seat, WL_SEAT_CAPABILITY_KEYBOARD);
}

int main(void)
{
    int pair[2], status, count, id;
    pid_t child;
    RillTask tasks[4];
    char fd[32];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    child = fork();
    assert(child >= 0);
    if(child == 0) {
        struct wl_display *server = wl_display_create();
        close(pair[0]);
        alarm(10);
        assert(wl_global_create(server, &zwlr_foreign_toplevel_manager_v1_interface, 3, NULL, bind_manager));
        assert(wl_global_create(server, &wl_seat_interface, 1, NULL, bind_seat));
        assert(wl_client_create(server, pair[1]));
        wl_display_run(server);
        _exit(0);
    }
    close(pair[1]);
    snprintf(fd, sizeof(fd), "%d", pair[0]);
    setenv("WAYLAND_SOCKET", fd, 1);
    setenv("WAYLAND_DISPLAY", "rill-test", 1);
    unsetenv("RILL_CONTAINED_X11");
    assert(RillWaylandAvailable());
    count = RillWaylandListTasks(tasks, 4);
    assert(count == 1 && strcmp(tasks[0].title, "Native window") == 0);
    id = tasks[0].id;
    assert(!tasks[0].focused && tasks[0].platform_owned);
    assert(RillWaylandFocusTask(id));
    for(int i = 0; i < 100; i++) {
        usleep(1000);
        RillWaylandListTasks(tasks, 4);
        if(tasks[0].focused) break;
    }
    assert(tasks[0].id == id && tasks[0].focused);
    assert(strcmp(tasks[0].title, "Focused native window") == 0);
    assert(RillWaylandCloseTask(id));
    for(int i = 0; i < 100; i++) {
        usleep(1000);
        count = RillWaylandListTasks(tasks, 4);
        if(count == 0) break;
    }
    assert(count == 0 && !RillWaylandFocusTask(id));
    RillWaylandShutdown();
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    return 0;
}
