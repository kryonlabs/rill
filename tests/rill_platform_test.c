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
    assert(platform->focus_task(7));
    assert(platform->close_task(7));
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
