/* Rill's session manager: an XSMP server, XDG autostart runner and logout
 * coordinator, replacing xfce4-session in Rill-owned sessions.
 *
 * rill-sessiond [--autostart-dir DIR | --autostart-dir=DIR]...
 *               [--no-default-autostart] -- COMMAND [ARGS...]
 *
 * It owns the SESSION_MANAGER endpoint (local transports only, secured with a
 * MIT-MAGIC-COOKIE-1 published to the ICE authority file), runs the XDG
 * autostart entries, then supervises COMMAND (the session body). When the
 * body exits, or a "logout" line arrives on the control fifo, clients get a
 * shutdown SaveYourself and the session ends. The control fifo path prints
 * on stdout as "control: PATH". */
#include <X11/SM/SMlib.h>
#include <X11/ICE/ICElib.h>
#include <X11/ICE/ICEutil.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SESSIOND_MAX_CLIENTS 64
#define SESSIOND_MAX_LISTENERS 8
#define SESSIOND_MAX_AUTOSTART_DIRS 8
#define SESSIOND_COOKIE_LEN 16

typedef struct {
    SmsConn connection;
    IceConn ice;
    char *id;
    int save_done;
} SessionClient;

typedef struct {
    char *dirs[SESSIOND_MAX_AUTOSTART_DIRS];
    int dir_count;
    SessionClient clients[SESSIOND_MAX_CLIENTS];
    int client_count;
    IceConn pending[SESSIOND_MAX_CLIENTS];
    int pending_count;
    IceListenObj listeners[SESSIOND_MAX_LISTENERS];
    int listener_count;
    char control_path[512];
    pid_t body;
    volatile sig_atomic_t stopping;
    int logout_requested;
    long logout_deadline;
} SessionState;

static SessionState state;
static char *sessiond_cookie;
static IceAuthDataEntry sessiond_auth_entries[SESSIOND_MAX_LISTENERS * 2];
static int sessiond_auth_entry_count;

static long
sessiond_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void
sessiond_stop_signal(int sig)
{
    (void)sig;
    state.stopping = 1;
}

static Bool
sessiond_host_acceptable(char *hostname)
{
    (void)hostname;
    return True;
}

/* A client that drops its connection must never take the session manager
   with it; the libICE default IO handler exits the process. */
static void
sessiond_io_error(IceConn connection)
{
    if(getenv("RILL_SESSIOND_DEBUG"))
        fprintf(stderr, "rill-sessiond: ICE connection lost (%p)\n",
                (void *)connection);
}

/* Publish one cookie for every listener in a single locked pass, and keep
   the entries registered so the server can verify the client handshake. */
static void
sessiond_publish_auth(const char **network_ids, int count)
{
    IceAuthFileEntry file_entry;
    char *authority = IceAuthFileName();
    int lock;
    FILE *file;

    sessiond_cookie = IceGenerateMagicCookie(SESSIOND_COOKIE_LEN);
    if(sessiond_cookie == NULL || authority == NULL) {
        free(sessiond_cookie);
        sessiond_cookie = NULL;
        free(authority);
        return;
    }
    for(int i = 0; i < count && i < SESSIOND_MAX_LISTENERS; i++) {
        IceAuthDataEntry *ice_entry =
            &sessiond_auth_entries[sessiond_auth_entry_count++];
        IceAuthDataEntry *sms_entry =
            &sessiond_auth_entries[sessiond_auth_entry_count++];
        memset(ice_entry, 0, sizeof(*ice_entry));
        ice_entry->protocol_name = (char *)"ICE";
        ice_entry->network_id = (char *)network_ids[i];
        ice_entry->auth_name = (char *)"MIT-MAGIC-COOKIE-1";
        ice_entry->auth_data_length = SESSIOND_COOKIE_LEN;
        ice_entry->auth_data = sessiond_cookie;
        memset(sms_entry, 0, sizeof(*sms_entry));
        *sms_entry = *ice_entry;
        sms_entry->protocol_name = (char *)"XSMP";
    }
    IceSetPaAuthData(sessiond_auth_entry_count, sessiond_auth_entries);
    memset(&file_entry, 0, sizeof(file_entry));
    file_entry.protocol_data = (char *)"";
    file_entry.auth_name = (char *)"MIT-MAGIC-COOKIE-1";
    file_entry.auth_data_length = SESSIOND_COOKIE_LEN;
    file_entry.auth_data = sessiond_cookie;
    lock = IceLockAuthFile(authority, 5, 10, 0);
    file = fopen(authority, "a");
    if(file != NULL) {
        for(int i = 0; i < sessiond_auth_entry_count; i++) {
            file_entry.protocol_name =
                sessiond_auth_entries[i].protocol_name;
            file_entry.network_id = sessiond_auth_entries[i].network_id;
            IceWriteAuthFileEntry(file, &file_entry);
        }
        fclose(file);
    }
    if(lock == IceAuthLockSuccess)
        IceUnlockAuthFile(authority);
    free(authority);
}

