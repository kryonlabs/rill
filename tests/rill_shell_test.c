#include "rill_shell.h"

#include <stdio.h>
#include <string.h>

static int launches;
static int focused_task;

static int
test_launchers(RillLauncher *out, int cap)
{
    if(cap < 2)
        return 0;
    snprintf(out[0].id, sizeof(out[0].id), "%s", "terminal");
    snprintf(out[0].name, sizeof(out[0].name), "%s", "Terminal");
    snprintf(out[0].command, sizeof(out[0].command), "%s",
             "host:kapsule");
    snprintf(out[1].id, sizeof(out[1].id), "%s", "settings");
    snprintf(out[1].name, sizeof(out[1].name), "%s", "Settings");
    snprintf(out[1].command, sizeof(out[1].command), "%s",
             "internal:settings");
    return 2;
}

static int
test_tasks(RillTask *out, int cap)
{
    if(cap < 1)
        return 0;
    out[0].id = 42;
    snprintf(out[0].title, sizeof(out[0].title), "%s", "Rio");
    out[0].focused = 1;
    return 1;
}

static int
test_launch(const RillLauncher *launcher)
{
    if(launcher == NULL || strcmp(launcher->id, "terminal") != 0)
        return 0;
    launches++;
    return 1;
}

static int
test_focus(int task_id)
{
    focused_task = task_id;
    return task_id == 42;
}

static const char *
test_settings(void)
{
    return "/tmp/rill-test";
}

static const RillPlatformServices services = {
    "test",
    test_launchers,
    test_tasks,
    test_launch,
    test_focus,
    test_focus,
    test_settings
};

static void
check(const char *name, int ok, int *failures)
{
    if(!ok) {
        fprintf(stderr, "rill shell test failed: %s\n", name);
        (*failures)++;
    }
}

int
main(void)
{
    RillShellState shell;
    int failures = 0;

    RillShellInit(&shell);
    check("initial launcher selection", shell.selected_launcher == -1,
          &failures);
    RillShellRefresh(&shell, &services);
    check("launcher count", shell.launcher_count == 2, &failures);
    check("initial task count", shell.task_count == 0, &failures);
    check("select launcher", RillShellSelectLauncher(&shell, 1), &failures);
    check("launch selected internal", RillShellLaunchSelected(&shell, &services),
          &failures);
    check("internal app count", shell.app_count == 1, &failures);
    check("external launch call count", launches == 0, &failures);
    RillShellRefresh(&shell, &services);
    check("task count after app launch", shell.task_count == 1, &failures);
    check("select task", RillShellSelectTask(&shell, 0), &failures);
    check("focus task", RillShellFocusSelectedTask(&shell, &services),
          &failures);
    check("focused internal app", shell.focused_app == shell.apps[0].id,
          &failures);
    check("platform focus not used", focused_task == 0, &failures);
    check("status updated", strstr(shell.status, "Settings") != NULL,
          &failures);
    check("select terminal", RillShellSelectLauncher(&shell, 0), &failures);
    check("launch hosted terminal", RillShellLaunchSelected(&shell, &services),
          &failures);
    check("terminal opened in shell", shell.app_count == 2, &failures);
    check("external launch call count unchanged", launches == 0, &failures);

    return failures == 0 ? 0 : 1;
}
