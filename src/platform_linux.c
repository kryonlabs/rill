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

static int
linux_wallpaper_dir(char (*paths)[512], int cap, int count, const char *dir)
{
    DIR *directory;
    struct dirent *entry;

    if(count >= cap || dir == NULL || dir[0] == '\0')
        return count;
    directory = opendir(dir);
    if(directory == NULL)
        return count;
    while(count < cap && (entry = readdir(directory)) != NULL) {
        const char *name = entry->d_name;
        const char *dot = strrchr(name, '.');
        if(name[0] == '.' || dot == NULL)
            continue;
        dot++;
        if(strcasecmp(dot, "png") != 0 && strcasecmp(dot, "jpg") != 0 &&
           strcasecmp(dot, "jpeg") != 0)
            continue;
        snprintf(paths[count], 512, "%s/%s", dir, name);
        count++;
    }
    closedir(directory);
    return count;
}

static int
linux_list_wallpapers(char (*paths)[512], int cap)
{
    const char *home = getenv("HOME");
    char dir[1024];
    int count = 0;

    if(paths == NULL || cap <= 0)
        return 0;
    if(home != NULL && home[0] != '\0') {
        snprintf(dir, sizeof(dir), "%s/Pictures", home);
        count = linux_wallpaper_dir(paths, cap, count, dir);
        snprintf(dir, sizeof(dir), "%s/.local/share/backgrounds", home);
        count = linux_wallpaper_dir(paths, cap, count, dir);
    }
    count = linux_wallpaper_dir(paths, cap, count, "/usr/share/backgrounds");
    return count;
}

static int
linux_session_action(const char *action)
{
    const char *record = getenv("RILL_SESSION_ACTION_FILE");
    GDBusConnection *bus;
    GError *error = NULL;
    const char *method = NULL;
    gboolean ok;

    if(action == NULL)
        return 0;
    if(strcmp(action, "restart") == 0)
        method = "Reboot";
    else if(strcmp(action, "shutdown") == 0)
        method = "PowerOff";
    else if(strcmp(action, "suspend") == 0)
        method = "Suspend";
    else if(strcmp(action, "logout") == 0 || strcmp(action, "lock") == 0) {
        if(record != NULL && record[0] != '\0') {
            FILE *file = fopen(record, "a");
            if(file == NULL)
                return 0;
            fprintf(file, "%s\n", action);
            fclose(file);
            return 1;
        }
        if(strcmp(action, "lock") == 0)
            return g_spawn_command_line_async("xflock4", NULL) ||
                   g_spawn_command_line_async("loginctl lock-session", NULL);
        /* Rill's own session manager coordinates the logout when running;
           fall back to the Xfce session otherwise. */
        {
            const char *runtime = getenv("XDG_RUNTIME_DIR");
            char control[512];
            FILE *fifo;
            if(runtime != NULL && runtime[0] != '\0') {
                snprintf(control, sizeof(control),
                         "%s/rill-session-%ld.control", runtime, (long)getuid());
                fifo = fopen(control, "a");
                if(fifo != NULL) {
                    fputs("logout\n", fifo);
                    fclose(fifo);
                    return 1;
                }
            }
        }
        return g_spawn_command_line_async("xfce4-session-logout", NULL);
    } else
        return 0;
    if(record != NULL && record[0] != '\0') {
        FILE *file = fopen(record, "a");
        if(file == NULL)
            return 0;
        fprintf(file, "%s\n", action);
        fclose(file);
        return 1;
    }
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if(bus == NULL) {
        fprintf(stderr, "rill: cannot reach the system bus for %s\n", action);
        return 0;
    }
    ok = g_dbus_connection_call_sync(bus, "org.freedesktop.login1",
                                     "/org/freedesktop/login1",
                                     "org.freedesktop.login1.Manager", method,
                                     g_variant_new("(b)", TRUE), NULL,
                                     G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                     &error) != NULL;
    if(error != NULL) {
        fprintf(stderr, "rill: session action %s failed: %s\n", action,
                error->message);
        g_error_free(error);
    }
    g_object_unref(bus);
    return ok;
}

