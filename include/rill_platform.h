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

/* A StatusNotifier-hosted tray icon snapshot; argb is ARGB per pixel and the
   caller owns the buffer. */
typedef struct RillTrayIcon {
    char id[160];
    char title[128];
    int width;
    int height;
    unsigned int *argb;
} RillTrayIcon;

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
    /* Wallpaper candidates for the settings app; paths are 512-byte buffers. */
    int (*list_wallpapers)(char (*paths)[512], int cap);
    /* Session actions: "logout", "restart", "shutdown", "suspend". */
    int (*session_action)(const char *action);
    /* Toggle the WM's show-desktop state (EWMH _NET_SHOWING_DESKTOP). */
    int (*show_desktop)(int show);
    /* Poll tray icons; buffers returned through out must be free()d. */
    int (*tray_icons)(RillTrayIcon *out, int cap);
    /* Activate (secondary=0) or secondary-activate (secondary=1) an icon. */
    int (*tray_activate)(const char *id, int secondary);
} RillPlatformServices;

int RillSettingsEnsureDirectory(const char *path);

/* Small persisted key/value store ("key = value" lines) for shell settings. */
#define RILL_SETTINGS_MAX 32
#define RILL_SETTINGS_KEY 48
#define RILL_SETTINGS_VALUE 256

typedef struct RillSettings {
    char keys[RILL_SETTINGS_MAX][RILL_SETTINGS_KEY];
    char values[RILL_SETTINGS_MAX][RILL_SETTINGS_VALUE];
    int count;
} RillSettings;

void RillSettingsInit(RillSettings *settings);
int RillSettingsLoad(RillSettings *settings, const char *path);
const char *RillSettingsGet(const RillSettings *settings, const char *key,
                            const char *fallback);
int RillSettingsGetInteger(const RillSettings *settings, const char *key,
                           int fallback);
void RillSettingsSet(RillSettings *settings, const char *key, const char *value);
void RillSettingsSetInteger(RillSettings *settings, const char *key, int value);
int RillSettingsSave(const RillSettings *settings, const char *path);

const RillPlatformServices *RillPlatformCurrent(void);
const RillPlatformServices *RillPlatformStub(void);

#endif
