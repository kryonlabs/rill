#ifndef RILL_PLATFORM_H
#define RILL_PLATFORM_H

#include "files.h"

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
    char file_path[1024];
    int is_directory;
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

/* A desktop notification snapshot; expired entries drop out on the next
   poll. */
typedef struct RillNotification {
    unsigned int id;
    char app_name[96];
    char summary[160];
    char body[256];
} RillNotification;

/* One DBusMenu row snapshot for a StatusNotifier tray icon. */
typedef struct RillTrayMenuRow {
    char label[160];
    int item_id;
    short depth;
    short toggle;       /* -1 not a toggle, 0 off, 1 on */
    unsigned char separator;
    unsigned char enabled;
} RillTrayMenuRow;

/* One XSETTINGS value published to X11 applications. */
#define RILL_XSETTING_NAME 64
#define RILL_XSETTING_STRING 160

typedef struct RillXSetting {
    char name[RILL_XSETTING_NAME];
    int type; /* 0 integer, 1 string */
    int integer_value;
    char string_value[RILL_XSETTING_STRING];
} RillXSetting;

/* One RandR output as reported for panel placement and display settings. */
typedef struct RillDisplayOutput {
    char name[64];
    int width;
    int height;
    int connected;
    int primary;
} RillDisplayOutput;

/* One usable mode of a connected output. */
typedef struct RillDisplayMode {
    int width;
    int height;
} RillDisplayMode;

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
    /* Session actions: "lock", "logout", "restart", "shutdown", "suspend". */
    int (*session_action)(const char *action);
    /* Toggle the WM's show-desktop state (EWMH _NET_SHOWING_DESKTOP). */
    int (*show_desktop)(int show);
    /* Poll tray icons; buffers returned through out must be free()d. */
    int (*tray_icons)(RillTrayIcon *out, int cap);
    /* Activate (secondary=0) or secondary-activate (secondary=1) an icon. */
    int (*tray_activate)(const char *id, int secondary);
    /* Fetch the tray item's DBusMenu rows (flattened, depth-indented). */
    int (*tray_menu)(const char *id, RillTrayMenuRow *out, int cap);
    /* Trigger one row of the item's DBusMenu. */
    int (*tray_menu_activate)(const char *id, int item_id);
    /* Desktop entries from the user's desktop directory. */
    int (*list_desktop_files)(RillLauncher *out, int cap);
    /* Open a file path or URI (for example trash://) with the default app. */
    int (*open_path)(const char *path);
    /* Active notifications; returns the count copied into out. */
    int (*notifications_poll)(RillNotification *out, int cap);
    /* Invoke the default action (dismiss=0) or close (dismiss=1) a toast. */
    int (*notification_action)(unsigned int id, int dismiss);
    /* Battery charge percent and charging state; 0 when unavailable. */
    int (*battery_state)(int *percent, int *charging);
    /* Legacy XEmbed system tray: docked icon count (starts hosting). */
    int (*xembed_tray_count)(void);
    /* Place the tray host window at the panel's tray slot; visible=0 moves
       it offscreen. */
    void (*xembed_tray_layout)(int x, int y, int height, int visible);
    /* Default audio sink volume; 0 when unavailable. */
    int (*volume_state)(int *percent, int *muted);
    /* Set sink volume/mute; -1 leaves a value unchanged. */
    int (*volume_set)(int percent, int muted);
    /* Audio sink names with the current default; returns the count. */
    int (*volume_sinks)(char (*names)[96], int cap, char *default_sink,
                        int default_size);
    /* Make one listed sink the default. */
    int (*volume_set_default)(const char *name);
    /* Own the XSETTINGS selection and publish values to X11 apps. */
    int (*xsettings_publish)(const RillXSetting *settings, int count);
    /* Read the currently published XSETTINGS values (ours or another
       daemon's); returns the count copied into out. */
    int (*xsettings_read)(RillXSetting *out, int cap);
    /* Poll clipboard history (newest first); pointers stay owned by the
       platform and are valid until the next poll. */
    int (*clipboard_history)(const char **texts, int cap);
    /* Push a history entry back onto the CLIPBOARD selection. */
    int (*clipboard_select)(int index);
    /* Actual XDG desktop directory, including localized user-dir overrides. */
    const char *(*desktop_directory)(void);
    /* Open an installed control panel for a named system category. */
    int (*open_settings)(const char *category);
    /* mkdir: destination is a basename in source; rename: destination is a
       sibling basename; copy: destination is a directory; trash: no target.
       Never overwrite an existing destination. Errors are returned to the UI. */
    int (*file_operation)(const char *operation, const char *source,
                          const char *destination, char *error, int error_size);
    int (*file_transfer_start)(const char *operation, const char *const *sources,
                               int count, const char *destination);
    int (*file_transfer_poll)(FileTransferStatus *status);
    void (*file_transfer_cancel)(void);
    void (*file_transfer_finish)(void);
    int (*file_clipboard_copy)(const char *const *paths, int count, int cut);
    int (*file_clipboard_paste)(const char *destination);
    /* Interactive conflict answers for a blocked transfer; see files.h. */
    int (*file_transfer_conflicts)(void);
    int (*file_transfer_resolve)(int answer, int apply_to_all);
    int (*file_transfer_retry)(void);
    /* Remove abandoned ".transfer-*" staging directories after a crash. */
    int (*file_recover_staging)(const char *directory, char *error, int error_size);
    /* Trash management on the user's XDG trash directories. */
    int (*file_trash_list)(FileTrashEntry *out, int cap);
    int (*file_trash_restore)(const char *name, char *error, int error_size);
    int (*file_trash_empty)(char *error, int error_size);
    /* Connected RandR outputs for panel placement and display settings. */
    int (*display_outputs)(RillDisplayOutput *out, int cap);
    /* Usable modes of one output; duplicates removed. */
    int (*display_modes)(const char *output, RillDisplayMode *out, int cap);
    /* Apply one mode to an output keeping its position and rotation. */
    int (*display_apply)(const char *output, int width, int height,
                         char *error, int error_size);
    /* Pointer acceleration and threshold; keyboard auto-repeat. */
    int (*pointer_settings)(int *numerator, int *denominator, int *threshold);
    int (*pointer_set)(int numerator, int denominator, int threshold);
    int (*keyboard_repeat)(int *delay, int *rate);
    int (*keyboard_set_repeat)(int delay, int rate);
    /* The panels.json list consumed by rill-session: load returns the stored
     * JSON text, store writes it atomically. */
    int (*panel_config_load)(char *json, int size);
    int (*panel_config_store)(const char *json);
    /* Supervised-service report from the session manager, plus screen-lock
     * readiness; empty text when unavailable. */
    int (*session_diagnostics)(char *text, int size);
} RillPlatformServices;

int RillSettingsEnsureDirectory(const char *path);

/* Small persisted key/value store ("key = value" lines) for shell settings. */
#define RILL_SETTINGS_MAX 256
#define RILL_SETTINGS_KEY 96
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
/* Merge keys changed since previous with changes made by another process. */
int RillSettingsMergeSave(RillSettings *settings, RillSettings *previous, const char *path);

const RillPlatformServices *RillPlatformCurrent(void);
const RillPlatformServices *RillPlatformStub(void);

#endif
