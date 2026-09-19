#include "rill_shell.h"

#include "kryon.h"
#include "rill_panel.h"

#if defined(__linux__) && !defined(KRYON_NATIVE_PLAN9)
#define RILL_HAS_X11 1
#include "rill_x11.h"
#include "rill_wayland.h"
#include "session.h"
#else
#define RILL_HAS_X11 0
#endif

#include <math.h>
#ifndef KRYON_NATIVE_PLAN9
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "plan9_overlay.h"

#ifdef KRYON_NATIVE_PLAN9
AppHost *KtermCreateAppHost(int abi_version, const char *project_path);
void KtermDestroyAppHost(AppHost *app_host);
AppHost *ShelfCreateAppHost(int abi_version, const char *project_path);
void ShelfDestroyAppHost(AppHost *app_host);
#endif

#if defined(__linux__) && !defined(KRYON_NATIVE_PLAN9)
#define RILL_HAS_DLOPEN 1
#include <dlfcn.h>
#include <unistd.h>
#else
#define RILL_HAS_DLOPEN 0
#endif

enum {
    RILL_WIDTH = 1120,
    RILL_HEIGHT = 720,
    PANEL_H = 26,
    RILL_HOST_CACHE_MAX = 8,
    RILL_ICON_CACHE_MAX = 32,
    RILL_PANEL_PLUGIN_MAX = 32,
    RILL_WALLPAPER_CHOICES = 24,
    RILL_DESKTOP_FILE_MAX = 128,
    DESKTOP_ICON_MAX = RILL_DESKTOP_FILE_MAX + RILL_MAX_LAUNCHERS + 3,
    RILL_TEST_LOWER_TEXT_X = 274,
    RILL_TEST_LOWER_TEXT_Y = 224,
    RILL_TEST_UPPER_X = 250,
    RILL_TEST_UPPER_Y = 180,
    RILL_TEST_UPPER_W = 420,
    RILL_TEST_UPPER_H = 260,
    RILL_TEST_SAMPLE_X = 310,
    RILL_TEST_SAMPLE_Y = 242
};

typedef struct RillIconCacheEntry {
    char path[512];
    Texture2D texture;
    int ready;
} RillIconCacheEntry;

typedef struct RillTrayEntry {
    char id[160];
    char title[96];
    Texture2D texture;
    int ready;
} RillTrayEntry;

typedef struct RillHostModule {
    char id[64];
    char path[512];
    void *library;
    AppHost *host;
    DestroyAppHostCallback destroy;
    int missing_reported;
} RillHostModule;

typedef struct RillVisualState {
    Texture2D wallpaper;
    int wallpaper_ready;
    RillHostModule hosts[RILL_HOST_CACHE_MAX];
    int host_count;
    RillIconCacheEntry icons[RILL_ICON_CACHE_MAX];
    int icon_count;
    char system_theme_name[128];
    char wallpaper_path[512];
    char system_font_name[128];
    char system_font_path[512];
    RillPanelPlugin left_panel[RILL_PANEL_PLUGIN_MAX];
    int left_panel_count;
    RillPanelPlugin right_panel[RILL_PANEL_PLUGIN_MAX];
    int right_panel_count;
    char panel_config_path[1024];
    int panel_dirty;
    int panel_context_open;
    int panel_context_side;
    int panel_context_index;
    int panel_context_x;
    int panel_context_y;
    Rectangle panel_item_bounds[2][RILL_PANEL_PLUGIN_MAX];
    int panel_drag_side;
    int panel_drag_index;
    char clock_format[64];
    int panel_height;
    int panel_bottom;
    int panel_autohide;
    int panel_hidden;
    int calendar_open;
    int properties_open;
    int properties_side;
    int properties_index;
    int show_desktop_on;
    int logout_open;
    int wallpaper_slideshow;
    double wallpaper_next_swap;
    char wallpaper_paths[24][512];
    int wallpaper_count;
    int wallpaper_scanned;
    RillTrayEntry tray[8];
    int tray_count;
    RillLauncher desktop_files[RILL_DESKTOP_FILE_MAX];
    double desktop_next_scan;
    Rectangle desktop_bounds[DESKTOP_ICON_MAX];
    const RillLauncher *desktop_entries[DESKTOP_ICON_MAX];
    RillLauncher desktop_snapshots[DESKTOP_ICON_MAX];
    int desktop_entry_count;
    RillSettings desktop_layout;
    char desktop_layout_path[1024];
    int desktop_drag_index;
    int desktop_drag_active;
    Vector2 desktop_drag_start;
    Vector2 desktop_drag_origin;
    Vector2 desktop_pointer;
    char file_action[16];
    char file_source[1024];
    char file_name[256];
    char file_error[256];
    int file_focused;
    int file_cursor;
    int desktop_file_count;
    int desktop_files_scanned;
    int desktop_selected;
    int desktop_last_index;
    double desktop_last_click;
    int desktop_menu_x;
    int desktop_menu_y;
    RillNotification notifications[4];
    int notification_count;
    const char *clipboard_texts[8];
    int clipboard_count;
    int clipboard_popup_open;
    int battery_percent;
    int battery_charging;
    int battery_available;
    int volume_percent;
    int volume_muted;
    int volume_available;
    char run_input[160];
    int run_selected;
    int settings_tab;
    char settings_error[160];
    int wallpaper_scroll;
} RillVisualState;

static int popup_input_blocked;
static RillSettings rill_settings;
static RillSettings settings_previous;
static char rill_settings_path[1024];

/* Panel geometry: height is configurable and autohide collapses the bar to a
   3-pixel sliver until the pointer reaches its screen edge. */
static int
rill_panel_visible_height(const RillVisualState *visuals)
{
    if(visuals == NULL)
        return PANEL_H;
    if(visuals->panel_autohide && visuals->panel_hidden)
        return 3;
    return visuals->panel_height;
}

static int
rill_panel_top(const RillVisualState *visuals)
{
    if(visuals != NULL && visuals->panel_bottom)
        return GetScreenHeight() - rill_panel_visible_height(visuals);
    return 0;
}

/* Menus anchor below a top panel or above a bottom panel. */
static int
rill_menu_anchor_y(const RillVisualState *visuals, int menu_height)
{
    int top = rill_panel_top(visuals);
    int height = rill_panel_visible_height(visuals);
    if(visuals != NULL && visuals->panel_bottom)
        return top - 2 - menu_height;
    return top + height + 2;
}

typedef struct RillTestState {
    const char *scene;
    const char *ready_file;
    int exit_after_frames;
    int disable_wallpaper;
    int frames;
    int ready_written;
} RillTestState;

typedef struct RillControlState {
    char path[128];
    long offset;
} RillControlState;

typedef enum RillRunMode {
    RILL_MODE_SHELL,
    RILL_MODE_DESKTOP,
    RILL_MODE_WM,
    RILL_MODE_WINDOWED,
    RILL_MODE_PANEL,
    RILL_MODE_RUN,
    RILL_MODE_SETTINGS
} RillRunMode;

typedef struct RillRuntimeOptions {
    RillRunMode mode;
    int xfce_panel;
    int external_panel;
    char panel_id[64];
    char panel_output[128];
    char launch_command[512];
} RillRuntimeOptions;

typedef struct RillMenuCategory {
    const char *name;
    const char *icon_id;
} RillMenuCategory;

static const RillMenuCategory rill_menu_categories[] = {
    {"Favorites", "favorite"},
    {"Recently Used", "recent"},
    {"All Applications", "all"},
    {"Accessories", "accessories"},
    {"Development", "development"},
    {"Education", "education"},
    {"Games", "games"},
    {"Graphics", "graphics"},
    {"Internet", "internet"},
    {"Multimedia", "multimedia"},
    {"Office", "office"},
    {"Other", "other"}
};

static void
include_panel_popup(Rectangle bounds)
{
#if RILL_HAS_X11
    PanelSurfaceInclude(bounds);
#else
    (void)bounds;
#endif
}

#ifdef KRYON_NATIVE_PLAN9
static volatile int rill_stop_requested;

static int
rill_note(void *context, char *note)
{
    (void)context;
    if(strcmp(note, "interrupt") != 0 && strcmp(note, "hangup") != 0)
        return 0;
    rill_stop_requested = 1;
    return 1;
}
#else
static volatile sig_atomic_t rill_stop_requested;

static void
rill_signal_stop(int signal_number)
{
    (void)signal_number;
    rill_stop_requested = 1;
}
#endif

static Color
mix_color(Color a, Color b, float t)
{
    Color c;

    if(t < 0.0f)
        t = 0.0f;
    if(t > 1.0f)
        t = 1.0f;
    c.r = (unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t);
    c.g = (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t);
    c.b = (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t);
    c.a = (unsigned char)((float)a.a + ((float)b.a - (float)a.a) * t);
    return c;
}

static Color
opaque_color(Color color)
{
    color.a = 255;
    return color;
}

static Color
panel_color(void)
{
    return (Color){30, 33, 46, 255};
}

static Color
panel_item_color(void)
{
    return (Color){38, 42, 58, 255};
}

static Color
panel_item_hover_color(void)
{
    return (Color){48, 54, 74, 255};
}

/* The bar keeps its own dark palette, so item text contrasts with the bar
   rather than the system theme (a light GTK theme makes theme text black). */
static Color
panel_text_color(void)
{
    return (Color){238, 240, 248, 255};
}

static Color
panel_text_dim(void)
{
    return (Color){178, 184, 202, 255};
}

static Color
panel_active_color(void)
{
    return opaque_color(mix_color(GetThemeButtonHover(),
                                  (Color){194, 0, 194, 255}, 0.42f));
}

static int
env_truthy(const char *name)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

static int
env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if(value == NULL || value[0] == '\0')
        return fallback;
    parsed = strtol(value, &end, 10);
    if(end == value || parsed < 0 || parsed > 1000000)
        return fallback;
    return (int)parsed;
}

static void
init_test_state(RillTestState *test)
{
    if(test == NULL)
        return;
    memset(test, 0, sizeof(*test));
    test->scene = getenv("RILL_TEST_SCENE");
    test->ready_file = getenv("RILL_TEST_READY_FILE");
    test->exit_after_frames = env_int("RILL_TEST_EXIT_AFTER_FRAMES", 0);
    test->disable_wallpaper = env_truthy("RILL_TEST_DISABLE_WALLPAPER");
}

static void
runtime_append_arg(RillRuntimeOptions *options, const char *arg)
{
    size_t len;
    size_t arg_len;

    if(options == NULL || arg == NULL || arg[0] == '\0')
        return;
    len = strlen(options->launch_command);
    arg_len = strlen(arg);
    if(len > 0) {
        if(len + 1 >= sizeof(options->launch_command))
            return;
        options->launch_command[len++] = ' ';
        options->launch_command[len] = '\0';
    }
    if(len + arg_len >= sizeof(options->launch_command))
        arg_len = sizeof(options->launch_command) - len - 1;
    memcpy(options->launch_command + len, arg, arg_len);
    options->launch_command[len + arg_len] = '\0';
}

static void
parse_runtime_options(int argc, char **argv, RillRuntimeOptions *options)
{
    int collect_command = 0;

    if(options == NULL)
        return;
    memset(options, 0, sizeof(*options));
    options->mode = RILL_MODE_SHELL;
    snprintf(options->panel_id, sizeof(options->panel_id), "primary");
    for(int i = 1; i < argc; i++) {
        if(collect_command) {
            runtime_append_arg(options, argv[i]);
            continue;
        }
        if(strcmp(argv[i], "--desktop") == 0) {
            options->mode = RILL_MODE_DESKTOP;
            continue;
        }
        if(strcmp(argv[i], "--panel") == 0) {
            options->mode = RILL_MODE_PANEL;
            if(i + 1 < argc && argv[i + 1][0] != '-')
                snprintf(options->panel_id, sizeof(options->panel_id), "%s", argv[++i]);
            continue;
        }
        if(strcmp(argv[i], "--panel-output") == 0 && i + 1 < argc) {
            snprintf(options->panel_output, sizeof(options->panel_output), "%s", argv[++i]);
            continue;
        }
        if(strcmp(argv[i], "--xfce-panel") == 0) {
            options->xfce_panel = 1;
            continue;
        }
        if(strcmp(argv[i], "--external-panel") == 0) {
            options->xfce_panel = options->external_panel = 1;
            continue;
        }
        if(strcmp(argv[i], "--sm-client-id") == 0 && i + 1 < argc) { i++; continue; }
        if(strcmp(argv[i], "--wm") == 0) {
            options->mode = RILL_MODE_WM;
            continue;
        }
        if(strcmp(argv[i], "--windowed") == 0) {
            options->mode = RILL_MODE_WINDOWED;
            continue;
        }
        if(strcmp(argv[i], "--settings") == 0) {
            options->mode = RILL_MODE_SETTINGS;
            continue;
        }
        if(strcmp(argv[i], "--run-dialog") == 0) {
            options->mode = RILL_MODE_RUN;
            continue;
        }
        if(strcmp(argv[i], "--") == 0) {
            collect_command = 1;
            continue;
        }
        if(options->mode == RILL_MODE_WM ||
           options->mode == RILL_MODE_WINDOWED ||
           options->mode == RILL_MODE_RUN)
            runtime_append_arg(options, argv[i]);
    }
}

static int
test_scene_active(const RillTestState *test)
{
    return test != NULL && test->scene != NULL && test->scene[0] != '\0';
}

static void
write_test_ready_file(RillTestState *test)
{
    FILE *file;

    if(test == NULL || test->ready_written || test->ready_file == NULL ||
       test->ready_file[0] == '\0')
        return;
    file = fopen(test->ready_file, "w");
    if(file == NULL)
        return;
    fprintf(file, "ready\n");
    fclose(file);
    test->ready_written = 1;
}

enum {
    LabelPrimary = 1, LabelMuted, LabelPanel, LabelPanelDim, LabelWhite,
    LabelAccent, LabelPanelAccent, LabelWarning, LabelTestRed
};

static void
configure_label_styles(void)
{
    static StyleSheet sheet;
    static StyleRule *rules;
    const StylePack *base = GetActiveStylePack();
    int count = base != NULL && base->sheet != NULL ? base->sheet->rule_count : 0;
    Color colors[] = {GetThemeText(), GetThemeIcon(), panel_text_color(),
                      panel_text_dim(), WHITE, GetThemeLink(),
                      {92, 185, 255, 255}, {224, 82, 68, 255}, {240, 16, 32, 255}};
    int labels = sizeof(colors) / sizeof(colors[0]);

    rules = calloc((size_t)(count + labels), sizeof(*rules));
    if(rules == NULL)
        return;
    if(count > 0)
        memcpy(rules, base->sheet->rules, (size_t)count * sizeof(*rules));
    for(int i = 0; i < labels; i++) {
        StyleRule *rule = &rules[count + i];
        rule->selector = StyleDefaultSelector();
        rule->selector.kind = StyleKindText();
        rule->selector.class_name = LabelPrimary + i;
        rule->state = StyleStateAny();
        rule->layer = 100;
        rule->style.fields = StyleForeground;
        rule->style.foreground = ColorToInt(colors[i]);
    }
    sheet.rules = rules;
    sheet.rule_count = count + labels;
    RegisterStylePack((StylePack){.id = "desktop", .label = "Desktop", .sheet = &sheet});
    SetActiveStylePack("desktop");
}

static void
configure_system_look(RillVisualState *visuals, const RillTestState *test)
{
    char font_path[512];
    char font_name[128];
    char wallpaper[512];

    memset(visuals, 0, sizeof(*visuals));
    RefreshSystemTheme();
    SetThemeSource(THEME_SOURCE_SYSTEM);
    SetThemeMode(THEME_MODE_SYSTEM);
    ApplyCurrentTheme();
    configure_label_styles();

    snprintf(visuals->system_theme_name, sizeof(visuals->system_theme_name),
             "%s", GetSystemThemeName());

    if(GetSystemTextFontName(font_name, sizeof(font_name)))
        snprintf(visuals->system_font_name, sizeof(visuals->system_font_name),
                 "%s", font_name);
    else
        snprintf(visuals->system_font_name, sizeof(visuals->system_font_name),
                 "%s", "Kryon UI");

    if(GetSystemTextFontFile(font_path, sizeof(font_path))) {
        snprintf(visuals->system_font_path, sizeof(visuals->system_font_path),
                 "%s", font_path);
        if(RegisterTextFontFileSource("system", font_path, NULL, 0))
            UseTextFont("system");
    }
    EnsureDefaultFont();

    if(test != NULL && test->disable_wallpaper)
        return;

    if(GetSystemDesktopBackground(wallpaper, sizeof(wallpaper))) {
        visuals->wallpaper = LoadTexture(wallpaper);
        if(visuals->wallpaper.id != 0) {
            visuals->wallpaper_ready = 1;
            snprintf(visuals->wallpaper_path, sizeof(visuals->wallpaper_path),
                     "%s", wallpaper);
        }
    }
}

static void
rill_settings_persist(const RillShellState *shell)
{
    char recents[64 * RILL_MAX_RECENT_LAUNCHERS];
    int length = 0;

    if(rill_settings_path[0] == '\0')
        return;
    if(shell != NULL) {
        recents[0] = '\0';
        for(int i = 0; i < shell->recent_launcher_count && i < RILL_MAX_RECENT_LAUNCHERS; i++) {
            int written = snprintf(recents + length, sizeof(recents) - (size_t)length,
                                   "%s%s", i > 0 ? "|" : "",
                                   shell->recent_launcher_ids[i]);
            if(written < 0 || (size_t)written >= sizeof(recents) - (size_t)length)
                break;
            length += written;
        }
        RillSettingsSet(&rill_settings, "recents", recents);
    }
    RillSettingsMergeSave(&rill_settings, &settings_previous, rill_settings_path);
}

static void
apply_wallpaper(RillShellState *shell, RillVisualState *visuals,
                const char *path, int persist)
{
    Texture2D texture;

    if(path == NULL || path[0] == '\0')
        return;
    texture = LoadTexture(path);
    if(texture.id == 0) {
        RillShellSetStatus(shell, "Could not load the selected wallpaper");
        return;
    }
    if(visuals->wallpaper_ready)
        UnloadTexture(visuals->wallpaper);
    visuals->wallpaper = texture;
    visuals->wallpaper_ready = 1;
    snprintf(visuals->wallpaper_path, sizeof(visuals->wallpaper_path), "%s", path);
    if(persist) {
        RillSettingsSet(&rill_settings, "wallpaper", path);
        rill_settings_persist(shell);
        RillShellSetStatus(shell, "Wallpaper updated");
    }
}

static void
apply_saved_settings(RillShellState *shell, RillVisualState *visuals)
{
    const char *wallpaper;
    const char *recents;
    int height;

    snprintf(visuals->clock_format, sizeof(visuals->clock_format), "%s",
             RillSettingsGet(&rill_settings, "clock-format", "%H:%M"));
    height = RillSettingsGetInteger(&rill_settings, "panel-height", PANEL_H);
    visuals->panel_height = height < 20 ? 20 : (height > 48 ? 48 : height);
    visuals->panel_bottom =
        strcmp(RillSettingsGet(&rill_settings, "panel-side", "top"),
               "bottom") == 0;
    visuals->panel_autohide =
        RillSettingsGetInteger(&rill_settings, "panel-autohide", 0) != 0;
    visuals->wallpaper_slideshow =
        RillSettingsGetInteger(&rill_settings, "wallpaper-slideshow", 0) != 0;
    wallpaper = RillSettingsGet(&rill_settings, "wallpaper", NULL);
    if(wallpaper != NULL && wallpaper[0] != '\0' &&
       strcmp(wallpaper, visuals->wallpaper_path) != 0)
        apply_wallpaper(shell, visuals, wallpaper, 0);
    recents = RillSettingsGet(&rill_settings, "recents", NULL);
    if(shell->recent_launcher_count == 0 && recents != NULL && recents[0] != '\0') {
        char copy[64 * RILL_MAX_RECENT_LAUNCHERS];
        char *item;

        snprintf(copy, sizeof(copy), "%s", recents);
        item = strtok(copy, "|");
        while(item != NULL &&
              shell->recent_launcher_count < RILL_MAX_RECENT_LAUNCHERS) {
            snprintf(shell->recent_launcher_ids[shell->recent_launcher_count],
                     sizeof(shell->recent_launcher_ids[0]), "%s", item);
            shell->recent_launcher_count++;
            item = strtok(NULL, "|");
        }
    }
}

static void
refresh_tray_icons(RillVisualState *visuals,
                   const RillPlatformServices *platform)
{
    RillTrayIcon polled[8];
    int count;
    int i;

    if(platform == NULL || platform->tray_icons == NULL)
        return;
    count = platform->tray_icons(polled, 8);
    if(count < 0)
        count = 0;
    if(count == visuals->tray_count) {
        int same = 1;
        for(i = 0; i < count; i++)
            if(strcmp(visuals->tray[i].id, polled[i].id) != 0)
                same = 0;
        if(same) {
            for(i = 0; i < count; i++)
                free(polled[i].argb);
            return;
        }
    }
    for(i = 0; i < visuals->tray_count; i++)
        if(visuals->tray[i].ready)
            UnloadTexture(visuals->tray[i].texture);
    visuals->tray_count = 0;
    for(i = 0; i < count && visuals->tray_count < 8; i++) {
        RillTrayEntry *entry = &visuals->tray[visuals->tray_count];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->id, sizeof(entry->id), "%s", polled[i].id);
        snprintf(entry->title, sizeof(entry->title), "%s", polled[i].title);
        if(polled[i].argb != NULL && polled[i].width > 0 && polled[i].height > 0) {
            unsigned char *rgba = malloc((size_t)polled[i].width *
                                         (size_t)polled[i].height * 4);
            if(rgba != NULL) {
                Image image;
                for(size_t p = 0; p < (size_t)polled[i].width * polled[i].height; p++) {
                    unsigned int argb = polled[i].argb[p];
                    rgba[p * 4] = (unsigned char)((argb >> 16) & 0xff);
                    rgba[p * 4 + 1] = (unsigned char)((argb >> 8) & 0xff);
                    rgba[p * 4 + 2] = (unsigned char)(argb & 0xff);
                    rgba[p * 4 + 3] = (unsigned char)((argb >> 24) & 0xff);
                }
                image.data = rgba;
                image.width = polled[i].width;
                image.height = polled[i].height;
                image.mipmaps = 1;
                image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                entry->texture = LoadTextureFromImage(image);
                entry->ready = entry->texture.id != 0;
                free(rgba);
            }
        }
        visuals->tray_count++;
    }
    for(i = 0; i < count; i++)
        free(polled[i].argb);
}

