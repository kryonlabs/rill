/* Session-manager parity test: XDG autostart, SESSION_MANAGER publication,
 * XSMP registration/save/die, and logout coordination against rill-sessiond. */
#include <X11/SM/SMlib.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>

static int saw_save_done;
static int saw_die;
static int saw_cancel;
static int cancel_next;
static int request_phase2;

static void
client_complete(SmcConn connection, SmPointer data)
{
    (void)connection;
    (void)data;
}

static void
client_cancelled(SmcConn connection, SmPointer data)
{
    (void)connection;
    (void)data;
    saw_cancel++;
}

static void
client_interact(SmcConn connection, SmPointer data)
{
    (void)data;
    SmcInteractDone(connection, True);
    SmcSaveYourselfDone(connection, True);
}

static void
client_phase2(SmcConn connection, SmPointer data)
{
    (void)data;
    request_phase2 = 0;
    SmcSaveYourselfDone(connection, True);
}

static void
client_save_yourself(SmcConn connection, SmPointer data, int save_type,
                     Bool shutdown, int interact_style, Bool fast)
{
    (void)data;
    (void)save_type;
    (void)interact_style;
    (void)fast;
    (void)shutdown;
    saw_save_done++;
    if(shutdown && cancel_next) {
        cancel_next = 0;
        SmcInteractRequest(connection, SmDialogNormal, client_interact, NULL);
        return;
    }
    if(shutdown && request_phase2) {
        SmcRequestSaveYourselfPhase2(connection, client_phase2, NULL);
        return;
    }
    SmcSaveYourselfDone(connection, True);
}

static void
client_die(SmcConn connection, SmPointer data)
{
    (void)connection;
    (void)data;
    saw_die = 1;
}

static void
pump_ice(IceConn ice, int milliseconds)
{
    struct pollfd fd = {IceConnectionNumber(ice), POLLIN, 0};
    if(poll(&fd, 1, milliseconds) > 0 && (fd.revents & POLLIN))
        IceProcessMessages(ice, NULL, NULL);
}

static int
file_contains(const char *path, const char *needle, int wait_seconds)
{
    for(int i = 0; i < wait_seconds * 10; i++) {
        FILE *file = fopen(path, "r");
        char text[512] = "";
        if(file != NULL) {
            size_t n = fread(text, 1, sizeof(text) - 1, file);
            text[n] = '\0';
            fclose(file);
            if(strstr(text, needle) != NULL)
                return 1;
        }
        usleep(100000);
    }
    return 0;
}

static void
fail(const char *message)
{
    fprintf(stderr, "rill sessiond test failed: %s\n", message);
    exit(1);
}

