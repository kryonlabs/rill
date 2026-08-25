#include "rill_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef RILL_KAPSULE_BIN
#define RILL_KAPSULE_BIN "kapsule"
#endif

static void
launcher(RillLauncher *out, const char *id, const char *name,
         const char *command)
{
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->command, sizeof(out->command), "%s", command);
}

static int
linux_list_launchers(RillLauncher *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    count = 0;
    if(count < cap)
        launcher(&out[count++], "terminal", "Terminal", "host:kapsule");
    if(count < cap)
        launcher(&out[count++], "files", "Files", "internal:files");
    if(count < cap)
        launcher(&out[count++], "settings", "Settings", "internal:settings");
    if(count < cap)
        launcher(&out[count++], "about", "About Rill", "internal:about");
    return count;
}

static int
linux_list_tasks(RillTask *out, int cap)
{
    if(out == NULL || cap <= 0)
        return 0;

    out[0].id = 1;
    snprintf(out[0].title, sizeof(out[0].title), "%s", "Rill desktop");
    out[0].focused = 1;
    out[0].urgent = 0;
    return 1;
}

static int
linux_launch(const RillLauncher *launcher)
{
    pid_t pid;

    if(launcher == NULL || launcher->command[0] == '\0')
        return 0;
    if(strncmp(launcher->command, "internal:", 9) == 0)
        return 0;

    pid = fork();
    if(pid < 0)
        return 0;
    if(pid == 0) {
        setsid();
        if(strcmp(launcher->command, "external:kapsule") == 0) {
            execl(RILL_KAPSULE_BIN, "kapsule", (char *)NULL);
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", launcher->command, (char *)NULL);
        _exit(127);
    }
    return 1;
}

static int
linux_focus_task(int task_id)
{
    return task_id == 1;
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