static void
refresh_notifications(RillVisualState *visuals,
                      const RillPlatformServices *platform)
{
    if(visuals == NULL || platform == NULL || platform->notifications_poll == NULL)
        return;
    visuals->notification_count = platform->notifications_poll(
        visuals->notifications, 4);
    if(visuals->notification_count < 0)
        visuals->notification_count = 0;
}

static void
refresh_battery(RillVisualState *visuals, const RillPlatformServices *platform)
{
    if(visuals == NULL || platform == NULL || platform->battery_state == NULL)
        return;
    visuals->battery_available = platform->battery_state(
        &visuals->battery_percent, &visuals->battery_charging);
}

static void
refresh_volume(RillVisualState *visuals, const RillPlatformServices *platform)
{
    if(visuals == NULL || platform == NULL || platform->volume_state == NULL)
        return;
    visuals->volume_available = platform->volume_state(
        &visuals->volume_percent, &visuals->volume_muted);
}

/* Publish XSETTINGS for X11 applications: import whatever the current
   settings daemon broadcast, then apply the Rill settings overrides. */
static void
refresh_clipboard(RillVisualState *visuals,
                  const RillPlatformServices *platform)
{
    if(visuals == NULL || platform == NULL || platform->clipboard_history == NULL)
        return;
    visuals->clipboard_count = platform->clipboard_history(
        visuals->clipboard_texts, 8);
}

static void
publish_xsettings(const RillPlatformServices *platform)
{
    RillXSetting settings[16];
    int count;
    const char *value;

    if(platform == NULL || platform->xsettings_publish == NULL ||
       platform->xsettings_read == NULL)
        return;
    count = platform->xsettings_read(settings, 14);
    value = RillSettingsGet(&rill_settings, "gtk-theme", NULL);
    if(value != NULL && value[0] != '\0') {
        RillXSetting setting = {0};
        snprintf(setting.name, sizeof(setting.name), "Net/ThemeName");
        setting.type = 1;
        snprintf(setting.string_value, sizeof(setting.string_value), "%s", value);
        int replaced = 0;
        for(int i = 0; i < count && !replaced; i++)
            if(strcmp(settings[i].name, "Net/ThemeName") == 0) {
                settings[i] = setting;
                replaced = 1;
            }
        if(!replaced && count < 14)
            settings[count++] = setting;
    }
    value = RillSettingsGet(&rill_settings, "gtk-font", NULL);
    if(value != NULL && value[0] != '\0') {
        int replaced = 0;
        for(int i = 0; i < count && !replaced; i++)
            if(strcmp(settings[i].name, "Net/FontName") == 0) {
                snprintf(settings[i].string_value,
                         sizeof(settings[i].string_value), "%s", value);
                replaced = 1;
            }
        if(!replaced && count < 14) {
            snprintf(settings[count].name, sizeof(settings[count].name),
                     "Net/FontName");
            settings[count].type = 1;
            snprintf(settings[count].string_value,
                     sizeof(settings[count].string_value), "%s", value);
            count++;
        }
    }
    if(count > 0)
        platform->xsettings_publish(settings, count);
}

static void
init_panel_plugins(RillVisualState *visuals)
{
    const RillPanelPlugin *plugins;
    int count;

    if(visuals == NULL)
        return;
    plugins = RillPanelDefaultLeft(&count);
    if(count > RILL_PANEL_PLUGIN_MAX)
        count = RILL_PANEL_PLUGIN_MAX;
    memcpy(visuals->left_panel, plugins,
           (size_t)count * sizeof(visuals->left_panel[0]));
    visuals->left_panel_count = count;

    plugins = RillPanelDefaultRight(&count);
    if(count > RILL_PANEL_PLUGIN_MAX)
        count = RILL_PANEL_PLUGIN_MAX;
    memcpy(visuals->right_panel, plugins,
           (size_t)count * sizeof(visuals->right_panel[0]));
    visuals->right_panel_count = count;
    visuals->panel_context_open = 0;
    visuals->panel_context_side = 0;
    visuals->panel_context_index = -1;
}

static int
path_exists(const char *path)
{
#if RILL_HAS_DLOPEN
    return path != NULL && path[0] != '\0' && access(path, R_OK) == 0;
#else
    (void)path;
    return 0;
#endif
}

static RillHostModule *
host_slot(RillVisualState *visuals, const char *id)
{
    RillHostModule *slot;

    if(visuals == NULL || id == NULL || id[0] == '\0')
        return NULL;
    for(int i = 0; i < visuals->host_count; i++)
        if(strcmp(visuals->hosts[i].id, id) == 0)
            return &visuals->hosts[i];
    if(visuals->host_count >= RILL_HOST_CACHE_MAX)
        return NULL;
    slot = &visuals->hosts[visuals->host_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->id, sizeof(slot->id), "%s", id);
    return slot;
}

static int
load_host_path(RillHostModule *slot, const char *path)
{
#if RILL_HAS_DLOPEN
    CreateAppHostCallback create;

    if(slot == NULL || path == NULL || path[0] == '\0')
        return 0;
    slot->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if(slot->library == NULL)
        return 0;
    create = (CreateAppHostCallback)dlsym(slot->library, "CreateAppHost");
    slot->destroy =
        (DestroyAppHostCallback)dlsym(slot->library, "DestroyAppHost");
    if(create == NULL || slot->destroy == NULL) {
        dlclose(slot->library);
        slot->library = NULL;
        slot->destroy = NULL;
        return 0;
    }
    slot->host = create(APP_HOST_ABI_VERSION, NULL);
    if(slot->host == NULL) {
        dlclose(slot->library);
        slot->library = NULL;
        slot->destroy = NULL;
        return 0;
    }
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    return 1;
#else
    (void)slot;
    (void)path;
    return 0;
#endif
}

#ifdef KRYON_NATIVE_PLAN9
static int
load_static_host(RillHostModule *slot, const char *id)
{
    CreateAppHostCallback create = NULL;
    DestroyAppHostCallback destroy = NULL;

    if(slot == NULL || id == NULL)
        return 0;
    if(strcmp(id, "ktrem") == 0 || strcmp(id, "kterm") == 0) {
        create = KtermCreateAppHost;
        destroy = KtermDestroyAppHost;
    } else if(strcmp(id, "shelf") == 0) {
        create = ShelfCreateAppHost;
        destroy = ShelfDestroyAppHost;
    }
    if(create == NULL || destroy == NULL)
        return 0;
    slot->host = create(APP_HOST_ABI_VERSION, NULL);
    if(slot->host == NULL)
        return 0;
    slot->destroy = destroy;
    snprintf(slot->path, sizeof(slot->path), "builtin:%s", id);
    return 1;
}
#endif

static int
try_host_dir(RillHostModule *slot, const char *dir)
{
    char path[512];

    if(slot == NULL || dir == NULL || dir[0] == '\0')
        return 0;
    snprintf(path, sizeof(path), "%s/%s-host.so", dir, slot->id);
    if(path_exists(path) && load_host_path(slot, path))
        return 1;
    return 0;
}

static int
try_host_dir_list(RillHostModule *slot, const char *dirs)
{
    char copy[1024];
    char *start;
    char *end;
    char *dir;

    if(slot == NULL || dirs == NULL || dirs[0] == '\0')
        return 0;
    snprintf(copy, sizeof(copy), "%s", dirs);
    for(start = copy; start != NULL && start[0] != '\0'; start = end) {
        end = strchr(start, ':');
        if(end != NULL)
            *end++ = '\0';
        dir = start;
        if(dir[0] != '\0' && try_host_dir(slot, dir))
            return 1;
    }
    return 0;
}

static RillHostModule *
load_host_module(RillVisualState *visuals, const char *id)
{
    RillHostModule *slot;
    char dir[512];
    const char *env_dir;
    const char *home;

    slot = host_slot(visuals, id);
    if(slot == NULL)
        return NULL;
    if(slot->host != NULL)
        return slot;

#ifdef KRYON_NATIVE_PLAN9
    if(load_static_host(slot, id))
        return slot;
#endif

    env_dir = getenv("RILL_APP_HOST_DIR");
    if(try_host_dir_list(slot, env_dir))
        return slot;
    home = getenv("HOME");
    if(home != NULL && home[0] != '\0') {
        snprintf(dir, sizeof(dir), "%s/.local/lib/rill/apps", home);
        if(try_host_dir(slot, dir))
            return slot;
    }
    if(try_host_dir(slot, "/usr/local/lib/rill/apps") ||
       try_host_dir(slot, "/usr/lib/rill/apps"))
        return slot;
    snprintf(dir, sizeof(dir), "/mnt/storage/Projects/%s/build/linux-x86_64/lib",
             id);
    if(try_host_dir(slot, dir))
        return slot;

    if(!slot->missing_reported) {
        fprintf(stderr, "rill: no host module found for %s\n", id);
        slot->missing_reported = 1;
    }
    return slot;
}

/* Fit bounded labels using the same font metrics as the text widget. */
static void
draw_text_fit(TextProps props)
{
    if(props.text == NULL)
        props.text = "";
    while(props.font > Text8 &&
          MeasureTextWidth(props.text, props.font, NULL) > props.bounds.width)
        props.font -= 2;
    props.wrap = TextWrapNone;
    Text(props);
}

static void
draw_wallpaper(const RillVisualState *visuals)
{
    Rectangle screen;
    Rectangle src;
    float scale;
    float sw;
    float sh;
    int top;
    int panel_space;

    top = visuals->panel_bottom ? 0 : rill_panel_visible_height(visuals);
    panel_space = rill_panel_visible_height(visuals);
    screen = (Rectangle){0, (float)top, (float)GetScreenWidth(),
                         (float)(GetScreenHeight() - panel_space)};
    if(visuals->wallpaper_ready) {
        sw = (float)visuals->wallpaper.width;
        sh = (float)visuals->wallpaper.height;
        scale = screen.width / sw;
        if(sh * scale < screen.height)
            scale = screen.height / sh;
        src = (Rectangle){(sw - screen.width / scale) * 0.5f,
                          (sh - screen.height / scale) * 0.5f,
                          screen.width / scale,
                          screen.height / scale};
        DrawTexturePro(visuals->wallpaper, src, screen, (Vector2){0, 0}, 0.0f,
                       WHITE);
    } else {
        DrawRectangle(0, top, GetScreenWidth(),
                      GetScreenHeight() - panel_space,
                      opaque_color(GetThemeBackground()));
    }
    DrawRectangle(0, top, GetScreenWidth(), GetScreenHeight() - panel_space,
                  Fade(BLACK, 0.05f));
}

static int
launcher_index_by_id(RillShellState *shell, const char *id)
{
    int i;

    if(shell == NULL || id == NULL)
        return -1;
    for(i = 0; i < shell->launcher_count; i++)
        if(strcmp(shell->launchers[i].id, id) == 0)
            return i;
    return -1;
}

static const RillLauncher *
launcher_by_id(RillShellState *shell, const char *id)
{
    int index = launcher_index_by_id(shell, id);

    return index >= 0 ? &shell->launchers[index] : NULL;
}

