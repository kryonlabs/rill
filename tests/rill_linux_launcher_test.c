#include "rill_platform.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <dirent.h>

static int
write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    if(file == NULL)
        return 0;
    fputs(text, file);
    fclose(file);
    return 1;
}

static void
check(const char *name, int ok, int *failures)
{
    if(!ok) {
        fprintf(stderr, "rill linux launcher test failed: %s\n", name);
        (*failures)++;
    }
}


int
main(int argc, char **argv)
{
    struct rlimit limits;

    getrlimit(RLIMIT_NOFILE, &limits);
    limits.rlim_cur = limits.rlim_max;
    setrlimit(RLIMIT_NOFILE, &limits);
    if(argc > 2 && strcmp(argv[1], "--probe") == 0) {
        FILE *file = fopen(argv[2], "w");
        char cwd[1024];
        if(file == NULL || getcwd(cwd, sizeof(cwd)) == NULL) return 1;
        fprintf(file, "%s\n%s\n%s\n%s\n", getenv("WAYLAND_DISPLAY"),
                getenv("DBUS_SESSION_BUS_ADDRESS"), cwd, argc > 3 ? argv[3] : "");
        return fclose(file) != 0;
    }
    char root[] = "/tmp/rill-linux-launchers.XXXXXX";
    char app_path[512];
    char user_apps[512];
    char hidden_path[512];
    char terminal_path[512];
    char system_dir[512], system_apps[512], override_path[512];
    char probe_path[512], probe_result[512], probe_text[4096], executable[1024];
    char walls[2][512];
    RillLauncher launchers[8];
    const RillPlatformServices *platform;
    int count;
    int hosted = -1, terminal = -1;
    int failures = 0;

    if(mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(user_apps, sizeof(user_apps), "%s/user", root);
    mkdir(user_apps, 0700);
    snprintf(app_path, sizeof(app_path), "%s/user/example.desktop", root);
    snprintf(hidden_path, sizeof(hidden_path), "%s/user/hidden.desktop", root);
    snprintf(terminal_path, sizeof(terminal_path), "%s/user/terminalonly.desktop", root);

    check("write app",
          write_file(app_path,
                     "[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name[ar]=طرفية ريل\n"
                     "Name=Rill Terminal\n"
                     "Comment[ar]=طرفية مضمّنة\n"
                     "Comment=Hosted terminal\n"
                     "Exec=host:ktrem %U\n"
                     "Categories=System;Utility;\n"
                     "X-Rill-ID=terminal\n"
                     "X-Rill-Favorite=true\n"),
          &failures);
    check("write hidden",
          write_file(hidden_path,
                     "[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=Hidden\n"
                     "Exec=hidden\n"
                     "NoDisplay=true\n"),
          &failures);
    check("write terminal",
          write_file(terminal_path,
                     "[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=Terminal Only\n"
                     "Exec=sh\n"
                     "Terminal=true\n"),
          &failures);
    if(failures)
        return 1;

    setenv("RILL_APPLICATION_DIRS", user_apps, 1);
    setenv("XDG_DATA_HOME", "/tmp/rill-linux-launchers-empty-home", 1);
    snprintf(system_dir, sizeof(system_dir), "%s/system", root);
    snprintf(system_apps, sizeof(system_apps), "%s/system/applications", root);
    check("system directory", mkdir(system_dir, 0700) == 0 && mkdir(system_apps, 0700) == 0, &failures);
    snprintf(override_path, sizeof(override_path), "%s/system/applications/hidden.desktop", root);
    check("write overridden app", write_file(override_path,
          "[Desktop Entry]\nType=Application\nName=Should stay hidden\nExec=/bin/true\n"), &failures);
    setenv("XDG_DATA_DIRS", system_dir, 1);
    setenv("LANGUAGE", "C", 1);

    platform = RillPlatformCurrent();
    count = platform->list_launchers(launchers, 8);

    check("count", count == 2, &failures);
    for(int i = 0; i < count; i++) {
        if(strcmp(launchers[i].id, "terminal") == 0) hosted = i;
        if(strcmp(launchers[i].id, "terminalonly") == 0) terminal = i;
    }
    check("host present", hosted >= 0, &failures);
    check("terminal application present", terminal >= 0, &failures);
    if(hosted < 0 || terminal < 0) return 1;
    check("desktop source retained", strcmp(launchers[terminal].desktop_file, terminal_path) == 0, &failures);
    check("id", strcmp(launchers[hosted].id, "terminal") == 0, &failures);
    check("name", strcmp(launchers[hosted].name, "Rill Terminal") == 0, &failures);
    check("command strips field codes",
          strcmp(launchers[hosted].command, "host:ktrem") == 0, &failures);
    check("category", strcmp(launchers[hosted].category, "Settings") == 0,
          &failures);
    check("favorite", launchers[hosted].favorite, &failures);

    /* Launch through GIO, preserving the bus/display and honoring Path and %c. */
    snprintf(probe_path, sizeof(probe_path), "%s/user/probe.desktop", root);
    snprintf(probe_result, sizeof(probe_result), "%s/probe-result", root);
    ssize_t exe_len = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    check("find probe executable", exe_len > 0, &failures);
    if(exe_len <= 0) return 1;
    executable[exe_len] = '\0';
    snprintf(probe_text, sizeof(probe_text),
        "[Desktop Entry]\nType=Application\nName=Probe Name With Spaces\n"
        "Exec=\"%s\" --probe %s %%c\nPath=%s\n", executable, probe_result, root);
    check("write probe", write_file(probe_path, probe_text), &failures);
    count = platform->list_launchers(launchers, 8);
    int probe = -1;
    for(int i = 0; i < count; i++) if(strcmp(launchers[i].id, "probe") == 0) probe = i;
    check("probe discovered", probe >= 0, &failures);
    if(probe >= 0) {
        setenv("WAYLAND_DISPLAY", "wayland-rill-probe", 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/rill-no-probe-bus", 1);
        unsetenv("RILL_CONTAINED_X11");
        check("probe launched", platform->launch(&launchers[probe]), &failures);
        FILE *result = NULL;
        for(int i = 0; i < 100; i++) {
            result = fopen(probe_result, "r");
            if(result != NULL) break;
            usleep(10000);
        }
        check("probe executed", result != NULL, &failures);
        if(result != NULL) {
            size_t n = fread(probe_text, 1, sizeof(probe_text) - 1, result);
            probe_text[n] = '\0';
            fclose(result);
            check("Wayland environment preserved", strstr(probe_text, "wayland-rill-probe\n") != NULL, &failures);
            check("bus environment preserved", strstr(probe_text, "unix:path=/tmp/rill-no-probe-bus\n") != NULL, &failures);
            check("working directory respected", strstr(probe_text, root) != NULL, &failures);
            check("name field code expanded", strstr(probe_text, "Probe Name With Spaces\n") != NULL, &failures);
        }
    }
    unlink(probe_result); unlink(probe_path);
    unlink(override_path); rmdir(system_apps); rmdir(system_dir);
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(app_path, sizeof(app_path), "%s/rill", root);
    check("settings in Rill subdirectory", strcmp(platform->settings_root(), app_path) == 0, &failures);

    /* Session actions are recorded for tests instead of reaching login1. */
    snprintf(app_path, sizeof(app_path), "%s/session-actions", root);
    setenv("RILL_SESSION_ACTION_FILE", app_path, 1);
    check("logout records an action", platform->session_action != NULL &&
          platform->session_action("logout"), &failures);
    check("suspend records an action", platform->session_action("suspend"),
          &failures);
    check("lock records an action", platform->session_action("lock"),
          &failures);
    check("unknown action rejected", !platform->session_action("moon"),
          &failures);
    {
        FILE *record = fopen(app_path, "r");
        char log[160] = "";
        if(record != NULL) {
            size_t n = fread(log, 1, sizeof(log) - 1, record);
            log[n] = '\0';
            fclose(record);
        }
        check("session actions logged",
              strcmp(log, "logout\nsuspend\nlock\n") == 0, &failures);
    }
    unsetenv("RILL_SESSION_ACTION_FILE");
    setenv("RILL_LOCK_COMMAND", "/bin/false", 1);
    check("failed locker does not report success", !platform->session_action("lock"), &failures);
    check("failed locker prevents suspend", !platform->session_action("suspend"), &failures);
    setenv("RILL_LOCK_COMMAND", "/bin/true", 1);
    check("successful locker command acknowledged", platform->session_action("lock"), &failures);
    unsetenv("RILL_LOCK_COMMAND");
    unlink(app_path);
    check("wallpaper listing is bounded", platform->list_wallpapers != NULL &&
          platform->list_wallpapers(walls, 2) <= 2, &failures);

    /* Tray hosting and notification server, end to end on one private bus. */
    {
        char command[2048];
        char socket_path[512];
        char conf_path[512];
        char address[256] = "";
        char log_text[160] = "";
        const char *previous_bus = getenv("DBUS_SESSION_BUS_ADDRESS");
        char saved_bus[256];
        RillTrayIcon icons[4];
        RillNotification notes[4];
        int tray_count = 0;
        int note_count = 0;
        unsigned int first_id = 0;
        pid_t mock_pid = 0;
        FILE *record;

        if(previous_bus != NULL)
            snprintf(saved_bus, sizeof(saved_bus), "%s", previous_bus);
        else
            saved_bus[0] = '\0';
        /* A config without servicedirs keeps the installed xfce4-notifyd from
           activating and winning the notification name; the concrete listen
           path makes teardown by socket possible. */
        snprintf(socket_path, sizeof(socket_path), "%s/mock-bus", root);
        snprintf(conf_path, sizeof(conf_path), "%s/mock-dbus.conf", root);
        snprintf(command, sizeof(command),
                 "<busconfig>\n"
                 "  <type>session</type>\n"
                 "  <listen>unix:path=%s</listen>\n"
                 "  <auth>EXTERNAL</auth>\n"
                 "  <policy context=\"default\">\n"
                 "    <allow send_destination=\"*\"/>\n"
                 "    <allow receive_sender=\"*\"/>\n"
                 "    <allow own=\"*\"/>\n"
                 "  </policy>\n"
                 "</busconfig>\n",
                 socket_path);
        check("write mock bus config", write_file(conf_path, command),
              &failures);
        snprintf(app_path, sizeof(app_path), "%s/mock-address", root);
        snprintf(probe_result, sizeof(probe_result), "%s/mock-ready", root);
        snprintf(probe_path, sizeof(probe_path), "%s/mock-activated", root);
        snprintf(executable, sizeof(executable), "%s/mock-pid", root);
        snprintf(log_text, sizeof(log_text), "%s", probe_path);
        snprintf(command, sizeof(command),
                 "setsid sh -c 'echo $$ > %s; exec dbus-run-session "
                 "--config-file=%s -- sh -c "
                 "\"echo \\$DBUS_SESSION_BUS_ADDRESS > %s; "
                 "exec python3 " RILL_DBUS_MOCK " %s %s %s\"' "
                 ">/dev/null 2>&1 &",
                 executable, conf_path, app_path, probe_result, probe_path,
                 log_text);
        check("start dbus mock", system(command) == 0, &failures);
        for(int i = 0; i < 100; i++) {
            FILE *file = fopen(app_path, "r");
            if(file != NULL && fgets(address, sizeof(address), file) != NULL) {
                fclose(file);
                if(address[0] != '\0')
                    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
            } else if(file != NULL)
                fclose(file);
            /* Give the young daemon time to accept before pumping; connecting
               during its startup can stall the synchronous handshake. */
            usleep(300000);
            /* Pump both hosts: the mock waits for the tray watcher and the
               notification server before it reports readiness. */
            platform->tray_icons(NULL, 0);
            platform->notifications_poll(NULL, 0);
            file = fopen(probe_result, "r");
            if(file != NULL) {
                fclose(file);
                break;
            }
        }
        record = fopen(executable, "r");
        if(record != NULL) {
            if(fscanf(record, "%d", &mock_pid) != 1)
                mock_pid = 0;
            fclose(record);
        }
        check("mock session started", address[0] != '\0', &failures);

        for(int i = 0; i < 50 && tray_count == 0; i++) {
            tray_count = platform->tray_icons(icons, 4);
            if(tray_count == 0)
                usleep(200000);
        }
        check("tray host lists the mock item", tray_count >= 1 &&
              strcmp(icons[0].title, "Rill tray mock") == 0, &failures);
        check("tray pixmap delivered", tray_count >= 1 && icons[0].width == 8 &&
              icons[0].height == 8 && icons[0].argb != NULL, &failures);
        check("tray activation reaches the item", tray_count >= 1 &&
              platform->tray_activate(icons[0].id, 1), &failures);
        for(int i = 0; i < 20; i++) {
            record = fopen(probe_path, "r");
            if(record != NULL) {
                size_t n = fread(log_text, 1, sizeof(log_text) - 1, record);
                log_text[n] = '\0';
                fclose(record);
                if(strstr(log_text, "SecondaryActivate") != NULL)
                    break;
            }
            usleep(100000);
        }
        check("secondary activation recorded",
              strstr(log_text, "SecondaryActivate") != NULL, &failures);
        for(int i = 0; i < tray_count; i++)
            free(icons[i].argb);

        for(int i = 0; i < 50 && note_count == 0; i++) {
            note_count = platform->notifications_poll(notes, 4);
            if(note_count == 0)
                usleep(200000);
        }
        check("notification banner delivered", note_count >= 1 &&
              strcmp(notes[0].summary, "Rill note one") == 0 &&
              strcmp(notes[0].body, "Banner body text") == 0, &failures);
        first_id = note_count >= 1 ? notes[0].id : 0;
        /* The default action must reach the client, which then notifies again. */
        check("default action invoked", first_id != 0 &&
              platform->notification_action(first_id, 0), &failures);
        note_count = 0;
        for(int i = 0; i < 50 && note_count == 0; i++) {
            note_count = platform->notifications_poll(notes, 4);
            if(note_count == 0)
                usleep(200000);
        }
        check("action triggers the follow-up notification", note_count >= 1 &&
              strcmp(notes[0].summary, "Rill note two") == 0, &failures);
        if(note_count >= 1) {
            platform->notification_action(notes[0].id, 1);
            check("dismissed notification disappears",
                  platform->notifications_poll(notes, 4) == 0, &failures);
        }

        /* setsid groups the wrapper, mock and daemon; the daemon additionally
           carries this run's unique config path and socket. */
        if(mock_pid > 0)
            kill(-mock_pid, SIGKILL);
        snprintf(command, sizeof(command),
                 "pkill -f 'dbus-run-session --config-file=%s' 2>/dev/null; "
                 "pkill -f 'dbus-daemon .*%s' 2>/dev/null; "
                 "fuser -k '%s' 2>/dev/null",
                 conf_path, conf_path, socket_path);
        system(command);
        if(saved_bus[0] != '\0')
            setenv("DBUS_SESSION_BUS_ADDRESS", saved_bus, 1);
        else
            unsetenv("DBUS_SESSION_BUS_ADDRESS");
        unlink(conf_path);
        unlink(app_path);
        unlink(probe_result);
        unlink(probe_path);
        unlink(executable);
    }

    /* Default-sink volume runs through pactl, faked via PATH for the test. */
    {
        char fake_dir[512];
        char fake_pactl[600];
        char record[512];
        char script[1024];
        char command[2600];
        char original_path[2048];
        const char *saved_path = getenv("PATH");
        char volume_log[400] = "";
        int percent = 0;
        int muted = 0;
        FILE *file;

        snprintf(fake_dir, sizeof(fake_dir), "%s/bin", root);
        mkdir(fake_dir, 0700);
        snprintf(record, sizeof(record), "%s/volume-log", root);
        snprintf(fake_pactl, sizeof(fake_pactl), "%s/pactl", fake_dir);
        snprintf(script, sizeof(script),
                 "#!/bin/sh\n"
                 "case \"$1\" in\n"
                 "  get-sink-volume) echo 'Volume: front-left: 27525 /"
                 "  42%% / -24.06 dB' ;;\n"
                 "  get-sink-mute) echo 'Mute: yes' ;;\n"
                 "  set-sink-*) echo \"$*\" >> '%s' ;;\n"
                 "esac\n",
                 record);
        check("write fake pactl", write_file(fake_pactl, script), &failures);
        chmod(fake_pactl, 0700);
        snprintf(original_path, sizeof(original_path), "%s",
                 saved_path != NULL ? saved_path : "/usr/bin:/bin");
        snprintf(command, sizeof(command), "%s:%s", fake_dir, original_path);
        setenv("PATH", command, 1);
        check("volume state parsed", platform->volume_state != NULL &&
              platform->volume_state(&percent, &muted) && percent == 42 &&
              muted, &failures);
        check("volume set dispatched", platform->volume_set != NULL &&
              platform->volume_set(55, 0), &failures);
        file = fopen(record, "r");
        if(file != NULL) {
            size_t n = fread(volume_log, 1, sizeof(volume_log) - 1, file);
            volume_log[n] = '\0';
            fclose(file);
        }
        check("volume commands recorded",
              strstr(volume_log, "set-sink-volume @DEFAULT_SINK@ 55%") !=
              NULL &&
              strstr(volume_log, "set-sink-mute @DEFAULT_SINK@ 0") != NULL,
              &failures);
        setenv("PATH", original_path, 1);
        unlink(fake_pactl);
        unlink(record);
        rmdir(fake_dir);
    }

    /* Battery state reads the platform power-supply directory. */
    {
        int percent = 0;
        int charging = 0;
        char battery[512];

        snprintf(battery, sizeof(battery), "%s/power/BAT0", root);
        snprintf(app_path, sizeof(app_path), "%s/power", root);
        mkdir(app_path, 0700);
        mkdir(battery, 0700);
        snprintf(app_path, sizeof(app_path), "%s/power/BAT0/capacity", root);
        check("write battery capacity", write_file(app_path, "87\n"),
              &failures);
        snprintf(app_path, sizeof(app_path), "%s/power/BAT0/status", root);
        check("write battery status", write_file(app_path, "Charging\n"),
              &failures);
        snprintf(app_path, sizeof(app_path), "%s/power", root);
        setenv("RILL_BATTERY_DIR", app_path, 1);
        check("battery state discovered", platform->battery_state != NULL &&
              platform->battery_state(&percent, &charging) &&
              percent == 87 && charging, &failures);
        unsetenv("RILL_BATTERY_DIR");
        snprintf(app_path, sizeof(app_path), "%s/power/BAT0/capacity", root);
        unlink(app_path);
        snprintf(app_path, sizeof(app_path), "%s/power/BAT0/status", root);
        unlink(app_path);
        rmdir(battery);
        snprintf(app_path, sizeof(app_path), "%s/power", root);
        rmdir(app_path);
    }

    /* Desktop directory entries feed the desktop icon grid. */
    {
        char home[512], desktop[512], original_home[512];
        RillLauncher desktop_launchers[4];
        const char *saved_home = getenv("HOME");

        snprintf(original_home, sizeof(original_home), "%s",
                 saved_home != NULL ? saved_home : "");
        snprintf(home, sizeof(home), "%s/deskhome", root);
        mkdir(home, 0700);
        snprintf(desktop, sizeof(desktop), "%s/Desktop", home);
        mkdir(desktop, 0700);
        snprintf(app_path, sizeof(app_path), "%s/notes.desktop", desktop);
        check("write desktop entry",
              write_file(app_path,
                         "[Desktop Entry]\n"
                         "Type=Application\n"
                         "Name=Desktop Notes\n"
                         "Exec=touch /tmp/rill-desktop-probe\n"),
              &failures);
        setenv("HOME", home, 1);
        count = platform->list_desktop_files(desktop_launchers, 4);
        check("desktop entries discovered",
              platform->list_desktop_files != NULL && count == 1 &&
              strcmp(desktop_launchers[0].name, "Desktop Notes") == 0,
              &failures);
        char regular[1024], renamed[1024], folder[1024], error[256];
        snprintf(regular, sizeof(regular), "%s/notes with spaces.txt", desktop);
        check("create ordinary desktop file", write_file(regular, "keep me\n"), &failures);
        snprintf(folder, sizeof(folder), "%s/Documents", desktop);
        check("create desktop folder", platform->file_operation("mkdir", desktop, "Documents",
                                                               error, sizeof(error)), &failures);
        count = platform->list_desktop_files(desktop_launchers, 4);
        check("ordinary files and directories discovered", count == 3, &failures);
        int found_file = 0, found_folder = 0;
        for(int i = 0; i < count; i++) {
            if(strcmp(desktop_launchers[i].file_path, regular) == 0)
                found_file = strcmp(desktop_launchers[i].name, "notes with spaces.txt") == 0;
            if(strcmp(desktop_launchers[i].file_path, folder) == 0)
                found_folder = strcmp(desktop_launchers[i].description, "Folder") == 0;
        }
        check("file identity and folder type", found_file && found_folder, &failures);
        check("refuse rename over existing file", !platform->file_operation("rename", regular,
              "notes.desktop", error, sizeof(error)) && error[0], &failures);
        check("refuse path traversal name", !platform->file_operation("mkdir", desktop,
              "../outside", error, sizeof(error)), &failures);
        check("rename with Unicode and spaces", platform->file_operation("rename", regular,
              "café notes.txt", error, sizeof(error)), &failures);
        snprintf(renamed, sizeof(renamed), "%s/café notes.txt", desktop);
        check("renamed file exists", access(renamed, F_OK) == 0 && access(regular, F_OK) != 0,
              &failures);
        check("copy into folder", platform->file_operation("copy", renamed, folder,
                                                          error, sizeof(error)), &failures);
        check("copy refuses overwrite", !platform->file_operation("copy", renamed, folder,
                                                                 error, sizeof(error)), &failures);
        snprintf(regular, sizeof(regular), "%s/Documents/café notes.txt", desktop);
        unlink(regular);
        unlink(renamed);
        rmdir(folder);
        count = platform->list_desktop_files(desktop_launchers, 4);
        check("deleted files disappear", count == 1, &failures);
        /* Read user-dirs as data without evaluating commands or shell variables. */
        const char *old_config = getenv("XDG_CONFIG_HOME");
        char *saved_config = old_config ? strdup(old_config) : NULL;
        setenv("XDG_CONFIG_HOME", home, 1);
        snprintf(regular, sizeof(regular), "%s/user-dirs.dirs", home);
        write_file(regular, "XDG_DESKTOP_DIR=\"$HOME/Escritorio\"\n");
        snprintf(folder, sizeof(folder), "%s/Escritorio", home);
        check("localized XDG desktop folder", strcmp(platform->desktop_directory(), folder) == 0,
              &failures);
        if(saved_config) { setenv("XDG_CONFIG_HOME", saved_config, 1); free(saved_config); }
        else unsetenv("XDG_CONFIG_HOME");
        unlink(regular);
        if(original_home[0] != '\0')
            setenv("HOME", original_home, 1);
        else
            unsetenv("HOME");
        unlink(app_path);
        rmdir(desktop);
        rmdir(home);
    }
    snprintf(app_path, sizeof(app_path), "%s/user/example.desktop", root);
    unlink(app_path);
    unlink(hidden_path);
    unlink(terminal_path);
    rmdir(user_apps);
    rmdir(root);
    return failures == 0 ? 0 : 1;
}
