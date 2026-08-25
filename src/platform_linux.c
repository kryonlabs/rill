#include "rill_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtk/gtk.h>

static void
lookup_icon_path(const char *name, char *out, int out_size)
{
    static int gtk_attempted = 0;
    static int gtk_ready = 0;
    GtkIconTheme *theme;
    GdkPixbuf *pixbuf;
    GError *error = NULL;
    char path[512];
    char safe[128];
    int i;

    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(name == NULL || name[0] == '\0')
        return;
    if(!gtk_attempted) {
        gtk_attempted = 1;
        gtk_ready = gtk_init_check(NULL, NULL) ? 1 : 0;
    }
    if(!gtk_ready)
        return;
    theme = gtk_icon_theme_get_default();
    if(theme == NULL)
        return;
    pixbuf = gtk_icon_theme_load_icon(theme, name, 32, 0, &error);
    if(pixbuf == NULL) {
        if(error != NULL)
            g_error_free(error);
        return;
    }
    for(i = 0; name[i] != '\0' && i < (int)sizeof(safe) - 1; i++) {
        char c = name[i];
        safe[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    safe[i] = '\0';
    snprintf(path, sizeof(path), "/tmp/rill-icon-%s.png", safe);
    if(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL))
        snprintf(out, (size_t)out_size, "%s", path);
    if(error != NULL)
        g_error_free(error);
    g_object_unref(pixbuf);
}

static void
launcher(RillLauncher *out, const char *id, const char *name,
         const char *command, const char *icon)
{
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->command, sizeof(out->command), "%s", command);
    lookup_icon_path(icon, out->icon_path, (int)sizeof(out->icon_path));
}

static int
linux_list_launchers(RillLauncher *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    count = 0;
    if(count < cap)
        launcher(&out[count++], "terminal", "Terminal", "host:kapsule",
                 "utilities-terminal");
    if(count < cap)
        launcher(&out[count++], "files", "Files", "host:shelf",
                 "system-file-manager");
    if(count < cap)
        launcher(&out[count++], "settings", "Settings", "internal:settings",
                 "preferences-system");
    if(count < cap)
        launcher(&out[count++], "workbook", "Workbook",
                 "/mnt/storage/Projects/workbook/workbook",
                 "x-office-spreadsheet");
    if(count < cap)
        launcher(&out[count++], "inbe", "Inner Breeze",
                 "/mnt/storage/Projects/inbe/build/bin/linux/inbe-linux-x86_64",
                 "applications-wellness");
    if(count < cap)
        launcher(&out[count++], "about", "About Rill", "internal:about",
                 "help-about");
    return count;
}

static int
linux_list_tasks(RillTask *out, int cap)
{
    (void)out;
    (void)cap;
    return 0;
}

static int
linux_launch(const RillLauncher *launcher)
{
    pid_t pid;

    if(launcher == NULL || launcher->command[0] == '\0')
        return 0;
    if(strncmp(launcher->command, "internal:", 9) == 0 ||
       strncmp(launcher->command, "host:", 5) == 0)
        return 0;

    pid = fork();
    if(pid < 0)
        return 0;
    if(pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", launcher->command, (char *)NULL);
        _exit(127);
    }
    return 1;
}

static int
linux_focus_task(int task_id)
{
    (void)task_id;
    return 0;
}

static int
linux_close_task(int task_id)
{
    (void)task_id;
    return 0;
}

static const char *
linux_settings_root(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if(xdg != NULL && xdg[0] != '\0')
        return xdg;
    return "~/.config/rill";
}

static const RillPlatformServices services = {
    "linux",
    linux_list_launchers,
    linux_list_tasks,
    linux_launch,
    linux_focus_task,
    linux_close_task,
    linux_settings_root
};

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