static int
linux_show_desktop(int show)
{
    XLibreSession session;
    XEvent event = {0};
    int result;
    if(RillWaylandSession()) return 0;
    if(!xlibre_open(&session)) return 0;
    event.xclient.type = ClientMessage;
    event.xclient.window = session.root;
    event.xclient.message_type = XInternAtom(session.display,
                                             "_NET_SHOWING_DESKTOP", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = show ? 1 : 0;
    result = XSendEvent(session.display, session.root, False,
                        SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(session.display);
    xlibre_close(&session);
    return result != 0;
}

/* StatusNotifier tray host. The watcher name is owned only when no other host
   (for example the real Xfce panel) already registered; icons are polled on
   each tray_icons call so the shell needs no D-Bus main loop of its own. */
#define RILL_TRAY_MAX 12

typedef struct {
    char bus[160];
    char path[200];
} TrayRegistration;

static TrayRegistration tray_registrations[RILL_TRAY_MAX];
static int tray_registration_count;
static GDBusConnection *tray_connection;
static guint tray_owner_id;
static int tray_started;
static int notify_started;
static int notify_count;
static char tray_bus_address[256];

static void
tray_register(const char *sender, const char *service)
{
    const char *bus;
    const char *path;

    if(service != NULL && service[0] == '/') {
        bus = sender;
        path = service;
    } else {
        bus = service;
        path = "/StatusNotifierItem";
    }
    if(bus == NULL || bus[0] == '\0' || path[0] == '\0')
        return;
    for(int i = 0; i < tray_registration_count; i++)
        if(strcmp(tray_registrations[i].bus, bus) == 0 &&
           strcmp(tray_registrations[i].path, path) == 0)
            return;
    if(tray_registration_count >= RILL_TRAY_MAX)
        return;
    snprintf(tray_registrations[tray_registration_count].bus, 160, "%s", bus);
    snprintf(tray_registrations[tray_registration_count].path, 200, "%s", path);
    tray_registration_count++;
}

static void
tray_method_call(GDBusConnection *connection, const gchar *sender,
                 const gchar *object_path, const gchar *interface_name,
                 const gchar *method, GVariant *parameters,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if(g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const gchar *service = NULL;
        g_variant_get(parameters, "(&s)", &service);
        tray_register(sender, service);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method %s", method);
}

static GVariant *
tray_get_property(GDBusConnection *connection, const gchar *sender,
                  const gchar *object_path, const gchar *interface_name,
                  const gchar *property, GError **error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if(g_strcmp0(property, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if(g_strcmp0(property, "ProtocolVersion") == 0)
        return g_variant_new_string("0.2");
    if(g_strcmp0(property, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        for(int i = 0; i < tray_registration_count; i++)
            g_variant_builder_add(&builder, "s", tray_registrations[i].bus);
        return g_variant_builder_end(&builder);
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "Unknown property %s", property);
    return NULL;
}

static const gchar tray_watcher_xml[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'>"
    "   <arg type='s' direction='in'/>"
    "  </method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='s' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    " </interface>"
    "</node>";

static const GDBusInterfaceVTable tray_vtable = {
    tray_method_call, tray_get_property, NULL, {0}
};

static void
tray_start(void)
{
    GError *error = NULL;
    GDBusNodeInfo *info;

    /* A replaced session bus (for example a private test bus shutting down)
       closes the connection; re-register everything on the current bus. */
    const char *bus_address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if(bus_address == NULL)
        bus_address = "";
    if(tray_connection != NULL &&
       (strcmp(tray_bus_address, bus_address) != 0 ||
        g_dbus_connection_is_closed(tray_connection))) {
        /* The session bus address changed (for example a private test bus);
           drop the old connection and re-register every service. */
        /* Detach without touching it: the old bus is gone, so its socket is
           dead and the address change is a rare, test-time event. */
        tray_connection = NULL;
        tray_started = 0;
        notify_started = 0;
        notify_count = 0;
        tray_registration_count = 0;
    }
    if(tray_started)
        return;
    /* Connecting may legitimately fail before a session bus exists; retry on
       the next poll instead of latching the failure. */
    tray_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if(tray_connection == NULL)
        return;
    snprintf(tray_bus_address, sizeof(tray_bus_address), "%s", bus_address);
    tray_started = 1;
    info = g_dbus_node_info_new_for_xml(tray_watcher_xml, &error);
    if(info == NULL) {
        if(error != NULL)
            g_error_free(error);
        return;
    }
    g_dbus_connection_register_object(tray_connection,
                                      "/StatusNotifierWatcher",
                                      info->interfaces[0], &tray_vtable,
                                      NULL, NULL, &error);
    g_dbus_node_info_unref(info);
    if(error != NULL)
        g_error_free(error);
    /* DO_NOT_QUEUE: when the Xfce panel already hosts the tray, Rill defers. */
    tray_owner_id = g_bus_own_name_on_connection(
        tray_connection, "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, NULL, NULL, NULL, NULL);
}

/* Desktop notification server (org.freedesktop.Notifications). Like the tray
   watcher, the name is only taken when no other server already owns it. */
#define RILL_NOTIFY_MAX 6

typedef struct {
    unsigned int id;
    char app_name[96];
    char summary[160];
    char body[256];
    char sender[96];
    long deadline_ms;
    int sticky;
} NotifyEntry;

static NotifyEntry notify_entries[RILL_NOTIFY_MAX];
static unsigned int notify_next_id = 1;

static long
notify_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static NotifyEntry *
notify_find(unsigned int id)
{
    for(int i = 0; i < notify_count; i++)
        if(notify_entries[i].id == id)
            return &notify_entries[i];
    return NULL;
}

static void
notify_remove(NotifyEntry *entry, unsigned int reason)
{
    if(entry == NULL)
        return;
    g_dbus_connection_emit_signal(tray_connection, NULL,
                                  "/org/freedesktop/Notifications",
                                  "org.freedesktop.Notifications",
                                  "NotificationClosed",
                                  g_variant_new("(uu)", entry->id, reason),
                                  NULL);
    int index = (int)(entry - notify_entries);
    memmove(&notify_entries[index], &notify_entries[index + 1],
            (size_t)(notify_count - index - 1) * sizeof(notify_entries[0]));
    notify_count--;
}

static void
notify_method_call(GDBusConnection *connection, const gchar *sender,
                   const gchar *object_path, const gchar *interface_name,
                   const gchar *method, GVariant *parameters,
                   GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if(g_strcmp0(method, "GetCapabilities") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&builder, "s", "actions");
        g_variant_builder_add(&builder, "s", "body");
        g_variant_builder_add(&builder, "s", "icon-static");
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(as)", &builder));
    } else if(g_strcmp0(method, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ssss)", "rill", "Rill", "1.0", "1.2"));
    } else if(g_strcmp0(method, "CloseNotification") == 0) {
        guint32 id = 0;
        g_variant_get(parameters, "(u)", &id);
        notify_remove(notify_find(id), 3);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if(g_strcmp0(method, "Notify") == 0) {
        const gchar *app_name, *app_icon, *summary, *body;
        guint32 replaces_id = 0;
        gint32 timeout = 0;
        gchar **actions = NULL;
        GVariant *hints = NULL;
        NotifyEntry *entry;
        long now = notify_now_ms();

        g_variant_get(parameters, "(&su&s&s&s^as@a{sv}i)", &app_name,
                      &replaces_id, &app_icon, &summary, &body, &actions,
                      &hints, &timeout);
        g_strfreev(actions);
        if(hints != NULL)
            g_variant_unref(hints);
        if(getenv("RILL_TRAY_DEBUG"))
            fprintf(stderr, "rill: Notify from %s: %s\n", sender, summary);
        entry = replaces_id != 0 ? notify_find(replaces_id) : NULL;
        if(entry == NULL) {
            if(notify_count >= RILL_NOTIFY_MAX)
                notify_remove(&notify_entries[0], 1);
            if(notify_count >= RILL_NOTIFY_MAX) {
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(u)", 0));
                return;
            }
            entry = &notify_entries[notify_count++];
            memset(entry, 0, sizeof(*entry));
            entry->id = notify_next_id++;
            if(notify_next_id == 0)
                notify_next_id = 1;
        }
        snprintf(entry->app_name, sizeof(entry->app_name), "%s", app_name);
        snprintf(entry->summary, sizeof(entry->summary), "%s", summary);
        snprintf(entry->body, sizeof(entry->body), "%s",
                 body != NULL ? body : "");
        snprintf(entry->sender, sizeof(entry->sender), "%s", sender);
        entry->sticky = timeout == -1;
        entry->deadline_ms = now + (timeout > 0 ? timeout : 5000);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u)", entry->id));
    } else
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method %s", method);
}

static const gchar notify_xml[] =
    "<node>"
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='Notify'>"
    "   <arg type='s' direction='in'/>"
    "   <arg type='u' direction='in'/>"
    "   <arg type='s' direction='in'/>"
    "   <arg type='s' direction='in'/>"
    "   <arg type='s' direction='in'/>"
    "   <arg type='as' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/>"
    "   <arg type='i' direction='in'/>"
    "   <arg type='u' direction='out'/>"
    "  </method>"
    "  <method name='CloseNotification'>"
    "   <arg type='u' direction='in'/>"
    "  </method>"
    "  <method name='GetCapabilities'>"
    "   <arg type='as' direction='out'/>"
    "  </method>"
    "  <method name='GetServerInformation'>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "  </method>"
    " </interface>"
    "</node>";

static const GDBusInterfaceVTable notify_vtable = {
    notify_method_call, NULL, NULL, {0}
};

static void
notify_name_acquired(GDBusConnection *connection, const gchar *name,
                     gpointer user_data)
{
    (void)connection;
    (void)user_data;
    if(getenv("RILL_TRAY_DEBUG"))
        fprintf(stderr, "rill: acquired %s on %p (%s)\n", name,
                (void *)connection, tray_bus_address);
}

static void
notify_name_lost(GDBusConnection *connection, const gchar *name,
                 gpointer user_data)
{
    (void)connection;
    (void)user_data;
    if(getenv("RILL_TRAY_DEBUG"))
        fprintf(stderr, "rill: lost %s\n", name);
}

static void
notify_start(void)
{
    GError *error = NULL;
    GDBusNodeInfo *info;

    /* tray_start also clears notify_started when the session bus was
       replaced, so registration repeats on the new connection. */
    tray_start();
    if(notify_started || tray_connection == NULL)
        return;
    info = g_dbus_node_info_new_for_xml(notify_xml, &error);
    if(info == NULL) {
        if(error != NULL)
            g_error_free(error);
        return;
    }
    g_dbus_connection_register_object(tray_connection,
                                      "/org/freedesktop/Notifications",
                                      info->interfaces[0], &notify_vtable,
                                      NULL, NULL, &error);
    g_dbus_node_info_unref(info);
    if(error != NULL)
        g_error_free(error);
    g_bus_own_name_on_connection(tray_connection,
                                 "org.freedesktop.Notifications",
                                 G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                 NULL, notify_name_acquired, notify_name_lost,
                                 NULL);
    notify_started = 1;
    if(getenv("RILL_TRAY_DEBUG"))
        fprintf(stderr, "rill: notification server registered on %p (%s)\n",
                (void *)tray_connection, tray_bus_address);
}

static int
linux_notifications_poll(RillNotification *out, int cap)
{
    int count = 0;
    long now = notify_now_ms();

    notify_start();
    for(int i = 0; i < 4; i++)
        g_main_context_iteration(NULL, FALSE);
    if(out == NULL || cap <= 0)
        return 0;
    for(int i = notify_count - 1; i >= 0; i--) {
        if(!notify_entries[i].sticky && now >= notify_entries[i].deadline_ms) {
            notify_remove(&notify_entries[i], 1);
            continue;
        }
        if(count < cap) {
            out[count].id = notify_entries[i].id;
            snprintf(out[count].app_name, sizeof(out[count].app_name), "%s",
                     notify_entries[i].app_name);
            snprintf(out[count].summary, sizeof(out[count].summary), "%s",
                     notify_entries[i].summary);
            snprintf(out[count].body, sizeof(out[count].body), "%s",
                     notify_entries[i].body);
            count++;
        }
    }
    return count;
}

static int
linux_notification_action(unsigned int id, int dismiss)
{
    NotifyEntry *entry = notify_find(id);
    GError *error = NULL;

    if(getenv("RILL_TRAY_DEBUG"))
        fprintf(stderr, "rill: notify action id=%u dismiss=%d found=%d count=%d\n",
                id, dismiss, entry != NULL, notify_count);
    if(entry == NULL)
        return 0;
    if(!dismiss && tray_connection != NULL && entry->sender[0] != '\0') {
        /* The spec broadcasts ActionInvoked; a unicast call would deadlock
           against clients that answer from their own main loop. */
        g_dbus_connection_emit_signal(tray_connection, entry->sender,
                                      "/org/freedesktop/Notifications",
                                      "org.freedesktop.Notifications",
                                      "ActionInvoked",
                                      g_variant_new("(us)", id, "default"),
                                      &error);
        if(error != NULL)
            g_error_free(error);
    }
    notify_remove(entry, dismiss ? 2 : 1);
    return 1;
}

static int
linux_battery_state(int *percent, int *charging)
{
    const char *base = getenv("RILL_BATTERY_DIR");
    DIR *directory;
    struct dirent *entry;

    if(percent == NULL || charging == NULL)
        return 0;
    if(base == NULL || base[0] == '\0')
        base = "/sys/class/power_supply";
    directory = opendir(base);
    if(directory == NULL)
        return 0;
    while((entry = readdir(directory)) != NULL) {
        char path[1024];
        FILE *file;
        int value = -1;
        char status[32] = "";

        if(entry->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/capacity", base, entry->d_name);
        file = fopen(path, "r");
        if(file == NULL)
            continue;
        if(fscanf(file, "%d", &value) != 1)
            value = -1;
        fclose(file);
        if(value < 0 || value > 100)
            continue;
        snprintf(path, sizeof(path), "%s/%s/status", base, entry->d_name);
        file = fopen(path, "r");
        if(file != NULL) {
            if(fgets(status, sizeof(status), file) == NULL)
                status[0] = '\0';
            fclose(file);
        }
        closedir(directory);
        *percent = value;
        *charging = strstr(status, "Charging") != NULL ||
                    strstr(status, "Full") != NULL;
        return 1;
    }
    closedir(directory);
    return 0;
}

/* Legacy XEmbed system tray host. Docked icons are reparented into an
   override-redirect window that the shell positions over its panel slot;
   the X server composites the icons, so no pixel capture is required. */
#define RILL_XEMBED_MAX 12
#define RILL_XEMBED_STEP 22

static Display *xembed_display;
static Window xembed_host;
static Window xembed_slots[RILL_XEMBED_MAX];
static int xembed_slot_count;
static int xembed_started;
static int xembed_last_x = -1, xembed_last_y, xembed_last_height;
static int xembed_last_visible = -1;

static void
xembed_remove_slot(int index)
{
    memmove(&xembed_slots[index], &xembed_slots[index + 1],
            (size_t)(xembed_slot_count - index - 1) * sizeof(xembed_slots[0]));
    xembed_slot_count--;
    xembed_last_x = -1; /* force a relayout of the remaining icons */
}

static void
xembed_pump_events(void)
{
    while(XPending(xembed_display)) {
        XEvent event;
        XNextEvent(xembed_display, &event);
        if(event.type == ClientMessage &&
           event.xclient.message_type ==
               XInternAtom(xembed_display, "_NET_SYSTEM_TRAY_MESSAGE", False) &&
           event.xclient.data.l[0] == 0 &&
           event.xclient.data.l[1] != None) {
            Window icon = (Window)event.xclient.data.l[1];
            XWindowAttributes attributes;
            XEvent notify;

            if(xembed_slot_count >= RILL_XEMBED_MAX ||
               !XGetWindowAttributes(xembed_display, icon, &attributes) ||
               attributes.override_redirect)
                continue;
            XSelectInput(xembed_display, icon, StructureNotifyMask);
            XReparentWindow(xembed_display, icon, xembed_host,
                            xembed_slot_count * RILL_XEMBED_STEP, 0);
            XMapWindow(xembed_display, icon);
            memset(&notify, 0, sizeof(notify));
            notify.xclient.type = ClientMessage;
            notify.xclient.window = icon;
            notify.xclient.message_type = XInternAtom(xembed_display, "_XEMBED",
                                                      False);
            notify.xclient.format = 32;
            notify.xclient.data.l[1] = 0; /* XEMBED_EMBEDDED_NOTIFY */
            notify.xclient.data.l[2] = xembed_host;
            notify.xclient.data.l[3] = 0; /* XEMBED protocol version */
            XSendEvent(xembed_display, icon, False, NoEventMask, &notify);
            xembed_slots[xembed_slot_count++] = icon;
            xembed_last_x = -1;
        } else if(event.type == DestroyNotify || event.type == UnmapNotify) {
            Window changed = event.type == DestroyNotify ?
                             event.xdestroywindow.window :
                             event.xunmap.window;
            for(int i = 0; i < xembed_slot_count; i++)
                if(xembed_slots[i] == changed) {
                    xembed_remove_slot(i);
                    break;
                }
        }
    }
}

static int
xembed_start(void)
{
    XSetWindowAttributes attributes;
    char name[40];
    Window selection_owner;
    Atom selection;

    if(xembed_started)
        return xembed_display != NULL;
    xembed_started = 1;
    if(RillWaylandSession())
        return 0;
    xembed_display = XOpenDisplay(NULL);
    if(xembed_display == NULL)
        return 0;
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d",
             DefaultScreen(xembed_display));
    selection = XInternAtom(xembed_display, name, False);
    selection_owner = XGetSelectionOwner(xembed_display, selection);
    if(selection_owner != None) {
        /* The real Xfce panel or another host already provides the tray. */
        XCloseDisplay(xembed_display);
        xembed_display = NULL;
        return 0;
    }
    memset(&attributes, 0, sizeof(attributes));
    attributes.override_redirect = True;
    attributes.event_mask = SubstructureNotifyMask | StructureNotifyMask;
    attributes.background_pixel =
        BlackPixel(xembed_display, DefaultScreen(xembed_display));
    xembed_host = XCreateWindow(
        xembed_display, DefaultRootWindow(xembed_display), -100, -100, 1, 1, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel, &attributes);
    XSetSelectionOwner(xembed_display, selection, xembed_host, CurrentTime);
    if(XGetSelectionOwner(xembed_display, selection) != xembed_host) {
        XCloseDisplay(xembed_display);
        xembed_display = NULL;
        return 0;
    }
    {
        XEvent manager;
        memset(&manager, 0, sizeof(manager));
        manager.xclient.type = ClientMessage;
        manager.xclient.window = DefaultRootWindow(xembed_display);
        manager.xclient.message_type = XInternAtom(xembed_display, "MANAGER",
                                                   False);
        manager.xclient.format = 32;
        manager.xclient.data.l[1] = selection;
        manager.xclient.data.l[2] = xembed_host;
        XSendEvent(xembed_display, DefaultRootWindow(xembed_display), False,
                   StructureNotifyMask, &manager);
    }
    XFlush(xembed_display);
    return 1;
}

static int
linux_xembed_tray_count(void)
{
    if(!xembed_start())
        return 0;
    xembed_pump_events();
    XFlush(xembed_display);
    return xembed_slot_count;
}

static void
linux_xembed_tray_layout(int x, int y, int height, int visible)
{
    int width;

    if(!xembed_start())
        return;
    xembed_pump_events();
    if(xembed_slot_count == 0)
        return;
    width = xembed_slot_count * RILL_XEMBED_STEP + 4;
    if(visible && (x != xembed_last_x || y != xembed_last_y ||
                   height != xembed_last_height ||
                   xembed_last_visible != 1)) {
        XMoveResizeWindow(xembed_display, xembed_host, x, y, width, height);
        xembed_last_x = x;
        xembed_last_y = y;
        xembed_last_height = height;
        xembed_last_visible = 1;
    } else if(!visible && xembed_last_visible != 0) {
        XMoveWindow(xembed_display, xembed_host, -100 - width, -100);
        xembed_last_visible = 0;
    }
    for(int i = 0; i < xembed_slot_count; i++) {
        int icon_x = i * RILL_XEMBED_STEP + 3;
        int icon_y = (height - 18) / 2;
        XWindowAttributes attributes;
        if(XGetWindowAttributes(xembed_display, xembed_slots[i], &attributes) &&
           (attributes.x != icon_x || attributes.y != icon_y))
            XMoveWindow(xembed_display, xembed_slots[i], icon_x, icon_y);
    }
    XFlush(xembed_display);
}

/* Default-sink volume through pactl, which serves both PulseAudio and
   PipeWire. Commands run through PATH so tests can substitute a fake. */
static int
linux_run_capture(const char *command, char *out, size_t size)
{
    gchar *output = NULL;
    GError *error = NULL;

    if(out != NULL && size > 0)
        out[0] = '\0';
    if(!g_spawn_command_line_sync(command, &output, NULL, NULL, &error)) {
        if(error != NULL)
            g_error_free(error);
        return 0;
    }
    if(out != NULL && size > 0 && output != NULL)
        snprintf(out, size, "%s", output);
    g_free(output);
    return 1;
}

static int
linux_volume_state(int *percent, int *muted)
{
    char output[1024];
    const char *percent_sign;
    const char *digits;
    int value = -1;

    if(percent == NULL || muted == NULL)
        return 0;
    if(!linux_run_capture("pactl get-sink-volume @DEFAULT_SINK@", output,
                          sizeof(output)))
        return 0;
    percent_sign = strchr(output, '%');
    if(percent_sign == NULL)
        return 0;
    digits = percent_sign;
    while(digits > output && digits[-1] >= '0' && digits[-1] <= '9')
        digits--;
    if(digits == percent_sign)
        return 0;
    value = atoi(digits);
    if(value < 0 || value > 100)
        return 0;
    *percent = value;
    if(linux_run_capture("pactl get-sink-mute @DEFAULT_SINK@", output,
                         sizeof(output)))
        *muted = strstr(output, "Mute: yes") != NULL;
    else
        *muted = 0;
    return 1;
}

static int
linux_volume_set(int percent, int muted)
{
    char command[128];
    int ok = 1;

    if(percent >= 0 && percent <= 100) {
        snprintf(command, sizeof(command),
                 "pactl set-sink-volume @DEFAULT_SINK@ %d%%", percent);
        ok = linux_run_capture(command, NULL, 0) && ok;
    }
    if(muted == 0 || muted == 1) {
        snprintf(command, sizeof(command), "pactl set-sink-mute @DEFAULT_SINK@ %d",
                 muted);
        ok = linux_run_capture(command, NULL, 0) && ok;
    }
    return ok;
}

static int
tray_item_property(const char *bus, const char *path, const char *name,
                   GVariant **out)
{
    GError *error = NULL;
    GVariant *value;

    if(tray_connection == NULL)
        return 0;
    value = g_dbus_connection_call_sync(tray_connection, bus, path,
                                        "org.freedesktop.DBus.Properties", "Get",
                                        g_variant_new("(ss)",
                                                      "org.kde.StatusNotifierItem",
                                                      name),
                                        G_VARIANT_TYPE("(v)"),
                                        G_DBUS_CALL_FLAGS_NONE, 800, NULL,
                                        &error);
    if(value == NULL) {
        if(error != NULL)
            g_error_free(error);
        return 0;
    }
    g_variant_get(value, "(v)", out);
    if(*out != NULL)
        g_variant_ref(*out);
    g_variant_unref(value);
    return *out != NULL;
}



static int
linux_tray_icons(RillTrayIcon *out, int cap)
{
    int count = 0;

    tray_start();
    for(int i = 0; i < 4; i++)
        g_main_context_iteration(NULL, FALSE);
    if(out == NULL || cap <= 0)
        return 0;
    for(int i = tray_registration_count - 1; i >= 0; i--) {
        TrayRegistration *item = &tray_registrations[i];
        GVariant *title = NULL;
        GVariant *pixmap = NULL;
        const gchar *text;
        gint32 width, height;
        GVariant *pixels;
        gsize bytes;

        /* Items that vanish from the bus are dropped on the next poll. */
        if(!tray_item_property(item->bus, item->path, "Title", &title)) {
            memmove(&tray_registrations[i], &tray_registrations[i + 1],
                    (size_t)(tray_registration_count - i - 1) *
                        sizeof(tray_registrations[0]));
            tray_registration_count--;
            continue;
        }
        if(count >= cap) {
            g_variant_unref(title);
            continue;
        }
        snprintf(out[count].id, sizeof(out[count].id), "%s", item->bus);
        text = g_variant_get_string(title, NULL);
        snprintf(out[count].title, sizeof(out[count].title), "%s",
                 text != NULL ? text : "");
        g_variant_unref(title);
        out[count].width = 0;
        out[count].height = 0;
        out[count].argb = NULL;
        if(tray_item_property(item->bus, item->path, "IconPixmap", &pixmap)) {
            g_variant_get(pixmap, "(ii@ay)", &width, &height, &pixels);
            bytes = g_variant_n_children(pixels);
            if(getenv("RILL_TRAY_DEBUG"))
                fprintf(stderr, "rill: tray pixmap %dx%d bytes=%lu\n", width,
                        height, (unsigned long)bytes);
            if(width > 0 && height > 0 && width <= 256 && height <= 256 &&
               bytes >= (gsize)width * height * 4) {
                out[count].argb = malloc((size_t)width * height * 4);
                if(out[count].argb != NULL) {
                    memcpy(out[count].argb, g_variant_get_data(pixels),
                           (size_t)width * height * 4);
                    out[count].width = width;
                    out[count].height = height;
                }
            }
            g_variant_unref(pixels);
            g_variant_unref(pixmap);
        }
        count++;
    }
    return count;
}

static int
linux_tray_activate(const char *id, int secondary)
{
    GError *error = NULL;
    const char *method = secondary ? "SecondaryActivate" : "Activate";

    if(tray_connection == NULL || id == NULL)
        return 0;
    for(int i = 0; i < tray_registration_count; i++)
        if(strcmp(tray_registrations[i].bus, id) == 0) {
            g_dbus_connection_call_sync(tray_connection,
                                        tray_registrations[i].bus,
                                        tray_registrations[i].path,
                                        "org.kde.StatusNotifierItem", method,
                                        g_variant_new("(ii)", 0, 0),
                                        NULL, G_DBUS_CALL_FLAGS_NONE, 800,
                                        NULL, &error);
            if(error != NULL) {
                if(getenv("RILL_TRAY_DEBUG"))
                    fprintf(stderr, "rill: tray activate %s failed: %s\n", method,
                            error->message);
                g_error_free(error);
                return 0;
            }
            return 1;
        }
    return 0;
}

static int
linux_list_desktop_files(RillLauncher *out, int cap)
{
    const char *home = getenv("HOME");
    char dir[1024];
    GHashTable *seen;
    int count;

    if(out == NULL || cap <= 0 || home == NULL || home[0] == '\0')
        return 0;
    snprintf(dir, sizeof(dir), "%s/Desktop", home);
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    count = scan_desktop_dir(dir, out, cap, 0, 0, dir, seen);
    g_hash_table_destroy(seen);
    return count;
}

static int
linux_open_path(const char *path)
{
    GError *error = NULL;
    char *uri;
    gboolean ok;

    if(path == NULL || path[0] == '\0')
        return 0;
    if(strstr(path, "://") != NULL)
        uri = g_strdup(path);
    else
        uri = g_filename_to_uri(path, NULL, NULL);
    if(uri == NULL)
        uri = g_strdup(path);
    ok = g_app_info_launch_default_for_uri(uri, NULL, &error);
    if(!ok) {
        if(error != NULL) {
            fprintf(stderr, "rill: cannot open %s: %s\n", path,
                    error->message);
            g_error_free(error);
        }
    }
    g_free(uri);
    return ok;
}

static const RillPlatformServices services = {
    "xlibre",
    linux_list_launchers,
    linux_list_tasks,
    linux_launch,
    linux_focus_task,
    linux_close_task,
    linux_settings_root,
    linux_workspace_count, linux_current_workspace, linux_switch_workspace,
    linux_list_wallpapers,
    linux_session_action,
    linux_show_desktop,
    linux_tray_icons,
    linux_tray_activate,
    linux_list_desktop_files,
    linux_open_path,
    linux_notifications_poll,
    linux_notification_action,
    linux_battery_state,
    linux_xembed_tray_count,
    linux_xembed_tray_layout,
    linux_volume_state,
    linux_volume_set
};

const RillPlatformServices *
RillPlatformCurrent(void)
{
    return &services;
}
