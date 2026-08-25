#include "rill_shell.h"

#include "kryon.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    PANEL_H = 32,
    RILL_HOST_CACHE_MAX = 8,
    RILL_ICON_CACHE_MAX = 32,
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

typedef struct RillHostModule {
    char id[64];
    char path[512];
    void *library;
    AppHost *host;
    DestroyAppHostCallback destroy;
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
} RillVisualState;

typedef struct RillTestState {
    const char *scene;
    const char *ready_file;
    int exit_after_frames;
    int disable_wallpaper;
    int frames;
    int ready_written;
} RillTestState;

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
    return opaque_color(mix_color(GetThemeSurface(), GetThemeBackground(), 0.10f));
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
    SetThemeStyle(THEME_STYLE_SYSTEM);
    SetCurrentTheme(GetDefaultThemeForThemeStyle(THEME_STYLE_SYSTEM),
                    SystemThemePrefersDark());
    ApplyCurrentUITheme();

    snprintf(visuals->system_theme_name, sizeof(visuals->system_theme_name),
             "%s", GetSystemThemeName());

    if(GetSystemUIFontName(font_name, sizeof(font_name)))
        snprintf(visuals->system_font_name, sizeof(visuals->system_font_name),
                 "%s", font_name);
    else
        snprintf(visuals->system_font_name, sizeof(visuals->system_font_name),
                 "%s", "Kryon UI");

    if(GetSystemUIFontFile(font_path, sizeof(font_path))) {
        snprintf(visuals->system_font_path, sizeof(visuals->system_font_path),
                 "%s", font_path);
        if(RegisterUIFontFileSource("system", font_path, NULL, 0))
            UseUIFont("system");
    }
    EnsureUIDefaultFont();

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
    char *save = NULL;
    char *dir;

    if(slot == NULL || dirs == NULL || dirs[0] == '\0')
        return 0;
    snprintf(copy, sizeof(copy), "%s", dirs);
    for(dir = strtok_r(copy, ":", &save); dir != NULL;
        dir = strtok_r(NULL, ":", &save)) {
        if(try_host_dir(slot, dir))
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

    fprintf(stderr, "rill: no host module found for %s\n", id);
    return slot;
}

static void
draw_text_fit(const char *text, int x, int y, int max_width, int font_size,
              Color color)
{
    if(text == NULL)
        text = "";
    while(font_size > Text8 && TextWidth(text, font_size) > max_width)
        font_size -= 2;
    Text(text, x, y, font_size, color);
}

static void
draw_text_fit_centered(const char *text, int x, int y, int max_width,
                       int font_size, Color color)
{
    int width;

    if(text == NULL)
        text = "";
    while(font_size > Text8 && TextWidth(text, font_size) > max_width)
        font_size -= 2;
    width = TextWidth(text, font_size);
    Text(text, x + (max_width - width) / 2, y, font_size, color);
}

static void
draw_wallpaper(const RillVisualState *visuals)
{
    Rectangle screen;
    Rectangle src;
    float scale;
    float sw;
    float sh;

    screen = (Rectangle){0, PANEL_H, GetScreenWidth(),
                         GetScreenHeight() - PANEL_H};
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
        DrawRectangle(0, PANEL_H, GetScreenWidth(),
                      GetScreenHeight() - PANEL_H,
                      opaque_color(GetThemeBackground()));
    }
    DrawRectangle(0, PANEL_H, GetScreenWidth(), GetScreenHeight() - PANEL_H,
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
    }
}

static void
draw_desktop_icon(RillShellState *shell, const RillPlatformServices *platform,
                  RillVisualState *visuals, int x, int y,
                  const char *label, const char *launcher_id, Color accent)
{
    Rectangle box;
    Rectangle icon;
    const RillLauncher *launcher;

    box = (Rectangle){x, y, 84, 82};
    icon = (Rectangle){x + 22, y + 5, 40, 40};
    launcher = launcher_by_id(shell, launcher_id);
    if(icon_hit_button(box, 7000 + x + y))
        open_launcher_id(shell, platform, launcher_id);
    draw_launcher_icon(visuals, launcher, icon, accent);
    draw_text_fit_centered(label, x + 4, y + 52, 76, Text12, GetThemeText());
}

