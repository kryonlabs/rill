#include "rill_platform.h"

#include <stdio.h>
#include <string.h>

static void
launcher(RillLauncher *out, const char *id, const char *name,
         const char *command)
{
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->command, sizeof(out->command), "%s", command);
}

static int
plan9_list_launchers(RillLauncher *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    count = 0;
    if(count < cap)
        launcher(&out[count++], "terminal", "Terminal", "host:kapsule");
    if(count < cap)
        launcher(&out[count++], "rc", "rc", "external:rc");
    if(count < cap)
        launcher(&out[count++], "Files", "Files", "internal:files");
    if(count < cap)
        launcher(&out[count++], "settings", "Rill Settings", "internal:settings");
    return count;
}

static int
plan9_list_tasks(RillTask *out, int cap)
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
plan9_launch(const RillLauncher *launcher)
{
    (void)launcher;
    return 0;
}

static int
plan9_focus_task(int task_id)
{
    return task_id == 1;
}

static int
plan9_close_task(int task_id)
{
    (void)task_id;
    return 0;
}

static const char *
plan9_settings_root(void)
{
    return "/usr/$user/lib/rill";
}

static const RillPlatformServices services = {
    "plan9",
    plan9_list_launchers,
    plan9_list_tasks,
    plan9_launch,
    plan9_focus_task,
    plan9_close_task,
    plan9_settings_root
};

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
