#include "kryon_plan9.h"
#include "rill_panel.h"
#include "rill_platform.h"

static void
check(int ok, char *message)
{
    if(!ok) {
        fprint(2, "rill native test: %s: %r\n", message);
        exits("test failed");
    }
}

void
main(void)
{
    RillPanelPlugin left[2], right[2], loaded[2], other[2];
    char directory[128], path[160], backup[160];
    Dir rename;
    int left_count = 0, right_count = 0;

    snprint(directory, sizeof(directory), "/tmp/rill-panel-test-%d", getpid());
    check(RillSettingsEnsureDirectory(directory), "create settings directory");
    snprint(path, sizeof(path), "%s/panel", directory);
    snprint(backup, sizeof(backup), "%s/panel.old", directory);
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    left[0].kind = RILL_PANEL_CLOCK;
    strcpy(left[0].id, "clock");
    strcpy(left[0].label, "First");
    left[0].width = 60;
    left[0].advance = 64;
    check(RillPanelSave(path, left, 1, right, 0), "initial save");
    strcpy(left[0].label, "Second");
    check(RillPanelSave(path, left, 1, right, 0), "replace existing settings");
    check(RillPanelLoad(path, loaded, &left_count, other, &right_count, 2), "load replacement");
    check(left_count == 1 && right_count == 0 && strcmp(loaded[0].label, "Second") == 0,
          "replacement contents");
    /* Simulate interruption after moving the previous settings aside. */
    nulldir(&rename);
    rename.name = "panel.old";
    check(dirwstat(path, &rename) >= 0, "move settings to recovery file");
    check(RillPanelLoad(path, loaded, &left_count, other, &right_count, 2), "recover interrupted save");
    check(strcmp(loaded[0].label, "Second") == 0, "recovered contents");
    check(RillPanelSave(path, left, 0, right, 0), "save empty panel after recovery");
    check(RillPanelLoad(path, loaded, &left_count, other, &right_count, 2), "load empty panel");
    check(left_count == 0 && right_count == 0, "empty panel retained");
    remove(path);
    remove(backup);
    remove(directory);
    print("RILL_NATIVE_TEST_OK\n");
    exits(nil);
}
