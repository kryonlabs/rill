#include "rill_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[] = "/tmp/rill-windows.XXXXXX";

static void
put(int id, const char *name, const char *text)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%d/%s", root, id, name);
    f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void
expect(int id, const char *name, const char *text)
{
    char path[256], content[256];
    FILE *f;
    size_t n;
    snprintf(path, sizeof(path), "%s/%d/%s", root, id, name);
    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(content, 1, sizeof(content) - 1, f);
    content[n] = '\0';
    fclose(f);
    assert(strcmp(content, text) == 0);
}

int
main(void)
{
    const RillPlatformServices *platform = RillPlatformCurrent();
    RillTask tasks[4];
    char path[256];
    int id, count, editor, terminal;
    const char *files[] = {"label", "winfo", "wctl"};

    assert(mkdtemp(root) != NULL);
    assert(setenv("RILL_WSYS_DIR", root, 1) == 0);
    for(id = 1; id <= 3; id++) {
        snprintf(path, sizeof(path), "%s/%d", root, id);
        assert(mkdir(path, 0700) == 0);
        put(id, "wctl", "");
    }
    put(1, "label", "Rill");
    put(1, "winfo", "0 0 1024 768 notcurrent visible desktop\n");
    put(2, "label", "Text Editor\n");
    put(2, "winfo", "32 32 680 548 current visible window\n");
    /* Opposite focus in wctl: snapshots must take precedence. */
    put(2, "wctl", "0 0 0 0 notcurrent visible");
    put(3, "label", "Terminal");
    put(3, "winfo", "0 0 0 0 notcurrent hidden window\n");
    count = platform->list_tasks(tasks, 4);
    assert(count == 2);
    editor = tasks[0].id == 2 ? 0 : 1;
    terminal = 1 - editor;
    assert(tasks[editor].id == 2 && tasks[editor].focused);
    assert(strcmp(tasks[editor].title, "Text Editor") == 0);
    assert(tasks[terminal].id == 3 && !tasks[terminal].focused);
    assert(platform->list_tasks(tasks, 1) == 1);
    assert(platform->list_tasks(NULL, 4) == 0);
    put(2, "wctl", "");
    assert(platform->focus_task(2));
    expect(2, "wctl", "activate");
    assert(platform->focus_task(3));
    expect(3, "wctl", "activate");
    assert(!platform->focus_task(-1));
    assert(!platform->focus_task(9999));
    /* An original rio without snapshots can focus a visible window. */
    snprintf(path, sizeof(path), "%s/2/winfo", root);
    assert(unlink(path) == 0);
    put(2, "wctl", "current visible");
    assert(platform->focus_task(2));
    expect(2, "wctl", "current visible");
    for(id = 1; id <= 3; id++) {
        for(int i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), "%s/%d/%s", root, id, files[i]);
            unlink(path);
        }
        snprintf(path, sizeof(path), "%s/%d", root, id);
        assert(rmdir(path) == 0);
    }
    assert(platform->list_tasks(tasks, 4) == 0);
    assert(rmdir(root) == 0);
    puts("rill Plan 9 window tests passed");
    return 0;
}
