#include "rill_platform.h"
#include "rill_wayland.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

typedef struct XLibreSession {
    Display *display;
    Window root;
    Atom active_window;
    Atom client_list;
    Atom close_window;
    Atom current_desktop;
    Atom wm_desktop;
    Atom wm_icon;
    Atom net_wm_name;
    Atom net_wm_state;
    Atom net_wm_state_skip_taskbar;
    Atom net_wm_window_type;
    Atom net_wm_window_type_desktop;
    Atom net_wm_window_type_dock;
    Atom utf8_string;
    Atom wm_name;
} XLibreSession;

static int
xlibre_open(XLibreSession *session)
{
    const char *client_display;

    if(session == NULL)
        return 0;
    memset(session, 0, sizeof(*session));
    client_display = getenv("RILL_CLIENT_DISPLAY");
    session->display = XOpenDisplay(client_display != NULL &&
                                    client_display[0] != '\0' ?
                                    client_display : NULL);
    if(session->display == NULL)
        return 0;
    session->root = RootWindow(session->display,
                               DefaultScreen(session->display));
    session->active_window = XInternAtom(session->display,
                                         "_NET_ACTIVE_WINDOW", True);
    session->client_list = XInternAtom(session->display,
                                       "_NET_CLIENT_LIST", True);
    session->close_window = XInternAtom(session->display,
                                        "_NET_CLOSE_WINDOW", True);
    session->current_desktop = XInternAtom(session->display,
                                           "_NET_CURRENT_DESKTOP", True);
    session->wm_desktop = XInternAtom(session->display,
                                      "_NET_WM_DESKTOP", True);
    session->wm_icon = XInternAtom(session->display, "_NET_WM_ICON", True);
    session->net_wm_name = XInternAtom(session->display,
                                       "_NET_WM_NAME", True);
    session->net_wm_state = XInternAtom(session->display,
                                        "_NET_WM_STATE", True);
    session->net_wm_state_skip_taskbar =
        XInternAtom(session->display, "_NET_WM_STATE_SKIP_TASKBAR", True);
    session->net_wm_window_type = XInternAtom(session->display,
                                              "_NET_WM_WINDOW_TYPE", True);
    session->net_wm_window_type_desktop =
        XInternAtom(session->display, "_NET_WM_WINDOW_TYPE_DESKTOP", True);
    session->net_wm_window_type_dock =
        XInternAtom(session->display, "_NET_WM_WINDOW_TYPE_DOCK", True);
    session->utf8_string = XInternAtom(session->display,
                                       "UTF8_STRING", True);
    session->wm_name = XA_WM_NAME;
    return 1;
}

static void
xlibre_close(XLibreSession *session)
{
    if(session != NULL && session->display != NULL)
        XCloseDisplay(session->display);
}

static Window
xlibre_window_property(XLibreSession *session, Window window, Atom property)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    Window result;

    if(session == NULL || session->display == NULL || property == None)
        return 0;
    data = NULL;
    result = 0;
    if(XGetWindowProperty(session->display, window, property, 0, 1, False,
                          XA_WINDOW, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) == Success &&
       data != NULL && actual_type == XA_WINDOW && actual_format == 32 &&
       item_count > 0)
        result = ((Window *)data)[0];
    if(data != NULL)
        XFree(data);
    return result;
}

static int
xlibre_cardinal_property(XLibreSession *session, Window window, Atom property,
                         unsigned long *out)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    int ok;

    if(session == NULL || session->display == NULL || property == None ||
       out == NULL)
        return 0;
    data = NULL;
    ok = 0;
    if(XGetWindowProperty(session->display, window, property, 0, 1, False,
                          XA_CARDINAL, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) == Success &&
       data != NULL && actual_type == XA_CARDINAL && actual_format == 32 &&
       item_count > 0) {
        *out = ((unsigned long *)data)[0];
        ok = 1;
    }
    if(data != NULL)
        XFree(data);
    return ok;
}

