#include "rill_platform.h"
#include "rill_panel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

int main(void)
{
    char root[] = "/tmp/rill-platform.XXXXXX", path[1024], window[1024];
    RillPanelPlugin left[4], right[4], loaded_left[4], loaded_right[4];
    RillTask tasks[4];
    int nl = 0, nr = 0, count;
    const RillPlatformServices *platform = RillPlatformCurrent();
    assert(mkdtemp(root) != NULL);
    snprintf(path, sizeof(path), "%s/config/rill", root);
    assert(RillSettingsEnsureDirectory(path));
    assert(RillSettingsEnsureDirectory(path));
    setenv("home", root, 1);
    snprintf(path, sizeof(path), "%s/lib/rill", root);
    assert(strcmp(platform->settings_root(), path) == 0);
    left[0] = (RillPanelPlugin){RILL_PANEL_MENU, "applications", "Applications menu", "", 1, 104, 106, 0};
    right[0] = (RillPanelPlugin){RILL_PANEL_CLOCK, "clock", "Time\nDate", "", 0, 60, 64, 0};
    snprintf(path, sizeof(path), "%s/panel", root);
    assert(RillPanelSave(path, left, 1, right, 1));
    assert(RillPanelLoad(path, loaded_left, &nl, loaded_right, &nr, 4));
    assert(nl == 1 && nr == 1);
    assert(strcmp(loaded_left[0].label, "Applications menu") == 0);
    assert(strcmp(loaded_right[0].label, "Time\nDate") == 0);
    put(path, "Rill panel 1\n2 0\nclock 0 20 20 0\n999999:bad\n");
    assert(!RillPanelLoad(path, loaded_left, &nl, loaded_right, &nr, 4));
    assert(nl == 1 && nr == 1 && loaded_left[0].kind == RILL_PANEL_MENU);
    assert(RillPanelSave(path, left, 0, right, 0));
    assert(RillPanelLoad(path, loaded_left, &nl, loaded_right, &nr, 4));
    assert(nl == 0 && nr == 0);
    unlink(path);

    /* Settings store round-trip. */
    {
        RillSettings settings, loaded;
        snprintf(path, sizeof(path), "%s/settings", root);
        RillSettingsInit(&settings);
        RillSettingsSet(&settings, "clock-format", "%a %H:%M");
        RillSettingsSetInteger(&settings, "panel-height", 32);
        RillSettingsSet(&settings, "clock-format", "%H:%M");
        assert(settings.count == 2);
        assert(RillSettingsSave(&settings, path));
        assert(RillSettingsLoad(&loaded, path));
        assert(loaded.count == 2);
        assert(strcmp(RillSettingsGet(&loaded, "clock-format", "bad"),
                      "%H:%M") == 0);
        assert(RillSettingsGetInteger(&loaded, "panel-height", 0) == 32);
        RillSettings first = loaded, second = loaded;
        RillSettings first_previous = loaded, second_previous = loaded;
        RillSettingsSetInteger(&first, "panel-height", 40);
        assert(RillSettingsMergeSave(&first, &first_previous, path));
        RillSettingsSet(&second, "wallpaper", "/tmp/another image.png");
        assert(RillSettingsMergeSave(&second, &second_previous, path));
        assert(RillSettingsLoad(&loaded, path));
        assert(RillSettingsGetInteger(&loaded, "panel-height", 0) == 40);
        assert(strcmp(RillSettingsGet(&loaded, "wallpaper", ""), "/tmp/another image.png") == 0);
        RillSettingsSet(&second, "invalid", "one\ntwo");
        assert(!RillSettingsSave(&second, path));
        assert(RillSettingsLoad(&loaded, path));
        assert(RillSettingsGet(&loaded, "invalid", NULL) == NULL);
        snprintf(window, sizeof(window), "%s/settings.lock", root);
        unlink(window);
        put(path, "wallpaper = /tmp/photo with spaces.png\n# comment\n\nnoise-without-equals\n");
        assert(RillSettingsLoad(&loaded, path));
        assert(loaded.count == 1);
        assert(strcmp(RillSettingsGet(&loaded, "wallpaper", "none"),
                      "/tmp/photo with spaces.png") == 0);
        assert(strcmp(RillSettingsGet(&loaded, "missing", "fallback"),
                      "fallback") == 0);
        unlink(path);
    }

    setenv("RILL_WSYS_DIR", root, 1);
    snprintf(window, sizeof(window), "%s/7", root);
    assert(RillSettingsEnsureDirectory(window));
    snprintf(path, sizeof(path), "%s/7/label", root);
    put(path, "Acme\n");
    snprintf(path, sizeof(path), "%s/7/wctl", root);
    put(path, "0 0 640 480 visible current\n");
    count = platform->list_tasks(tasks, 4);
    assert(count == 1 && tasks[0].id == 7 && tasks[0].focused);
    assert(strcmp(tasks[0].title, "Acme") == 0);
    put(path, "0 0 640 480 hidden notcurrent\n");
    assert(platform->list_tasks(tasks, 4) == 1 && !tasks[0].focused);
    /* Discovery is not authority to control a foreign process's window. */
    assert(!platform->focus_task(7));
    assert(!platform->close_task(7));
    assert(!platform->focus_task(-1));
    assert(!platform->close_task(999));
    unlink(path);
    snprintf(path, sizeof(path), "%s/7/label", root);
    unlink(path);
    rmdir(window);
    snprintf(path, sizeof(path), "%s/config/rill", root); rmdir(path);
    snprintf(path, sizeof(path), "%s/config", root); rmdir(path);
    assert(rmdir(root) == 0);
    return 0;
}