/* ---------- XSMP client handling ---------- */

static void
sessiond_remove_client(SessionClient *client)
{
    int index = (int)(client - state.clients);
    SmsCleanUp(client->connection);
    free(client->id);
    memmove(&state.clients[index], &state.clients[index + 1],
            (size_t)(state.client_count - index - 1) *
                sizeof(state.clients[0]));
    state.client_count--;
}

static Status
sessiond_register_client(SmsConn connection, SmPointer data, char *previous_id)
{
    SessionClient *client = data;
    char *new_id = SmsGenerateClientID(connection);

    (void)previous_id;
    if(new_id == NULL) {
        new_id = malloc(64);
        if(new_id == NULL)
            return 0;
        snprintf(new_id, 64, "10rill.%ld", (long)getpid());
    }
    free(client->id);
    client->id = new_id;
    if(!SmsRegisterClientReply(connection, new_id))
        return 0;
    SmsSaveYourself(connection, SmSaveLocal, False, False, False);
    IceFlush(SmsGetIceConnection(connection));
    return 1;
}

static void
sessiond_save_yourself_request(SmsConn connection, SmPointer data,
                               int save_type, Bool shutdown, int interact_style,
                               Bool fast, Bool global)
{
    (void)connection;
    (void)data;
    (void)save_type;
    (void)interact_style;
    (void)fast;
    (void)global;
    if(shutdown)
        state.logout_requested = 1;
    for(int i = 0; i < state.client_count; i++) {
        SmsSaveYourself(state.clients[i].connection, SmSaveBoth, shutdown,
                        SmInteractStyleAny, False);
        IceFlush(state.clients[i].ice);
    }
}

static void
sessiond_save_yourself_done(SmsConn connection, SmPointer data, Bool success)
{
    SessionClient *client = data;
    (void)success;
    client->save_done = 1;
    if(state.logout_requested) {
        IceConn ice = SmsGetIceConnection(connection);
        SmsDie(connection);
        /* SmsDie only queues the message; flush before the teardown path
           can close the connection out from under it. */
        IceFlush(ice);
    }
}

static void
sessiond_close_connection(SmsConn connection, SmPointer data, int count,
                          char **reasons)
{
    (void)connection;
    for(int i = 0; i < count; i++)
        free(reasons[i]);
    sessiond_remove_client(data);
}

static Status
sessiond_new_client(SmsConn connection, SmPointer manager_data,
                    unsigned long *mask_ret, SmsCallbacks *callbacks_ret,
                    char **failure_reason_ret)
{
    SessionClient *client;
    (void)manager_data;

    if(state.client_count >= SESSIOND_MAX_CLIENTS) {
        *failure_reason_ret = strdup("too many session clients");
        return 0;
    }
    client = &state.clients[state.client_count];
    memset(client, 0, sizeof(*client));
    client->connection = connection;
    client->ice = SmsGetIceConnection(connection);
    state.client_count++;

    memset(callbacks_ret, 0, sizeof(*callbacks_ret));
    callbacks_ret->register_client.callback = sessiond_register_client;
    callbacks_ret->register_client.manager_data = client;
    callbacks_ret->save_yourself_request.callback =
        sessiond_save_yourself_request;
    callbacks_ret->save_yourself_request.manager_data = client;
    callbacks_ret->save_yourself_done.callback = sessiond_save_yourself_done;
    callbacks_ret->save_yourself_done.manager_data = client;
    callbacks_ret->close_connection.callback = sessiond_close_connection;
    callbacks_ret->close_connection.manager_data = client;
    *mask_ret = SmsRegisterClientProcMask |
                SmsSaveYourselfRequestProcMask |
                SmsSaveYourselfDoneProcMask | SmsCloseConnectionProcMask;
    return 1;
}

