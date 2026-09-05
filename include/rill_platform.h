#ifndef RILL_PLATFORM_H
#define RILL_PLATFORM_H

#define RILL_MAX_LAUNCHERS 32
#define RILL_MAX_TASKS 32

typedef struct RillLauncher {
    char id[64];
    char name[96];
    char description[128];
    char category[64];
    char command[256];
    char icon_path[512];
    char desktop_file[1024];
    int favorite;
} RillLauncher;

typedef struct RillTask {
    int id;
    char title[128];
    char icon_path[512];
    int focused;
    int urgent;
    int platform_owned;
} RillTask;

typedef struct RillPlatformServices {
    const char *name;
    int (*list_launchers)(RillLauncher *out, int cap);
    int (*list_tasks)(RillTask *out, int cap);
    int (*launch)(const RillLauncher *launcher);
    int (*focus_task)(int task_id);
    int (*close_task)(int task_id);
    const char *(*settings_root)(void);
    int (*workspace_count)(void);
    int (*current_workspace)(void);
    int (*switch_workspace)(int index);
} RillPlatformServices;

int RillSettingsEnsureDirectory(const char *path);

const RillPlatformServices *RillPlatformCurrent(void);
const RillPlatformServices *RillPlatformStub(void);

#endif
