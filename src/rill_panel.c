#include "rill_panel.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct RillPanelPluginKindMap {
    RillPanelPluginKind kind;
    const char *name;
} RillPanelPluginKindMap;

static const RillPanelPluginKindMap kind_names[] = {
    {RILL_PANEL_SEPARATOR, "separator"},
    {RILL_PANEL_MENU, "menu"},
    {RILL_PANEL_LAUNCHER, "launcher"},
    {RILL_PANEL_TASK_LIST, "task-list"},
    {RILL_PANEL_WORKSPACES, "workspaces"},
    {RILL_PANEL_TRAY, "tray"},
    {RILL_PANEL_LANGUAGE, "language"},
    {RILL_PANEL_CLOCK, "clock"},
    {RILL_PANEL_RESOURCE, "resource"}
};

static const RillPanelPlugin left_plugins[] = {
    {RILL_PANEL_MENU, "applications", "Applications", "", 1, 104, 106, 0},
    {RILL_PANEL_SEPARATOR, "sep-tasks", "", "", 0, 0, 6, 0},
    {RILL_PANEL_TASK_LIST, "task-list", "", "", 0, 0, 0, 0}
};

static const RillPanelPlugin right_plugins[] = {
    {RILL_PANEL_WORKSPACES, "workspaces", "", "", 0, 42, 42, 0},
    {RILL_PANEL_SEPARATOR, "sep-status", "", "", 0, 0, 8, 0},
    {RILL_PANEL_CLOCK, "clock", "", "", 0, 60, 64, 0}
};

const RillPanelPlugin *
RillPanelDefaultLeft(int *count)
{
    if(count != NULL)
        *count = (int)(sizeof(left_plugins) / sizeof(left_plugins[0]));
    return left_plugins;
}

const RillPanelPlugin *
RillPanelDefaultRight(int *count)
{
    if(count != NULL)
        *count = (int)(sizeof(right_plugins) / sizeof(right_plugins[0]));
    return right_plugins;
}

const char *
RillPanelPluginKindName(RillPanelPluginKind kind)
{
    for(int i = 0; i < (int)(sizeof(kind_names) / sizeof(kind_names[0])); i++)
        if(kind_names[i].kind == kind)
            return kind_names[i].name;
    return "unknown";
}

int
RillPanelPluginKindFromName(const char *name, RillPanelPluginKind *out)
{
    if(name == NULL)
        return 0;
    for(int i = 0; i < (int)(sizeof(kind_names) / sizeof(kind_names[0])); i++) {
        if(strcmp(kind_names[i].name, name) == 0) {
            if(out != NULL)
                *out = kind_names[i].kind;
            return 1;
        }
    }
    return 0;
}

/* Length-prefixed strings allow labels containing whitespace without escaping. */
static int
write_string(FILE *f, const char *value)
{
    size_t len = strlen(value);
    return fprintf(f, "%lu:", (unsigned long)len) > 0 &&
           fwrite(value, 1, len, f) == len && fputc('\n', f) != EOF;
}

static int
read_string(FILE *f, char *value, int cap)
{
    unsigned int len;
    if(fscanf(f, "%u", &len) != 1 || fgetc(f) != ':' || len >= (unsigned int)cap)
        return 0;
    if(fread(value, 1, len, f) != len || fgetc(f) != '\n') return 0;
    value[len] = '\0';
    return strlen(value) == len;
}