/* ---------- XDG autostart ---------- */

static char *
entry_value(const char *text, const char *key)
{
    char pattern[128];
    size_t length;
    const char *cursor;

    snprintf(pattern, sizeof(pattern), "%s=", key);
    length = strlen(pattern);
    cursor = text;
    while((cursor = strstr(cursor, pattern)) != NULL) {
        if(cursor == text || cursor[-1] == '\n') {
            const char *end = strchr(cursor + length, '\n');
            size_t size = end != NULL ? (size_t)(end - cursor - length) :
                                        strlen(cursor + length);
            char *value = malloc(size + 1);
            if(value == NULL)
                return NULL;
            memcpy(value, cursor + length, size);
            value[size] = '\0';
            return value;
        }
        cursor += length;
    }
    return NULL;
}

static int
entry_runs_here(const char *text)
{
    char *hidden = entry_value(text, "Hidden");
    char *only = entry_value(text, "OnlyShowIn");
    char *not_show = entry_value(text, "NotShowIn");
    char *try_exec = entry_value(text, "TryExec");
    int runs = 1;

    if(hidden != NULL && strcasecmp(hidden, "true") == 0)
        runs = 0;
    if(only != NULL && only[0] != '\0') {
        runs = 0;
        /* Rill replaces Xfce, so entries offered to Xfce run here too. */
        if(strstr(only, "XFCE") != NULL || strstr(only, "Rill") != NULL)
            runs = 1;
    }
    if(not_show != NULL &&
       (strstr(not_show, "XFCE") != NULL || strstr(not_show, "Rill") != NULL))
        runs = 0;
    if(runs && try_exec != NULL && try_exec[0] != '\0') {
        char path[4096];
        const char *path_env = getenv("PATH");
        char *directories = strdup(path_env != NULL ? path_env :
                                    "/usr/bin:/bin");
        char *cursor = directories;
        runs = 0;
        while(cursor != NULL && *cursor != '\0') {
            char *end = strchr(cursor, ':');
            size_t size = end != NULL ? (size_t)(end - cursor) :
                                        strlen(cursor);
            snprintf(path, sizeof(path), "%.*s/%s", (int)size, cursor,
                     try_exec);
            if(access(path, X_OK) == 0) {
                runs = 1;
                break;
            }
            if(end == NULL)
                break;
            cursor = end + 1;
        }
        free(directories);
    }
    free(hidden);
    free(only);
    free(not_show);
    free(try_exec);
    return runs;
}

static void
sessiond_spawn_command(const char *command)
{
    pid_t pid = fork();
    if(pid != 0) {
        if(pid > 0)
            fprintf(stderr, "rill-sessiond: autostart %ld: %s\n", (long)pid,
                    command);
        return;
    }
    setsid();
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
}

static void
sessiond_run_entry(const char *path)
{
    FILE *file = fopen(path, "r");
    char *text, *exec_value, *type_value, *stripped, *out;
    long size;

    if(file == NULL)
        return;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if(size <= 0) {
        fclose(file);
        return;
    }
    text = malloc((size_t)size + 1);
    if(text == NULL) {
        fclose(file);
        return;
    }
    if(fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return;
    }
    text[size] = '\0';
    type_value = entry_value(text, "Type");
    exec_value = entry_value(text, "Exec");
    if(type_value != NULL && exec_value != NULL &&
       strcmp(type_value, "Application") == 0 && entry_runs_here(text)) {
        stripped = malloc(strlen(exec_value) + 1);
        if(stripped != NULL) {
            const char *in = exec_value;
            /* Strip desktop-entry field codes (%f, %F, %u, ...). */
            for(out = stripped; *in != '\0';) {
                if(*in == '%' && in[1] != '\0') {
                    in += 2;
                    continue;
                }
                *out++ = *in++;
            }
            *out = '\0';
            sessiond_spawn_command(stripped);
            free(stripped);
        }
    }
    free(type_value);
    free(exec_value);
    free(text);
    fclose(file);
}