static void
draw_desktop(RillShellState *shell, const RillPlatformServices *platform,
             RillVisualState *visuals)
{
    draw_desktop_icon(shell, platform, visuals, 28, PANEL_H + 28, "Home",
                      "files", GetThemeLink());
    draw_desktop_icon(shell, platform, visuals, 28, PANEL_H + 122, "Terminal",
                      "terminal", GetThemeButtonHover());
    draw_desktop_icon(shell, platform, visuals, 28, PANEL_H + 216, "Settings",
                      "settings", GetThemeIcon());
}

static void
draw_panel_separator(int x)
{
    DrawRectangle(x, 6, 1, PANEL_H - 12, Fade(GetThemeText(), 0.26f));
    DrawRectangle(x + 1, 6, 1, PANEL_H - 12, Fade(WHITE, 0.16f));
}

static int
panel_menu_button(RillShellState *shell, int menu_id, int x, int w,
                  const char *label, int id)
{
    ButtonStyle style;

    style = shell->menu_open == menu_id ? ButtonStylePrimary :
                                          ButtonStyleSecondary;
    if(Button((ButtonProps){(Rectangle){x, 3, w, 26}, label, style, Text12, id,
                            0})) {
        shell->menu_open = shell->menu_open == menu_id ? 0 : menu_id;
        return 1;
    }
    return 0;
}

static void
draw_quick_launcher(RillShellState *shell, const RillPlatformServices *platform,
                    RillVisualState *visuals, int x, const char *launcher_id,
                    int id)
{
    Rectangle bounds = {x, 4, 24, 24};
    Rectangle icon = {x + 3, 7, 18, 18};
    const RillLauncher *launcher = launcher_by_id(shell, launcher_id);

    if(icon_hit_button(bounds, id))
        open_launcher_id(shell, platform, launcher_id);
    draw_launcher_icon(visuals, launcher, icon, GetThemeText());
}

static void
draw_tray_indicator(int x, int kind, Color color)
{
    Rectangle r = {x, 7, 22, 18};

    DrawRectangleRounded(r, 0.08f, 6, Fade(GetThemeButton(), 0.42f));
    if(kind == 0) {
        DrawLine(x + 5, 16, x + 10, 11, color);
        DrawLine(x + 10, 11, x + 17, 11, color);
        DrawLine(x + 6, 17, x + 11, 13, color);
        DrawLine(x + 11, 13, x + 16, 13, color);
    } else if(kind == 1) {
        DrawRectangle(x + 5, 13, 4, 5, color);
        DrawTriangle((Vector2){x + 9, 13}, (Vector2){x + 15, 9},
                     (Vector2){x + 15, 21}, color);
        DrawCircleLines(x + 17, 15, 4, color);
    } else {
        DrawCircle(x + 11, 16, 5, color);
        DrawLine(x + 11, 9, x + 11, 6, color);
    }
}

static void
draw_task_icon(RillVisualState *visuals, RillShellState *shell,
               const RillTask *task, Rectangle icon);

static void
draw_top_panel(RillShellState *shell, const RillPlatformServices *platform,
               RillVisualState *visuals)
{
    char clock_text[32];
    time_t now;
    struct tm *local;
    int x;
    int right;
    int i;

    DrawRectangle(0, 0, GetScreenWidth(), PANEL_H, panel_color());
    DrawRectangle(0, PANEL_H - 1, GetScreenWidth(), 1,
                  Fade(GetThemeText(), 0.24f));

    x = 4;
    panel_menu_button(shell, 1, x, 108, "Applications", 100);
    x += 112;
    panel_menu_button(shell, 2, x, 62, "Places", 101);
    x += 66;
    panel_menu_button(shell, 3, x, 66, "System", 102);
    x += 72;
    draw_panel_separator(x);
    x += 8;
    draw_quick_launcher(shell, platform, visuals, x, "terminal", 130);
    x += 28;
    draw_quick_launcher(shell, platform, visuals, x, "files", 131);
    x += 32;
    draw_panel_separator(x);
    x += 8;

    right = GetScreenWidth() - 176;
    for(i = 0; i < shell->task_count && x < right - 112; i++) {
        Rectangle task_rect;
        int hover;
        int width;

        width = 148;
        task_rect = (Rectangle){x, 3, width, 26};
        hover = CheckCollisionPointRec(GetMousePosition(), task_rect);
        DrawRectangleRounded(task_rect, 0.06f, 6,
                             shell->tasks[i].focused ?
                                 GetThemeButtonHover() :
                                 Fade(GetThemeButton(), hover ? 0.72f : 0.46f));
        DrawRectangleRoundedLinesEx(task_rect, 0.06f, 6, 1.0f,
                                    Fade(GetThemeText(), 0.22f));
        draw_task_icon(visuals, shell, &shell->tasks[i],
                       (Rectangle){x + 5, 7, 18, 18});
        draw_text_fit(shell->tasks[i].title, x + 28, 9, width - 34, Text12,
                      GetThemeText());
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            RillShellSelectTask(shell, i);
            RillShellFocusSelectedTask(shell, platform);
        }
        x += width + 6;
    }

    now = time(NULL);
    local = localtime(&now);
    if(local != NULL)
        strftime(clock_text, sizeof(clock_text), "%H:%M", local);
    else
        snprintf(clock_text, sizeof(clock_text), "--:--");

    draw_panel_separator(right - 8);
    draw_tray_indicator(right, 0, GetThemeLink());
    draw_tray_indicator(right + 28, 1, GetThemeIcon());
    draw_tray_indicator(right + 56, 2, GetThemeButtonHover());
    draw_text_fit(clock_text, right + 88, 8, 70, Text12, GetThemeText());
}