static int
xlibre_atom_list_contains(XLibreSession *session, Window window, Atom property,
                          Atom needle)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    int found;

    if(session == NULL || session->display == NULL || property == None ||
       needle == None)
        return 0;
    data = NULL;
    found = 0;
    if(XGetWindowProperty(session->display, window, property, 0, 64, False,
                          XA_ATOM, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) == Success &&
       data != NULL && actual_type == XA_ATOM && actual_format == 32) {
        Atom *atoms = (Atom *)data;

        for(unsigned long i = 0; i < item_count; i++) {
            if(atoms[i] == needle) {
                found = 1;
                break;
            }
        }
    }
    if(data != NULL)
        XFree(data);
    return found;
}

static int
xlibre_window_on_current_desktop(XLibreSession *session, Window window,
                                 unsigned long current_desktop)
{
    unsigned long window_desktop;

    if(current_desktop == ULONG_MAX || session->wm_desktop == None)
        return 1;
    if(!xlibre_cardinal_property(session, window, session->wm_desktop,
                                 &window_desktop))
        return 1;
    return window_desktop == current_desktop || window_desktop == 0xffffffffUL;
}

static int
xlibre_window_is_task(XLibreSession *session, Window window,
                      unsigned long current_desktop)
{
    if(session == NULL || window == 0)
        return 0;
    if(xlibre_atom_list_contains(session, window, session->net_wm_window_type,
                                 session->net_wm_window_type_desktop))
        return 0;
    if(xlibre_atom_list_contains(session, window, session->net_wm_window_type,
                                 session->net_wm_window_type_dock))
        return 0;
    if(xlibre_atom_list_contains(session, window, session->net_wm_state,
                                 session->net_wm_state_skip_taskbar))
        return 0;
    return xlibre_window_on_current_desktop(session, window, current_desktop);
}

static int
xlibre_read_text_property(XLibreSession *session, Window window, Atom property,
                          char *out, int out_size)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    size_t len;

    if(session == NULL || session->display == NULL || property == None ||
       out == NULL || out_size <= 0)
        return 0;
    data = NULL;
    if(XGetWindowProperty(session->display, window, property, 0, 1024, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) != Success ||
       data == NULL)
        return 0;
    if(property == session->net_wm_name && session->utf8_string != None &&
       actual_type != session->utf8_string) {
        XFree(data);
        return 0;
    }
    if(actual_format != 8 || item_count == 0) {
        XFree(data);
        return 0;
    }
    len = item_count;
    if(len >= (size_t)out_size)
        len = (size_t)out_size - 1;
    memcpy(out, data, len);
    out[len] = '\0';
    XFree(data);
    return out[0] != '\0';
}

static void
xlibre_window_title(XLibreSession *session, Window window, char *out,
                    int out_size)
{
    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(xlibre_read_text_property(session, window, session->net_wm_name, out,
                                 out_size))
        return;
    if(xlibre_read_text_property(session, window, session->wm_name, out,
                                 out_size))
        return;
    snprintf(out, (size_t)out_size, "Window 0x%lx", (unsigned long)window);
}