static void
sessiond_run_autostart(void)
{
    for(int d = 0; d < state.dir_count; d++) {
        DIR *directory = opendir(state.dirs[d]);
        struct dirent *entry;
        if(directory == NULL)
            continue;
        while((entry = readdir(directory)) != NULL) {
            char path[1024];
            size_t name_length = strlen(entry->d_name);

            if(entry->d_name[0] == '.' || name_length < 8 ||
               strcmp(entry->d_name + name_length - 8, ".desktop") != 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", state.dirs[d],
                     entry->d_name);
            sessiond_run_entry(path);
        }
        closedir(directory);
    }
}

/* ---------- Session lifecycle ---------- */

static void
sessiond_begin_logout(void)
{
    state.logout_requested = 1;
    state.logout_deadline = sessiond_now_ms() + 5000;
    if(state.body > 0)
        kill(state.body, SIGTERM);
    /* The registration-time save already flagged clients; require a fresh
       acknowledgment of the shutdown save before closing anything. */
    for(int i = 0; i < state.client_count; i++) {
        state.clients[i].save_done = 0;
        SmsSaveYourself(state.clients[i].connection, SmSaveBoth, True,
                        SmInteractStyleAny, False);
        IceFlush(state.clients[i].ice);
    }
    fflush(stdout);
}

static int
sessiond_all_saved(void)
{
    if(state.client_count == 0)
        return 1;
    for(int i = 0; i < state.client_count; i++)
        if(!state.clients[i].save_done)
            return 0;
    return 1;
}

static void
sessiond_finish_clients(void)
{
    while(state.client_count > 0)
        sessiond_remove_client(&state.clients[0]);
}

