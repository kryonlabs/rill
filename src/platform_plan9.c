#include "rill_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KRYON_NATIVE_PLAN9
#include "kryon_plan9.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#define PLAN9_SYSTEM_APPLICATIONS "/lib/rill/applications"
#define PLAN9_SYSTEM_ICON_MAP "/lib/rill/icon.map"

typedef struct Plan9IconMapEntry {
    char key[64];
    char path[512];
} Plan9IconMapEntry;

static Plan9IconMapEntry icon_map[64];
static int icon_map_count;

static void
launcher(RillLauncher *out, const char *id, const char *name,
         const char *description, const char *category, const char *command,
         int favorite)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->description, sizeof(out->description), "%s", description);
    snprintf(out->category, sizeof(out->category), "%s", category);
    snprintf(out->command, sizeof(out->command), "%s", command);
    out->favorite = favorite;
}

static void
trim_line(char *line)
{
    int n;

    n = strlen(line);
    while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
}

static void
take_field(char *dst, int dst_size, const char *value)
{
    int len = strlen(value);
    if(dst_size <= 0) return;
    if(len >= dst_size) len = dst_size - 1;
    memcpy(dst, value, len);
    dst[len] = '\0';
}

static void
take_category(char *dst, int dst_size, const char *value)
{
    int i;

    for(i = 0; value[i] != '\0' && value[i] != ';' && i + 1 < dst_size; i++)
        dst[i] = value[i];
    dst[i] = '\0';
}

