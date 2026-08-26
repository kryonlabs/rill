#include "rill_platform.h"

#include <stdio.h>
#include <string.h>

static void
launcher(RillLauncher *out, const char *id, const char *name,
         const char *description, const char *category, const char *command,
         int favorite)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->description, sizeof(out->description), "%s", description);
    snprintf(out->category, sizeof(out->category), "%s", category);
    snprintf(out->command, sizeof(out->command), "%s", command);
    out->favorite = favorite;
}

static int
stub_list_launchers(RillLauncher *out, int cap)
{
    if(out == NULL || cap <= 0)
        return 0;
    launcher(&out[0], "terminal", "Terminal Emulator", "Use the command line",
             "Accessories", "host:ktrem", 1);
    if(cap > 1)
        launcher(&out[1], "files", "File Manager",
                 "Browse the file system", "Accessories", "host:shelf", 1);
    if(cap > 2)
        launcher(&out[2], "settings", "Settings",
                 "Configure the desktop", "Settings", "internal:settings", 1);
    if(cap > 3)
        launcher(&out[3], "workbook", "Workbook", "Edit spreadsheets",
                 "Office", "external:workbook", 0);
    if(cap > 4)
        launcher(&out[4], "inbe", "Inner Breeze", "Breathe and focus",
                 "Other", "external:inbe", 0);
    return cap < 5 ? cap : 5;
}

static int
stub_list_tasks(RillTask *out, int cap)
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
stub_launch(const RillLauncher *launcher)
{
    (void)launcher;
    return 0;
}

static int
stub_task_action(int task_id)
{
    (void)task_id;
    return 0;
}

static const char *
stub_settings_root(void)
{
    return "/cfg/rill";
}

static const RillPlatformServices services = {
    "stub",
    stub_list_launchers,
    stub_list_tasks,
    stub_launch,
    stub_task_action,
    stub_task_action,
    stub_settings_root
};

const RillPlatformServices *
RillPlatformStub(void)
{
    return &services;
}

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