static int
xlibre_save_window_icon(XLibreSession *session, Window window, char *out,
                        int out_size)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    unsigned long *items;
    unsigned long best_pos;
    unsigned long best_score;
    int best_w;
    int best_h;
    int ok;

    if(out == NULL || out_size <= 0)
        return 0;
    out[0] = '\0';
    if(session == NULL || session->display == NULL || session->wm_icon == None)
        return 0;
    data = NULL;
    if(XGetWindowProperty(session->display, window, session->wm_icon, 0, 65536,
                          False, XA_CARDINAL, &actual_type, &actual_format,
                          &item_count, &bytes_after, &data) != Success ||
       data == NULL || actual_type != XA_CARDINAL || actual_format != 32) {
        if(data != NULL)
            XFree(data);
        return 0;
    }

    items = (unsigned long *)data;
    best_pos = 0;
    best_score = ULONG_MAX;
    best_w = 0;
    best_h = 0;
    ok = 0;
    for(unsigned long pos = 0; pos + 2 < item_count;) {
        unsigned long w = items[pos];
        unsigned long h = items[pos + 1];
        unsigned long pixels;
        unsigned long size;
        unsigned long score;

        if(w == 0 || h == 0 || w > 256 || h > 256)
            break;
        pixels = w * h;
        if(pixels > item_count - pos - 2)
            break;
        size = w > h ? w : h;
        score = size > 32 ? size - 32 : 32 - size;
        if(!ok || score < best_score) {
            best_pos = pos + 2;
            best_score = score;
            best_w = (int)w;
            best_h = (int)h;
            ok = 1;
        }
        pos += 2 + pixels;
    }

    if(ok) {
        static int gtk_attempted = 0;
        static int gtk_ready = 0;
        GdkPixbuf *pixbuf;
        GError *error = NULL;
        char path[128];

        if(!gtk_attempted) {
            gtk_attempted = 1;
            gtk_ready = gtk_init_check(NULL, NULL) ? 1 : 0;
        }
        if(gtk_ready) {
            pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, best_w,
                                    best_h);
            if(pixbuf != NULL) {
                int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
                unsigned char *pixels = gdk_pixbuf_get_pixels(pixbuf);

                for(int y = 0; y < best_h; y++) {
                    for(int x = 0; x < best_w; x++) {
                        unsigned long argb =
                            items[best_pos + (unsigned long)y * best_w + x];
                        unsigned char *p = pixels + y * rowstride + x * 4;

                        p[0] = (unsigned char)((argb >> 16) & 0xff);
                        p[1] = (unsigned char)((argb >> 8) & 0xff);
                        p[2] = (unsigned char)(argb & 0xff);
                        p[3] = (unsigned char)((argb >> 24) & 0xff);
                    }
                }
                snprintf(path, sizeof(path), "/tmp/rill-window-icon-%lx.png",
                         (unsigned long)window);
                if(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL))
                    snprintf(out, (size_t)out_size, "%s", path);
                if(error != NULL)
                    g_error_free(error);
                g_object_unref(pixbuf);
            }
        }
    }
    XFree(data);
    return out[0] != '\0';
}