int
RillPanelLoad(const char *path, RillPanelPlugin *left, int *left_count,
              RillPanelPlugin *right, int *right_count, int cap)
{
    FILE *f;
    char magic[32], kind[32];
    RillPanelPlugin *items;
    int counts[2], ok = 0, i, side;
    if(path == NULL || cap <= 0 || cap > 1024) return 0;
    f = fopen(path, "r");
#ifdef KRYON_NATIVE_PLAN9
    if(f == NULL) {
        char backup[1200];
        snprintf(backup, sizeof(backup), "%s.old", path);
        f = fopen(backup, "r");
    }
#endif
    if(f == NULL) return 0;
    items = calloc(2 * cap, sizeof(*items));
    if(items == NULL) { fclose(f); return 0; }
    if(fgets(magic, sizeof(magic), f) == NULL || strcmp(magic, "Rill panel 1\n") != 0)
        goto done;
    if(fscanf(f, "%d %d", &counts[0], &counts[1]) != 2 || fgetc(f) != '\n' ||
       counts[0] < 0 || counts[1] < 0 || counts[0] > cap || counts[1] > cap)
        goto done;
    for(side = 0; side < 2; side++) {
        for(i = 0; i < counts[side]; i++) {
            RillPanelPlugin *p = &items[side * cap + i];
            if(fscanf(f, "%31s %d %d %d %d", kind, &p->menu_id, &p->width,
                       &p->advance, &p->variant) != 5 || fgetc(f) != '\n' ||
               !RillPanelPluginKindFromName(kind, &p->kind) ||
               p->width < 0 || p->width > 4096 || p->advance < 0 || p->advance > 4096 ||
               p->menu_id < 0 || p->menu_id > 3 || p->variant < 0 || p->variant > 32 ||
               !read_string(f, p->id, sizeof(p->id)) ||
               !read_string(f, p->label, sizeof(p->label)) ||
               !read_string(f, p->launcher_id, sizeof(p->launcher_id)))
                goto done;
        }
    }
    if(fgetc(f) != EOF || ferror(f)) goto done;
    memcpy(left, items, counts[0] * sizeof(*items));
    memcpy(right, items + cap, counts[1] * sizeof(*items));
    *left_count = counts[0]; *right_count = counts[1];
    ok = 1;
done:
    free(items);
    fclose(f);
    return ok;
}

int
RillPanelSave(const char *path, const RillPanelPlugin *left, int left_count,
              const RillPanelPlugin *right, int right_count)
{
    FILE *f;
    int i, side, ok;
    char temporary[1200];
    if(path == NULL || strlen(path) + 5 >= sizeof(temporary)) return 0;
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    f = fopen(temporary, "w");
    if(f == NULL) return 0;
    ok = fprintf(f, "Rill panel 1\n%d %d\n", left_count, right_count) > 0;
    for(side = 0; side < 2 && ok; side++) {
        const RillPanelPlugin *items = side == 0 ? left : right;
        int count = side == 0 ? left_count : right_count;
        for(i = 0; i < count && ok; i++) {
            const RillPanelPlugin *p = &items[i];
            ok = fprintf(f, "%s %d %d %d %d\n", RillPanelPluginKindName(p->kind),
                          p->menu_id, p->width, p->advance, p->variant) > 0 &&
                 write_string(f, p->id) && write_string(f, p->label) &&
                 write_string(f, p->launcher_id);
        }
    }
    if(fclose(f) != 0) ok = 0;
#ifdef KRYON_NATIVE_PLAN9
    if(ok) {
        char backup[1200];
        Dir *existing;
        Dir change;
        const char *base;
        int moved = 0;
        snprintf(backup, sizeof(backup), "%s.old", path);
        existing = dirstat((char *)path);
        if(existing != nil) {
            free(existing);
            remove(backup);
            nulldir(&change);
            base = strrchr(backup, '/');
            change.name = (char *)(base != NULL ? base + 1 : backup);
            if(dirwstat((char *)path, &change) < 0) ok = 0;
            else moved = 1;
        }
        if(ok) {
            nulldir(&change);
            base = strrchr(path, '/');
            change.name = (char *)(base != NULL ? base + 1 : path);
            if(dirwstat(temporary, &change) >= 0) {
                remove(backup);
                return 1;
            }
            if(moved) dirwstat(backup, &change);
        }
    }
#else
    if(ok && rename(temporary, path) == 0) return 1;
#endif
    remove(temporary);
    return 0;
}