static Texture2D *
visual_icon_texture(RillVisualState *visuals, const char *path)
{
    RillIconCacheEntry *entry;

    if(visuals == NULL || path == NULL || path[0] == '\0')
        return NULL;
    for(int i = 0; i < visuals->icon_count; i++) {
        if(strcmp(visuals->icons[i].path, path) == 0)
            return visuals->icons[i].ready ? &visuals->icons[i].texture : NULL;
    }
    if(visuals->icon_count >= RILL_ICON_CACHE_MAX)
        return NULL;
    entry = &visuals->icons[visuals->icon_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->texture = LoadTexture(path);
    entry->ready = entry->texture.id != 0;
    return entry->ready ? &entry->texture : NULL;
}

static void
load_launcher_icons(RillVisualState *visuals, const RillShellState *shell)
{
    if(visuals == NULL || shell == NULL)
        return;
    for(int i = 0; i < shell->launcher_count; i++)
        (void)visual_icon_texture(visuals, shell->launchers[i].icon_path);
}

static void
draw_texture_icon(Texture2D *texture, Rectangle dest)
{
    Rectangle src;
    float size;

    if(texture == NULL || texture->id == 0)
        return;
    size = dest.width < dest.height ? dest.width : dest.height;
    dest.x += (dest.width - size) * 0.5f;
    dest.y += (dest.height - size) * 0.5f;
    dest.width = size;
    dest.height = size;
    src = (Rectangle){0, 0, (float)texture->width, (float)texture->height};
    DrawTexturePro(*texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

static void
draw_symbol_icon(Rectangle r, const char *id, Color color)
{
    float cx = r.x + r.width * 0.5f;
    float cy = r.y + r.height * 0.5f;

    if(id != NULL && strcmp(id, "terminal") == 0) {
        DrawRectangleRoundedLinesEx((Rectangle){r.x + 3, r.y + 5,
                                                r.width - 6, r.height - 10},
                                    0.08f, 5, 2.0f, color);
        DrawLine((int)r.x + 10, (int)cy - 2, (int)r.x + 15, (int)cy + 3,
                 color);
        DrawLine((int)r.x + 10, (int)cy + 8, (int)r.x + 19, (int)cy + 8,
                 color);
    } else if(id != NULL && strcmp(id, "files") == 0) {
        DrawRectangleRounded((Rectangle){r.x + 4, r.y + 11, r.width - 8,
                                         r.height - 16},
                             0.08f, 5, Fade(color, 0.82f));
        DrawRectangleRounded((Rectangle){r.x + 7, r.y + 6, r.width * 0.42f,
                                         9},
                             0.08f, 4, color);
    } else if(id != NULL && strcmp(id, "settings") == 0) {
        DrawCircleLines((int)cx, (int)cy, r.width * 0.24f, color);
        DrawCircle((int)cx, (int)cy, r.width * 0.08f, color);
        for(int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f;
            DrawLine((int)(cx + cosf(a) * r.width * 0.28f),
                     (int)(cy + sinf(a) * r.width * 0.28f),
                     (int)(cx + cosf(a) * r.width * 0.39f),
                     (int)(cy + sinf(a) * r.width * 0.39f), color);
        }
    } else if(id != NULL && strcmp(id, "power") == 0) {
        DrawCircleLines((int)cx, (int)cy, r.width * 0.30f, color);
        DrawLine((int)cx, (int)r.y + 4, (int)cx, (int)cy, color);
    } else if(id != NULL && strcmp(id, "about") == 0) {
        DrawCircleLines((int)cx, (int)cy, r.width * 0.32f, color);
        DrawCircle((int)cx, (int)r.y + 8, 1.6f, color);
        DrawLine((int)cx, (int)cy - 1, (int)cx, (int)cy + 7, color);
    } else if(id != NULL && strcmp(id, "favorite") == 0) {
        Vector2 p[10];
        for(int i = 0; i < 10; i++) {
            float radius = (i % 2) == 0 ? r.width * 0.36f : r.width * 0.16f;
            float angle = -1.570796f + (float)i * 0.628319f;
            p[i] = (Vector2){cx + cosf(angle) * radius,
                              cy + sinf(angle) * radius};
        }
        for(int i = 0; i < 10; i++)
            DrawLine((int)p[i].x, (int)p[i].y, (int)p[(i + 1) % 10].x,
                     (int)p[(i + 1) % 10].y, color);
    } else if(id != NULL && strcmp(id, "recent") == 0) {
        DrawCircleLines((int)cx, (int)cy, r.width * 0.32f, color);
        DrawLine((int)cx, (int)cy, (int)cx, (int)cy - 7, color);
        DrawLine((int)cx, (int)cy, (int)cx + 6, (int)cy + 4, color);
    } else if(id != NULL && strcmp(id, "all") == 0) {
        DrawRectangleLines((int)r.x + 4, (int)r.y + 4, 6, 6, color);
        DrawRectangleLines((int)r.x + 14, (int)r.y + 4, 6, 6, color);
        DrawRectangleLines((int)r.x + 4, (int)r.y + 14, 6, 6, color);
        DrawRectangleLines((int)r.x + 14, (int)r.y + 14, 6, 6, color);
    } else if(id != NULL && strcmp(id, "internet") == 0) {
        DrawCircleLines((int)cx, (int)cy, r.width * 0.34f, color);
        DrawLine((int)(cx - r.width * 0.28f), (int)cy,
                 (int)(cx + r.width * 0.28f), (int)cy, color);
        DrawLine((int)cx, (int)(cy - r.width * 0.32f),
                 (int)cx, (int)(cy + r.width * 0.32f), color);
    } else if(id != NULL && strcmp(id, "office") == 0) {
        DrawRectangleLines((int)r.x + 5, (int)r.y + 3,
                           (int)r.width - 10, (int)r.height - 6, color);
        DrawLine((int)r.x + 9, (int)r.y + 9, (int)r.x + r.width - 8,
                 (int)r.y + 9, color);
        DrawLine((int)r.x + 9, (int)r.y + 15, (int)r.x + r.width - 8,
                 (int)r.y + 15, color);
    } else if(id != NULL && strcmp(id, "graphics") == 0) {
        DrawCircle((int)cx - 4, (int)cy - 4, 3, color);
        DrawCircle((int)cx + 4, (int)cy - 3, 3, color);
        DrawCircle((int)cx, (int)cy + 4, 3, color);
    } else if(id != NULL && strcmp(id, "multimedia") == 0) {
        DrawRectangleLines((int)r.x + 5, (int)r.y + 5,
                           (int)r.width - 10, (int)r.height - 10, color);
        DrawTriangle((Vector2){cx - 3, cy - 6}, (Vector2){cx - 3, cy + 6},
                     (Vector2){cx + 7, cy}, color);
    } else if(id != NULL && strcmp(id, "development") == 0) {
        DrawLine((int)r.x + 5, (int)cy, (int)r.x + 10, (int)cy - 5, color);
        DrawLine((int)r.x + 5, (int)cy, (int)r.x + 10, (int)cy + 5, color);
        DrawLine((int)r.x + r.width - 5, (int)cy,
                 (int)r.x + r.width - 10, (int)cy - 5, color);
        DrawLine((int)r.x + r.width - 5, (int)cy,
                 (int)r.x + r.width - 10, (int)cy + 5, color);
        DrawLine((int)cx + 2, (int)r.y + 5, (int)cx - 2,
                 (int)r.y + r.height - 5, color);
    } else {
        DrawCircleLines((int)cx, (int)cy, r.width * 0.32f, color);
        DrawCircle((int)cx, (int)cy, r.width * 0.07f, color);
    }
}

static void
draw_launcher_icon(RillVisualState *visuals, const RillLauncher *launcher,
                   Rectangle icon_rect, Color color)
{
    Texture2D *texture = NULL;

    if(launcher != NULL)
        texture = visual_icon_texture(visuals, launcher->icon_path);
    if(texture != NULL)
        draw_texture_icon(texture, icon_rect);
    else
        draw_symbol_icon(icon_rect, launcher != NULL ? launcher->id : NULL,
                         color);
}

static int
icon_hit_button(Rectangle bounds, int id)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), bounds);

    (void)id;
    if(hover)
        DrawRectangleRounded(bounds, 0.08f, 6, Fade(GetThemeButtonHover(), 0.38f));
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void
open_launcher_id(RillShellState *shell, const RillPlatformServices *platform,
                 const char *id)
{
    int index;

    index = launcher_index_by_id(shell, id);
    if(index >= 0) {
        RillShellSelectLauncher(shell, index);
        RillShellLaunchSelected(shell, platform);
    } else if(strcmp(id, "settings") == 0 || strcmp(id, "about") == 0) {
        RillLauncher built_in = {0};
        snprintf(built_in.id, sizeof(built_in.id), "%s", id);
        snprintf(built_in.name, sizeof(built_in.name), "%s", strcmp(id, "settings") == 0 ? "Settings" : "About Rill");
        snprintf(built_in.command, sizeof(built_in.command), "internal:%s", id);
        RillShellOpenLauncher(shell, &built_in);
    } else if(strcmp(id, "files") == 0 && platform->open_path != NULL) {
        platform->open_path(getenv("HOME") != NULL ? getenv("HOME") : "/");
    } else if(strcmp(id, "terminal") == 0 && platform->open_settings != NULL) {
        if(!platform->open_settings("terminal"))
            RillShellSetStatus(shell, "No terminal application is installed");
    }
}

static void
rill_control_init(RillControlState *control)
{
    FILE *file;

    if(control == NULL)
        return;
    memset(control, 0, sizeof(*control));
#ifdef KRYON_NATIVE_PLAN9
    snprintf(control->path, sizeof(control->path), "/tmp/rillctl");
    file = fopen(control->path, "w");
    if(file != NULL)
        fclose(file);
    putenv("rillctl", control->path);
    putenv("RILLCTL", control->path);
#else
    (void)file;
#endif
}

static void
rill_control_close(RillControlState *control)
{
    if(control == NULL)
        return;
#ifdef KRYON_NATIVE_PLAN9
    if(control->path[0] != '\0')
        remove(control->path);
#endif
}

static void
rill_control_poll(RillControlState *control, RillShellState *shell,
                  const RillPlatformServices *platform)
{
#ifdef KRYON_NATIVE_PLAN9
    FILE *file;
    char line[160];
    char *trimmed;
    int len;

    if(control == NULL || control->path[0] == '\0')
        return;
    file = fopen(control->path, "r");
    if(file == NULL)
        return;
    if(control->offset > 0)
        fseek(file, control->offset, SEEK_SET);
    while(fgets(line, sizeof(line), file) != NULL) {
        if(shell == NULL || platform == NULL)
            continue;
        len = (int)strlen(line);
        while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                          line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
        trimmed = line;
        while(*trimmed == ' ' || *trimmed == '\t')
            trimmed++;
        if(strcmp(trimmed, "open ktrem") == 0 ||
           strcmp(trimmed, "open kterm") == 0 ||
           strcmp(trimmed, "open terminal") == 0 ||
           strcmp(trimmed, "ktrem") == 0 ||
           strcmp(trimmed, "kterm") == 0 ||
           strcmp(trimmed, "terminal") == 0)
            open_launcher_id(shell, platform, "terminal");
    }
    control->offset = ftell(file);
    fclose(file);
#else
    (void)control;
    (void)shell;
    (void)platform;
#endif
}

static int
launcher_id_installed(const RillShellState *shell,
                      const RillVisualState *visuals, const char *id)
{
    for(int i = 0; i < visuals->desktop_file_count; i++)
        if(strcmp(visuals->desktop_files[i].id, id) == 0)
            return 1;
    (void)shell;
    return 0;
}

static void
open_special_icon(RillShellState *shell, const RillPlatformServices *platform,
                   const RillLauncher *launcher)
{
    const char *target = NULL;
    const char *home;

    if(strcmp(launcher->id, "desktop-home") == 0) {
        home = getenv("HOME");
        target = home != NULL && home[0] != '\0' ? home : "/";
    } else if(strcmp(launcher->id, "desktop-filesystem") == 0)
        target = "/";
    else if(strcmp(launcher->id, "desktop-trash") == 0)
        target = "trash://";
    if(target != NULL && platform != NULL && platform->open_path != NULL &&
       platform->open_path(target))
        return;
    open_launcher_id(shell, platform, "files");
}

static int
launcher_is_special(const RillLauncher *launcher)
{
    return launcher != NULL &&
           (strcmp(launcher->id, "desktop-home") == 0 ||
            strcmp(launcher->id, "desktop-filesystem") == 0 ||
            strcmp(launcher->id, "desktop-trash") == 0);
}

static void
open_desktop_launcher(RillShellState *shell,
                      const RillPlatformServices *platform,
                      const RillLauncher *launcher)
{
    if(launcher_is_special(launcher)) {
        open_special_icon(shell, platform, launcher);
        return;
    }
    if(strncmp(launcher->command, "internal:", 9) == 0 ||
       strncmp(launcher->command, "host:", 5) == 0) {
        RillShellOpenLauncher(shell, launcher);
    } else if(launcher->desktop_file[0] && platform->launch != NULL) {
        if(!platform->launch(launcher))
            RillShellSetStatus(shell, "Could not launch the desktop application");
    } else if(launcher->file_path[0] && platform->open_path != NULL) {
        if(!platform->open_path(launcher->file_path))
            RillShellSetStatus(shell, "No application could open this file");
    } else
        open_launcher_id(shell, platform, launcher->id);
}

static void
draw_desktop_icon(RillShellState *shell, const RillPlatformServices *platform,
                  RillVisualState *visuals, int x, int y, int index,
                  const RillLauncher *launcher, Color accent)
{
    Rectangle box;
    Rectangle icon;
    int pressed;

    if(launcher == NULL)
        return;
    box = (Rectangle){x, y, 84, 82};
    icon = (Rectangle){x + 22, y + 5, 40, 40};
    visuals->desktop_bounds[index] = box;
    int interactive = shell->menu_open == 0 && !visuals->file_action[0] &&
                      !visuals->logout_open && !visuals->properties_open;
    for(int i = 0; i < shell->app_count; i++) {
        const RillAppWindow *app = &shell->apps[i];
        if(CheckCollisionPointRec(GetMousePosition(),
                                  (Rectangle){app->x, app->y, app->w, app->h}))
            interactive = 0;
    }
    pressed = interactive && icon_hit_button(box, 7000 + index);
    if(visuals->desktop_selected == index) {
        DrawRectangleRounded(box, 0.06f, 6, Fade(GetThemeLink(), 0.30f));
        DrawRectangleRoundedLinesEx(box, 0.06f, 6, 1.0f,
                                    Fade(GetThemeLink(), 0.60f));
    }
    draw_launcher_icon(visuals, launcher, icon, accent);
    draw_text_fit((TextProps){
        .bounds = {x + 4, y + 52, 76, 0},
        .text = launcher->name, .font = Text12, .class_name = LabelPrimary,
        .wrap = TextWrapNone, .align = TextAlignCenter});
    if(pressed) {
        visuals->desktop_drag_index = index;
        visuals->desktop_drag_start = GetMousePosition();
        visuals->desktop_drag_origin = (Vector2){box.x, box.y};
        double now = GetTime();
        visuals->desktop_selected = index;
        if(visuals->desktop_last_index == index &&
           now - visuals->desktop_last_click < 0.45) {
            open_desktop_launcher(shell, platform, launcher);
            visuals->desktop_last_index = -1;
            visuals->desktop_last_click = 0;
        } else {
            visuals->desktop_last_index = index;
            visuals->desktop_last_click = now;
        }
    }
}

static const RillLauncher *
desktop_special_entry(int index)
{
    static RillLauncher entries[3];
    static int initialized;

    if(!initialized) {
        initialized = 1;
        memset(entries, 0, sizeof(entries));
        snprintf(entries[0].id, sizeof(entries[0].id), "desktop-home");
        snprintf(entries[0].name, sizeof(entries[0].name), "Home");
        snprintf(entries[0].command, sizeof(entries[0].command), "internal:home");
        snprintf(entries[0].category, sizeof(entries[0].category), "files");
        snprintf(entries[1].id, sizeof(entries[1].id), "desktop-filesystem");
        snprintf(entries[1].name, sizeof(entries[1].name), "File System");
        snprintf(entries[1].command, sizeof(entries[1].command), "internal:filesystem");
        snprintf(entries[1].category, sizeof(entries[1].category), "files");
        snprintf(entries[2].id, sizeof(entries[2].id), "desktop-trash");
        snprintf(entries[2].name, sizeof(entries[2].name), "Trash");
        snprintf(entries[2].command, sizeof(entries[2].command), "internal:trash");
        snprintf(entries[2].category, sizeof(entries[2].category), "files");
    }
    return &entries[index];
}

static void
ensure_desktop_files(RillVisualState *visuals,
                     const RillPlatformServices *platform)
{
    if(visuals == NULL || platform == NULL || platform->list_desktop_files == NULL ||
       (visuals->desktop_files_scanned && GetTime() < visuals->desktop_next_scan) ||
       visuals->desktop_drag_active || visuals->file_action[0] != '\0')
        return;
    visuals->desktop_next_scan = GetTime() + 2.0;
    RillLauncher *files = calloc(RILL_DESKTOP_FILE_MAX, sizeof(*files));
    if(files == NULL)
        return;
    int count = platform->list_desktop_files(files, RILL_DESKTOP_FILE_MAX);
    if(count < 0) count = 0;
    if(count > RILL_DESKTOP_FILE_MAX) count = RILL_DESKTOP_FILE_MAX;
    if(count != visuals->desktop_file_count ||
       memcmp(files, visuals->desktop_files, (size_t)count * sizeof(*files)) != 0) {
        visuals->desktop_selected = -1;
        visuals->desktop_last_index = -1;
        visuals->desktop_file_count = count;
        memcpy(visuals->desktop_files, files, (size_t)count * sizeof(*files));
    }
    free(files);
    visuals->desktop_files_scanned = 1;
}

static void
save_desktop_position(RillVisualState *visuals, int index, int x, int y)
{
    char position[64];
    snprintf(position, sizeof(position), "%d %d", x, y);
    RillSettingsSet(&visuals->desktop_layout, visuals->desktop_entries[index]->id,
                    position);
    if(visuals->desktop_layout_path[0])
        RillSettingsSave(&visuals->desktop_layout, visuals->desktop_layout_path);
}

static void
begin_file_action(RillVisualState *visuals, const char *action, const char *source)
{
    snprintf(visuals->file_action, sizeof(visuals->file_action), "%s", action);
    snprintf(visuals->file_source, sizeof(visuals->file_source), "%s", source);
    const char *name = strrchr(source, '/');
    snprintf(visuals->file_name, sizeof(visuals->file_name), "%s",
             strcmp(action, "rename") == 0 ? (name ? name + 1 : source) : "New Folder");
    visuals->file_error[0] = '\0';
    visuals->file_focused = 1;
    visuals->file_cursor = (int)strlen(visuals->file_name);
}

static int
point_on_desktop_icon_grid(const RillShellState *shell,
                           const RillVisualState *visuals, Vector2 mouse)
{
    int top = visuals->panel_bottom ? 0 : rill_panel_visible_height(visuals);

    if(mouse.y < top || mouse.y >= GetScreenHeight() -
       (visuals->panel_bottom ? rill_panel_visible_height(visuals) : 0))
        return 0;
    for(int i = 0; i < shell->app_count; i++) {
        Rectangle frame = {shell->apps[i].x, shell->apps[i].y,
                           shell->apps[i].w, shell->apps[i].h};
        if(CheckCollisionPointRec(mouse, frame))
            return 0;
    }
    return 1;
}

static void
process_desktop_mouse(RillShellState *shell,
                      const RillPlatformServices *platform,
                      RillVisualState *visuals)
{
    Vector2 mouse;
    float wheel;

    if(shell == NULL || visuals == NULL || platform == NULL)
        return;
    if(shell->menu_open != 0 || visuals->file_action[0] || visuals->logout_open ||
       visuals->properties_open)
        return;
    mouse = GetMousePosition();
    int down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    int over = 1;
#if RILL_HAS_X11
    DesktopSurfacePointer(&mouse, &down, &over);
#endif
    visuals->desktop_pointer = mouse;
    int hit = -1;
    for(int i = visuals->desktop_entry_count - 1; i >= 0; i--)
        if(CheckCollisionPointRec(mouse, visuals->desktop_bounds[i])) {
            hit = i;
            break;
        }
    if(visuals->desktop_drag_index >= 0) {
        float dx = mouse.x - visuals->desktop_drag_start.x;
        float dy = mouse.y - visuals->desktop_drag_start.y;
        if(dx * dx + dy * dy > 25) {
            visuals->desktop_drag_active = 1;
            visuals->desktop_last_index = -1;
        }
        if(!down) {
            if(visuals->desktop_drag_active)
                save_desktop_position(visuals, visuals->desktop_drag_index,
                                      visuals->desktop_drag_origin.x + dx,
                                      visuals->desktop_drag_origin.y + dy);
            visuals->desktop_drag_index = -1;
            visuals->desktop_drag_active = 0;
        }
    }
    if(!over || !point_on_desktop_icon_grid(shell, visuals, mouse))
        return;
    wheel = GetMouseWheelMove();
    if(wheel != 0 && platform->workspace_count != NULL &&
       platform->current_workspace != NULL &&
       platform->switch_workspace != NULL) {
        int count = platform->workspace_count();
        int current = platform->current_workspace();
        if(count > 0 && current >= 0)
            platform->switch_workspace(
                (current + (wheel > 0 ? count - 1 : 1)) % count);
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        visuals->desktop_selected = hit;
        shell->menu_open = 4;
        visuals->desktop_menu_x = (int)mouse.x;
        visuals->desktop_menu_y = (int)mouse.y;
    } else if(IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
        shell->menu_open = 5;
        visuals->desktop_menu_x = (int)mouse.x;
        visuals->desktop_menu_y = (int)mouse.y;
    } else if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
              (down && visuals->desktop_drag_index < 0)) {
        if(hit < 0) {
            visuals->desktop_selected = -1;
            visuals->desktop_last_index = -1;
        } else {
            visuals->desktop_drag_index = hit;
            visuals->desktop_drag_start = mouse;
            visuals->desktop_drag_origin = (Vector2){visuals->desktop_bounds[hit].x,
                                                     visuals->desktop_bounds[hit].y};
        }
    }
    if(IsKeyPressed(KEY_F5))
        visuals->desktop_files_scanned = 0;
    if(visuals->desktop_selected >= 0 && platform->file_operation != NULL) {
        const RillLauncher *item = visuals->desktop_entries[visuals->desktop_selected];
        if(item->file_path[0] && IsKeyPressed(KEY_F2))
            begin_file_action(visuals, "rename", item->file_path);
        if(item->file_path[0] && IsKeyPressed(KEY_DELETE))
            begin_file_action(visuals, "trash", item->file_path);
        if(IsKeyPressed(KEY_ENTER))
            open_desktop_launcher(shell, platform, item);
    }
}

static void
draw_desktop(RillShellState *shell, const RillPlatformServices *platform,
             RillVisualState *visuals)
{
    int top = visuals->panel_bottom ? 0 : rill_panel_visible_height(visuals);
    int bottom = visuals->panel_bottom ? rill_panel_visible_height(visuals) : 0;
    int rows = (GetScreenHeight() - top - bottom - 28) / 94;
    int entry_count = 0;
    if(rows < 1) rows = 1;
    if(shell == NULL)
        return;
    ensure_desktop_files(visuals, platform);
    for(int i = 0; i < 3; i++)
        visuals->desktop_entries[entry_count++] = desktop_special_entry(i);
    for(int i = 0; i < visuals->desktop_file_count; i++)
        visuals->desktop_entries[entry_count++] = &visuals->desktop_files[i];
    for(int i = 0; i < shell->launcher_count && entry_count < DESKTOP_ICON_MAX; i++)
        if(shell->launchers[i].favorite &&
           !launcher_id_installed(shell, visuals, shell->launchers[i].id))
            visuals->desktop_entries[entry_count++] = &shell->launchers[i];
    visuals->desktop_entry_count = entry_count;
    for(int i = 0; i < entry_count; i++) {
        visuals->desktop_snapshots[i] = *visuals->desktop_entries[i];
        visuals->desktop_entries[i] = &visuals->desktop_snapshots[i];
        int x = 28 + (i / rows) * 94, y = top + 28 + (i % rows) * 94;
        const char *position = RillSettingsGet(&visuals->desktop_layout,
                                               visuals->desktop_entries[i]->id, "");
        int saved_x, saved_y;
        if(sscanf(position, "%d %d", &saved_x, &saved_y) == 2) {
            x = saved_x;
            y = saved_y;
        }
        if(visuals->desktop_drag_active && visuals->desktop_drag_index == i) {
            Vector2 mouse = visuals->desktop_pointer;
            x = visuals->desktop_drag_origin.x + mouse.x - visuals->desktop_drag_start.x;
            y = visuals->desktop_drag_origin.y + mouse.y - visuals->desktop_drag_start.y;
        }
        if(x > GetScreenWidth() - 84) x = GetScreenWidth() - 84;
        if(y > GetScreenHeight() - bottom - 82) y = GetScreenHeight() - bottom - 82;
        if(x < 0) x = 0;
        if(y < top) y = top;
        draw_desktop_icon(shell, platform, visuals, x, y, i, visuals->desktop_entries[i],
                          i < 3 ? GetThemeLink() : GetThemeIcon());
    }
}

static void
draw_panel_separator(int x, int y, int ph)
{
    DrawRectangle(x, y + 4, 1, ph - 8, Fade(BLACK, 0.45f));
    DrawRectangle(x + 1, y + 4, 1, ph - 8, Fade(WHITE, 0.13f));
}

static void
draw_applications_mark(int x, int y)
{
    Color blue = {55, 186, 236, 255};
    Color white = {238, 246, 255, 255};

    DrawCircle(x + 7, y + 7, 7, blue);
    DrawCircle(x + 5, y + 5, 2, white);
    DrawLine(x + 5, y + 9, x + 11, y + 4, white);
    DrawLine(x + 7, y + 11, x + 12, y + 8, white);
}

static int
panel_menu_button(RillShellState *shell, int menu_id, int x, int w,
                  const char *label, int id, int y, int ph)
{
    Rectangle bounds = {x, (float)y + 2, (float)w, (float)ph - 4};
    int glyph_y = y + (ph - 14) / 2;
    int text_y = y + (ph - 12) / 2;
    int hover;

    (void)id;
    hover = CheckCollisionPointRec(GetMousePosition(), bounds);
    if(shell->menu_open == menu_id || hover)
        DrawRectangleRec(bounds, shell->menu_open == menu_id ?
                         panel_active_color() : panel_item_hover_color());
    if(menu_id == 1)
        draw_applications_mark(x + 3, y + (ph - 14) / 2 + 1);
    else
        draw_launcher_icon(NULL, NULL, (Rectangle){x + 5, (float)glyph_y, 14, 14},
                           menu_id == 2 ? GetThemeLink() : GetThemeIcon());
    draw_text_fit((TextProps){
        .bounds = {x + (menu_id == 1 ? 22 : 24), text_y, w - (menu_id == 1 ? 26 : 28), 0},
        .text = label, .font = Text12, .class_name = LabelPanel,
        .wrap = TextWrapNone});
    if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        shell->menu_open = shell->menu_open == menu_id ? 0 : menu_id;
        return 1;
    }
    return 0;
}

static void
draw_quick_launcher(RillShellState *shell, const RillPlatformServices *platform,
                    RillVisualState *visuals, int x, const char *launcher_id,
                    int id, int y, int ph)
{
    Rectangle bounds = {x, (float)y + 2, 22, (float)ph - 4};
    Rectangle icon = {x + 3, (float)(y + (ph - 16) / 2), 16, 16};
    const RillLauncher *launcher = launcher_by_id(shell, launcher_id);
    int hover = CheckCollisionPointRec(GetMousePosition(), bounds);

    (void)id;
    if(hover)
        DrawRectangleRec(bounds, panel_item_hover_color());
    if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        open_launcher_id(shell, platform, launcher_id);
    draw_launcher_icon(visuals, launcher, icon, GetThemeText());
}

static void
draw_tray_indicator(int x, int kind, Color color, int y, int ph)
{
    int oy = y + (ph - PANEL_H) / 2;
    if(kind == 0) {
        DrawLine(x + 3, oy + 15, x + 8, oy + 10, color);
        DrawLine(x + 8, oy + 10, x + 15, oy + 10, color);
        DrawLine(x + 4, oy + 16, x + 9, oy + 12, color);
        DrawLine(x + 9, oy + 12, x + 14, oy + 12, color);
    } else if(kind == 1) {
        DrawRectangle(x + 3, oy + 12, 4, 5, color);
        DrawTriangle((Vector2){x + 7, (float)oy + 12}, (Vector2){x + 13, (float)oy + 8},
                     (Vector2){x + 13, (float)oy + 20}, color);
        DrawCircleLines(x + 15, oy + 14, 4, color);
    } else {
        DrawCircle(x + 10, oy + 14, 5, color);
        DrawLine(x + 10, oy + 7, x + 10, oy + 4, color);
    }
}

