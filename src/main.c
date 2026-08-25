#include "rill_shell.h"

#include "kryon.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    RILL_WIDTH = 1120,
    RILL_HEIGHT = 720,
    PANEL_H = 32,
    RILL_ICON_CACHE_MAX = 32
};

typedef struct RillIconCacheEntry {
    char path[512];
    Texture2D texture;
    int ready;
} RillIconCacheEntry;

typedef struct RillVisualState {
    Texture2D wallpaper;
    int wallpaper_ready;
    AppHost *kapsule_host;
    RillIconCacheEntry icons[RILL_ICON_CACHE_MAX];
    int icon_count;
    char system_theme_name[128];
    char wallpaper_path[512];
    char system_font_name[128];
    char system_font_path[512];
} RillVisualState;

extern AppHost *CreateAppHost(int abi_version, const char *project_path);
extern void DestroyAppHost(AppHost *host);

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
panel_color(void)
{
    return mix_color(GetThemeSurface(), GetThemeBackground(), 0.10f);
}

static void
configure_system_look(RillVisualState *visuals)
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
                      GetScreenHeight() - PANEL_H, GetThemeBackground());
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
    draw_text_fit(label, x + 4, y + 52, 76, Text12, GetThemeText());
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

static void
draw_files_app(Rectangle content)
{
    Text("Files", (int)content.x + 16, (int)content.y + 14, Text18,
         GetThemeText());
    Text("/home /mnt /tmp", (int)content.x + 16, (int)content.y + 48, Text14,
         GetThemeIcon());
    DrawRectangleRounded((Rectangle){content.x + 16, content.y + 82,
                                     content.width - 32, 36},
                         0.04f, 8, Fade(GetThemeButton(), 0.82f));
    Text("Filesystem browser will use Plan 9 namespaces and XDG paths.",
         (int)content.x + 28, (int)content.y + 92, Text14, GetThemeText());
}

static void
draw_terminal_app(RillAppWindow *app, Rectangle content,
                  RillVisualState *visuals)
{
    Vector2 mouse;
    Vector2 delta;
    KryonInputOverride input;

    if(visuals == NULL || visuals->kapsule_host == NULL) {
        return;
    }

    mouse = GetMousePosition();
    delta = GetMouseDelta();
    input.enabled = 1;
    input.mouse_inside = app != NULL && app->focused &&
                         CheckCollisionPointRec(mouse, content);
    input.pass_buttons = app != NULL && app->focused;
    input.mouse_position = mouse;
    input.mouse_delta = delta;

    SetAppHostFocused(visuals->kapsule_host, app != NULL && app->focused);
    ResizeAppHost(visuals->kapsule_host, (int)content.width,
                  (int)content.height);
    BeginKryonInputOverride(input);
    DrawAppScreen(visuals->kapsule_host, content);
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

    DrawRectangleRounded(frame, 0.025f, 8, Fade(GetThemeSurface(), 0.97f));
    DrawRectangleRoundedLinesEx(frame, 0.025f, 8, 2.0f, frame_color);
    DrawRectangleRec(title, mix_color(GetThemeSurface(), frame_color, 0.18f));
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
    if(app->kind == RILL_APP_TERMINAL)
        draw_terminal_app(app, content, visuals);
    else if(app->kind == RILL_APP_FILES)
        draw_files_app(content);
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

int
main(void)
{
    RillShellState shell;
    RillVisualState visuals;
    const RillPlatformServices *platform;
    double next_refresh;

    platform = RillPlatformCurrent();
    RillShellInit(&shell);
    RillShellRefresh(&shell, platform);
    RillShellSetStatus(&shell, "Ready");

    InitWindow(RILL_WIDTH, RILL_HEIGHT, "Rill");
    SetTargetFPS(60);
    SetUIDefaultFontAutoLoad(1);
    configure_system_look(&visuals);
    visuals.kapsule_host = CreateAppHost(APP_HOST_ABI_VERSION,
                                         "vendor/kapsule");
    if(visuals.kapsule_host == NULL) {
        fprintf(stderr, "rill: failed to create Kapsule app host\n");
        CloseWindow();
        return 1;
    }

    next_refresh = 0.0;
    while(!WindowShouldClose()) {
        if(GetTime() >= next_refresh) {
            RillShellRefresh(&shell, platform);
            load_launcher_icons(&visuals, &shell);
            next_refresh = GetTime() + 1.0;
        }

        BeginDrawing();
        ClearBackground(GetThemeBackground());
        draw_wallpaper(&visuals);
        BeginUIFrame(GetScreenWidth(), GetScreenHeight(), 1.0f);
        BeginUI(0x52494c4c);

        draw_desktop(&shell, platform, &visuals);
        draw_apps(&shell, &visuals);
        draw_top_panel(&shell, platform, &visuals);
        draw_applications_menu(&shell, platform, &visuals);
        draw_places_menu(&shell, platform);
        draw_system_menu(&shell, platform);

        EndUI();
        EndUIFrame();
        EndDrawing();
    }

    if(visuals.kapsule_host != NULL)
        DestroyAppHost(visuals.kapsule_host);
    for(int i = 0; i < visuals.icon_count; i++)
        if(visuals.icons[i].ready)
            UnloadTexture(visuals.icons[i].texture);
    if(visuals.wallpaper_ready)
        UnloadTexture(visuals.wallpaper);
    CloseWindow();
    return 0;
}