static void
lookup_icon_path(const char *name, char *out, int out_size)
{
    static int gtk_attempted = 0;
    static int gtk_ready = 0;
    GtkIconTheme *theme;
    GdkPixbuf *pixbuf;
    GError *error = NULL;
    char path[512];
    char safe[128];
    int i;

    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(name == NULL || name[0] == '\0')
        return;
    if(!gtk_attempted) {
        gtk_attempted = 1;
        gtk_ready = gtk_init_check(NULL, NULL) ? 1 : 0;
    }
    if(!gtk_ready)
        return;
    theme = gtk_icon_theme_get_default();
    if(theme == NULL)
        return;
    pixbuf = gtk_icon_theme_load_icon(theme, name, 32, 0, &error);
    if(pixbuf == NULL) {
        if(error != NULL)
            g_error_free(error);
        return;
    }
    for(i = 0; name[i] != '\0' && i < (int)sizeof(safe) - 1; i++) {
        char c = name[i];
        safe[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    safe[i] = '\0';
    snprintf(path, sizeof(path), "/tmp/rill-icon-%s.png", safe);
    if(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL))
        snprintf(out, (size_t)out_size, "%s", path);
    if(error != NULL)
        g_error_free(error);
    g_object_unref(pixbuf);
}

static char *
trim(char *text)
{
    char *end;

    if(text == NULL)
        return NULL;
    while(*text != '\0' && isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while(end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static int
desktop_has_suffix(const char *name)
{
    size_t len;

    if(name == NULL)
        return 0;
    len = strlen(name);
    return len > 8 && strcmp(name + len - 8, ".desktop") == 0;
}

static void
desktop_id_from_path(const char *path, char *out, int out_size)
{
    const char *base;
    size_t len;

    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(path == NULL)
        return;
    base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    len = strlen(base);
    if(len > 8 && strcmp(base + len - 8, ".desktop") == 0)
        len -= 8;
    if(len >= (size_t)out_size)
        len = (size_t)out_size - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

static int
desktop_id_exists(const RillLauncher *out, int count, const char *id)
{
    int i;

    if(id == NULL || id[0] == '\0')
        return 1;
    for(i = 0; i < count; i++)
        if(strcmp(out[i].id, id) == 0)
            return 1;
    return 0;
}

static int
desktop_categories_contain(const char *categories, const char *needle)
{
    size_t needle_len;
    const char *p;

    if(categories == NULL || needle == NULL)
        return 0;
    needle_len = strlen(needle);
    p = categories;
    while(*p != '\0') {
        while(*p == ';')
            p++;
        if(strncmp(p, needle, needle_len) == 0 &&
           (p[needle_len] == ';' || p[needle_len] == '\0'))
            return 1;
        while(*p != '\0' && *p != ';')
            p++;
    }
    return 0;
}

static const char *
desktop_category(const char *categories)
{
    if(desktop_categories_contain(categories, "Development"))
        return "Development";
    if(desktop_categories_contain(categories, "Education"))
        return "Education";
    if(desktop_categories_contain(categories, "Game"))
        return "Games";
    if(desktop_categories_contain(categories, "Graphics"))
        return "Graphics";
    if(desktop_categories_contain(categories, "Network"))
        return "Internet";
    if(desktop_categories_contain(categories, "AudioVideo") ||
       desktop_categories_contain(categories, "Audio") ||
       desktop_categories_contain(categories, "Video"))
        return "Multimedia";
    if(desktop_categories_contain(categories, "Office"))
        return "Office";
    if(desktop_categories_contain(categories, "Settings") ||
       desktop_categories_contain(categories, "System"))
        return "Settings";
    if(desktop_categories_contain(categories, "Utility"))
        return "Accessories";
    return "Other";
}

static void
desktop_exec_command(const char *exec, char *out, int out_size)
{
    int o;
    char *trimmed;

    if(out == NULL || out_size <= 0)
        return;
    out[0] = '\0';
    if(exec == NULL)
        return;
    o = 0;
    for(int i = 0; exec[i] != '\0' && o < out_size - 1; i++) {
        if(exec[i] != '%') {
            out[o++] = exec[i];
            continue;
        }
        i++;
        if(exec[i] == '\0')
            break;
        if(exec[i] == '%')
            out[o++] = '%';
    }
    out[o] = '\0';
    trimmed = trim(out);
    memmove(out, trimmed, strlen(trimmed) + 1);
}

static int
read_desktop_launcher(const char *path, const char *desktop_id, RillLauncher *out)
{
    GKeyFile *file = g_key_file_new();
    GDesktopAppInfo *app = NULL;
    char *name = NULL, *comment = NULL, *exec = NULL, *icon = NULL;
    char *categories = NULL, *id = NULL, *type = NULL;
    int ok = 0, internal;
    const char *group = G_KEY_FILE_DESKTOP_GROUP;

    if(!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
        goto done;
    type = g_key_file_get_string(file, group, "Type", NULL);
    exec = g_key_file_get_string(file, group, "Exec", NULL);
    name = g_key_file_get_locale_string(file, group, "Name", NULL, NULL);
    comment = g_key_file_get_locale_string(file, group, "Comment", NULL, NULL);
    if(g_strcmp0(type, "Application") != 0 || name == NULL || name[0] == '\0' ||
       g_key_file_get_boolean(file, group, "Hidden", NULL) ||
       g_key_file_get_boolean(file, group, "NoDisplay", NULL))
        goto done;
    internal = exec != NULL && (g_str_has_prefix(exec, "host:") ||
                                 g_str_has_prefix(exec, "internal:"));
    if(!internal) {
        app = g_desktop_app_info_new_from_filename(path);
        if(app == NULL || !g_app_info_should_show(G_APP_INFO(app)))
            goto done;
    }
    memset(out, 0, sizeof(*out));
    id = g_key_file_get_string(file, group, "X-Rill-ID", NULL);
    if(id != NULL && id[0] != '\0')
        snprintf(out->id, sizeof(out->id), "%s", id);
    else
        desktop_id_from_path(desktop_id, out->id, sizeof(out->id));
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->description, sizeof(out->description), "%s", comment ? comment : "");
    categories = g_key_file_get_string(file, group, "Categories", NULL);
    snprintf(out->category, sizeof(out->category), "%s", desktop_category(categories));
    if(internal)
        desktop_exec_command(exec, out->command, sizeof(out->command));
    else {
        snprintf(out->desktop_file, sizeof(out->desktop_file), "%s", path);
        /* The command is display metadata; GIO executes the original entry. */
        snprintf(out->command, sizeof(out->command), "%s", exec ? exec : "desktop:");
    }
    icon = g_key_file_get_string(file, group, "Icon", NULL);
    if(icon != NULL && g_path_is_absolute(icon))
        snprintf(out->icon_path, sizeof(out->icon_path), "%s", icon);
    else
        lookup_icon_path(icon, out->icon_path, sizeof(out->icon_path));
    out->favorite = g_key_file_get_boolean(file, group, "X-Rill-Favorite", NULL);
    ok = 1;
done:
    if(app != NULL) g_object_unref(app);
    g_key_file_unref(file);
    g_free(type); g_free(exec); g_free(name); g_free(comment);
    g_free(categories); g_free(icon); g_free(id);
    return ok;
}

static int
scan_desktop_dir(const char *dir, RillLauncher *out, int cap, int count,
                 int depth, const char *base, GHashTable *seen)
{
    DIR *dp;
    struct dirent *entry;

    if(dir == NULL || dir[0] == '\0' || out == NULL || count >= cap ||
       depth > 4)
        return count;
    dp = opendir(dir);
    if(dp == NULL)
        return count;
    while((entry = readdir(dp)) != NULL && count < cap) {
        char path[1024];
        struct stat st;

        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if(stat(path, &st) != 0)
            continue;
        if(S_ISDIR(st.st_mode)) {
            count = scan_desktop_dir(path, out, cap, count, depth + 1, base, seen);
            continue;
        }
        if(S_ISREG(st.st_mode) && desktop_has_suffix(entry->d_name)) {
            RillLauncher launcher_entry;
            char *desktop_id = g_strdup(path + strlen(base) + 1);
            for(char *p = desktop_id; *p; p++)
                if(*p == '/') *p = '-';
            if(g_hash_table_contains(seen, desktop_id)) {
                g_free(desktop_id);
                continue;
            }
            g_hash_table_add(seen, desktop_id);
            if(read_desktop_launcher(path, desktop_id, &launcher_entry) &&
               !desktop_id_exists(out, count, launcher_entry.id))
                out[count++] = launcher_entry;
        }
    }
    closedir(dp);
    return count;
}

static int
scan_desktop_dir_list(const char *dirs, const char *suffix,
                      RillLauncher *out, int cap, int count, GHashTable *seen)
{
    char copy[2048];
    char *start;
    char *end;

    if(dirs == NULL || dirs[0] == '\0')
        return count;
    snprintf(copy, sizeof(copy), "%s", dirs);
    for(start = copy; start != NULL && start[0] != '\0' && count < cap;
        start = end) {
        char path[1024];

        end = strchr(start, ':');
        if(end != NULL)
            *end++ = '\0';
        if(start[0] == '\0')
            continue;
        if(suffix != NULL && suffix[0] != '\0') {
            size_t start_len = strlen(start);
            size_t suffix_len = strlen(suffix);

            if(start_len + suffix_len + 2 > sizeof(path))
                continue;
            memcpy(path, start, start_len);
            path[start_len] = '/';
            memcpy(path + start_len + 1, suffix, suffix_len + 1);
        } else {
            size_t start_len = strlen(start);

            if(start_len + 1 > sizeof(path))
                continue;
            memcpy(path, start, start_len + 1);
        }
        count = scan_desktop_dir(path, out, cap, count, 0, path, seen);
    }
    return count;
}

static int
linux_list_launchers(RillLauncher *out, int cap)
{
    int count;
    const char *dirs;
    const char *home;
    char user_apps[1024];
    GHashTable *seen;

    if(out == NULL || cap <= 0)
        return 0;

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    count = 0;
    dirs = getenv("RILL_APPLICATION_DIRS");
    count = scan_desktop_dir_list(dirs, NULL, out, cap, count, seen);
    home = getenv("XDG_DATA_HOME");
    if(home != NULL && home[0] != '\0') {
        snprintf(user_apps, sizeof(user_apps), "%s/applications", home);
        count = scan_desktop_dir(user_apps, out, cap, count, 0, user_apps, seen);
    } else {
        home = getenv("HOME");
        if(home != NULL && home[0] != '\0') {
            snprintf(user_apps, sizeof(user_apps),
                     "%s/.local/share/applications", home);
            count = scan_desktop_dir(user_apps, out, cap, count, 0, user_apps, seen);
        }
    }
    dirs = getenv("XDG_DATA_DIRS");
    if(dirs == NULL || dirs[0] == '\0')
        dirs = "/usr/local/share:/usr/share";
    count = scan_desktop_dir_list(dirs, "applications", out, cap, count, seen);
    g_hash_table_unref(seen);
    return count;
}

static int
linux_list_tasks(RillTask *out, int cap)
{
    XLibreSession session;
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *data;
    Window active;
    Window *windows;
    unsigned long current_desktop;
    int count;

    if(out == NULL || cap <= 0)
        return 0;
    if(RillWaylandSession()) return RillWaylandListTasks(out, cap);
    if(!xlibre_open(&session))
        return 0;

    data = NULL;
    count = 0;
    active = xlibre_window_property(&session, session.root,
                                    session.active_window);
    if(!xlibre_cardinal_property(&session, session.root,
                                 session.current_desktop, &current_desktop))
        current_desktop = ULONG_MAX;
    if(XGetWindowProperty(session.display, session.root, session.client_list,
                          0, 1048576, False, XA_WINDOW, &actual_type,
                          &actual_format, &item_count, &bytes_after,
                          &data) == Success &&
       data != NULL && actual_type == XA_WINDOW && actual_format == 32) {
        windows = (Window *)data;
        for(unsigned long i = 0; i < item_count && count < cap; i++) {
            if((unsigned long)windows[i] > INT_MAX)
                continue;
            if(!xlibre_window_is_task(&session, windows[i], current_desktop))
                continue;
            memset(&out[count], 0, sizeof(out[count]));
            out[count].id = (int)windows[i];
            out[count].focused = windows[i] == active;
            out[count].urgent = 0;
            xlibre_window_title(&session, windows[i], out[count].title,
                                (int)sizeof(out[count].title));
            xlibre_save_window_icon(&session, windows[i], out[count].icon_path,
                                    (int)sizeof(out[count].icon_path));
            count++;
        }
    }
    if(data != NULL)
        XFree(data);
    xlibre_close(&session);
    return count;
}

static int
linux_launch(const RillLauncher *launcher)
{
    const char *command;
    GError *error = NULL;
    int ok;
    char **argv = NULL;
    char **env;

    if(launcher == NULL || launcher->command[0] == '\0')
        return 0;
    command = launcher->command;
    if(getenv("RILL_CONTAINED_X11") != NULL) {
        if(strcmp(command, "internal:terminal") == 0 ||
           strcmp(command, "host:ktrem") == 0 ||
           strcmp(command, "host:kterm") == 0)
            command = "xterm";
        else if(strcmp(command, "internal:files") == 0 ||
                strcmp(command, "host:shelf") == 0)
            command = "thunar";
        else if(strcmp(command, "internal:settings") == 0)
            command = "xfce4-settings-manager";
        else if(strcmp(command, "internal:about") == 0)
            command = "xmessage Rill";
    } else if(strncmp(command, "internal:", 9) == 0 ||
              strncmp(command, "host:", 5) == 0) {
        return 0;
    }

    env = g_get_environ();
    if(getenv("RILL_CONTAINED_X11") != NULL) {
        const char *display = getenv("RILL_CLIENT_DISPLAY");
        if(display != NULL) env = g_environ_setenv(env, "DISPLAY", display, TRUE);
        env = g_environ_setenv(env, "GDK_BACKEND", "x11", TRUE);
        env = g_environ_setenv(env, "QT_QPA_PLATFORM", "xcb", TRUE);
        env = g_environ_setenv(env, "SDL_VIDEODRIVER", "x11", TRUE);
        env = g_environ_setenv(env, "MOZ_ENABLE_WAYLAND", "0", TRUE);
        env = g_environ_unsetenv(env, "WAYLAND_DISPLAY");
        env = g_environ_unsetenv(env, "DBUS_SESSION_BUS_ADDRESS");
    }
    if(launcher->desktop_file[0] != '\0') {
        GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(launcher->desktop_file);
        GAppLaunchContext *context = g_app_launch_context_new();
        if(getenv("RILL_CONTAINED_X11") != NULL) {
            /* Contained apps must not activate an existing desktop instance. */
            g_app_launch_context_unsetenv(context, "WAYLAND_DISPLAY");
            g_app_launch_context_unsetenv(context, "DBUS_SESSION_BUS_ADDRESS");
            for(int i = 0; env[i] != NULL; i++) {
                char *eq = strchr(env[i], '=');
                if(eq != NULL) {
                    *eq = '\0';
                    g_app_launch_context_setenv(context, env[i], eq + 1);
                    *eq = '=';
                }
            }
        }
        ok = app != NULL && g_app_info_launch(G_APP_INFO(app), NULL, context, &error);
        if(app != NULL) g_object_unref(app);
        g_object_unref(context);
    } else {
        ok = g_shell_parse_argv(command, NULL, &argv, &error) &&
             g_spawn_async(NULL, argv, env, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
        g_strfreev(argv);
    }
    g_strfreev(env);
    if(error != NULL) {
        fprintf(stderr, "rill: cannot launch %s: %s\n", launcher->name, error->message);
        g_error_free(error);
    }
    return ok;
}

static int
linux_focus_task(int task_id)
{
    XLibreSession session;
    XEvent event;
    Window window;
    int sent;

    if(RillWaylandSession()) return RillWaylandFocusTask(task_id);
    if(task_id <= 0 || !xlibre_open(&session))
        return 0;
    if(session.active_window == None) {
        xlibre_close(&session);
        return 0;
    }
    window = (Window)task_id;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = session.active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;
    sent = XSendEvent(session.display, session.root, False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &event);
    XFlush(session.display);
    xlibre_close(&session);
    return sent != 0;
}

static int
linux_close_task(int task_id)
{
    XLibreSession session;
    XEvent event;
    Window window;
    int sent;

    if(RillWaylandSession()) return RillWaylandCloseTask(task_id);
    if(task_id <= 0 || !xlibre_open(&session))
        return 0;
    if(session.close_window == None) {
        xlibre_close(&session);
        return 0;
    }
    window = (Window)task_id;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = session.close_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = CurrentTime;
    event.xclient.data.l[1] = 2;
    sent = XSendEvent(session.display, session.root, False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &event);
    XFlush(session.display);
    xlibre_close(&session);
    return sent != 0;
}

static const char *
linux_settings_root(void)
{
    static char path[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if(xdg != NULL && xdg[0] == '/')
        snprintf(path, sizeof(path), "%s/rill", xdg);
    else if(home != NULL && home[0] == '/')
        snprintf(path, sizeof(path), "%s/.config/rill", home);
    else
        return NULL;
    return path;
}

static int
linux_workspace_property(const char *name, int fallback)
{
    XLibreSession session;
    unsigned long value;
    int result = fallback;
    if(RillWaylandSession()) return fallback;
    if(!xlibre_open(&session)) return fallback;
    if(xlibre_cardinal_property(&session, session.root,
         XInternAtom(session.display, name, True), &value) && value <= 1024)
        result = value;
    xlibre_close(&session);
    return result;
}

static int linux_workspace_count(void)
{
    return linux_workspace_property("_NET_NUMBER_OF_DESKTOPS", 0);
}

static int linux_current_workspace(void)
{
    return linux_workspace_property("_NET_CURRENT_DESKTOP", -1);
}

static int linux_switch_workspace(int index)
{
    XLibreSession session;
    XEvent event = {0};
    int result;
    if(index < 0 || index >= linux_workspace_count() || !xlibre_open(&session))
        return 0;
    event.xclient.type = ClientMessage;
    event.xclient.window = session.root;
    event.xclient.message_type = session.current_desktop;
    event.xclient.format = 32;
    event.xclient.data.l[0] = index;
    event.xclient.data.l[1] = CurrentTime;
    result = XSendEvent(session.display, session.root, False,
                        SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(session.display);
    xlibre_close(&session);
    return result != 0;
}

static const RillPlatformServices services = {
    "xlibre",
    linux_list_launchers,
    linux_list_tasks,
    linux_launch,
    linux_focus_task,
    linux_close_task,
    linux_settings_root,
    linux_workspace_count, linux_current_workspace, linux_switch_workspace
};

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