static void
draw_workspace_switcher(int x, int width, RillShellState *shell,
                         const RillPlatformServices *platform, int y, int ph)
{
    int count, current;
    int oy = (ph - PANEL_H) / 2;
    if(platform->workspace_count == NULL || platform->current_workspace == NULL ||
       platform->switch_workspace == NULL) return;
    count = platform->workspace_count();
    current = platform->current_workspace();
    if(count <= 0 || current < 0 || width < 20) return;
    /* Scroll cycles every workspace even when the panel item is narrow. */
    Rectangle bounds = {x, (float)y + 2, (float)width, (float)ph - 4};
    if(CheckCollisionPointRec(GetMousePosition(), bounds)) {
        float wheel = GetMouseWheelMove();
        int next = (current + (wheel > 0 ? -1 : 1) + count) % count;
        if(wheel != 0 && !platform->switch_workspace(next))
            RillShellSetStatus(shell, "Could not switch workspace");
    }
    int visible = width / 20;
    if(visible > count) visible = count;
    int first = current / visible * visible;
    for(int i = 0; i < visible && first + i < count; i++) {
        char label[16];
        int index = first + i;
        Rectangle button = {x + i * 20, (float)(y + oy + 4), 18, 18};
        DrawRectangleRec(button, index == current ? panel_active_color() : panel_item_color());
        snprintf(label, sizeof(label), "%d", index + 1);
        Text((TextProps){
            .bounds = {(int)button.x + 4, y + oy + 7, 0, 0},
            .text = label, .font = Text12, .class_name = LabelPanel,
            .wrap = TextWrapNone});
        if(CheckCollisionPointRec(GetMousePosition(), button) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            if(!platform->switch_workspace(index))
                RillShellSetStatus(shell, "Could not switch workspace");
    }
}

static void
draw_panel_resource(int x, const char *label, Color color, int y, int ph)
{
    int oy = (ph - PANEL_H) / 2;
    Text((TextProps){
        .bounds = {x, y + oy + 7, 0, 0},
        .text = label, .font = 11, .class_name = LabelPanelDim,
        .wrap = TextWrapNone});
    DrawRectangle(x + 26, y + oy + 18, 28, 3, Fade(BLACK, 0.45f));
    DrawRectangle(x + 26, y + oy + 18, 16, 3, color);
}

static void
draw_task_icon(RillVisualState *visuals, RillShellState *shell,
               const RillTask *task, Rectangle icon);
static void
draw_menu_panel(Rectangle menu);

static void
open_panel_context(RillVisualState *visuals, int side, int index)
{
    Vector2 mouse;

    if(visuals == NULL)
        return;
    mouse = GetMousePosition();
    visuals->panel_context_open = 1;
    visuals->panel_context_side = side;
    visuals->panel_context_index = index;
    visuals->panel_context_x = (int)mouse.x;
    visuals->panel_context_y = (int)mouse.y;
}

static RillPanelPlugin *
panel_plugins_for_side(RillVisualState *visuals, int side, int *count)
{
    if(visuals == NULL)
        return NULL;
    if(side == 0) {
        if(count != NULL)
            *count = visuals->left_panel_count;
        return visuals->left_panel;
    }
    if(count != NULL)
        *count = visuals->right_panel_count;
    return visuals->right_panel;
}

static void
set_panel_count_for_side(RillVisualState *visuals, int side, int count)
{
    if(visuals == NULL)
        return;
    if(count < 0)
        count = 0;
    if(count > RILL_PANEL_PLUGIN_MAX)
        count = RILL_PANEL_PLUGIN_MAX;
    if(side == 0)
        visuals->left_panel_count = count;
    else
        visuals->right_panel_count = count;
}

static int
panel_context_row(Rectangle row, const char *label)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), row);

    DrawRectangleRec(row, hover ? panel_item_hover_color() :
                     panel_item_color());
    DrawRectangle((int)row.x, (int)(row.y + row.height - 1), (int)row.width,
                  1, Fade(BLACK, 0.28f));
    draw_text_fit((TextProps){
        .bounds = {(int)row.x + 10, (int)row.y + 7, (int)row.width - 20, 0},
        .text = label, .font = Text12, .class_name = LabelPanel,
        .wrap = TextWrapNone});
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void
panel_swap_item(RillVisualState *visuals, int side, int index, int delta)
{
    RillPanelPlugin *plugins;
    RillPanelPlugin tmp;
    int count;
    int other;

    plugins = panel_plugins_for_side(visuals, side, &count);
    other = index + delta;
    if(plugins == NULL || index < 0 || index >= count || other < 0 ||
       other >= count)
        return;
    tmp = plugins[index];
    plugins[index] = plugins[other];
    plugins[other] = tmp;
    visuals->panel_context_index = other;
    visuals->panel_dirty = 1;
}

static void
panel_remove_item(RillVisualState *visuals, int side, int index)
{
    RillPanelPlugin *plugins;
    int count;

    plugins = panel_plugins_for_side(visuals, side, &count);
    if(plugins == NULL || index < 0 || index >= count)
        return;
    memmove(&plugins[index], &plugins[index + 1],
            (size_t)(count - index - 1) * sizeof(plugins[0]));
    set_panel_count_for_side(visuals, side, count - 1);
    visuals->panel_dirty = 1;
    visuals->panel_context_open = 0;
}

static void
panel_append_item(RillVisualState *visuals, int side, RillPanelPlugin plugin)
{
    RillPanelPlugin *plugins;
    int count;

    plugins = panel_plugins_for_side(visuals, side, &count);
    if(plugins == NULL || count >= RILL_PANEL_PLUGIN_MAX)
        return;
    plugins[count] = plugin;
    set_panel_count_for_side(visuals, side, count + 1);
    visuals->panel_dirty = 1;
    visuals->panel_context_index = count;
}

static RillPanelPlugin
panel_plugin(RillPanelPluginKind kind, const char *id, int width, int advance)
{
    RillPanelPlugin plugin;
    memset(&plugin, 0, sizeof(plugin));
    plugin.kind = kind;
    snprintf(plugin.id, sizeof(plugin.id), "%s", id);
    plugin.width = width;
    plugin.advance = advance;
    return plugin;
}

static void
draw_panel_context_menu(RillShellState *shell, RillVisualState *visuals,
                         const RillPlatformServices *platform)
{
    RillPanelPlugin *plugins;
    RillPanelPlugin plugin;
    Rectangle menu;
    int count;
    int index;
    int side;
    int x;
    int y;

    if(visuals == NULL || !visuals->panel_context_open)
        return;
    side = visuals->panel_context_side;
    index = visuals->panel_context_index;
    plugins = panel_plugins_for_side(visuals, side, &count);
    if(plugins == NULL || index < -1 || index >= count) {
        visuals->panel_context_open = 0;
        return;
    }

    x = visuals->panel_context_x;
    y = visuals->panel_context_y;
    if(x + 210 > GetScreenWidth())
        x = GetScreenWidth() - 210;
    if(y + 188 > GetScreenHeight())
        y = GetScreenHeight() - 188;
    if(x < 2)
        x = 2;
    if(y < 2)
        y = 2;
    menu = (Rectangle){x, y, 208, index >= 0 ? 188 : 268};
    draw_menu_panel(menu);

    if(index >= 0) {
        char title[96];

        snprintf(title, sizeof(title), "%s",
                 plugins[index].id[0] != '\0' ? plugins[index].id :
                 RillPanelPluginKindName(plugins[index].kind));
        draw_text_fit((TextProps){
            .bounds = {x + 10, y + 8, 188, 0},
            .text = title, .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        if(panel_context_row((Rectangle){x + 6, y + 30, 196, 26},
                             "Move Left"))
            panel_swap_item(visuals, side, index, -1);
        if(panel_context_row((Rectangle){x + 6, y + 58, 196, 26},
                             "Move Right"))
            panel_swap_item(visuals, side, visuals->panel_context_index, 1);
        if(panel_context_row((Rectangle){x + 6, y + 86, 196, 26},
                             "Remove"))
            panel_remove_item(visuals, side, visuals->panel_context_index);
        if(panel_context_row((Rectangle){x + 6, y + 114, 196, 26},
                             "Properties")) {
            visuals->properties_open = 1;
            visuals->properties_side = side;
            visuals->properties_index = index;
            visuals->panel_context_open = 0;
        }
        y += 140;
    } else {
        if(panel_context_row((Rectangle){x + 6, y + 6, 196, 26},
                             visuals->panel_bottom ? "Move Panel to Top" :
                             "Move Panel to Bottom")) {
            visuals->panel_bottom = !visuals->panel_bottom;
            RillSettingsSet(&rill_settings, "panel-side",
                            visuals->panel_bottom ? "bottom" : "top");
            rill_settings_persist(shell);
            visuals->panel_context_open = 0;
        }
        if(panel_context_row((Rectangle){x + 6, y + 34, 196, 26},
                             visuals->panel_autohide ? "Autohide: on" :
                             "Autohide: off")) {
            visuals->panel_autohide = !visuals->panel_autohide;
            RillSettingsSetInteger(&rill_settings, "panel-autohide",
                                   visuals->panel_autohide);
            rill_settings_persist(shell);
            visuals->panel_context_open = 0;
        }
        y += 62;
    }

    plugin = panel_plugin(RILL_PANEL_SEPARATOR, "separator", 0, 8);
    if(panel_context_row((Rectangle){x + 6, y, 196, 26}, "Add Separator"))
        panel_append_item(visuals, side, plugin);
    plugin = panel_plugin(RILL_PANEL_TASK_LIST, "task-list", 0, 0);
    if(panel_context_row((Rectangle){x + 6, y + 28, 196, 26}, "Add Task List"))
        panel_append_item(visuals, side, plugin);
    plugin = panel_plugin(RILL_PANEL_WORKSPACES, "workspaces", 42, 42);
    if(panel_context_row((Rectangle){x + 6, y + 56, 196, 26},
                         "Add Workspaces"))
        panel_append_item(visuals, side, plugin);
    plugin = panel_plugin(RILL_PANEL_SHOW_DESKTOP, "show-desktop", 26, 28);
    if(panel_context_row((Rectangle){x + 6, y + 84, 196, 26},
                         "Add Show Desktop"))
        panel_append_item(visuals, side, plugin);
    plugin = panel_plugin(RILL_PANEL_ACTIONS, "actions", 26, 28);
    if(panel_context_row((Rectangle){x + 6, y + 112, 196, 26},
                         "Add Action Buttons"))
        panel_append_item(visuals, side, plugin);
    plugin = panel_plugin(RILL_PANEL_VOLUME, "volume", 58, 60);
    if(panel_context_row((Rectangle){x + 6, y + 140, 196, 26},
                         "Add Volume Control"))
        panel_append_item(visuals, side, plugin);
    plugin = panel_plugin(RILL_PANEL_CLIPBOARD, "clipboard", 26, 28);
    if(panel_context_row((Rectangle){x + 6, y + 168, 196, 26},
                         "Add Clipboard History"))
        panel_append_item(visuals, side, plugin);
    if(panel_context_row((Rectangle){x + 6, y + 84, 196, 26},
                         "Add XFCE Plugin...")) {
#if RILL_HAS_X11
        RillLauncher launcher;
        memset(&launcher, 0, sizeof(launcher));
        snprintf(launcher.name, sizeof(launcher.name), "Xfce panel items");
        snprintf(launcher.command, sizeof(launcher.command), "xfce4-panel --add-items");
        if(!platform->launch(&launcher))
            RillShellSetStatus(shell, "Could not open Xfce panel; install xfce4-panel");
#else
        (void)platform;
        RillShellSetStatus(shell, "Xfce plugins require a Linux panel host");
#endif
        visuals->panel_context_open = 0;
    }

    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), menu))
        visuals->panel_context_open = 0;
}

static int
draw_panel_task_list(RillShellState *shell, const RillPlatformServices *platform,
                     RillVisualState *visuals, int x, int right, int y, int ph)
{
    int i;
    int oy = (ph - PANEL_H) / 2;
    int flash = (int)(GetTime() * 2.0f) % 2 == 0;

    for(i = 0; i < shell->task_count && x < right - 120; i++) {
        Rectangle task_rect;
        int hover;
        int width;

        width = shell->tasks[i].focused ? 190 : 154;
        task_rect = (Rectangle){x, (float)y + 1, (float)width, (float)ph - 2};
        hover = CheckCollisionPointRec(GetMousePosition(), task_rect);
        DrawRectangleRec(task_rect, shell->tasks[i].focused ?
                         panel_active_color() :
                         (shell->tasks[i].urgent && flash ?
                          (Color){0x80, 0x53, 0x28, 0xff} :
                          (hover ? panel_item_hover_color() :
                           panel_item_color())));
        DrawRectangleLinesEx(task_rect, 1.0f, shell->tasks[i].focused ?
                             Fade(WHITE, 0.55f) : Fade(BLACK, 0.40f));
        draw_task_icon(visuals, shell, &shell->tasks[i],
                       (Rectangle){x + 5, (float)(y + oy + 5), 16, 16});
        draw_text_fit((TextProps){
            .bounds = {x + 27, y + oy + 7, width - 32, 0},
            .text = shell->tasks[i].title, .font = Text12, .class_name = LabelPanel,
            .wrap = TextWrapNone});
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            RillShellSelectTask(shell, i);
            RillShellFocusSelectedTask(shell, platform);
        }
        x += width + 4;
    }
    return x;
}

static int
draw_panel_plugin(const RillPanelPlugin *plugin, RillShellState *shell,
                  const RillPlatformServices *platform,
                  RillVisualState *visuals, int x, int task_right, int side,
                  int index, const char *clock_text, int y, int ph)
{
    Rectangle context_bounds;
    int oy = (ph - PANEL_H) / 2;

    if(plugin == NULL)
        return x;
    context_bounds = (Rectangle){x, (float)y,
                                 plugin->kind == RILL_PANEL_TASK_LIST ?
                                 (float)(task_right - x) :
                                 (float)(plugin->advance > 0 ? plugin->advance :
                                  plugin->width),
                                 (float)ph};
    if(context_bounds.width < 12)
        context_bounds.width = 12;
    if(CheckCollisionPointRec(GetMousePosition(), context_bounds) &&
       IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        open_panel_context(visuals, side, index);
    switch(plugin->kind) {
    case RILL_PANEL_SEPARATOR:
        draw_panel_separator(x, y, ph);
        return x + plugin->advance;
    case RILL_PANEL_MENU:
        if(plugin->menu_id != 1 && GetScreenWidth() < 620)
            return x;
        panel_menu_button(shell, plugin->menu_id, x, plugin->width,
                          plugin->label, 0, y, ph);
        return x + plugin->advance;
    case RILL_PANEL_LAUNCHER:
        draw_quick_launcher(shell, platform, visuals, x, plugin->launcher_id,
                            0, y, ph);
        return x + plugin->advance;
    case RILL_PANEL_TASK_LIST:
        return draw_panel_task_list(shell, platform, visuals, x, task_right,
                                    y, ph);
    case RILL_PANEL_WORKSPACES:
        draw_workspace_switcher(x, plugin->width, shell, platform, y, ph);
        return x + plugin->advance;
    case RILL_PANEL_TRAY: {
        int shown = 0;
        for(int t = 0; t < visuals->tray_count && shown < 6; t++) {
            RillTrayEntry *entry = &visuals->tray[t];
            Rectangle icon = {x + 3 + shown * 22,
                              (float)(y + (ph - 18) / 2), 18, 18};
            int hover = CheckCollisionPointRec(GetMousePosition(), icon);
            if(hover)
                DrawRectangleRec((Rectangle){icon.x - 2, icon.y - 2, 22, 22},
                                 panel_item_hover_color());
            if(entry->ready) {
                DrawTexturePro(entry->texture,
                               (Rectangle){0, 0, (float)entry->texture.width,
                                           (float)entry->texture.height},
                               icon, (Vector2){0, 0}, 0.0f, WHITE);
            } else
                DrawCircleLines((int)icon.x + 9, (int)icon.y + 9, 6,
                                GetThemeLink());
            if(hover && entry->id[0] != '\0' &&
               platform != NULL && platform->tray_activate != NULL) {
                if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    platform->tray_activate(entry->id, 0);
                else if(IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                    platform->tray_activate(entry->id, 1);
            }
            shown++;
        }
        /* Legacy XEmbed icons live in the platform's host window, which the
           X server composites over this slot. */
        if(platform != NULL && platform->xembed_tray_count != NULL &&
           platform->xembed_tray_layout != NULL) {
            int embedded = platform->xembed_tray_count();
            if(embedded > 6)
                embedded = 6;
            if(embedded > 0) {
                Vector2 origin = {0, 0};
#if RILL_HAS_X11
                origin = PanelSurfaceOrigin();
#endif
                platform->xembed_tray_layout(x + 3 + shown * 22 + (int)origin.x,
                                             y + (int)origin.y, ph, 1);
                shown += embedded;
            }
        }
        if(shown == 0)
            draw_tray_indicator(x, plugin->variant,
                                plugin->variant == 0 ? GetThemeLink() :
                                (plugin->variant == 1 ? GetThemeIcon() :
                                 GetThemeButtonHover()), y, ph);
        return x + (plugin->advance > shown * 22 + 8 ? plugin->advance :
                    shown * 22 + 8);
    }
    case RILL_PANEL_LANGUAGE:
        Text((TextProps){
            .bounds = {x, y + oy + 7, 0, 0},
            .text = plugin->label, .font = Text12, .class_name = LabelPanelAccent,
            .wrap = TextWrapNone});
        return x + plugin->advance;
    case RILL_PANEL_CLOCK: {
        Rectangle bounds = {x, (float)y, (float)plugin->width, (float)ph};
        int hover = CheckCollisionPointRec(GetMousePosition(), bounds);
        if(hover)
            DrawRectangleRec((Rectangle){x, (float)y + 2,
                                         (float)plugin->width, (float)ph - 4},
                             panel_item_hover_color());
        draw_text_fit((TextProps){
            .bounds = {x, y + oy + 7, plugin->width, 0},
            .text = clock_text, .font = Text12, .class_name = LabelPanel,
            .wrap = TextWrapNone});
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            visuals->calendar_open = !visuals->calendar_open;
        return x + plugin->advance;
    }
    case RILL_PANEL_RESOURCE: {
        Color color = plugin->variant == 0 ? (Color){104, 190, 255, 255} :
                       (Color){86, 218, 154, 255};
        char label[48];
        int fill;
        if(visuals->battery_available) {
            int percent = visuals->battery_percent;
            Color charge = percent < 20 ? (Color){224, 82, 68, 255} :
                           percent < 55 ? (Color){222, 160, 62, 255} :
                           (Color){86, 218, 154, 255};
            snprintf(label, sizeof(label), "%s%d%%",
                     visuals->battery_charging ? "+" : "", percent);
            Text((TextProps){
                .bounds = {x, y + oy + 7, 0, 0},
                .text = label, .font = 11, .class_name = LabelPanel,
                .wrap = TextWrapNone});
            fill = 28 * percent / 100;
            DrawRectangle(x + 26, y + oy + 18, 28, 3, Fade(BLACK, 0.45f));
            DrawRectangle(x + 26, y + oy + 18, fill, 3, charge);
        } else
            draw_panel_resource(x, plugin->label, color, y, ph);
        return x + plugin->advance;
    }
    case RILL_PANEL_SHOW_DESKTOP: {
        Rectangle bounds = {x, (float)y + 2, 20, (float)ph - 4};
        int hover = CheckCollisionPointRec(GetMousePosition(), bounds);
        if(visuals->show_desktop_on || hover)
            DrawRectangleRec(bounds, hover ? panel_item_hover_color() :
                             panel_active_color());
        DrawRectangle((int)bounds.x + 4, (int)bounds.y + 4, 12, 9,
                      panel_text_color());
        DrawLine((int)bounds.x + 4, (int)bounds.y + 15,
                 (int)bounds.x + 15, (int)bounds.y + 15, panel_text_color());
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
           platform != NULL && platform->show_desktop != NULL) {
            visuals->show_desktop_on = !visuals->show_desktop_on;
            if(!platform->show_desktop(visuals->show_desktop_on))
                RillShellSetStatus(shell, "Show desktop unavailable");
        }
        return x + plugin->advance;
    }
    case RILL_PANEL_VOLUME: {
        Rectangle bounds = {x, (float)y + 2, (float)plugin->width,
                            (float)ph - 4};
        int hover = CheckCollisionPointRec(GetMousePosition(), bounds);
        char label[24];
        Color accent = visuals->volume_muted ?
                       (Color){224, 82, 68, 255} : panel_text_color();
        int cy = y + ph / 2;

        if(hover)
            DrawRectangleRec(bounds, panel_item_hover_color());
        /* Speaker glyph with sound waves (or a cross when muted). */
        DrawTriangle((Vector2){x + 6, (float)cy - 3},
                     (Vector2){x + 6, (float)cy + 3},
                     (Vector2){x + 11, (float)cy}, accent);
        DrawRectangle(x + 3, (float)cy - 2, 3, 4, accent);
        if(visuals->volume_muted) {
            DrawLine(x + 14, (float)cy - 4, x + 19, (float)cy + 4, accent);
            DrawLine(x + 19, (float)cy - 4, x + 14, (float)cy + 4, accent);
        } else {
            DrawCircleLines(x + 15, (float)cy, 3, accent);
            if(visuals->volume_percent > 50)
                DrawCircleLines(x + 15, (float)cy, 6, accent);
        }
        snprintf(label, sizeof(label), "%d%%", visuals->volume_percent);
        draw_text_fit((TextProps){
            .bounds = {x + 26, y + oy + 7, plugin->width - 30, 0},
            .text = label, .font = Text12, .class_name = visuals->volume_muted ? LabelWarning : LabelPanel,
            .wrap = TextWrapNone});
        if(hover && visuals->volume_available && platform != NULL &&
           platform->volume_set != NULL) {
            float wheel = GetMouseWheelMove();
            if(wheel != 0) {
                int next = visuals->volume_percent + (wheel > 0 ? 5 : -5);
                if(next < 0)
                    next = 0;
                if(next > 100)
                    next = 100;
                if(platform->volume_set(next, -1)) {
                    visuals->volume_percent = next;
                    visuals->volume_muted = next == 0;
                } else
                    RillShellSetStatus(shell, "Could not change the volume");
            } else if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if(platform->volume_set(-1, !visuals->volume_muted))
                    visuals->volume_muted = !visuals->volume_muted;
            }
        }
        return x + plugin->advance;
    }
    case RILL_PANEL_CLIPBOARD: {
        Rectangle bounds = {x, (float)y + 2, 22, (float)ph - 4};
        int hover = CheckCollisionPointRec(GetMousePosition(), bounds);
        int cy = y + ph / 2;

        if(hover)
            DrawRectangleRec(bounds, panel_item_hover_color());
        /* Clipboard glyph: two stacked sheets. */
        DrawRectangleLines((int)bounds.x + 4, (int)bounds.y + 4, 12, 15,
                           panel_text_color());
        DrawRectangle((int)bounds.x + 7, (int)bounds.y + 7, 12, 15,
                      panel_color());
        DrawRectangleLines((int)bounds.x + 7, (int)bounds.y + 7, 12, 15,
                           panel_text_color());
        if(visuals->clipboard_count > 0) {
            char label[8];
            snprintf(label, sizeof(label), "%d", visuals->clipboard_count);
            Text((TextProps){
                .bounds = {(int)bounds.x + 8, (int)bounds.y + 12, 0, 0},
                .text = label, .font = Text12, .class_name = LabelAccent,
                .wrap = TextWrapNone});
        }
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            visuals->clipboard_popup_open = !visuals->clipboard_popup_open;
        (void)cy;
        return x + plugin->advance;
    }
    case RILL_PANEL_ACTIONS: {
        Rectangle bounds = {x, (float)y + 2, 20, (float)ph - 4};
        int hover = CheckCollisionPointRec(GetMousePosition(), bounds);
        if(hover)
            DrawRectangleRec(bounds, panel_item_hover_color());
        draw_symbol_icon((Rectangle){x + 3, (float)(y + (ph - 16) / 2), 16, 16},
                         "power", GetThemeLink());
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            shell->menu_open = 0;
            visuals->logout_open = 1;
        }
        return x + plugin->advance;
    }
    default:
        return x;
    }
}