static void
draw_menu_panel(Rectangle menu)
{
    DrawRectangleRounded(menu, 0.02f, 6, Fade(GetThemeSurface(), 0.98f));
    DrawRectangleRoundedLinesEx(menu, 0.02f, 6, 1.0f,
                                Fade(GetThemeText(), 0.30f));
}

static void
draw_applications_menu(RillShellState *shell,
                       const RillPlatformServices *platform,
                       RillVisualState *visuals)
{
    Rectangle menu;
    int i;
    int y;

    if(shell->menu_open != 1)
        return;

    menu = (Rectangle){4, PANEL_H + 2, 238, 10 + shell->launcher_count * 32};
    draw_menu_panel(menu);

    y = PANEL_H + 8;
    for(i = 0; i < shell->launcher_count; i++) {
        Rectangle row = {10, y, 226, 28};
        int hover = CheckCollisionPointRec(GetMousePosition(), row);

        DrawRectangleRounded(row, 0.04f, 5,
                             Fade(GetThemeButton(), hover ? 0.72f : 0.34f));
        draw_launcher_icon(visuals, &shell->launchers[i],
                           (Rectangle){row.x + 6, row.y + 5, 18, 18},
                           GetThemeText());
        draw_text_fit(shell->launchers[i].name, (int)row.x + 30,
                      (int)row.y + 8, (int)row.width - 38, Text12,
                      GetThemeText());
        if(hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            RillShellSelectLauncher(shell, i);
            RillShellLaunchSelected(shell, platform);
            shell->menu_open = 0;
        }
        y += 34;
    }
}

