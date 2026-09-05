#include "rill_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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
    snprintf(app_path, sizeof(app_path), "%s/user/example.desktop", root);
    unlink(app_path);
    unlink(hidden_path);
    unlink(terminal_path);
    rmdir(user_apps);
    rmdir(root);
    return failures == 0 ? 0 : 1;
}