static int
parse_boolean(const char *value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

/* Entries from later files replace earlier ones with the same Id, so a
 * user's $home/lib/rill/applications can override the system registry. */
static void
store_launcher(RillLauncher *out, int cap, int *count,
               const RillLauncher *entry)
{
    int i;

    if(entry->id[0] == '\0' || entry->name[0] == '\0' ||
       entry->command[0] == '\0')
        return;
    for(i = 0; i < *count; i++) {
        if(strcmp(out[i].id, entry->id) == 0) {
            out[i] = *entry;
            return;
        }
    }
    if(*count < cap)
        out[(*count)++] = *entry;
}

static void
load_icon_map(const char *path)
{
    FILE *f;
    char line[512];
    char *sep;

    f = fopen(path, "r");
    if(f == NULL)
        return;
    while(fgets(line, sizeof(line), f) != NULL) {
        trim_line(line);
        if(line[0] == '#' || line[0] == '\0')
            continue;
        sep = strchr(line, '=');
        if(sep == NULL)
            continue;
        *sep = '\0';
        if(icon_map_count >= (int)(sizeof(icon_map) / sizeof(icon_map[0])))
            break;
        take_field(icon_map[icon_map_count].key,
                   sizeof(icon_map[0].key), line);
        take_field(icon_map[icon_map_count].path,
                   sizeof(icon_map[0].path), sep + 1);
        icon_map_count++;
    }
    fclose(f);
}

static const char *
lookup_icon_path(const char *key)
{
    int i;

    for(i = icon_map_count - 1; i >= 0; i--) {
        if(strcmp(icon_map[i].key, key) == 0)
            return icon_map[i].path;
    }
    return NULL;
}

/* Desktop-entry style registry: repeated [Desktop Entry] sections with
 * Id=/Name=/Comment=/Categories=/Exec=/Icon=/X-Rill-Favorite= lines. */
static int
parse_applications(const char *path, RillLauncher *out, int cap, int *count)
{
    FILE *f;
    char line[512];
    RillLauncher entry;
    const char *icon_path;
    int in_entry;

    f = fopen(path, "r");
    if(f == NULL)
        return 0;
    in_entry = 0;
    memset(&entry, 0, sizeof(entry));
    while(fgets(line, sizeof(line), f) != NULL) {
        trim_line(line);
        if(line[0] == '#' || line[0] == '\0')
            continue;
        if(line[0] == '[') {
            if(in_entry)
                store_launcher(out, cap, count, &entry);
            memset(&entry, 0, sizeof(entry));
            in_entry = 1;
            continue;
        }
        if(!in_entry)
            continue;
        if(strncmp(line, "Id=", 3) == 0)
            take_field(entry.id, sizeof(entry.id), line + 3);
        else if(strncmp(line, "Name=", 5) == 0)
            take_field(entry.name, sizeof(entry.name), line + 5);
        else if(strncmp(line, "Comment=", 8) == 0)
            take_field(entry.description, sizeof(entry.description),
                       line + 8);
        else if(strncmp(line, "Categories=", 11) == 0)
            take_category(entry.category, sizeof(entry.category), line + 11);
        else if(strncmp(line, "Exec=", 5) == 0)
            take_field(entry.command, sizeof(entry.command), line + 5);
        else if(strncmp(line, "Icon=", 5) == 0) {
            char key[64];

            take_field(key, sizeof(key), line + 5);
            icon_path = lookup_icon_path(key);
            if(icon_path != NULL)
                take_field(entry.icon_path, sizeof(entry.icon_path),
                           icon_path);
        }
        else if(strncmp(line, "X-Rill-Favorite=", 16) == 0)
            entry.favorite = parse_boolean(line + 16);
    }
    if(in_entry)
        store_launcher(out, cap, count, &entry);
    fclose(f);
    return 1;
}

static int
plan9_list_launchers(RillLauncher *out, int cap)
{
    const char *home;
    char path[512];
    int count;

    if(out == NULL || cap <= 0)
        return 0;

    icon_map_count = 0;
    load_icon_map(PLAN9_SYSTEM_ICON_MAP);
    home = getenv("home");
    if(home == NULL)
        home = getenv("HOME");
    if(home != NULL) {
        snprintf(path, sizeof(path), "%s/lib/rill/icon.map", home);
        load_icon_map(path);
    }

    count = 0;
    parse_applications(PLAN9_SYSTEM_APPLICATIONS, out, cap, &count);
    if(home != NULL) {
        snprintf(path, sizeof(path), "%s/lib/rill/applications", home);
        parse_applications(path, out, cap, &count);
    }

    if(count > 0)
        return count;

    launcher(&out[0], "terminal", "Terminal Emulator",
             "Use the command line", "Accessories", "host:ktrem", 1);
    if(cap > 1)
        launcher(&out[1], "files", "File Manager",
                 "Browse the file system", "Accessories", "host:shelf", 1);
    if(cap > 2)
        launcher(&out[2], "settings", "Settings",
                 "Configure the desktop", "Settings", "internal:settings", 1);
    if(cap > 3)
        launcher(&out[3], "about", "About Rill",
                 "Desktop information", "System", "internal:about", 0);
    return cap < 4 ? cap : 4;
}

/* rio exports the same window tree to every client in its namespace. */
static const char *
wsys_root(void)
{
    const char *root = getenv("RILL_WSYS_DIR");
    return root != NULL && root[0] != '\0' ? root : "/dev/wsys";
}

static int
read_window_file(int id, const char *name, char *out, int cap)
{
    char path[1024];
    int fd, n;
    snprintf(path, sizeof(path), "%s/%d/%s", wsys_root(), id, name);
    fd = open(path, 0);
    if(fd < 0) return 0;
    /* A second read of wctl blocks; read only its initial state. */
    n = read(fd, out, cap - 1);
    close(fd);
    if(n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

static int
add_rio_task(const char *name, RillTask *task)
{
    char *end;
    unsigned long id;
    char state[128], own[32];
    int fd, n, own_id = -1;
    id = strtoul(name, &end, 10);
    if(*name == '\0' || *end != '\0' || id == 0 || id > 2147483647UL)
        return 0;
    fd = open("/dev/winid", 0);
    if(fd >= 0) {
        n = read(fd, own, sizeof(own) - 1);
        close(fd);
        if(n > 0) { own[n] = '\0'; own_id = atoi(own); }
    }
    if(own_id > 0 && id == (unsigned long)own_id) return 0;
    memset(task, 0, sizeof(*task));
    task->id = id;
    if(!read_window_file(id, "label", task->title, sizeof(task->title)))
        return 0;
    trim_line(task->title);
    if(read_window_file(id, "wctl", state, sizeof(state)))
        task->focused = strstr(state, "current") != NULL &&
                        strstr(state, "notcurrent") == NULL;
    return 1;
}

static int
plan9_list_tasks(RillTask *out, int cap)
{
    int count = 0;
#ifdef KRYON_NATIVE_PLAN9
    Dir *entries;
    int fd, n, i;
    if(out == NULL || cap <= 0) return 0;
    fd = open(wsys_root(), OREAD);
    if(fd < 0) return 0;
    n = dirreadall(fd, &entries);
    close(fd);
    for(i = 0; i < n && count < cap; i++)
        if(add_rio_task(entries[i].name, &out[count])) count++;
    if(n >= 0) free(entries);
#else
    DIR *dir;
    struct dirent *entry;
    if(out == NULL || cap <= 0) return 0;
    dir = opendir(wsys_root());
    if(dir == NULL) return 0;
    while(count < cap && (entry = readdir(dir)) != NULL)
        if(add_rio_task(entry->d_name, &out[count])) count++;
    closedir(dir);
#endif
    return count;
}

static int
write_window_control(int id, const char *command)
{
    char path[1024];
    int fd, len, written;
    if(id <= 0) return 0;
    snprintf(path, sizeof(path), "%s/%d/wctl", wsys_root(), id);
#ifdef KRYON_NATIVE_PLAN9
    fd = open(path, OWRITE);
#else
    fd = open(path, O_WRONLY);
#endif
    if(fd < 0) return 0;
    len = strlen(command);
    written = write(fd, command, len);
    close(fd);
    return written == len;
}

static int
plan9_launch(const RillLauncher *launcher)
{
#ifdef KRYON_NATIVE_PLAN9
    if(launcher == NULL || launcher->command[0] == '\0' ||
       strncmp(launcher->command, "internal:", 9) == 0)
        return 0;
    switch(rfork(RFPROC|RFFDG|RFENVG|RFNOTEG)) {
    case -1:
        return 0;
    case 0:
        execl("/bin/rc", "rc", "-c", launcher->command, nil);
        exits("exec");
    default:
        return 1;
    }
#else
    (void)launcher;
    return 0;
#endif
}

static int
plan9_focus_task(int task_id)
{
    /* unhide also focuses, but top is needed for an already visible window. */
    if(!write_window_control(task_id, "unhide")) return 0;
    if(!write_window_control(task_id, "top")) return 0;
    return write_window_control(task_id, "current");
}

static int
plan9_close_task(int task_id)
{
    return write_window_control(task_id, "delete");
}

static const char *
plan9_settings_root(void)
{
    static char path[512];
    const char *home = getenv("home");
    if(home == NULL) home = getenv("HOME");
    if(home == NULL || home[0] != '/') return NULL;
    snprintf(path, sizeof(path), "%s/lib/rill", home);
    return path;
}

static const RillPlatformServices services = {
    "plan9",
    plan9_list_launchers,
    plan9_list_tasks,
    plan9_launch,
    plan9_focus_task,
    plan9_close_task,
    plan9_settings_root,
    NULL, NULL, NULL
};

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