static void
draw_task_icon(RillVisualState *visuals, RillShellState *shell,
               const RillTask *task, Rectangle icon)
{
    const RillLauncher *launcher = NULL;

    if(task != NULL) {
        for(int i = 0; i < shell->app_count; i++) {
            if(shell->apps[i].id == task->id) {
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
draw_places_menu(RillShellState *shell, const RillPlatformServices *platform)
{
    Rectangle menu;
    const char *items[] = {"Home", "Desktop", "File System"};
    int y;

    if(shell->menu_open != 2)
        return;
    menu = (Rectangle){116, PANEL_H + 2, 190, 106};
    draw_menu_panel(menu);
    y = PANEL_H + 8;
    for(int i = 0; i < 3; i++) {
        if(Button((ButtonProps){(Rectangle){122, y, 178, 28}, items[i],
                                ButtonStyleSecondary, Text12, 1400 + i, 0})) {
            open_launcher_id(shell, platform, "files");
            shell->menu_open = 0;
        }
        y += 32;
    }
}

static void
draw_system_menu(RillShellState *shell, const RillPlatformServices *platform)
{
    Rectangle menu;

    if(shell->menu_open != 3)
        return;
    menu = (Rectangle){182, PANEL_H + 2, 190, 104};
    draw_menu_panel(menu);
    if(Button((ButtonProps){(Rectangle){188, PANEL_H + 8, 178, 28},
                            "Settings", ButtonStyleSecondary, Text12, 1500,
                            0})) {
        open_launcher_id(shell, platform, "settings");
        shell->menu_open = 0;
    }
    if(Button((ButtonProps){(Rectangle){188, PANEL_H + 40, 178, 28},
                            "About Rill", ButtonStyleSecondary, Text12, 1501,
                            0})) {
        open_launcher_id(shell, platform, "about");
        shell->menu_open = 0;
    }
    if(Button((ButtonProps){(Rectangle){188, PANEL_H + 72, 178, 28},
                            "Log Out", ButtonStyleSecondary, Text12, 1502,
                            0})) {
        RillShellSetStatus(shell, "Log out");
        shell->menu_open = 0;
    }
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
    if(app->y < PANEL_H)
        app->y = PANEL_H;
    if(app->x > max_x)
        app->x = max_x;
    if(app->y > max_y)
        app->y = max_y;
}

static void
process_window_mouse(RillShellState *shell)
{
    Vector2 mouse;

    if(shell == NULL)
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
              (app->kind == RILL_APP_TERMINAL ? "kapsule" : "shelf");
    module = load_host_module(visuals, host_id);
    if(module == NULL || module->host == NULL) {
        draw_text_fit("Host module not installed", (int)content.x + 16,
                      (int)content.y + 18, (int)content.width - 32, Text14,
                      GetThemeText());
        draw_text_fit(host_id, (int)content.x + 16, (int)content.y + 46,
                      (int)content.width - 32, Text12, GetThemeIcon());
        return;
    }

    mouse = GetMousePosition();
    delta = GetMouseDelta();
    input = (KryonInputOverride){0};
    input.enabled = 1;
    input.mouse_inside = app != NULL && app->focused &&
                         CheckCollisionPointRec(mouse, content);
    input.pass_buttons = app != NULL && app->focused;
    input.pass_keyboard = app != NULL && app->focused;
    input.mouse_position = mouse;
    input.mouse_delta = delta;

    SetAppHostFocused(module->host, app != NULL && app->focused);
    ResizeAppHost(module->host, (int)content.width, (int)content.height);
    BeginKryonInputOverride(input);
    DrawAppScreen(module->host, content);
    EndKryonInputOverride();
}

static void
draw_settings_app(Rectangle content, const RillVisualState *visuals)
{
    Text("Appearance", (int)content.x + 16, (int)content.y + 14, Text18,
         GetThemeText());
    draw_text_fit(visuals->system_theme_name, (int)content.x + 16,
                  (int)content.y + 48, (int)content.width - 32, Text14,
                  GetThemeText());
    draw_text_fit(visuals->system_font_name, (int)content.x + 16,
                  (int)content.y + 74, (int)content.width - 32, Text14,
                  GetThemeIcon());
    draw_text_fit(visuals->wallpaper_ready ? visuals->wallpaper_path :
                                             "Desktop background unavailable",
                  (int)content.x + 16, (int)content.y + 100,
                  (int)content.width - 32, Text12, GetThemeIcon());
    Text("XFCE settings", (int)content.x + 16, (int)content.y + 132,
         Text14, GetThemeText());
}

static void
draw_about_app(Rectangle content)
{
    Text("Rill", (int)content.x + 16, (int)content.y + 14, Text24,
         GetThemeText());
    Text("A Kryon/libdraw desktop inspired by rio and XFCE.",
         (int)content.x + 16, (int)content.y + 54, Text14, GetThemeText());
}

static void
draw_app_window(RillShellState *shell, RillAppWindow *app,
                RillVisualState *visuals)
{
    Rectangle frame;
    Rectangle title;
    Rectangle content;
    Color frame_color;

    frame = (Rectangle){app->x, app->y, app->w, app->h};
    title = (Rectangle){app->x, app->y, app->w, 30};
    content = (Rectangle){app->x + 1, app->y + 31, app->w - 2, app->h - 32};
    frame_color = app->focused ? GetThemeLink() : Fade(GetThemeText(), 0.32f);

    DrawRectangleRec(frame, opaque_color(GetThemeSurface()));
    DrawRectangleRounded(frame, 0.025f, 8, opaque_color(GetThemeSurface()));
    DrawRectangleRoundedLinesEx(frame, 0.025f, 8, 2.0f, frame_color);
    DrawRectangleRec(title, opaque_color(mix_color(GetThemeSurface(),
                                                  frame_color, 0.18f)));
    BeginScissorMode((int)title.x + 6, (int)title.y,
                     (int)title.width - 42, (int)title.height);
    Text(app->title, app->x + 10, app->y + 8, Text14, GetThemeText());
    EndScissorMode();
    if(Button((ButtonProps){(Rectangle){app->x + app->w - 30, app->y + 4,
                                        22, 22},
                            "x", ButtonStyleSecondary, Text12,
                            5000 + app->id, 0})) {
        RillShellCloseApp(shell, app->id);
        return;
    }

    BeginScissorMode((int)content.x, (int)content.y, (int)content.width,
                     (int)content.height);
    DrawRectangleRec(content, opaque_color(GetThemeBackground()));
    if(app->kind == RILL_APP_TERMINAL || app->kind == RILL_APP_FILES)
        draw_host_app(app, content, visuals);
    else if(app->kind == RILL_APP_SETTINGS)
        draw_settings_app(content, visuals);
    else
        draw_about_app(content);
    EndScissorMode();
}

static void
draw_apps(RillShellState *shell, RillVisualState *visuals)
{
    int i;

    for(i = 0; i < shell->app_count; i++)
        if(!shell->apps[i].focused)
            draw_app_window(shell, &shell->apps[i], visuals);
    for(i = 0; i < shell->app_count; i++)
        if(shell->apps[i].focused)
            draw_app_window(shell, &shell->apps[i], visuals);
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
    Text(title, (int)title_rect.x + 10, (int)title_rect.y + 8, Text14, WHITE);
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
    const Color red = {240, 16, 32, 255};
    const Color lower_bg = {18, 18, 22, 255};
    const Color lower_title = {94, 28, 36, 255};
    const Color upper_bg = {24, 172, 128, 255};
    const Color upper_title = {20, 92, 72, 255};

    ClearBackground((Color){8, 9, 12, 255});
    DrawRectangle(0, 0, GetScreenWidth(), PANEL_H, (Color){20, 22, 30, 255});
    Text("Rill visual test", 10, 8, Text12, WHITE);

    draw_test_window(lower, "Lower text producer", lower_title, lower_bg, 0);
    BeginScissorMode((int)lower_content.x, (int)lower_content.y,
                     (int)lower_content.width, (int)lower_content.height);
    for(int i = 0; i < 8; i++) {
        Text("TEXT-HIERARCHY-LEAK TEXT-HIERARCHY-LEAK",
             RILL_TEST_LOWER_TEXT_X, RILL_TEST_LOWER_TEXT_Y + i * 26, Text24,
             red);
    }
    EndScissorMode();

    draw_test_window(upper, "Upper opaque cover", upper_title, upper_bg, 1);
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
    return 0;
}

int
main(void)
{
    RillShellState shell;
    RillVisualState visuals;
    RillTestState test;
    const RillPlatformServices *platform;
    double next_refresh;

    init_test_state(&test);
    platform = RillPlatformCurrent();
    RillShellInit(&shell);
    RillShellRefresh(&shell, platform);
    RillShellSetStatus(&shell, "Ready");

    SetSingleInstance(0);
    InitWindow(RILL_WIDTH, RILL_HEIGHT, "Rill");
    if(!IsWindowReady()) {
        fprintf(stderr, "rill: failed to open Kryon window\n");
        CloseWindow();
        return 1;
    }
    if(WindowShouldClose()) {
        fprintf(stderr, "rill: Kryon window closed during startup\n");
        CloseWindow();
        return 1;
    }
    SetTargetFPS(60);
    SetUIDefaultFontAutoLoad(1);
    configure_system_look(&visuals, &test);

    next_refresh = 0.0;
    while(!WindowShouldClose()) {
        if(!test_scene_active(&test) && GetTime() >= next_refresh) {
            RillShellRefresh(&shell, platform);
            load_launcher_icons(&visuals, &shell);
            next_refresh = GetTime() + 1.0;
        }
        if(!test_scene_active(&test))
            process_window_mouse(&shell);

        BeginDrawing();
        ClearBackground(opaque_color(GetThemeBackground()));
        BeginUIFrame(GetScreenWidth(), GetScreenHeight(), 1.0f);
        BeginUI(0x52494c4c);

        if(test_scene_active(&test)) {
            if(!draw_test_scene(&test))
                RillShellSetStatus(&shell, "Unknown visual test scene");
        } else {
            draw_wallpaper(&visuals);
            draw_desktop(&shell, platform, &visuals);
            draw_apps(&shell, &visuals);
            draw_top_panel(&shell, platform, &visuals);
            draw_applications_menu(&shell, platform, &visuals);
            draw_places_menu(&shell, platform);
            draw_system_menu(&shell, platform);
        }

        EndUI();
        EndUIFrame();
        EndDrawing();

        if(test_scene_active(&test)) {
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
    if(visuals.wallpaper_ready)
        UnloadTexture(visuals.wallpaper);
    CloseWindow();
    return 0;
}