int
main(int argc, char **argv)
{
    char error[256];
    int body_index = argc;
    int control_fd = -1;
    int autostart_user = 1;

    signal(SIGINT, sessiond_stop_signal);
    signal(SIGTERM, sessiond_stop_signal);
    IceSetIOErrorHandler(sessiond_io_error);
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--autostart-dir") == 0 && i + 1 < argc) {
            state.dirs[state.dir_count++] = argv[++i];
            autostart_user = 0;
        } else if(strncmp(argv[i], "--autostart-dir=", 16) == 0) {
            state.dirs[state.dir_count++] = argv[i] + 16;
            autostart_user = 0;
        } else if(strcmp(argv[i], "--no-default-autostart") == 0) {
            /* Tests use this so a regression can never touch real
               autostart directories on a live desktop. */
            autostart_user = 0;
        } else if(strcmp(argv[i], "--") == 0) {
            body_index = ++i;
            break;
        }
    }

    if(!SmsInitialize("rill", "Rill Session", sessiond_new_client, NULL, NULL,
                      (int)sizeof(error), error)) {
        fprintf(stderr, "rill-sessiond: cannot start the session server: %s\n",
                error[0] != '\0' ? error : "unknown error");
        return 1;
    }
    {
        IceListenObj *listeners = NULL;
        const char *kept_ids[SESSIOND_MAX_LISTENERS];
        char composed[1024];
        int count = 0;
        int length = 0;
        if(!IceListenForConnections(&count, &listeners, (int)sizeof(error),
                                    error)) {
            fprintf(stderr, "rill-sessiond: cannot listen: %s\n",
                    error[0] != '\0' ? error : "unknown error");
            return 1;
        }
        composed[0] = '\0';
        /* Publish only local transports: TCP endpoints advertise a hostname
           clients may not resolve, breaking XSMP connects. */
        for(int i = 0; i < count; i++) {
            char *address = IceGetListenConnectionString(listeners[i]);
            int keep = address != NULL &&
                       (strncmp(address, "local/", 6) == 0 ||
                        strncmp(address, "unix/", 5) == 0 ||
                        strncmp(address, "local:", 6) == 0);
            if(keep && state.listener_count < SESSIOND_MAX_LISTENERS) {
                IceSetHostBasedAuthProc(listeners[i], sessiond_host_acceptable);
                kept_ids[state.listener_count] = address;
                state.listeners[state.listener_count++] = listeners[i];
                if(length > 0)
                    composed[length++] = ',';
                length += snprintf(composed + length,
                                   sizeof(composed) - (size_t)length, "%s",
                                   address);
            } else {
                close(IceGetListenConnectionNumber(listeners[i]));
                if(address != NULL)
                    free(address);
            }
        }
        sessiond_publish_auth(kept_ids, state.listener_count);
        for(int i = 0; i < state.listener_count; i++)
            free((char *)kept_ids[i]);
        free(listeners);
        if(state.listener_count == 0) {
            fprintf(stderr, "rill-sessiond: no local session endpoints\n");
            return 1;
        }
        setenv("SESSION_MANAGER", composed, 1);
    }

    /* XDG autostart: the user directory first, then system directories. */
    if(autostart_user) {
        const char *config = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char user_dir[512];
        if(config != NULL && config[0] != '\0')
            snprintf(user_dir, sizeof(user_dir), "%s/autostart", config);
        else if(home != NULL && home[0] != '\0')
            snprintf(user_dir, sizeof(user_dir), "%s/.config/autostart", home);
        else
            user_dir[0] = '\0';
        if(user_dir[0] != '\0') {
            state.dirs[state.dir_count++] = strdup(user_dir);
            state.dirs[state.dir_count++] = "/etc/xdg/autostart";
        }
    }
    sessiond_run_autostart();

    {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        const char *base = runtime != NULL && runtime[0] != '\0' ?
                           runtime : "/tmp";
        snprintf(state.control_path, sizeof(state.control_path),
                 "%s/rill-session-%ld.control", base, (long)getuid());
        unlink(state.control_path);
        if(mkfifo(state.control_path, 0600) == 0)
            control_fd = open(state.control_path, O_RDONLY | O_NONBLOCK);
    }
    printf("control: %s\n", state.control_path);
    fflush(stdout);

    if(body_index < argc) {
        state.body = fork();
        if(state.body == 0) {
            execvp(argv[body_index], &argv[body_index]);
            _exit(127);
        }
    }

    while(!state.stopping) {
        struct pollfd fds[SESSIOND_MAX_CLIENTS * 2 + SESSIOND_MAX_LISTENERS +
                          1];
        /* What each pollfd refers to: 0..listeners-1 listeners, then
           clients, then pending, then the control fifo. */
        enum { FD_LISTENER, FD_CLIENT, FD_PENDING, FD_CONTROL } kinds[
            SESSIOND_MAX_CLIENTS * 2 + SESSIOND_MAX_LISTENERS + 1];
        int index_of[SESSIOND_MAX_CLIENTS * 2 + SESSIOND_MAX_LISTENERS + 1];
        int count = 0;
        char line[64];

        /* Reap autostart children; notice when the session body exits. */
        for(;;) {
            pid_t done = waitpid(-1, NULL, WNOHANG);
            if(done <= 0)
                break;
            if(done == state.body)
                state.stopping = 1;
        }
        if(state.stopping && !state.logout_requested) {
            state.logout_requested = 1;
            state.logout_deadline = sessiond_now_ms() + 5000;
        }

        for(int i = 0; i < state.listener_count; i++) {
            kinds[count] = FD_LISTENER;
            index_of[count] = i;
            fds[count++] = (struct pollfd){IceGetListenConnectionNumber(
                                               state.listeners[i]),
                                           POLLIN, 0};
        }
        for(int i = 0; i < state.client_count; i++) {
            kinds[count] = FD_CLIENT;
            index_of[count] = i;
            fds[count++] = (struct pollfd){IceConnectionNumber(
                                               state.clients[i].ice),
                                           POLLIN, 0};
        }
        for(int i = 0; i < state.pending_count; i++) {
            kinds[count] = FD_PENDING;
            index_of[count] = i;
            fds[count++] = (struct pollfd){IceConnectionNumber(
                                               state.pending[i]),
                                           POLLIN, 0};
        }
        if(control_fd >= 0) {
            kinds[count] = FD_CONTROL;
            index_of[count] = 0;
            fds[count++] = (struct pollfd){control_fd, POLLIN, 0};
        }

        int timeout = state.logout_requested ?
                      (int)(state.logout_deadline - sessiond_now_ms()) : 500;
        if(timeout < 0)
            timeout = 0;
        poll(fds, (nfds_t)count, timeout);

        /* IceProcessMessages blocks internally when it needs more bytes, so
           only ever run it for fds poll reported readable. */
        for(int f = 0; f < count; f++) {
            if(!(fds[f].revents & (POLLIN | POLLHUP)))
                continue;
            if(kinds[f] == FD_LISTENER) {
                IceAcceptStatus accept_status;
                IceConn accepted =
                    IceAcceptConnection(state.listeners[index_of[f]],
                                        &accept_status);
                if(accepted != NULL &&
                   IceConnectionStatus(accepted) != IceConnectRejected &&
                   state.pending_count < SESSIOND_MAX_CLIENTS)
                    state.pending[state.pending_count++] = accepted;
            } else if(kinds[f] == FD_CLIENT) {
                IceProcessMessages(state.clients[index_of[f]].ice, NULL,
                                   NULL);
            } else if(kinds[f] == FD_PENDING) {
                IceProcessMessages(state.pending[index_of[f]], NULL, NULL);
            } else if(kinds[f] == FD_CONTROL) {
                ssize_t n;
                while((n = read(control_fd, line, sizeof(line) - 1)) > 0) {
                    line[n] = '\0';
                    if(strncmp(line, "logout", 6) == 0) {
                        if(!state.logout_requested)
                            sessiond_begin_logout();
                        else
                            state.stopping = 1;
                    }
                }
            }
        }

        /* Promote setup-phase connections that finished RegisterClient, and
           drop the failed ones. */
        for(int i = state.pending_count - 1; i >= 0; i--) {
            IceConnectStatus status;
            int promoted = 0;
            for(int c = 0; c < state.client_count; c++)
                if(state.clients[c].ice == state.pending[i])
                    promoted = 1;
            status = IceConnectionStatus(state.pending[i]);
            if(promoted || status == IceConnectRejected ||
               status == IceConnectIOError) {
                if(!promoted)
                    IceCloseConnection(state.pending[i]);
                memmove(&state.pending[i], &state.pending[i + 1],
                        (size_t)(state.pending_count - i - 1) *
                            sizeof(state.pending[0]));
                state.pending_count--;
            }
        }
        for(int i = state.client_count - 1; i >= 0; i--) {
            IceConnectStatus status;
            status = IceConnectionStatus(state.clients[i].ice);
            if(status == IceConnectRejected || status == IceConnectIOError)
                sessiond_remove_client(&state.clients[i]);
        }

        if(state.logout_requested &&
           (sessiond_all_saved() || sessiond_now_ms() >= state.logout_deadline)) {
            sessiond_finish_clients();
            if(state.body > 0) {
                int status;
                kill(state.body, SIGTERM);
                for(int w = 0; w < 20; w++) {
                    if(waitpid(state.body, &status, WNOHANG) == state.body ||
                       errno == ECHILD)
                        break;
                    usleep(100000);
                }
            }
            break;
        }
    }
    printf("control: %s\n", state.control_path);
    fflush(stdout);

    if(body_index < argc) {
        state.body = fork();
        if(state.body == 0) {
            execvp(argv[body_index], &argv[body_index]);
            _exit(127);
        }
    }

    unlink(state.control_path);
    return 0;
}