static int
draw_panel_item(const RillPanelPlugin *plugin, RillShellState *shell,
                 const RillPlatformServices *platform, RillVisualState *visuals,
                 int x, int right, int side, int index, const char *clock, int y, int height)
{
    Vector2 mouse = GetMousePosition();
    int shifting = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
#if RILL_HAS_X11
    PanelPointer pointer;
    if(PanelSurfacePointer(&pointer)) {
        mouse = pointer.position;
        shifting = pointer.shift;
        pressed = pointer.pressed;
    }
#endif
    int suppress = shifting || visuals->panel_drag_index >= 0;
    if(suppress) {
        KryonInputOverride input = {0};
        input.enabled = 1;
        input.mouse_inside = 1;
        input.mouse_position = mouse;
        input.mouse_delta = GetMouseDelta();
        input.pass_keyboard = 1;
        BeginKryonInputOverride(input);
    }
    int next = draw_panel_plugin(plugin, shell, platform, visuals, x, right, side, index,
                                  clock, y, height);
    if(suppress)
        EndKryonInputOverride();
    Rectangle bounds = {x, y, next > x ? next - x : 12, height};
    visuals->panel_item_bounds[side][index] = bounds;
    if(shifting && pressed && CheckCollisionPointRec(mouse, bounds)) {
        visuals->panel_drag_side = side;
        visuals->panel_drag_index = index;
        shell->menu_open = 0;
    }
    if(visuals->panel_drag_index >= 0 && CheckCollisionPointRec(mouse, bounds))
        DrawRectangleLinesEx(bounds, 2, GetThemeLink());
    return next;
}

static void
finish_panel_drag(RillVisualState *visuals)
{
    int released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    Vector2 mouse = GetMousePosition();
#if RILL_HAS_X11
    PanelPointer pointer;
    if(PanelSurfacePointer(&pointer)) {
        released = pointer.released;
        mouse = pointer.position;
    }
#endif
    if(visuals->panel_drag_index < 0 || !released)
        return;
    int from_side = visuals->panel_drag_side, from = visuals->panel_drag_index;
    visuals->panel_drag_index = -1;
    int source_count;
    RillPanelPlugin *source = panel_plugins_for_side(visuals, from_side, &source_count);
    if(from < 0 || from >= source_count)
        return;
    for(int side = 0; side < 2; side++) {
        int count;
        RillPanelPlugin *items = panel_plugins_for_side(visuals, side, &count);
        for(int to = 0; to < count; to++) {
            if(!CheckCollisionPointRec(mouse, visuals->panel_item_bounds[side][to]))
                continue;
            if(side != from_side && count >= RILL_PANEL_PLUGIN_MAX)
                return;
            RillPanelPlugin moving = source[from];
            memmove(source + from, source + from + 1, (size_t)(source_count - from - 1) * sizeof(*source));
            if(side == from_side) {
                memmove(items + to + 1, items + to, (size_t)(count - 1 - to) * sizeof(*items));
            } else {
                set_panel_count_for_side(visuals, from_side, source_count - 1);
                memmove(items + to + 1, items + to, (size_t)(count - to) * sizeof(*items));
                set_panel_count_for_side(visuals, side, count + 1);
            }
            items[to] = moving;
            visuals->panel_dirty = 1;
            return;
        }
    }
}

static void
draw_top_panel(RillShellState *shell, const RillPlatformServices *platform,
               RillVisualState *visuals)
{
    char clock_text[32];
    time_t now;
    struct tm *local;
    int x;
    int right;
    int screen_w;
    int i;
    int left_count;
    int right_count;
    int panel_y;
    int panel_h;
    const RillPanelPlugin *left_plugins;
    const RillPanelPlugin *right_plugins;

    screen_w = GetScreenWidth();
    panel_h = visuals->panel_height;
    if(visuals->panel_autohide) {
        Vector2 mouse = GetMousePosition();
        panel_y = rill_panel_top(visuals);
        int edge = visuals->panel_bottom ? mouse.y >= GetScreenHeight() - 4 :
                                           mouse.y <= 3;
        Rectangle bar = {0, (float)panel_y, (float)screen_w,
                         (float)panel_h};
        visuals->panel_hidden = !edge && !CheckCollisionPointRec(mouse, bar) &&
                                shell->menu_open == 0 && !visuals->calendar_open &&
                                visuals->panel_drag_index < 0;
        if(visuals->panel_hidden)
            panel_h = 3;
        panel_y = rill_panel_top(visuals);
    } else {
        visuals->panel_hidden = 0;
        panel_y = rill_panel_top(visuals);
    }
    include_panel_popup((Rectangle){0, panel_y, screen_w, panel_h});
    plan9_overlay_rect((Rectangle){0, panel_y, screen_w, panel_h});
    DrawRectangle(0, panel_y, screen_w, panel_h, panel_color());
    DrawRectangle(0, visuals->panel_bottom ? panel_y : panel_y + panel_h - 1,
                  screen_w, 1, Fade(BLACK, 0.72f));
    DrawRectangle(0, visuals->panel_bottom ? panel_y + panel_h - 1 : panel_y,
                  screen_w, 1, Fade(WHITE, 0.10f));
    if(visuals->panel_hidden) {
        /* Docked XEmbed icons are real windows and must leave with the bar. */
        if(platform != NULL && platform->xembed_tray_layout != NULL &&
           platform->xembed_tray_count != NULL && platform->xembed_tray_count())
            platform->xembed_tray_layout(0, 0, panel_h, 0);
        return;
    }

    now = time(NULL);
    local = localtime(&now);
    if(local != NULL)
        strftime(clock_text, sizeof(clock_text), visuals->clock_format, local);
    else
        snprintf(clock_text, sizeof(clock_text), "--:--");

    int right_width = 0;
    for(int item = 0; item < visuals->right_panel_count; item++) {
        int width = visuals->right_panel[item].advance;
        if(visuals->right_panel[item].kind == RILL_PANEL_TRAY) {
            int embedded = platform->xembed_tray_count ? platform->xembed_tray_count() : 0;
            if(embedded > 6) embedded = 6;
            int tray_width = ((visuals->tray_count > 6 ? 6 : visuals->tray_count) + embedded) * 22 + 8;
            if(width < tray_width) width = tray_width;
        }
        right_width += width;
    }
    right = screen_w - (screen_w >= 760 ? right_width + 12 : 72);
    x = 0;
    left_plugins = visuals->left_panel;
    left_count = visuals->left_panel_count;
    for(i = 0; i < left_count; i++)
        x = draw_panel_item(&left_plugins[i], shell, platform,
                              visuals, x, right, 0, i, clock_text,
                              panel_y, panel_h);

    if(screen_w < 760) {
        int oy = (panel_h - PANEL_H) / 2;
        if(screen_w > 70)
            draw_text_fit((TextProps){
                .bounds = {screen_w - 58, panel_y + oy + 7, 54, 0},
                .text = clock_text, .font = Text12, .class_name = LabelPanel,
                .wrap = TextWrapNone});
        if(CheckCollisionPointRec(GetMousePosition(),
                                  (Rectangle){0, (float)panel_y, (float)screen_w,
                                              (float)panel_h}) &&
           IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            open_panel_context(visuals, 1, -1);
        return;
    }

    x = screen_w - right_width - 6;
    draw_panel_separator(x - 6, panel_y, panel_h);
    right_plugins = visuals->right_panel;
    right_count = visuals->right_panel_count;
    for(i = 0; i < right_count; i++)
        x = draw_panel_item(&right_plugins[i], shell, platform,
                              visuals, x, right, 1, i, clock_text,
                              panel_y, panel_h);
    finish_panel_drag(visuals);
    if(CheckCollisionPointRec(GetMousePosition(),
                              (Rectangle){0, (float)panel_y, (float)screen_w,
                                          (float)panel_h}) &&
       IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) &&
       !visuals->panel_context_open)
        open_panel_context(visuals, 1, -1);
}

static void
draw_menu_panel(Rectangle menu)
{
    plan9_overlay_rect(menu);
    include_panel_popup(menu);
    DrawRectangleRounded(menu, 0.02f, 6, opaque_color(GetThemeSurface()));
    DrawRectangleRoundedLinesEx(menu, 0.02f, 6, 1.0f,
                                Fade(GetThemeText(), 0.30f));
}

static int
draw_menu_row(Rectangle row, const char *label, const char *icon_id)
{
    int hover;

    hover = CheckCollisionPointRec(GetMousePosition(), row);
    DrawRectangleRec(row, hover ? panel_item_hover_color() :
                     panel_item_color());
    DrawRectangle((int)row.x, (int)(row.y + row.height - 1), (int)row.width,
                  1, Fade(BLACK, 0.28f));
    if(icon_id != NULL)
        draw_symbol_icon((Rectangle){row.x + 6, row.y + 6, 16, 16}, icon_id,
                         GetThemeLink());
    draw_text_fit((TextProps){
        .bounds = {(int)row.x + 30, (int)row.y + 8, (int)row.width - 38, 0},
        .text = label, .font = Text12, .class_name = LabelPanel,
        .wrap = TextWrapNone});
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static int
ascii_fold(int c)
{
    if(c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

static int
ascii_contains_fold(const char *haystack, const char *needle)
{
    int h;
    int n;

    if(needle == NULL || needle[0] == '\0')
        return 1;
    if(haystack == NULL)
        return 0;
    for(h = 0; haystack[h] != '\0'; h++) {
        for(n = 0; needle[n] != '\0'; n++) {
            if(haystack[h + n] == '\0' ||
               ascii_fold((unsigned char)haystack[h + n]) !=
               ascii_fold((unsigned char)needle[n]))
                break;
        }
        if(needle[n] == '\0')
            return 1;
    }
    return 0;
}

static int
launcher_is_recent(const RillShellState *shell, const RillLauncher *launcher)
{
    if(shell == NULL || launcher == NULL)
        return 0;
    for(int i = 0; i < shell->recent_launcher_count; i++)
        if(strcmp(shell->recent_launcher_ids[i], launcher->id) == 0)
            return 1;
    return 0;
}

static int
launcher_in_app_menu_category(const RillShellState *shell,
                              const RillLauncher *launcher, int category)
{
    if(launcher == NULL)
        return 0;
    if(category <= 0)
        return launcher->favorite != 0;
    if(category == 1)
        return launcher_is_recent(shell, launcher);
    if(category == 2)
        return 1;
    if(category >= 0 &&
       category < (int)(sizeof(rill_menu_categories) /
                        sizeof(rill_menu_categories[0])))
        return strcmp(launcher->category,
                      rill_menu_categories[category].name) == 0;
    return 0;
}

static int
launcher_matches_app_menu(const RillShellState *shell,
                          const RillLauncher *launcher)
{
    if(shell == NULL || launcher == NULL)
        return 0;
    if(shell->app_menu_search[0] != '\0')
        return ascii_contains_fold(launcher->name, shell->app_menu_search) ||
               ascii_contains_fold(launcher->description,
                                   shell->app_menu_search) ||
               ascii_contains_fold(launcher->category, shell->app_menu_search);
    return launcher_in_app_menu_category(shell, launcher,
                                         shell->app_menu_category);
}

static const char *
rill_user_name(void)
{
    const char *user;

    user = getenv("USER");
    if(user != NULL && user[0] != '\0')
        return user;
    user = getenv("user");
    if(user != NULL && user[0] != '\0')
        return user;
    return "glenda";
}

static void
update_app_menu_search_input(RillShellState *shell)
{
    int c;
    int len;

    if(shell == NULL || shell->menu_open != 1 || !shell->app_menu_search_active)
        return;
    while((c = GetCharPressed()) > 0) {
        len = (int)strlen(shell->app_menu_search);
        if(c >= 32 && c < 127 && len < RILL_APP_MENU_SEARCH_MAX - 1) {
            shell->app_menu_search[len] = (char)c;
            shell->app_menu_search[len + 1] = '\0';
        }
    }
    if(IsKeyPressed(KEY_BACKSPACE)) {
        len = (int)strlen(shell->app_menu_search);
        if(len > 0)
            shell->app_menu_search[len - 1] = '\0';
    }
    if(IsKeyPressed(KEY_ESCAPE)) {
        shell->app_menu_search[0] = '\0';
        shell->app_menu_search_active = 0;
    }
}

static void
draw_search_mark(Rectangle r, Color color)
{
    float cx = r.x + r.width * 0.42f;
    float cy = r.y + r.height * 0.42f;

    DrawCircleLines((int)cx, (int)cy, r.width * 0.22f, color);
    DrawLine((int)(cx + r.width * 0.16f), (int)(cy + r.height * 0.16f),
             (int)(r.x + r.width - 4), (int)(r.y + r.height - 4), color);
}

static void
draw_whisker_header(Rectangle menu, RillShellState *shell,
                    const RillPlatformServices *platform,
                    RillVisualState *visuals)
{
    Rectangle user_icon = {menu.x + 12, menu.y + 11, 30, 30};
    Rectangle search = {menu.x + 10, menu.y + 50, menu.width - 20, 30};
    Vector2 mouse = GetMousePosition();
    int hover;

    DrawCircle((int)(user_icon.x + 15), (int)(user_icon.y + 15), 15,
               GetThemeButtonHover());
    DrawCircle((int)(user_icon.x + 15), (int)(user_icon.y + 11), 5,
               Fade(WHITE, 0.88f));
    DrawCircle((int)(user_icon.x + 15), (int)(user_icon.y + 26), 10,
               Fade(WHITE, 0.35f));
    draw_text_fit((TextProps){
        .bounds = {(int)menu.x + 50, (int)menu.y + 18, (int)menu.width - 150, 0},
        .text = rill_user_name(), .font = Text16, .class_name = LabelPrimary,
        .wrap = TextWrapNone});

    draw_symbol_icon((Rectangle){menu.x + menu.width - 86, menu.y + 14,
                                 22, 22}, "settings", GetThemeButtonHover());
    draw_symbol_icon((Rectangle){menu.x + menu.width - 54, menu.y + 14,
                                 22, 22}, "power", GetThemeLink());
    draw_symbol_icon((Rectangle){menu.x + menu.width - 25, menu.y + 14,
                                 20, 20}, "about", GetThemeIcon());
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if(CheckCollisionPointRec(mouse, (Rectangle){menu.x + menu.width - 88,
                                                     menu.y + 12, 26, 26})) {
            open_launcher_id(shell, platform, "settings");
            shell->menu_open = 0;
        } else if(CheckCollisionPointRec(mouse, (Rectangle){menu.x + menu.width - 56,
                                                            menu.y + 12, 26, 26})) {
            shell->menu_open = 0;
            if(visuals != NULL)
                visuals->logout_open = 1;
        } else if(CheckCollisionPointRec(mouse, (Rectangle){menu.x + menu.width - 28,
                                                            menu.y + 12, 24, 24})) {
            open_launcher_id(shell, platform, "about");
            shell->menu_open = 0;
        }
    }

    hover = CheckCollisionPointRec(GetMousePosition(), search);
    DrawRectangleRounded(search, 0.04f, 5, Fade(BLACK, 0.20f));
    DrawRectangleRoundedLinesEx(search, 0.04f, 5, 1.0f,
                                shell->app_menu_search_active ?
                                GetThemeButtonHover() :
                                (hover ? GetThemeLink() :
                                 Fade(GetThemeText(), 0.38f)));
    draw_search_mark((Rectangle){search.x + 8, search.y + 7, 16, 16},
                     GetThemeIcon());
    if(shell->app_menu_search[0] != '\0')
        draw_text_fit((TextProps){
            .bounds = {(int)search.x + 30, (int)search.y + 8, (int)search.width - 38, 0},
            .text = shell->app_menu_search, .font = Text12, .class_name = LabelPrimary,
            .wrap = TextWrapNone});
    if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        shell->app_menu_search_active = 1;
}

static int
draw_whisker_category_row(Rectangle row, const RillMenuCategory *category,
                          int active)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), row);
    Color icon = active ? WHITE : GetThemeButtonHover();
    int icon_size = (int)row.height - 6;
    int text_y = (int)(row.y + (row.height - 12) * 0.5f);

    if(icon_size > 20)
        icon_size = 20;
    if(icon_size < 12)
        icon_size = 12;

    if(active)
        DrawRectangleRounded(row, 0.02f, 4, panel_active_color());
    else if(hover)
        DrawRectangleRec(row, panel_item_hover_color());
    draw_symbol_icon((Rectangle){row.x + 6, row.y + (row.height - icon_size) * 0.5f,
                                 icon_size, icon_size},
                     category->icon_id, icon);
    draw_text_fit((TextProps){
        .bounds = {(int)row.x + 32, text_y, (int)row.width - 38, 0},
        .text = category->name, .font = Text12, .class_name = active || hover ? LabelPanel : LabelPrimary,
        .wrap = TextWrapNone});
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static int
draw_whisker_launcher_row(RillVisualState *visuals,
                          const RillLauncher *launcher, Rectangle row)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), row);

    if(hover)
        DrawRectangleRounded(row, 0.02f, 4, panel_item_hover_color());
    draw_launcher_icon(visuals, launcher,
                       (Rectangle){row.x + 8, row.y + 6, 30, 30},
                       GetThemeLink());
    draw_text_fit((TextProps){
        .bounds = {(int)row.x + 48, (int)row.y + 7, (int)row.width - 56, 0},
        .text = launcher->name, .font = Text14, .class_name = hover ? LabelPanel : LabelPrimary,
        .wrap = TextWrapNone});
    draw_text_fit((TextProps){
        .bounds = {(int)row.x + 48, (int)row.y + 25, (int)row.width - 56, 0},
        .text = launcher->description[0] != '\0' ?
                  launcher->description : launcher->category, .font = Text12, .class_name = hover ? LabelPanelDim : LabelMuted,
        .wrap = TextWrapNone});
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static int
draw_window_close_button(Rectangle close)
{
    int hover;
    Color stroke;

    hover = CheckCollisionPointRec(GetMousePosition(), close);
    DrawRectangleRec(close, hover ? panel_active_color() : panel_item_color());
    DrawRectangleLinesEx(close, 1.0f, Fade(GetThemeText(), 0.36f));
    stroke = hover ? WHITE : GetThemeText();
    DrawLine((int)close.x + 7, (int)close.y + 7,
             (int)close.x + (int)close.width - 7,
             (int)close.y + (int)close.height - 7, stroke);
    DrawLine((int)close.x + (int)close.width - 7, (int)close.y + 7,
             (int)close.x + 7,
             (int)close.y + (int)close.height - 7, stroke);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void
draw_applications_menu(RillShellState *shell,
                       const RillPlatformServices *platform,
                       RillVisualState *visuals)
{
    Rectangle menu;
    Rectangle app_area;
    Rectangle category_area;
    int i;
    int y;
    int matches;
    int category_count;
    int screen_w;
    int screen_h;
    int menu_w;
    int menu_h;
    int category_w;
    int category_step;

    if(shell->menu_open != 1)
        return;

    update_app_menu_search_input(shell);
    category_count = (int)(sizeof(rill_menu_categories) /
                           sizeof(rill_menu_categories[0]));
    if(shell->app_menu_category < 0 ||
       shell->app_menu_category >= category_count)
        shell->app_menu_category = 0;

    screen_w = GetScreenWidth();
    screen_h = GetScreenHeight();
    menu_w = screen_w < 456 ? screen_w - 8 : 440;
    if(menu_w < 320)
        menu_w = screen_w - 8;
    menu_h = screen_h - rill_panel_visible_height(visuals) - 10;
    if(menu_h > 430)
        menu_h = 430;
    if(menu_h < 300)
        menu_h = screen_h - rill_panel_visible_height(visuals) - 4;
    category_w = menu_w >= 400 ? 128 : 112;
    menu = (Rectangle){4, (float)rill_menu_anchor_y(visuals, menu_h), menu_w,
                       (float)menu_h};
    draw_menu_panel(menu);

    draw_whisker_header(menu, shell, platform, visuals);

    category_area = (Rectangle){menu.x + menu.width - category_w - 6,
                                menu.y + 88, category_w,
                                menu.height - 96};
    app_area = (Rectangle){menu.x + 8, menu.y + 88,
                           menu.width - category_w - 18,
                           category_area.height};
    category_step = (int)(category_area.height / category_count);
    if(category_step > 30)
        category_step = 30;
    if(category_step < 18)
        category_step = 18;

    DrawRectangle((int)(category_area.x - 7), (int)app_area.y, 1,
                  (int)app_area.height, Fade(BLACK, 0.45f));
    DrawRectangle((int)(category_area.x - 6), (int)app_area.y, 1,
                  (int)app_area.height, Fade(WHITE, 0.12f));

    BeginScissorMode((int)app_area.x, (int)app_area.y,
                     (int)app_area.width, (int)app_area.height);
    y = (int)app_area.y;
    matches = 0;
    for(i = 0; i < shell->launcher_count; i++) {
        Rectangle row;

        if(!launcher_matches_app_menu(shell, &shell->launchers[i]))
            continue;
        row = (Rectangle){app_area.x, y, app_area.width, 44};
        matches++;
        if(draw_whisker_launcher_row(visuals, &shell->launchers[i], row)) {
            RillShellSelectLauncher(shell, i);
            RillShellLaunchSelected(shell, platform);
            shell->menu_open = 0;
            shell->app_menu_search_active = 0;
        }
        y += 48;
    }
    if(matches == 0) {
        const char *message = shell->app_menu_search[0] != '\0' ?
                              "No matching applications" :
                              "No applications in this category";
        draw_text_fit((TextProps){
            .bounds = {(int)app_area.x + 8, (int)app_area.y + 10, (int)app_area.width - 16, 0},
            .text = message, .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
    }
    EndScissorMode();

    BeginScissorMode((int)category_area.x, (int)category_area.y,
                     (int)category_area.width, (int)category_area.height);
    y = (int)category_area.y;
    for(i = 0; i < category_count; i++) {
        Rectangle row = {category_area.x, y, category_area.width,
                         category_step - 2};
        if(draw_whisker_category_row(row, &rill_menu_categories[i],
                                     i == shell->app_menu_category)) {
            shell->app_menu_category = i;
            shell->app_menu_search[0] = '\0';
        }
        y += category_step;
    }
    EndScissorMode();

    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), menu)) {
        shell->menu_open = 0;
        shell->app_menu_search_active = 0;
    }
}

