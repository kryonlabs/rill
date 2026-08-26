#include "rill_platform.h"

#include <stdio.h>
#include <string.h>

#ifdef KRYON_NATIVE_PLAN9
#include "kryon_plan9.h"
#endif

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
plan9_list_launchers(RillLauncher *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    count = 0;
    if(count < cap)
        launcher(&out[count++], "terminal", "Terminal Emulator",
                 "Use the command line", "Accessories", "host:ktrem", 1);
    if(count < cap)
        launcher(&out[count++], "files", "File Manager",
                 "Browse the file system", "Accessories", "host:shelf", 1);
    if(count < cap)
        launcher(&out[count++], "settings", "Settings",
                 "Configure the desktop", "Settings", "internal:settings", 1);
    if(count < cap)
        launcher(&out[count++], "about", "About Rill",
                 "Desktop information", "System", "internal:about", 0);
    return count;
}

static int
plan9_list_tasks(RillTask *out, int cap)
{
    (void)out;
    (void)cap;
    return 0;
}

static int
plan9_launch(const RillLauncher *launcher)
{
#ifdef KRYON_NATIVE_PLAN9
    if(launcher == NULL || launcher->command[0] == '\0' ||
       strncmp(launcher->command, "internal:", 9) == 0)
        return 0;
    switch(rfork(RFPROC|RFFDG|RFENVG|RFNOTEG)) {
    case -1:
        return 0;
    case 0:
        execl("/bin/rc", "rc", "-c", launcher->command, nil);
        exits("exec");
    default:
        return 1;
    }
#else
    (void)launcher;
    return 0;
#endif
}

static int
plan9_focus_task(int task_id)
{
    (void)task_id;
    return 0;
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