int
main(int argc, char **argv)
{
    char root[] = "/tmp/rill-sessiond-test.XXXXXX";
    char autostart[256], entry[1024], ran[512], env_file[512], out[512], saved[512];
    char error[256] = "";
    char *client_id = NULL;
    SmcCallbacks callbacks;
    SmcConn connection;
    IceConn ice;
    char control[512] = "";
    char session_manager[512] = "";
    FILE *file;
    pid_t daemon;
    int status;
    char command[1024];

    if(argc != 2 || access(argv[1], X_OK) != 0) {
        fprintf(stderr, "usage: rill_sessiond_test BINARY\n");
        return 2;
    }
    if(mkdtemp(root) == NULL)
        fail("mkdtemp");
    snprintf(autostart, sizeof(autostart), "%s/autostart", root);
    if(mkdir(autostart, 0700) != 0)
        fail("mkdir autostart");
    snprintf(entry, sizeof(entry), "%s/probe.desktop", autostart);
    snprintf(ran, sizeof(ran), "%s/ran", root);
    file = fopen(entry, "w");
    if(file == NULL)
        fail("write entry");
    fputs("[Desktop Entry]\n"
          "Type=Application\n"
          "Name=Probe\n"
          "Exec=touch ", file);
    fputs(ran, file);
    fputs("\n"
          "X-GNOME-Autostart-enabled=true\n", file);
    fclose(file);
    snprintf(entry, sizeof(entry), "%s/hidden.desktop", autostart);
    file = fopen(entry, "w");
    if(file == NULL)
        fail("write hidden entry");
    fputs("[Desktop Entry]\n"
          "Type=Application\n"
          "Name=Hidden\n"
          "Exec=sleep 500\n"
          "Hidden=true\n", file);
    fclose(file);
    snprintf(entry, sizeof(entry), "%s/elsewhere.desktop", autostart);
    file = fopen(entry, "w");
    if(file == NULL)
        fail("write elsewhere entry");
    fputs("[Desktop Entry]\n"
          "Type=Application\n"
          "Name=Elsewhere\n"
          "Exec=sleep 500\n"
          "OnlyShowIn=GNOME\n", file);
    fclose(file);

    snprintf(env_file, sizeof(env_file), "%s/body-env", root);
    snprintf(out, sizeof(out), "%s/out", root);
    snprintf(entry, sizeof(entry),
             "echo SESSION_MANAGER=$SESSION_MANAGER > %s; exec sleep 60",
             env_file);
    snprintf(command, sizeof(command), "--autostart-dir=%s", autostart);
    snprintf(saved, sizeof(saved), "%s/saved-session.ini", root);
    char *daemon_argv[] = {(char *)argv[1], (char *)"--no-default-autostart",
                           command, (char *)"--state-file", saved,
                           (char *)"--", (char *)"sh", (char *)"-c",
                           entry, NULL};
    daemon = fork();
    if(daemon == 0) {
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        /* The manager may ask windows to close at logout; without this it
           could see the developer's live desktop display. */
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
        setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/tmp/rill-sessiond-no-system-bus", 1);
        char runtime[300];
        char home[300];
        snprintf(runtime, sizeof(runtime), "%s/runtime", root);
        mkdir(runtime, 0700);
        setenv("XDG_RUNTIME_DIR", runtime, 1);
        /* Never inherit a live desktop: even a flag regression must not
           reach real autostart directories. */
        snprintf(home, sizeof(home), "%s/home", root);
        mkdir(home, 0700);
        setenv("HOME", home, 1);
        setenv("XDG_CONFIG_HOME", home, 1);
        char stderr_path[600];
        snprintf(stderr_path, sizeof(stderr_path), "%s/stderr", root);
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int errors = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if(fd >= 0) {
            dup2(fd, 1);
            close(fd);
        }
        if(errors >= 0) {
            dup2(errors, 2);
            close(errors);
        }
        execv(argv[1], daemon_argv);
        _exit(127);
    }
    for(int i = 0; i < 100; i++) {
        file = fopen(out, "r");
        if(file != NULL) {
            char line[512];
            while(fgets(line, sizeof(line), file) != NULL)
                if(strncmp(line, "control: ", 9) == 0) {
                    line[strcspn(line, "\n")] = '\0';
                    snprintf(control, sizeof(control), "%s", line + 9);
                }
            fclose(file);
            if(control[0] != '\0')
                break;
        }
        usleep(100000);
    }
    if(control[0] == '\0')
        fail("control fifo path not published");
    if(!file_contains(ran, "", 3) || access(ran, F_OK) != 0)
        fail("autostart entry did not run");
    {
        /* Tripwire: exactly one autostart child may ever spawn here. A leak
           into real autostart directories makes this count explode. */
        char stderr_path[600];
        char buffer[8192];
        FILE *file;
        int spawned = 0;
        size_t n;
        snprintf(stderr_path, sizeof(stderr_path), "%s/stderr", root);
        file = fopen(stderr_path, "r");
        if(file == NULL)
            fail("daemon stderr missing");
        n = fread(buffer, 1, sizeof(buffer) - 1, file);
        buffer[n] = '\0';
        fclose(file);
        for(size_t i = 0; i < n; i++)
            if(memcmp(buffer + i, "rill-sessiond: autostart ", 25) == 0)
                spawned++;
        if(spawned != 1)
            fail("unexpected autostart spawn count; real directories leaked");
    }
    if(!file_contains(env_file, "SESSION_MANAGER=local/", 3))
        fail("SESSION_MANAGER not published to the session body");
    file = fopen(env_file, "r");
    if(file == NULL)
        fail("body env missing");
    if(fgets(session_manager, sizeof(session_manager), file) == NULL ||
       strncmp(session_manager, "SESSION_MANAGER=", 16) != 0)
        fail("body env malformed");
    session_manager[strcspn(session_manager, "\n")] = '\0';
    fclose(file);
    if(access(control, W_OK) != 0)
        fail("control fifo not writable");

    /* Register as an XSMP session client against the new manager. The
       client must read cookies from the same authority file the daemon
       published to, so mirror its HOME and runtime directory. */
    setenv("SESSION_MANAGER", session_manager + 16, 1);
    {
        char mirror[600];
        char *slash = strrchr(control, '/');
        snprintf(mirror, sizeof(mirror), "%.*s/ICEauthority",
                 (int)(slash - control), control);
        setenv("ICEAUTHORITY", mirror, 1);
        snprintf(mirror, sizeof(mirror), "%s/runtime", root);
        setenv("XDG_RUNTIME_DIR", mirror, 1);
        snprintf(mirror, sizeof(mirror), "%s/home", root);
        setenv("HOME", mirror, 1);
    }
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.save_yourself.callback = client_save_yourself;
    callbacks.die.callback = client_die;
    callbacks.save_complete.callback = client_complete;
    callbacks.shutdown_cancelled.callback = client_cancelled;
    connection = SmcOpenConnection(NULL, NULL, 1, 0,
                                   SmcSaveYourselfProcMask | SmcDieProcMask |
                                   SmcSaveCompleteProcMask | SmcShutdownCancelledProcMask,
                                   &callbacks, NULL, &client_id,
                                   sizeof(error), error);
    if(connection == NULL) {
        fprintf(stderr, "SmcOpenConnection error: %s\n", error[0] ? error : "(empty)");
        fail("SmcOpenConnection to rill-sessiond");
    }
    ice = SmcGetIceConnection(connection);
    for(int i = 0; i < 100 && (saw_save_done == 0 || saw_die != 0); i++)
        pump_ice(ice, 50);

    /* A save dialog can cancel logout without killing the desktop body. */
    cancel_next = 1;
    file = fopen(control, "w");
    if(file == NULL)
        fail("open control fifo for cancellable logout");
    fputs("logout\n", file);
    fclose(file);
    for(int i = 0; i < 100 && !saw_cancel; i++)
        pump_ice(ice, 50);
    if(!saw_cancel || saw_die || kill(daemon, 0) != 0)
        fail("logout did not preserve the session after cancellation");

    /* A new client can disconnect without moving the callback data for
       other clients. This caught the old compacted-array lifetime bug. */
    char *second_id = NULL;
    SmcConn second = SmcOpenConnection(NULL, (void *)1, 1, 0,
        SmcSaveYourselfProcMask | SmcDieProcMask | SmcSaveCompleteProcMask |
        SmcShutdownCancelledProcMask, &callbacks, NULL, &second_id,
        sizeof(error), error);
    if(second == NULL)
        fail("second client registration");
    for(int i = 0; i < 10; i++)
        pump_ice(SmcGetIceConnection(second), 20);
    SmcCloseConnection(connection, 0, NULL);
    connection = second;
    ice = SmcGetIceConnection(second);
    free(client_id);
    client_id = second_id;

    const char *arguments[] = {"/bin/true", "two words", "semi;colon"};
    SmPropValue restart_args[3];
    for(int i = 0; i < 3; i++)
        restart_args[i] = (SmPropValue){(int)strlen(arguments[i]), (char *)arguments[i]};
    SmProp restart_property = {SmRestartCommand, SmLISTofARRAY8, 3, restart_args};
    SmProp *properties[] = {&restart_property};
    SmcSetProperties(connection, 1, properties);
    IceFlush(ice);
    /* A rejected power request must leave the session alive after saving. */
    saw_cancel = 0;
    file = fopen(control, "w");
    if(file == NULL) fail("open power control");
    fputs("shutdown\n", file);
    fclose(file);
    for(int i = 0; i < 100 && !saw_cancel; i++)
        pump_ice(ice, 50);
    if(!saw_cancel || saw_die || kill(daemon, 0) != 0)
        fail("rejected power request did not preserve the session");
    if(!file_contains(saved, "Command=/bin/true;two words;semi\\;colon;", 2))
        fail("saved session did not preserve restart arguments");

    /* Logout must complete phase two, deliver Die, and end the session. */
    request_phase2 = 1;
    file = fopen(control, "w");
    if(file == NULL)
        fail("open control fifo");
    fputs("logout\n", file);
    fclose(file);
    for(int i = 0; i < 100 && !saw_die; i++)
        pump_ice(ice, 50);
    if(!saw_save_done)
        fail("client never completed a SaveYourself");
    if(!saw_die)
        fail("client never received Die at logout");
    if(request_phase2)
        fail("client never completed phase two");
    {
        pid_t reaped = 0;
        for(int i = 0; i < 100; i++) {
            reaped = waitpid(daemon, &status, WNOHANG);
            if(reaped == daemon)
                break;
            usleep(100000);
        }
        if(reaped != daemon) {
            fprintf(stderr, "waitpid=%d errno=%d (daemon=%d) alive=%d\n",
                    (int)reaped, errno, (int)daemon,
                    kill(daemon, 0) == 0);
            fail("rill-sessiond did not exit after logout");
        }
    }
    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("rill-sessiond exited nonzero");
    if(access(control, F_OK) == 0)
        fail("control fifo not removed at exit");

    unlink(ran);
    unlink(env_file);
    unlink(out);
    snprintf(entry, sizeof(entry), "%s/stderr", root);
    unlink(entry);
    snprintf(entry, sizeof(entry), "%s/runtime", root);
    rmdir(entry);
    snprintf(entry, sizeof(entry), "%s/home", root);
    rmdir(entry);
    rmdir(autostart);
    puts("Rill session manager parity tests passed");
    return 0;
}