static void
draw_task_icon(RillVisualState *visuals, RillShellState *shell,
               const RillTask *task, Rectangle icon)
{
    const RillLauncher *launcher = NULL;

    if(task != NULL) {
        if(task->icon_path[0] != '\0') {
            Texture2D *texture = visual_icon_texture(visuals, task->icon_path);

            if(texture != NULL) {
                draw_texture_icon(texture, icon);
                return;
            }
        }
        for(int i = 0; i < shell->app_count; i++) {
            if(!task->platform_owned && shell->apps[i].id == task->id) {
                if(shell->apps[i].kind == RILL_APP_TERMINAL)
                    launcher = launcher_by_id(shell, "terminal");
                else if(shell->apps[i].kind == RILL_APP_FILES)
                    launcher = launcher_by_id(shell, "files");
                else if(shell->apps[i].kind == RILL_APP_SETTINGS)
                    launcher = launcher_by_id(shell, "settings");
                break;
            }
        }
    }
    draw_launcher_icon(visuals, launcher, icon, GetThemeText());
}

static void
draw_places_menu(RillShellState *shell, const RillPlatformServices *platform,
                 RillVisualState *visuals)
{
    Rectangle menu;
    const char *items[] = {"Home", "Desktop", "File System"};
    int y;

    if(shell->menu_open != 2)
        return;
    menu = (Rectangle){116, (float)rill_menu_anchor_y(visuals, 106), 190, 106};
    draw_menu_panel(menu);
    y = (int)menu.y + 6;
    for(int i = 0; i < 3; i++) {
        if(draw_menu_row((Rectangle){122, (float)y, 178, 28}, items[i], "files")) {
            open_launcher_id(shell, platform, "files");
            shell->menu_open = 0;
        }
        y += 32;
    }
}

static int draw_settings_button(Rectangle bounds, const char *label, int active);

static void
draw_system_menu(RillShellState *shell, const RillPlatformServices *platform,
                 RillVisualState *visuals)
{
    Rectangle menu;
    int y;

    if(shell->menu_open != 3)
        return;
    menu = (Rectangle){182, (float)rill_menu_anchor_y(visuals, 104), 190, 104};
    draw_menu_panel(menu);
    y = (int)menu.y + 6;
    if(draw_menu_row((Rectangle){188, (float)y, 178, 28}, "Settings",
                     "settings")) {
        open_launcher_id(shell, platform, "settings");
        shell->menu_open = 0;
    }
    if(draw_menu_row((Rectangle){188, (float)y + 32, 178, 28}, "About Rill",
                     "about")) {
        open_launcher_id(shell, platform, "about");
        shell->menu_open = 0;
    }
    if(draw_menu_row((Rectangle){188, (float)y + 64, 178, 28}, "Log Out",
                     "power")) {
        shell->menu_open = 0;
        if(visuals != NULL)
            visuals->logout_open = 1;
    }
}

static void
clamp_menu_origin(RillVisualState *visuals, int *x, int *y, int width,
                  int height)
{
    if(*x + width > GetScreenWidth())
        *x = GetScreenWidth() - width;
    if(*y + height > GetScreenHeight())
        *y = GetScreenHeight() - height;
    if(visuals != NULL && !visuals->panel_bottom &&
       *y < rill_panel_visible_height(visuals) + 2)
        *y = rill_panel_visible_height(visuals) + 2;
    if(*x < 2)
        *x = 2;
    if(*y < 2)
        *y = 2;
}

static void
draw_desktop_context_menu(RillShellState *shell,
                          const RillPlatformServices *platform,
                          RillVisualState *visuals)
{
    if(shell->menu_open != 4)
        return;
    const RillLauncher *item = visuals->desktop_selected >= 0 &&
        visuals->desktop_selected < visuals->desktop_entry_count ?
        visuals->desktop_entries[visuals->desktop_selected] : NULL;
    int editable = item != NULL && item->file_path[0] && platform->file_operation != NULL;
    int x = visuals->desktop_menu_x, y = visuals->desktop_menu_y;
    int height = item != NULL ? (editable ? 148 : 52) : 276;
    clamp_menu_origin(visuals, &x, &y, 230, height);
    Rectangle menu = {(float)x, (float)y, 230, (float)height};
    draw_menu_panel(menu);
    if(item != NULL) {
        if(draw_menu_row((Rectangle){x + 6, y + 6, 218, 28}, "Open", "files")) {
            open_desktop_launcher(shell, platform, item);
            shell->menu_open = 0;
        }
        if(editable) {
            if(draw_menu_row((Rectangle){x + 6, y + 38, 218, 28}, "Rename...", "files")) {
                begin_file_action(visuals, "rename", item->file_path);
                shell->menu_open = 0;
            }
            if(draw_menu_row((Rectangle){x + 6, y + 70, 218, 28}, "Move to Trash...", "trash")) {
                begin_file_action(visuals, "trash", item->file_path);
                shell->menu_open = 0;
            }
            if(draw_menu_row((Rectangle){x + 6, y + 102, 218, 28}, "Open Desktop Folder", "files")) {
                if(platform->desktop_directory && platform->open_path)
                    platform->open_path(platform->desktop_directory());
                shell->menu_open = 0;
            }
        }
    } else {
        const char *labels[] = {"Applications", "Terminal", "Open Desktop Folder", "New Folder...",
                                "Arrange Icons", "Refresh", "Settings", "Log Out"};
        const char *icons[] = {"all", "terminal", "files", "files", "all", "all", "settings", "power"};
        for(int i = 0; i < 8; i++) {
            if(!draw_menu_row((Rectangle){x + 6, y + 6 + i * 32, 218, 28}, labels[i], icons[i]))
                continue;
            shell->menu_open = 0;
            switch(i) {
            case 0: shell->menu_open = 1; shell->app_menu_search_active = 1; break;
            case 1: open_launcher_id(shell, platform, "terminal"); break;
            case 2:
                if(platform->desktop_directory && platform->open_path)
                    platform->open_path(platform->desktop_directory());
                break;
            case 3:
                if(platform->desktop_directory && platform->file_operation)
                    begin_file_action(visuals, "mkdir", platform->desktop_directory());
                break;
            case 4:
                RillSettingsInit(&visuals->desktop_layout);
                RillSettingsSave(&visuals->desktop_layout, visuals->desktop_layout_path);
                break;
            case 5: visuals->desktop_files_scanned = 0; break;
            case 6: open_launcher_id(shell, platform, "settings"); break;
            case 7: visuals->logout_open = 1; break;
            }
        }
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), menu))
        shell->menu_open = 0;
}

