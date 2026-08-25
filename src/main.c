#include "rill_shell.h"

#include "kryon.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    RILL_WIDTH = 1120,
    RILL_HEIGHT = 720,
    PANEL_H = 32
};

typedef struct RillVisualState {
    Texture2D wallpaper;
    int wallpaper_ready;
    char system_theme_name[128];
    char wallpaper_path[512];
    char system_font_name[128];
    char system_font_path[512];
} RillVisualState;

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
    SetCurrentTheme(THEME_PLAN9, SystemThemePrefersDark());
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
        DrawRectangleGradientV(0, PANEL_H, GetScreenWidth(),
                               GetScreenHeight() - PANEL_H,
                               mix_color(GetThemeBackground(), GetThemeLink(),
                                         0.26f),
                               mix_color(GetThemeBackground(), GetThemeSurface(),
                                         0.44f));
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
                  int x, int y, const char *label, const char *launcher_id,
                  Color accent)
{
    Rectangle box;

    box = (Rectangle){x, y, 84, 82};
    DrawRectangleRounded((Rectangle){x + 16, y + 3, 52, 42}, 0.08f, 8,
                         Fade(GetThemeSurface(), 0.88f));
    DrawRectangleRoundedLines((Rectangle){x + 16, y + 3, 52, 42}, 0.08f, 8,
                              Fade(GetThemeText(), 0.25f));
    DrawRectangle(x + 28, y + 15, 28, 18, accent);
    DrawRectangle(x + 33, y + 10, 18, 8, mix_color(accent, WHITE, 0.35f));
    if(Button((ButtonProps){(Rectangle){box.x + 4, box.y + 50, box.width - 8,
                                        26},
                            label, ButtonStyleSecondary, Text12,
                            7000 + x + y, 0})) {
        open_launcher_id(shell, platform, launcher_id);
    }
}

static void
draw_desktop(RillShellState *shell, const RillPlatformServices *platform)
{
    draw_desktop_icon(shell, platform, 28, PANEL_H + 28, "Home", "files",
                      GetThemeLink());
    draw_desktop_icon(shell, platform, 28, PANEL_H + 122, "Terminal", "terminal",
                      GetThemeButtonHover());
    draw_desktop_icon(shell, platform, 28, PANEL_H + 216, "Settings", "settings",
                      GetThemeIcon());
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
                    int x, const char *label, const char *launcher_id,
                    int id)
{
    if(Button((ButtonProps){(Rectangle){x, 4, 24, 24}, label,
                            ButtonStyleSecondary, Text12, id, 0}))
        open_launcher_id(shell, platform, launcher_id);
}

static void
draw_tray_indicator(int x, const char *label, Color color)
{
    DrawRectangleRounded((Rectangle){x, 7, 22, 18}, 0.08f, 6,
                         Fade(GetThemeButton(), 0.72f));
    DrawRectangle(x + 5, 12, 12, 8, color);
    if(label != NULL && label[0] != '\0')
        Text(label, x + 4, 9, Text8, GetThemeText());
}

static void
draw_top_panel(RillShellState *shell, const RillPlatformServices *platform)
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
    draw_quick_launcher(shell, platform, x, "T", "terminal", 130);
    x += 28;
    draw_quick_launcher(shell, platform, x, "F", "files", 131);
    x += 32;
    draw_panel_separator(x);
    x += 8;

    right = GetScreenWidth() - 176;
    for(i = 0; i < shell->task_count && x < right - 112; i++) {
        ButtonStyle style;
        int width;

        width = 148;
        style = shell->tasks[i].focused ? ButtonStylePrimary :
                                          ButtonStyleSecondary;
        if(Button((ButtonProps){(Rectangle){x, 3, width, 26},
                                shell->tasks[i].title, style, Text12,
                                3000 + shell->tasks[i].id, 0})) {
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
    draw_tray_indicator(right, "N", GetThemeLink());
    draw_tray_indicator(right + 28, "V", GetThemeIcon());
    draw_tray_indicator(right + 56, "", GetThemeButtonHover());
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
                       const RillPlatformServices *platform)
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
        if(Button((ButtonProps){(Rectangle){10, y, 226, 28},
                                shell->launchers[i].name,
                                ButtonStyleSecondary, Text12, 1100 + i, 0})) {
            RillShellSelectLauncher(shell, i);
            RillShellLaunchSelected(shell, platform);
            shell->menu_open = 0;
        }
        y += 34;
    }
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
                const RillVisualState *visuals)
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
    Text(app->title, app->x + 10, app->y + 8, Text14, GetThemeText());
    if(Button((ButtonProps){(Rectangle){app->x + app->w - 30, app->y + 4,
                                        22, 22},
                            "x", ButtonStyleSecondary, Text12,
                            5000 + app->id, 0})) {
        RillShellCloseApp(shell, app->id);
        return;
    }

    if(app->kind == RILL_APP_FILES)
        draw_files_app(content);
    else if(app->kind == RILL_APP_SETTINGS)
        draw_settings_app(content, visuals);
    else
        draw_about_app(content);
}

static void
draw_apps(RillShellState *shell, const RillVisualState *visuals)
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

    next_refresh = 0.0;
    while(!WindowShouldClose()) {
        if(GetTime() >= next_refresh) {
            RillShellRefresh(&shell, platform);
            next_refresh = GetTime() + 1.0;
        }

        BeginDrawing();
        ClearBackground(GetThemeBackground());
        draw_wallpaper(&visuals);
        BeginUIFrame(GetScreenWidth(), GetScreenHeight(), 1.0f);
        BeginUI(0x52494c4c);

        draw_desktop(&shell, platform);
        draw_apps(&shell, &visuals);
        draw_top_panel(&shell, platform);
        draw_applications_menu(&shell, platform);
        draw_places_menu(&shell, platform);
        draw_system_menu(&shell, platform);

        EndUI();
        EndUIFrame();
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