static void
draw_file_dialog(RillShellState *shell, RillVisualState *visuals,
                  const RillPlatformServices *platform)
{
    if(!visuals->file_action[0])
        return;
    int trash = strcmp(visuals->file_action, "trash") == 0;
    Rectangle panel = {(GetScreenWidth() - 480) / 2.0f,
                        (GetScreenHeight() - 200) / 2.0f, 480, 200};
    draw_menu_panel(panel);
    const char *title = trash ? "Move this item to Trash?" :
                        strcmp(visuals->file_action, "rename") == 0 ? "Rename" : "New Folder";
    Text((TextProps){
        .bounds = {panel.x + 16, panel.y + 14, 0, 0},
        .text = title, .font = Text18, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    draw_text_fit((TextProps){
        .bounds = {panel.x + 16, panel.y + 44, 448, 0},
        .text = visuals->file_source, .font = Text12, .class_name = LabelMuted,
        .wrap = TextWrapNone});
    int commit = 0;
    if(!trash)
        TextField((TextFieldProps){.bounds = {panel.x + 16, panel.y + 68, 448, 32},
                                  .text = visuals->file_name, .text_size = sizeof(visuals->file_name),
                                  .cursor_position = &visuals->file_cursor, .focused = &visuals->file_focused,
                                  .focus_id = 9701, .commit_pressed = &commit});
    draw_text_fit((TextProps){
        .bounds = {panel.x + 16, panel.y + 110, 448, 0},
        .text = visuals->file_error, .font = Text12, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    if(draw_menu_row((Rectangle){panel.x + 264, panel.y + 154, 96, 30}, "Cancel", "") ||
       IsKeyPressed(KEY_ESCAPE)) {
        visuals->file_action[0] = '\0';
        return;
    }
    if(draw_menu_row((Rectangle){panel.x + 366, panel.y + 154, 96, 30},
                     trash ? "Trash" : "Save", "") || commit) {
        if(platform->file_operation &&
           platform->file_operation(visuals->file_action, visuals->file_source,
                                     visuals->file_name, visuals->file_error,
                                     sizeof(visuals->file_error))) {
            visuals->file_action[0] = '\0';
            visuals->desktop_files_scanned = 0;
            RillShellSetStatus(shell, "Desktop updated");
        }
    }
}

static void
draw_window_list_menu(RillShellState *shell,
                      const RillPlatformServices *platform,
                      RillVisualState *visuals)
{
    Rectangle menu;
    int x;
    int y;
    int rows;
    int i;

    if(shell->menu_open != 5)
        return;
    rows = shell->task_count > 8 ? 8 : (shell->task_count > 0 ?
                                        shell->task_count : 1);
    x = visuals->desktop_menu_x;
    y = visuals->desktop_menu_y;
    clamp_menu_origin(visuals, &x, &y, 250, rows * 30 + 14);
    menu = (Rectangle){(float)x, (float)y, 250, (float)(rows * 30 + 14)};
    draw_menu_panel(menu);
    if(shell->task_count == 0) {
        draw_text_fit((TextProps){
            .bounds = {x + 12, y + 10, 226, 0},
            .text = "No windows", .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        return;
    }
    for(i = 0; i < rows; i++) {
        Rectangle row = {x + 6, (float)(y + 6 + i * 30), 238, 28};
        int task_index = i;
        if(CheckCollisionPointRec(GetMousePosition(), row))
            DrawRectangleRec(row, panel_item_hover_color());
        draw_task_icon(visuals, shell, &shell->tasks[task_index],
                       (Rectangle){row.x + 4, row.y + 5, 16, 16});
        draw_text_fit((TextProps){
            .bounds = {(int)row.x + 26, (int)row.y + 8, (int)row.width - 34, 0},
            .text = shell->tasks[task_index].title, .font = Text12, .class_name = CheckCollisionPointRec(GetMousePosition(), row) ? LabelPanel : LabelPrimary,
            .wrap = TextWrapNone});
        if(CheckCollisionPointRec(GetMousePosition(), row) &&
           IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            RillShellSelectTask(shell, task_index);
            RillShellFocusSelectedTask(shell, platform);
            shell->menu_open = 0;
        }
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), menu))
        shell->menu_open = 0;
}

static void
draw_panel_properties(RillShellState *shell, RillVisualState *visuals)
{
    RillPanelPlugin *plugins;
    int count;
    RillPanelPlugin *plugin;
    Rectangle full = {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()};
    Rectangle panel;
    char sample[32];

    if(visuals == NULL || !visuals->properties_open)
        return;
    plugins = panel_plugins_for_side(visuals, visuals->properties_side, &count);
    if(plugins == NULL || visuals->properties_index < 0 ||
       visuals->properties_index >= count) {
        visuals->properties_open = 0;
        return;
    }
    plugin = &plugins[visuals->properties_index];
    panel = (Rectangle){(GetScreenWidth() - 264) / 2.0f,
                        (GetScreenHeight() - 132) / 2.0f, 264, 132};
    include_panel_popup(full);
    DrawRectangleRec(full, Fade(BLACK, 0.30f));
    DrawRectangleRounded(panel, 0.03f, 8, opaque_color(GetThemeSurface()));
    DrawRectangleRoundedLinesEx(panel, 0.03f, 8, 2.0f, GetThemeLink());
    Text((TextProps){
        .bounds = {(int)panel.x + 14, (int)panel.y + 12, 0, 0},
        .text = "Panel item", .font = Text16, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    draw_text_fit((TextProps){
        .bounds = {(int)panel.x + 14, (int)panel.y + 36, (int)panel.width - 28, 0},
        .text = RillPanelPluginKindName(plugin->kind), .font = Text12, .class_name = LabelMuted,
        .wrap = TextWrapNone});

    Text((TextProps){
        .bounds = {(int)panel.x + 14, (int)panel.y + 64, 0, 0},
        .text = "Width", .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    if(draw_settings_button((Rectangle){panel.x + 150, panel.y + 60, 26, 24},
                            "-", 0) && plugin->width > 8) {
        plugin->width -= 2;
        plugin->advance = plugin->advance > 2 ? plugin->advance - 2 : 0;
        visuals->panel_dirty = 1;
    }
    snprintf(sample, sizeof(sample), "%d", plugin->width);
    draw_text_fit((TextProps){
        .bounds = {(int)panel.x + 184, (int)panel.y + 64, 34, 0},
        .text = sample, .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    if(draw_settings_button((Rectangle){panel.x + 222, panel.y + 60, 26, 24},
                            "+", 0) && plugin->width < 600) {
        plugin->width += 2;
        plugin->advance += 2;
        visuals->panel_dirty = 1;
    }
    if(draw_settings_button((Rectangle){panel.x + 14, panel.y + 94, 100, 26},
                            "Done", 0)) {
        visuals->properties_open = 0;
        RillShellSetStatus(shell, "Panel item updated");
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), panel))
        visuals->properties_open = 0;
}

static void
draw_calendar_popup(RillVisualState *visuals)
{
    const char *weekdays[7] = {"S", "M", "T", "W", "T", "F", "S"};
    struct tm day;
    time_t now;
    struct tm *local;
    Rectangle menu;
    int width = 216;
    int height = 190;
    int x;
    int y;
    int first;
    int days;
    int today;

    if(visuals == NULL || !visuals->calendar_open)
        return;
    now = time(NULL);
    local = localtime(&now);
    if(local == NULL)
        return;
    x = GetScreenWidth() - width - 8;
    y = rill_menu_anchor_y(visuals, height);
    menu = (Rectangle){(float)x, (float)y, (float)width, (float)height};
    draw_menu_panel(menu);

    char month[48];
    strftime(month, sizeof(month), "%B %Y", local);
    Text((TextProps){
        .bounds = {(int)menu.x + 14, (int)menu.y + 10, 0, 0},
        .text = month, .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    for(int i = 0; i < 7; i++)
        Text((TextProps){
            .bounds = {(int)menu.x + 16 + i * 28, (int)menu.y + 34, 0, 0},
            .text = weekdays[i], .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});

    memset(&day, 0, sizeof(day));
    day.tm_year = local->tm_year;
    day.tm_mon = local->tm_mon;
    day.tm_mday = 1;
    mktime(&day);
    first = day.tm_wday;
    days = 31;
    if(local->tm_mon == 1)
        days = day.tm_year % 4 == 0 && (day.tm_year % 100 != 0 ||
                                        day.tm_year % 400 == 0) ? 29 : 28;
    else if(local->tm_mon == 3 || local->tm_mon == 5 || local->tm_mon == 8 ||
            local->tm_mon == 10)
        days = 30;
    today = local->tm_mday;
    for(int d = 1; d <= days; d++) {
        char label[8];
        int cell = first + d - 1;
        Rectangle cell_rect = {menu.x + 12 + (cell % 7) * 28,
                               menu.y + 50 + (cell / 7) * 22, 26, 20};
        snprintf(label, sizeof(label), "%d", d);
        if(d == today)
            DrawRectangleRec(cell_rect, panel_active_color());
        else if(CheckCollisionPointRec(GetMousePosition(), cell_rect))
            DrawRectangleRec(cell_rect, panel_item_hover_color());
        Text((TextProps){
            .bounds = {(int)cell_rect.x + 8, (int)cell_rect.y + 4, 0, 0},
            .text = label, .font = Text12, .class_name = d == today ? LabelWhite : LabelPrimary,
            .wrap = TextWrapNone});
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), menu))
        visuals->calendar_open = 0;
}

static void
draw_clipboard_popup(RillShellState *shell, RillVisualState *visuals,
                     const RillPlatformServices *platform)
{
    Rectangle menu;
    int rows = visuals->clipboard_count > 8 ? 8 : visuals->clipboard_count;
    int x;
    int y;
    int i;

    if(visuals == NULL || !visuals->clipboard_popup_open)
        return;
    if(rows == 0)
        rows = 1;
    x = GetScreenWidth() - 280;
    if(x < 4)
        x = 4;
    y = rill_menu_anchor_y(visuals, rows * 28 + 14);
    menu = (Rectangle){(float)x, (float)y, 272, (float)(rows * 28 + 14)};
    draw_menu_panel(menu);
    if(visuals->clipboard_count == 0) {
        draw_text_fit((TextProps){
            .bounds = {x + 12, y + 10, 248, 0},
            .text = "Clipboard is empty", .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        return;
    }
    for(i = 0; i < rows; i++) {
        Rectangle row = {x + 6, (float)(y + 6 + i * 28), 260, 26};
        if(CheckCollisionPointRec(GetMousePosition(), row))
            DrawRectangleRec(row, panel_item_hover_color());
        draw_text_fit((TextProps){
            .bounds = {(int)row.x + 10, (int)row.y + 6, (int)row.width - 20, 0},
            .text = visuals->clipboard_texts[i], .font = Text12, .class_name = LabelPanel,
            .wrap = TextWrapNone});
        if(CheckCollisionPointRec(GetMousePosition(), row) &&
           IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if(platform != NULL && platform->clipboard_select != NULL &&
               platform->clipboard_select(i)) {
                char status[96];
                snprintf(status, sizeof(status), "Copied from history: %.40s",
                         visuals->clipboard_texts[i]);
                RillShellSetStatus(shell, status);
            }
            visuals->clipboard_popup_open = 0;
            return;
        }
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), menu))
        visuals->clipboard_popup_open = 0;
}

static void
draw_notifications(RillShellState *shell, RillVisualState *visuals,
                   const RillPlatformServices *platform)
{
    int width = 320;
    int height = 62;
    int x;
    int y;
    int step;
    Rectangle banner;

    if(visuals == NULL || visuals->notification_count <= 0)
        return;
    x = GetScreenWidth() - width - 10;
    y = rill_menu_anchor_y(visuals, 0);
    if(visuals->panel_bottom)
        y = rill_panel_top(visuals) - 10;
    step = visuals->panel_bottom ? -(height + 8) : height + 8;
    for(int i = 0; i < visuals->notification_count && i < 4; i++) {
        RillNotification *note = &visuals->notifications[i];
        int clicked;

        banner = (Rectangle){(float)x, (float)(y + i * step), (float)width,
                             (float)height};
        include_panel_popup(banner);
        DrawRectangleRec(banner, opaque_color(GetThemeSurface()));
        DrawRectangleRounded(banner, 0.05f, 6, opaque_color(GetThemeSurface()));
        DrawRectangleRoundedLinesEx(banner, 0.05f, 6, 1.0f,
                                    Fade(GetThemeLink(), 0.55f));
        DrawRectangle(x, (int)banner.y + 6, 3, height - 12, GetThemeLink());
        draw_text_fit((TextProps){
            .bounds = {x + 12, (int)banner.y + 8, width - 24, 0},
            .text = note->summary, .font = Text14, .class_name = LabelPrimary,
            .wrap = TextWrapNone});
        draw_text_fit((TextProps){
            .bounds = {x + 12, (int)banner.y + 28, width - 24, 0},
            .text = note->body[0] != '\0' ? note->body : note->app_name, .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        clicked = CheckCollisionPointRec(GetMousePosition(), banner) &&
                  IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        if(clicked && platform != NULL &&
           platform->notification_action != NULL) {
            platform->notification_action(note->id, 0);
            RillShellSetStatus(shell, note->summary);
        }
    }
}

static void
draw_logout_dialog(RillShellState *shell, RillVisualState *visuals,
                   const RillPlatformServices *platform)
{
    const char *labels[6] = {"Lock", "Log out", "Restart", "Shut down",
                             "Suspend", "Cancel"};
    const char *actions[5] = {"lock", "logout", "restart", "shutdown",
                              "suspend"};
    Rectangle full = {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()};
    Rectangle panel;
    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    int i;

    if(visuals == NULL || !visuals->logout_open)
        return;
    panel = (Rectangle){(screen_w - 280) / 2.0f, (screen_h - 246) / 2.0f,
                        280, 246};
    include_panel_popup(full);
    plan9_overlay_rect(full);
    DrawRectangleRec(full, Fade(BLACK, 0.38f));
    DrawRectangleRounded(panel, 0.03f, 8, opaque_color(GetThemeSurface()));
    DrawRectangleRoundedLinesEx(panel, 0.03f, 8, 2.0f, GetThemeLink());
    Text((TextProps){
        .bounds = {(int)panel.x + 16, (int)panel.y + 14, 0, 0},
        .text = "End session", .font = Text18, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    for(i = 0; i < 6; i++) {
        Rectangle button = {panel.x + 16, panel.y + 46 + i * 32,
                            panel.width - 32, 28};
        if(draw_settings_button(button, labels[i], 0)) {
            if(i < 5) {
                if(platform != NULL && platform->session_action != NULL &&
                   platform->session_action(actions[i]))
                    RillShellSetStatus(shell, labels[i]);
                else
                    RillShellSetStatus(shell, "Session action unavailable");
            }
            visuals->logout_open = 0;
            return;
        }
    }
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       !CheckCollisionPointRec(GetMousePosition(), panel))
        visuals->logout_open = 0;
}

static int
find_app_index_by_id(RillShellState *shell, int app_id)
{
    if(shell == NULL)
        return -1;
    for(int i = 0; i < shell->app_count; i++)
        if(shell->apps[i].id == app_id)
            return i;
    return -1;
}

static void
clamp_window_to_screen(RillAppWindow *app)
{
    int max_x;
    int max_y;

    if(app == NULL)
        return;
    max_x = GetScreenWidth() - 48;
    max_y = GetScreenHeight() - 48;
    if(app->x < 0)
        app->x = 0;
    if(app->y < 0)
        app->y = 0;
    if(app->x > max_x)
        app->x = max_x;
    if(app->y > max_y)
        app->y = max_y;
}

static void
process_window_mouse(RillShellState *shell)
{
    Vector2 mouse;

    if(shell == NULL || popup_input_blocked)
        return;
    mouse = GetMousePosition();
    if(IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        shell->dragging_app = 0;
    if(shell->dragging_app != 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        int index = find_app_index_by_id(shell, shell->dragging_app);
        if(index >= 0) {
            RillAppWindow *app = &shell->apps[index];
            app->x = (int)mouse.x - shell->drag_offset_x;
            app->y = (int)mouse.y - shell->drag_offset_y;
            clamp_window_to_screen(app);
        }
        return;
    }
    if(!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;

    for(int i = shell->app_count - 1; i >= 0; i--) {
        RillAppWindow *app = &shell->apps[i];
        Rectangle frame = {app->x, app->y, app->w, app->h};
        Rectangle title = {app->x, app->y, app->w, 30};
        Rectangle close = {app->x + app->w - 30, app->y + 4, 22, 22};

        if(!CheckCollisionPointRec(mouse, frame))
            continue;
        shell->menu_open = 0;
        if(!app->focused) {
            int app_id = app->id;
            RillShellFocusApp(shell, app_id);
            i = find_app_index_by_id(shell, app_id);
            if(i >= 0)
                app = &shell->apps[i];
        }
        if(CheckCollisionPointRec(mouse, title) &&
           !CheckCollisionPointRec(mouse, close)) {
            shell->dragging_app = app->id;
            shell->drag_offset_x = (int)mouse.x - app->x;
            shell->drag_offset_y = (int)mouse.y - app->y;
        }
        return;
    }
}

static void
draw_host_app(RillAppWindow *app, Rectangle content, RillVisualState *visuals)
{
    Vector2 mouse;
    Vector2 delta;
    KryonInputOverride input;
    RillHostModule *module;
    const char *host_id;

    if(visuals == NULL || app == NULL)
        return;
    host_id = app->host_id[0] != '\0' ? app->host_id :
              (app->kind == RILL_APP_TERMINAL ? "ktrem" : "shelf");
    module = load_host_module(visuals, host_id);
    if(module == NULL || module->host == NULL) {
        draw_text_fit((TextProps){
            .bounds = {(int)content.x + 16, (int)content.y + 18, (int)content.width - 32, 0},
            .text = "Host module not installed", .font = Text14, .class_name = LabelPrimary,
            .wrap = TextWrapNone});
        draw_text_fit((TextProps){
            .bounds = {(int)content.x + 16, (int)content.y + 46, (int)content.width - 32, 0},
            .text = host_id, .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        return;
    }

    mouse = GetMousePosition();
    delta = GetMouseDelta();
    memset(&input, 0, sizeof(input));
    input.enabled = 1;
    input.mouse_inside = !popup_input_blocked && app != NULL && app->focused &&
                         CheckCollisionPointRec(mouse, content);
    input.pass_buttons = !popup_input_blocked && app != NULL && app->focused;
    input.pass_keyboard = !popup_input_blocked && app != NULL && app->focused;
    input.mouse_position = mouse;
    input.mouse_delta = delta;

    SetAppHostFocused(module->host, app != NULL && app->focused);
    ResizeAppHost(module->host, (int)content.width, (int)content.height);
    BeginKryonInputOverride(input);
    DrawAppScreen(module->host, content);
    EndKryonInputOverride();
}

static int
draw_settings_button(Rectangle bounds, const char *label, int active)
{
    int hover = CheckCollisionPointRec(GetMousePosition(), bounds);

    DrawRectangleRounded(bounds, 0.06f, 4,
                         active ? panel_active_color() :
                         hover ? panel_item_hover_color() : panel_item_color());
    DrawRectangleRoundedLinesEx(bounds, 0.06f, 4, 1.0f,
                                Fade(GetThemeText(), 0.30f));
    draw_text_fit((TextProps){
        .bounds = {(int)bounds.x + 8, (int)bounds.y + 5, (int)bounds.width - 16, 0},
        .text = label, .font = Text12, .class_name = LabelPanel,
        .wrap = TextWrapNone});
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static const char *
wallpaper_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static void
draw_settings_app(RillShellState *shell, Rectangle content,
                  RillVisualState *visuals,
                  const RillPlatformServices *platform)
{
    const char *clock_choices[3] = {"%H:%M", "%H:%M:%S", "%a %d %b %H:%M"};
    char sample[64];
    time_t now;
    int y;
    int i;

    if(draw_settings_button((Rectangle){content.x + 12, content.y + 8, 138, 28},
                            "Desktop", visuals->settings_tab == 0))
        visuals->settings_tab = 0;
    if(draw_settings_button((Rectangle){content.x + 158, content.y + 8, 138, 28},
                            "System", visuals->settings_tab == 1))
        visuals->settings_tab = 1;
    content.y += 40;
    content.height -= 40;
    if(visuals->settings_tab == 1) {
        const char *labels[] = {"Displays", "Keyboard", "Mouse and Touchpad", "Themes and Fonts",
                                "Default Applications", "Accessibility", "Power Management", "Network",
                                "Bluetooth", "Sound"};
        const char *categories[] = {"display", "keyboard", "mouse", "appearance", "defaults",
                                    "accessibility", "power", "network", "bluetooth", "audio"};
        Text((TextProps){
            .bounds = {content.x + 16, content.y + 12, 0, 0},
            .text = "System settings", .font = Text18, .class_name = LabelPrimary,
            .wrap = TextWrapNone});
        draw_text_fit((TextProps){
            .bounds = {content.x + 16, content.y + 42, content.width - 32, 0},
            .text = "Open the installed control panel for each device or service.", .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        for(int row = 0; row < 10; row++) {
            float width = (content.width - 36) / 2;
            if(draw_settings_button((Rectangle){content.x + 12 + (row % 2) * (width + 12),
                                     content.y + 72 + (row / 2) * 44, width, 34}, labels[row], 0)) {
                if(platform->open_settings != NULL && platform->open_settings(categories[row]))
                    visuals->settings_error[0] = '\0';
                else
                    snprintf(visuals->settings_error, sizeof(visuals->settings_error),
                             "No installed control panel for %s.", labels[row]);
            }
        }
        draw_text_fit((TextProps){
            .bounds = {content.x + 16, content.y + 306, content.width - 32, 0},
            .text = visuals->settings_error, .font = Text12, .class_name = LabelPrimary,
            .wrap = TextWrapNone});
        return;
    }

    Text((TextProps){
        .bounds = {(int)content.x + 16, (int)content.y + 12, 0, 0},
        .text = "Appearance", .font = Text18, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    draw_text_fit((TextProps){
        .bounds = {(int)content.x + 16, (int)content.y + 40, (int)content.width - 32, 0},
        .text = visuals->system_theme_name, .font = Text12, .class_name = LabelMuted,
        .wrap = TextWrapNone});
    draw_text_fit((TextProps){
        .bounds = {(int)content.x + 16, (int)content.y + 60, (int)content.width - 32, 0},
        .text = visuals->system_font_name, .font = Text12, .class_name = LabelMuted,
        .wrap = TextWrapNone});

    Text((TextProps){
        .bounds = {(int)content.x + 16, (int)content.y + 86, 0, 0},
        .text = "Wallpaper", .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    if(!visuals->wallpaper_scanned && platform != NULL &&
       platform->list_wallpapers != NULL) {
        visuals->wallpaper_count =
            platform->list_wallpapers(visuals->wallpaper_paths,
                                      RILL_WALLPAPER_CHOICES);
        visuals->wallpaper_scanned = 1;
    }
    BeginScissorMode((int)content.x, (int)content.y + 104, (int)content.width, 108);
    if(CheckCollisionPointRec(GetMousePosition(),
                              (Rectangle){content.x, content.y + 104, content.width, 108})) {
        visuals->wallpaper_scroll -= (int)GetMouseWheelMove();
        int maximum = visuals->wallpaper_count > 4 ? visuals->wallpaper_count - 4 : 0;
        if(visuals->wallpaper_scroll < 0) visuals->wallpaper_scroll = 0;
        if(visuals->wallpaper_scroll > maximum) visuals->wallpaper_scroll = maximum;
    }
    y = (int)content.y + 104;
    if(visuals->wallpaper_count <= 0)
        draw_text_fit((TextProps){
            .bounds = {(int)content.x + 16, y + 6, (int)content.width - 32, 0},
            .text = "No wallpapers found in Pictures or system backgrounds", .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
    for(i = visuals->wallpaper_scroll; i < visuals->wallpaper_count &&
                                      i < visuals->wallpaper_scroll + 4; i++) {
        const char *path = visuals->wallpaper_paths[i];
        if(draw_settings_button((Rectangle){content.x + 12, y + 2,
                                            content.width - 24, 22},
                                wallpaper_basename(path),
                                strcmp(path, visuals->wallpaper_path) == 0))
            apply_wallpaper(shell, visuals, path, 1);
        y += 26;
    }
    EndScissorMode();
    y = (int)content.y + 216;
    if(draw_settings_button((Rectangle){content.x + 12, y, 150, 26},
                            "System background", 0)) {
        char wallpaper[512];
        if(GetSystemDesktopBackground(wallpaper, sizeof(wallpaper))) {
            apply_wallpaper(shell, visuals, wallpaper, 1);
            RillSettingsSet(&rill_settings, "wallpaper", "");
            rill_settings_persist(shell);
        } else
            RillShellSetStatus(shell, "No system background configured");
    }
    if(draw_settings_button((Rectangle){content.x + 172, y, 150, 26},
                            visuals->wallpaper_slideshow ?
                            "Slideshow: 5 min" : "Slideshow: off",
                            visuals->wallpaper_slideshow)) {
        visuals->wallpaper_slideshow = !visuals->wallpaper_slideshow;
        RillSettingsSetInteger(&rill_settings, "wallpaper-slideshow",
                               visuals->wallpaper_slideshow);
        rill_settings_persist(shell);
    }

    y = (int)content.y + 252;
    Text((TextProps){
        .bounds = {(int)content.x + 16, y, 0, 0},
        .text = "Clock", .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    now = time(NULL);
    for(i = 0; i < 3; i++) {
        strftime(sample, sizeof(sample), clock_choices[i], localtime(&now));
        if(draw_settings_button((Rectangle){content.x + 12 + i * 148, y + 20,
                                            140, 24},
                                sample,
                                strcmp(visuals->clock_format,
                                       clock_choices[i]) == 0)) {
            snprintf(visuals->clock_format, sizeof(visuals->clock_format),
                     "%s", clock_choices[i]);
            RillSettingsSet(&rill_settings, "clock-format",
                            visuals->clock_format);
            rill_settings_persist(shell);
        }
    }

    y = (int)content.y + 312;
    Text((TextProps){
        .bounds = {(int)content.x + 16, y + 4, 0, 0},
        .text = "Panel height", .font = Text12, .class_name = LabelMuted,
        .wrap = TextWrapNone});
    if(draw_settings_button((Rectangle){content.x + 240, y, 26, 24}, "-", 0) &&
       visuals->panel_height > 20) {
        visuals->panel_height--;
        RillSettingsSetInteger(&rill_settings, "panel-height",
                               visuals->panel_height);
        rill_settings_persist(shell);
    }
    snprintf(sample, sizeof(sample), "%d", visuals->panel_height);
    draw_text_fit((TextProps){
        .bounds = {(int)content.x + 272, y + 4, 34, 0},
        .text = sample, .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    if(draw_settings_button((Rectangle){content.x + 310, y, 26, 24}, "+", 0) &&
       visuals->panel_height < 48) {
        visuals->panel_height++;
        RillSettingsSetInteger(&rill_settings, "panel-height",
                               visuals->panel_height);
        rill_settings_persist(shell);
    }
}

static void
draw_about_app(Rectangle content)
{
    Text((TextProps){
        .bounds = {(int)content.x + 16, (int)content.y + 14, 0, 0},
        .text = "Rill", .font = Text24, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    Text((TextProps){
        .bounds = {(int)content.x + 16, (int)content.y + 54, 0, 0},
        .text = "A Kryon/libdraw desktop for Taiji and Plan 9.", .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
}

static int
rill_case_prefix(const char *text, const char *prefix)
{
    while(*prefix != '\0') {
        if(*text == '\0' ||
           tolower((unsigned char)*text) != tolower((unsigned char)*prefix))
            return 0;
        text++;
        prefix++;
    }
    return 1;
}

static const RillLauncher *
run_match_launcher(const RillShellState *shell, const char *text, int index)
{
    int found = 0;
    size_t length;

    if(shell == NULL || text == NULL)
        return NULL;
    length = strlen(text);
    if(length == 0)
        return NULL;
    for(int i = 0; i < shell->launcher_count; i++) {
        const RillLauncher *launcher = &shell->launchers[i];
        if(rill_case_prefix(launcher->name, text) ||
           rill_case_prefix(launcher->id, text)) {
            if(found == index)
                return launcher;
            found++;
        }
    }
    return NULL;
}

static void
run_record_history(const char *text)
{
    const char *previous = RillSettingsGet(&rill_settings, "run-history", "");
    char history[512];
    int length;
    int count = 1;

    length = snprintf(history, sizeof(history), "%s", text);
    for(const char *cursor = previous; *cursor != '\0' && count < 8; ) {
        const char *end = strchr(cursor, '|');
        size_t size = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if(size != strlen(text) || strncmp(cursor, text, size) != 0) {
            int written = snprintf(history + length,
                                   sizeof(history) - (size_t)length,
                                   "|%.*s", (int)size, cursor);
            if(written < 0 || (size_t)written >= sizeof(history) - (size_t)length)
                break;
            length += written;
            count++;
        }
        if(end == NULL)
            break;
        cursor = end + 1;
    }
    RillSettingsSet(&rill_settings, "run-history", history);
    rill_settings_persist(NULL);
}

static int
run_execute(RillShellState *shell, const RillPlatformServices *platform,
            const RillLauncher *match, const char *text)
{
    RillLauncher command;
    const RillLauncher *target = match;
    int ok;

    if(text == NULL || text[0] == '\0')
        return 0;
    if(target == NULL) {
        memset(&command, 0, sizeof(command));
        snprintf(command.name, sizeof(command.name), "%s", text);
        snprintf(command.command, sizeof(command.command), "%s", text);
        target = &command;
    }
    ok = platform != NULL && platform->launch != NULL && platform->launch(target);
    if(ok)
        run_record_history(match != NULL ? match->name : text);
    else
        RillShellSetStatus(shell, "Could not run the command");
    return ok;
}

static void
update_run_dialog_input(RillVisualState *visuals)
{
    size_t length;
    int c;

    while((c = GetCharPressed()) > 0) {
        length = strlen(visuals->run_input);
        if(c >= 32 && c < 127 && length < sizeof(visuals->run_input) - 1) {
            visuals->run_input[length] = (char)c;
            visuals->run_input[length + 1] = '\0';
            visuals->run_selected = 0;
        }
    }
    if(IsKeyPressed(KEY_BACKSPACE)) {
        length = strlen(visuals->run_input);
        if(length > 0) {
            visuals->run_input[length - 1] = '\0';
            visuals->run_selected = 0;
        }
    }
    if(IsKeyPressed(KEY_ESCAPE))
        rill_stop_requested = 1;
}

static void
draw_run_dialog(RillShellState *shell, RillVisualState *visuals,
                const RillPlatformServices *platform)
{
    const char *history;
    const RillLauncher *match;
    char item[128];
    int width = GetScreenWidth();
    int matches = 0;
    int y = 86;
    int i;

    update_run_dialog_input(visuals);

    DrawRectangleRounded((Rectangle){0, 0, (float)width, 30}, 0.02f, 4,
                         panel_color());
    Text((TextProps){
        .bounds = {10, 8, 0, 0},
        .text = "Run program", .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    DrawRectangleRounded((Rectangle){8, 40, (float)width - 16, 32}, 0.05f, 4,
                         Fade(BLACK, 0.22f));
    DrawRectangleRoundedLinesEx((Rectangle){8, 40, (float)width - 16, 32},
                                0.05f, 4, 1.0f, GetThemeLink());
    if(visuals->run_input[0] != '\0')
        draw_text_fit((TextProps){
            .bounds = {16, 48, width - 32, 0},
            .text = visuals->run_input, .font = Text14, .class_name = LabelPrimary,
            .wrap = TextWrapNone});
    else
        draw_text_fit((TextProps){
            .bounds = {16, 48, width - 32, 0},
            .text = "Type a command or application name", .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});

    if(visuals->run_input[0] != '\0') {
        for(i = 0; i < 6; i++) {
            match = run_match_launcher(shell, visuals->run_input, i);
            if(match == NULL)
                break;
            if(draw_settings_button((Rectangle){8, (float)y,
                                                (float)width - 16, 26},
                                    match->name, 0)) {
                if(run_execute(shell, platform, match, match->name))
                    rill_stop_requested = 1;
                return;
            }
            draw_text_fit((TextProps){
                .bounds = {200, y + 5, width - 220, 0},
                .text = match->description, .font = Text12, .class_name = LabelMuted,
                .wrap = TextWrapNone});
            matches++;
            y += 30;
        }
        if(matches == 0)
            draw_text_fit((TextProps){
                .bounds = {16, y + 2, width - 32, 0},
                .text = "Press Enter to run the typed command", .font = Text12, .class_name = LabelMuted,
                .wrap = TextWrapNone});
        if(IsKeyPressed(KEY_ENTER)) {
            match = run_match_launcher(shell, visuals->run_input, 0);
            if(run_execute(shell, platform, match, visuals->run_input))
                rill_stop_requested = 1;
        }
        return;
    }

    history = RillSettingsGet(&rill_settings, "run-history", "");
    if(history[0] == '\0') {
        draw_text_fit((TextProps){
            .bounds = {16, 90, width - 32, 0},
            .text = "Recent commands appear here", .font = Text12, .class_name = LabelMuted,
            .wrap = TextWrapNone});
        return;
    }
    for(i = 0; i < 4; i++) {
        const char *end = strchr(history, '|');
        size_t size = end != NULL ? (size_t)(end - history) : strlen(history);
        if(size == 0)
            break;
        if(size > sizeof(item) - 1)
            size = sizeof(item) - 1;
        memcpy(item, history, size);
        item[size] = '\0';
        if(draw_settings_button((Rectangle){8, (float)y, (float)width - 16, 26},
                                item, 0)) {
            if(run_execute(shell, platform, NULL, item))
                rill_stop_requested = 1;
            return;
        }
        y += 30;
        if(end == NULL)
            break;
        history = end + 1;
    }
}

static void
draw_app_window(RillShellState *shell, RillAppWindow *app,
                RillVisualState *visuals, const RillPlatformServices *platform)
{
    Rectangle frame;
    Rectangle title;
    Rectangle content;
    Color frame_color;

    frame = (Rectangle){app->x, app->y, app->w, app->h};
    title = (Rectangle){app->x, app->y, app->w, 30};
    content = (Rectangle){app->x + 1, app->y + 31, app->w - 2, app->h - 32};
    frame_color = app->focused ? GetThemeLink() : Fade(GetThemeText(), 0.32f);
    include_panel_popup(frame);

    DrawRectangleRec(frame, opaque_color(GetThemeSurface()));
    DrawRectangleRounded(frame, 0.025f, 8, opaque_color(GetThemeSurface()));
    DrawRectangleRoundedLinesEx(frame, 0.025f, 8, 2.0f, frame_color);
    DrawRectangleRec(title, opaque_color(mix_color(GetThemeSurface(),
                                                  frame_color, 0.18f)));
    BeginScissorMode((int)title.x + 6, (int)title.y,
                     (int)title.width - 42, (int)title.height);
    Text((TextProps){
        .bounds = {app->x + 10, app->y + 8, 0, 0},
        .text = app->title, .font = Text14, .class_name = LabelPrimary,
        .wrap = TextWrapNone});
    EndScissorMode();
    if(draw_window_close_button((Rectangle){app->x + app->w - 30,
                                            app->y + 4, 22, 22})) {
        RillShellCloseApp(shell, app->id);
        return;
    }

    BeginScissorMode((int)content.x, (int)content.y, (int)content.width,
                     (int)content.height);
    DrawRectangleRec(content, opaque_color(GetThemeBackground()));
    if(app->kind == RILL_APP_TERMINAL || app->kind == RILL_APP_FILES)
        draw_host_app(app, content, visuals);
    else if(app->kind == RILL_APP_SETTINGS)
        draw_settings_app(shell, content, visuals, platform);
    else
        draw_about_app(content);
    EndScissorMode();
}

static void
draw_apps(RillShellState *shell, RillVisualState *visuals,
          const RillPlatformServices *platform)
{
    int i;

    for(i = 0; i < shell->app_count; i++)
        if(!shell->apps[i].focused)
            draw_app_window(shell, &shell->apps[i], visuals, platform);
    for(i = 0; i < shell->app_count; i++)
        if(shell->apps[i].focused)
            draw_app_window(shell, &shell->apps[i], visuals, platform);
}

static void
draw_test_window(Rectangle frame, const char *title, Color title_color,
                 Color content_color, int focused)
{
    Rectangle title_rect = {frame.x, frame.y, frame.width, 30};
    Rectangle content = {frame.x + 1, frame.y + 31, frame.width - 2,
                         frame.height - 32};
    Color border = focused ? WHITE : Fade(WHITE, 0.44f);

    DrawRectangleRec(frame, opaque_color(GetThemeSurface()));
    DrawRectangleRounded(frame, 0.025f, 8, opaque_color(GetThemeSurface()));
    DrawRectangleRoundedLinesEx(frame, 0.025f, 8, 2.0f, border);
    DrawRectangleRec(title_rect, opaque_color(title_color));
    BeginScissorMode((int)title_rect.x + 8, (int)title_rect.y,
                     (int)title_rect.width - 16, (int)title_rect.height);
    Text((TextProps){
        .bounds = {(int)title_rect.x + 10, (int)title_rect.y + 8, 0, 0},
        .text = title, .font = Text14, .class_name = LabelWhite,
        .wrap = TextWrapNone});
    EndScissorMode();
    DrawRectangleRec(content, opaque_color(content_color));
}

static void
draw_compositor_stack_test_scene(void)
{
    Rectangle lower = {140, 100, 460, 300};
    Rectangle lower_content = {lower.x + 1, lower.y + 31, lower.width - 2,
                               lower.height - 32};
    Rectangle upper = {RILL_TEST_UPPER_X, RILL_TEST_UPPER_Y, RILL_TEST_UPPER_W,
                       RILL_TEST_UPPER_H};
    const Color lower_bg = {18, 18, 22, 255};
    const Color lower_title = {94, 28, 36, 255};
    const Color upper_bg = {24, 172, 128, 255};
    const Color upper_title = {20, 92, 72, 255};

    ClearBackground((Color){8, 9, 12, 255});
    DrawRectangle(0, 0, GetScreenWidth(), PANEL_H, (Color){20, 22, 30, 255});
    Text((TextProps){
        .bounds = {10, 8, 0, 0},
        .text = "Rill visual test", .font = Text12, .class_name = LabelWhite,
        .wrap = TextWrapNone});

    draw_test_window(lower, "Lower text producer", lower_title, lower_bg, 0);
    BeginScissorMode((int)lower_content.x, (int)lower_content.y,
                     (int)lower_content.width, (int)lower_content.height);
    for(int i = 0; i < 8; i++) {
        Text((TextProps){
            .bounds = {RILL_TEST_LOWER_TEXT_X, RILL_TEST_LOWER_TEXT_Y + i * 26, 0, 0},
            .text = "TEXT-HIERARCHY-LEAK TEXT-HIERARCHY-LEAK", .font = Text24, .class_name = LabelTestRed,
            .wrap = TextWrapNone});
    }
    EndScissorMode();

    draw_test_window(upper, "Upper opaque cover", upper_title, upper_bg, 1);
}

static void
draw_menu_stack_test_scene(void)
{
    Rectangle lower = {118, 72, 460, 300};
    Rectangle lower_content = {lower.x + 1, lower.y + 31, lower.width - 2,
                               lower.height - 32};
    Rectangle menu = {176, 118, 238, 112};

    ClearBackground((Color){8, 9, 12, 255});
    DrawRectangle(0, 0, GetScreenWidth(), PANEL_H, panel_color());
    Text((TextProps){
        .bounds = {10, 8, 0, 0},
        .text = "Rill visual test", .font = Text12, .class_name = LabelWhite,
        .wrap = TextWrapNone});

    draw_test_window(lower, "Lower text producer", (Color){92, 28, 96, 255},
                     (Color){18, 18, 22, 255}, 1);
    BeginScissorMode((int)lower_content.x, (int)lower_content.y,
                     (int)lower_content.width, (int)lower_content.height);
    for(int i = 0; i < 5; i++) {
        Text((TextProps){
            .bounds = {190, 138 + i * 24, 0, 0},
            .text = "TEXT-HIERARCHY-LEAK TEXT-HIERARCHY-LEAK", .font = Text24, .class_name = LabelTestRed,
            .wrap = TextWrapNone});
    }
    EndScissorMode();

    draw_window_close_button((Rectangle){lower.x + lower.width - 30,
                                         lower.y + 4, 22, 22});
    draw_menu_panel(menu);
    draw_menu_row((Rectangle){182, 124, 226, 28}, "ktrem", "terminal");
    draw_menu_row((Rectangle){182, 156, 226, 28}, "Files", "files");
    draw_menu_row((Rectangle){182, 188, 226, 28}, "Settings", "settings");
}

static int
draw_test_scene(const RillTestState *test)
{
    if(test == NULL || test->scene == NULL)
        return 0;
    if(strcmp(test->scene, "compositor-stack") == 0) {
        draw_compositor_stack_test_scene();
        return 1;
    }
    if(strcmp(test->scene, "menu-stack") == 0) {
        draw_menu_stack_test_scene();
        return 1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    static RillShellState shell;
    static RillVisualState visuals;
    RillTestState test;
    RillControlState control;
    RillRuntimeOptions options;
    const RillPlatformServices *platform;
    char startup_status[160];
    char window_title[80] = "Rill";
    double next_refresh;
    int first_refresh = 1;
#if RILL_HAS_X11
    RillX11Manager x11;
#endif

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--help") == 0) {
            puts("rill [--desktop [--external-panel|--xfce-panel]]\n"
                 "     [--panel ID [--panel-output OUTPUT]]\n"
                 "     [--settings | --run-dialog [COMMAND]]\n"
                 "     [--windowed [COMMAND] | --wm]\n"
                 "Use rill-session for login or rill-window for a nested X11 desktop.");
            return 0;
        }
    }
#ifdef KRYON_NATIVE_PLAN9
    atnotify(rill_note, 1);
#else
    signal(SIGINT, rill_signal_stop);
    signal(SIGTERM, rill_signal_stop);
#endif

    init_test_state(&test);
    parse_runtime_options(argc, argv, &options);
    snprintf(startup_status, sizeof(startup_status), "%s", "Ready");

#if RILL_HAS_X11
    if(options.mode == RILL_MODE_DESKTOP || options.mode == RILL_MODE_PANEL) {
        if(RillWaylandSession()) {
            fprintf(stderr, "rill: --desktop currently requires an X11 session; Wayland desktop surfaces are not implemented\n");
            return 1;
        }
        snprintf(window_title, sizeof(window_title), "Rill desktop %ld", (long)getpid());
    }
    if(options.mode == RILL_MODE_PANEL) {
        if(options.panel_id[0] == '\0' ||
           strspn(options.panel_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
           strlen(options.panel_id)) {
            fprintf(stderr, "rill: panel name must contain only letters, numbers, - or _\n");
            return 2;
        }
        snprintf(window_title, sizeof(window_title), "Rill panel %s", options.panel_id);
        test.disable_wallpaper = 1;
    }
    if(options.xfce_panel && options.mode != RILL_MODE_DESKTOP) {
        fprintf(stderr, "rill: --xfce-panel requires --desktop and an existing X11 window manager\n");
        return 1;
    }
    RillX11Init(&x11);
    if(options.mode == RILL_MODE_WINDOWED) {
        if(RillX11StartWindowed(&x11, RILL_WIDTH, RILL_HEIGHT))
            snprintf(startup_status, sizeof(startup_status),
                     "Windowed X11 display %s", RillX11DisplayName(&x11));
        else {
            fprintf(stderr, "rill: windowed X11 display unavailable\n");
            return 1;
        }
    }
#else
    if(options.mode == RILL_MODE_WM || options.mode == RILL_MODE_WINDOWED)
        snprintf(startup_status, sizeof(startup_status),
                 "%s", "X11 window management unavailable on this platform");
#endif

    platform = RillPlatformCurrent();
    RillShellInit(&shell);
    RillShellRefresh(&shell, platform);
    RillShellSetStatus(&shell, startup_status);
    memset(&control, 0, sizeof(control));
    if(options.mode != RILL_MODE_SETTINGS)
        rill_control_init(&control);

    SetSingleInstance(0);
    if(options.mode == RILL_MODE_RUN)
        InitWindow(480, 240, "Rill run dialog");
    else if(options.mode == RILL_MODE_SETTINGS)
        InitWindow(540, 440, "Rill Settings");
    else
        InitWindow(RILL_WIDTH, RILL_HEIGHT, window_title);
    SetExitKey(0); /* Escape cancels UI; it must never terminate the desktop. */
    if(!IsWindowReady()) {
        fprintf(stderr, "rill: failed to open Kryon window\n");
#if RILL_HAS_X11
        RillX11Shutdown(&x11);
#endif
        RillShellDispose(&shell);
        CloseWindow();
        return 1;
    }
    if(WindowShouldClose()) {
        fprintf(stderr, "rill: Kryon window closed during startup\n");
#if RILL_HAS_X11
        RillX11Shutdown(&x11);
#endif
        RillShellDispose(&shell);
        CloseWindow();
        return 1;
    }
    EnableEventWaiting();
    SetTargetFPS(60);
    SetDefaultFontAutoLoad(1);
    configure_system_look(&visuals, &test);
    init_panel_plugins(&visuals);
    snprintf(visuals.clock_format, sizeof(visuals.clock_format), "%%H:%%M");
    visuals.panel_height = PANEL_H;
    visuals.desktop_drag_index = -1;
    visuals.panel_drag_index = -1;
    visuals.desktop_selected = -1;
    visuals.desktop_last_index = -1;
    if(!test_scene_active(&test)) {
        const char *root = platform->settings_root();
        if(root != NULL && RillSettingsEnsureDirectory(root)) {
            snprintf(visuals.desktop_layout_path, sizeof(visuals.desktop_layout_path),
                     "%s/desktop-layout", root);
            RillSettingsLoad(&visuals.desktop_layout, visuals.desktop_layout_path);
            snprintf(visuals.panel_config_path, sizeof(visuals.panel_config_path),
                     "%s/panel", root);
            if(options.mode == RILL_MODE_PANEL && strcmp(options.panel_id, "primary") != 0)
                snprintf(visuals.panel_config_path, sizeof(visuals.panel_config_path),
                         "%s/panel-%s", root, options.panel_id);
            RillPanelLoad(visuals.panel_config_path, visuals.left_panel,
                          &visuals.left_panel_count, visuals.right_panel,
                          &visuals.right_panel_count, RILL_PANEL_PLUGIN_MAX);
            snprintf(rill_settings_path, sizeof(rill_settings_path),
                     "%s/settings", root);
            if(options.mode == RILL_MODE_PANEL && strcmp(options.panel_id, "primary") != 0)
                snprintf(rill_settings_path, sizeof(rill_settings_path),
                         "%s/settings-%s", root, options.panel_id);
        }
    }
    if(rill_settings_path[0] != '\0')
        RillSettingsLoad(&rill_settings, rill_settings_path);
    settings_previous = rill_settings;
    if(options.mode != RILL_MODE_RUN)
        apply_saved_settings(&shell, &visuals);
    if(options.mode == RILL_MODE_RUN) {
        /* A command argument runs immediately; otherwise the dialog opens. */
        if(options.launch_command[0] != '\0') {
            RillShellSetStatus(&shell, run_execute(&shell, platform, NULL,
                                                   options.launch_command) ?
                               "Ran command" : "Could not run the command");
            rill_stop_requested = 1;
        }
    } else if(visuals.wallpaper_slideshow && !visuals.wallpaper_scanned &&
              platform != NULL && platform->list_wallpapers != NULL) {
        visuals.wallpaper_count =
            platform->list_wallpapers(visuals.wallpaper_paths,
                                      RILL_WALLPAPER_CHOICES);
        visuals.wallpaper_scanned = 1;
    }

#if RILL_HAS_X11
    if(options.mode == RILL_MODE_PANEL && !PanelSurfaceInit(window_title, options.panel_output)) {
        fprintf(stderr, "rill: could not create the X11 panel surface\n");
        RillShellDispose(&shell);
        CloseWindow();
        return 1;
    }
    if(options.mode == RILL_MODE_DESKTOP) {
        if(!RillX11SetDesktop(window_title)) {
            fprintf(stderr, "rill: could not assign the X11 desktop surface\n");
            RillShellDispose(&shell);
            CloseWindow();
            return 1;
        }
        if(options.xfce_panel && !options.external_panel) {
            RillLauncher launcher;
            memset(&launcher, 0, sizeof(launcher));
            snprintf(launcher.name, sizeof(launcher.name), "Xfce panel");
            snprintf(launcher.command, sizeof(launcher.command), "xfce4-panel");
            if(!platform->launch(&launcher)) {
                fprintf(stderr, "rill: xfce4-panel is required for --xfce-panel\n");
                RillShellDispose(&shell);
                CloseWindow();
                return 1;
            }
        }
    }
    if(options.mode == RILL_MODE_WM) {
        if(RillX11StartRoot(&x11))
            RillShellSetStatus(&shell, "Managing current X11 display");
        else
            RillShellSetStatus(&shell, "Could not claim X11 window manager");
    }
    if(options.launch_command[0] != '\0' && x11.active &&
       RillX11Launch(&x11, options.launch_command))
        RillShellSetStatus(&shell, "Launched contained X11 application");
#endif

    next_refresh = 0.0;
#if RILL_HAS_X11
    if(options.mode == RILL_MODE_DESKTOP || options.mode == RILL_MODE_PANEL)
        SessionConnect(argc, argv);
#endif
    while(!rill_stop_requested && !WindowShouldClose()) {
#if RILL_HAS_X11
        if(!SessionPoll()) break;
#endif
        if(!test_scene_active(&test)) {
#if RILL_HAS_X11
            RillX11Poll(&x11);
            RillX11ProcessInput(&x11);
#endif
        }
        if(!test_scene_active(&test) && GetTime() >= next_refresh) {
#if RILL_HAS_X11
            if(options.mode == RILL_MODE_DESKTOP) RillX11SyncDesktop();
#endif
            RillSettings disk_settings;
            if(RillSettingsLoad(&disk_settings, rill_settings_path) &&
               memcmp(&disk_settings, &settings_previous, sizeof(disk_settings)) != 0) {
                rill_settings = disk_settings;
                settings_previous = disk_settings;
                apply_saved_settings(&shell, &visuals);
            }
            RillShellRefresh(&shell, platform);
            load_launcher_icons(&visuals, &shell);
            if(!options.external_panel && options.mode != RILL_MODE_SETTINGS &&
               options.mode != RILL_MODE_RUN) {
                refresh_tray_icons(&visuals, platform);
                refresh_notifications(&visuals, platform);
                refresh_battery(&visuals, platform);
                refresh_volume(&visuals, platform);
                refresh_clipboard(&visuals, platform);
            }
            if(first_refresh) {
                const char *external_settings = getenv("RILL_SESSION_EXTERNAL_SETTINGS");
                if(options.mode != RILL_MODE_SETTINGS && options.mode != RILL_MODE_RUN &&
                   (external_settings == NULL || strcmp(external_settings, "1") != 0))
                    publish_xsettings(platform);
                first_refresh = 0;
            }
            next_refresh = GetTime() + 1.0;
        }
        if(!test_scene_active(&test))
            rill_control_poll(&control, &shell, platform);

#if RILL_HAS_X11
        if(options.mode == RILL_MODE_PANEL &&
           PanelSurfaceBegin(rill_panel_visible_height(&visuals), visuals.panel_bottom)) {
            shell.menu_open = 0;
            visuals.panel_context_open = 0;
            visuals.calendar_open = 0;
            visuals.clipboard_popup_open = 0;
        }
#endif
        plan9_overlay_begin();
        BeginDrawing();
        ClearBackground(opaque_color(GetThemeBackground()));
        BeginInterfaceFrame(GetScreenWidth(), GetScreenHeight(), 1.0f);
        if(IsKeyPressed(KEY_ESCAPE) && options.mode != RILL_MODE_RUN) {
            shell.menu_open = 0;
            visuals.panel_context_open = 0;
            visuals.calendar_open = 0;
            visuals.clipboard_popup_open = 0;
            visuals.properties_open = 0;
            visuals.logout_open = 0;
            visuals.file_action[0] = '\0';
        }
        popup_input_blocked = shell.menu_open != 0 || visuals.panel_context_open ||
                              visuals.calendar_open || visuals.clipboard_popup_open ||
                              visuals.properties_open || visuals.logout_open;
        if(!test_scene_active(&test))
            process_window_mouse(&shell);
        if(!test_scene_active(&test) && options.mode != RILL_MODE_PANEL &&
           options.mode != RILL_MODE_SETTINGS && options.mode != RILL_MODE_RUN)
            process_desktop_mouse(&shell, platform, &visuals);
        /* This compositor draws each window immediately, including its text.
         * Deferring widgets to EndTree would paint them over later windows. */

        if(test_scene_active(&test)) {
            if(!draw_test_scene(&test))
                RillShellSetStatus(&shell, "Unknown visual test scene");
        } else if(options.mode == RILL_MODE_RUN) {
            draw_run_dialog(&shell, &visuals, platform);
        } else if(options.mode == RILL_MODE_SETTINGS) {
            draw_settings_app(&shell, (Rectangle){0, 0, GetScreenWidth(), GetScreenHeight()},
                              &visuals, platform);
        } else {
            if(options.mode != RILL_MODE_PANEL) {
                draw_wallpaper(&visuals);
                draw_desktop(&shell, platform, &visuals);
            }
            if(popup_input_blocked) {
                KryonInputOverride blocked;
                memset(&blocked, 0, sizeof(blocked));
                blocked.enabled = 1;
                BeginKryonInputOverride(blocked);
            }
            draw_apps(&shell, &visuals, platform);
            if(popup_input_blocked)
                EndKryonInputOverride();
#if RILL_HAS_X11
            RillX11Draw(&x11);
#endif
            if(!options.xfce_panel) draw_top_panel(&shell, platform, &visuals);
            draw_applications_menu(&shell, platform, &visuals);
            draw_places_menu(&shell, platform, &visuals);
            draw_system_menu(&shell, platform, &visuals);
            draw_desktop_context_menu(&shell, platform, &visuals);
            draw_window_list_menu(&shell, platform, &visuals);
            draw_panel_context_menu(&shell, &visuals, platform);
            draw_calendar_popup(&visuals);
            draw_clipboard_popup(&shell, &visuals, platform);
            draw_panel_properties(&shell, &visuals);
            draw_notifications(&shell, &visuals, platform);
            draw_logout_dialog(&shell, &visuals, platform);
            draw_file_dialog(&shell, &visuals, platform);
            if(visuals.wallpaper_slideshow && visuals.wallpaper_count > 1 &&
               GetTime() >= visuals.wallpaper_next_swap) {
                int current = -1;
                visuals.wallpaper_next_swap = GetTime() + 300.0;
                for(int i = 0; i < visuals.wallpaper_count; i++)
                    if(strcmp(visuals.wallpaper_paths[i],
                              visuals.wallpaper_path) == 0)
                        current = i;
                apply_wallpaper(&shell, &visuals,
                                visuals.wallpaper_paths[(current + 1) %
                                                        visuals.wallpaper_count],
                                0);
            }
        }

        if(visuals.panel_dirty) {
            if(!RillPanelSave(visuals.panel_config_path, visuals.left_panel,
                              visuals.left_panel_count, visuals.right_panel,
                              visuals.right_panel_count))
                RillShellSetStatus(&shell, "Could not save panel settings");
            visuals.panel_dirty = 0;
        }
        EndInterfaceFrame();
        EndDrawing();
        plan9_overlay_end();
#if RILL_HAS_X11
        if(options.mode == RILL_MODE_DESKTOP)
            DesktopSurfaceInput(visuals.file_action[0] || shell.app_count > 0 || shell.menu_open != 0);
        if(options.mode == RILL_MODE_PANEL)
            PanelSurfaceEnd(shell.menu_open != 0 || visuals.properties_open ||
                            visuals.logout_open || shell.app_count > 0);
#endif

        if(test_scene_active(&test) || test.ready_file != NULL ||
           test.exit_after_frames > 0) {
            test.frames++;
            write_test_ready_file(&test);
            if(test.exit_after_frames > 0 &&
               test.frames >= test.exit_after_frames)
                break;
        }
    }

    for(int i = 0; i < visuals.host_count; i++) {
        if(visuals.hosts[i].host != NULL && visuals.hosts[i].destroy != NULL)
            visuals.hosts[i].destroy(visuals.hosts[i].host);
#if RILL_HAS_DLOPEN
        if(visuals.hosts[i].library != NULL)
            dlclose(visuals.hosts[i].library);
#endif
    }
    for(int i = 0; i < visuals.icon_count; i++)
        if(visuals.icons[i].ready)
            UnloadTexture(visuals.icons[i].texture);
    for(int i = 0; i < visuals.tray_count; i++)
        if(visuals.tray[i].ready)
            UnloadTexture(visuals.tray[i].texture);
    if(visuals.wallpaper_ready)
        UnloadTexture(visuals.wallpaper);
    rill_settings_persist(&shell);
    rill_control_close(&control);
#if RILL_HAS_X11
    RillX11Shutdown(&x11);
    RillWaylandShutdown();
    SessionDisconnect();
#endif
    CloseWindow();
    RillShellDispose(&shell);
    return 0;
}
