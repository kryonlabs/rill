/* Rill's X11 session manager. Owns XSMP, XDG autostart, supervised services,
 * saved-session commands and a private control FIFO. User logout is a save
 * transaction: no client is terminated until every client has accepted it. */
#include <X11/SM/SMlib.h>
#include <X11/ICE/ICElib.h>
#include <X11/ICE/ICEutil.h>
#include <gio/gdesktopappinfo.h>
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
#include <unistd.h>

#define MAX_CLIENTS 128
#define MAX_LISTENERS 16
#define MAX_RESTARTS 5

typedef struct {
    SmsConn connection;
    IceConn ice;
    char *id;
    GHashTable *properties;
    int closing;
    int saving;
    int saved;
    int phase2;
    int wants_interaction;
} SessionClient;

typedef struct {
    char *name;
    char **argv;
    char *directory;
    pid_t pid;
    int restart;
    int required;
    int body;
    int failures;
    gint64 started;
    gint64 retry_at;
} SessionProcess;

static SessionClient clients[MAX_CLIENTS];
static IceConn connections[MAX_CLIENTS];
static IceListenObj listeners[MAX_LISTENERS];
static int listener_count;
typedef struct {
    pid_t group;
    gint64 deadline;
} RetiredGroup;

static GPtrArray *processes;
static GArray *retired_groups;
static GPtrArray *autostart_dirs;
static GHashTable *restored_programs;
static SessionClient *interacting;
static volatile sig_atomic_t stop_requested;
static int logout_pending;
static int checkpoint_pending;
static int finishing;
static const char *power_action;
static int exit_status;
static gint64 save_deadline;
static char *state_file;
static char *runtime_directory;
static char *control_path;
static char *authority_path;
static IceAuthDataEntry auth_entries[MAX_LISTENERS * 2];
static int auth_count;
static char *auth_cookie;

static void begin_save(int shutdown);
static void cancel_logout(void);

static gint64
now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static void
stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static Bool
reject_host_auth(char *hostname)
{
    (void)hostname;
    return False; /* A local transport still requires the session cookie. */
}

static void
ice_error(IceConn connection)
{
    (void)connection; /* The poll loop disposes of a disconnected client. */
}

static void
child_setup(gpointer data)
{
    (void)data;
    setsid();
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

static SessionProcess *
add_process(const char *name, char **argv, const char *directory,
            int restart, int required, int body)
{
    SessionProcess *process = g_new0(SessionProcess, 1);
    process->name = g_strdup(name);
    process->argv = argv != NULL ? g_strdupv(argv) : NULL;
    process->directory = g_strdup(directory);
    process->restart = restart;
    process->required = required;
    process->body = body;
    g_ptr_array_add(processes, process);
    return process;
}

static void
start_process(SessionProcess *process)
{
    GError *error = NULL;
    process->retry_at = 0;
    process->started = now_ms();
    if(!g_spawn_async(process->directory, process->argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       child_setup, NULL, &process->pid, &error)) {
        fprintf(stderr, "rill-sessiond: cannot start %s: %s\n",
                process->name, error->message);
        g_clear_error(&error);
        process->pid = 0;
        if(process->required) {
            exit_status = 1;
            stop_requested = 1;
        }
        return;
    }
    fprintf(stderr, "rill-sessiond: started %s (%ld)\n",
            process->name, (long)process->pid);
}

static void
retire_group(pid_t group)
{
    kill(-group, SIGTERM);
    if(kill(-group, 0) == 0) {
        RetiredGroup retired = {group, now_ms() + 2000};
        g_array_append_val(retired_groups, retired);
    }
}

static void
reap_groups(int force)
{
    for(guint i = retired_groups->len; i > 0; i--) {
        RetiredGroup group = g_array_index(retired_groups, RetiredGroup, i - 1);
        if(force || now_ms() >= group.deadline) {
            kill(-group.group, SIGKILL);
            g_array_remove_index_fast(retired_groups, i - 1);
        } else if(kill(-group.group, 0) != 0 && errno == ESRCH)
            g_array_remove_index_fast(retired_groups, i - 1);
    }
}

static void
reap_processes(void)
{
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for(guint i = 0; i < processes->len; i++) {
            SessionProcess *process = g_ptr_array_index(processes, i);
            if(process->pid != pid)
                continue;
            /* Children of a crashed service must not survive its restart. */
            retire_group(pid);
            process->pid = 0;
            if(finishing || logout_pending || stop_requested)
                break;
            if(process->restart) {
                if(now_ms() - process->started > 60000)
                    process->failures = 0;
                process->failures++;
                if(process->failures <= MAX_RESTARTS) {
                    process->retry_at = now_ms() + 200 * (1 << process->failures);
                    fprintf(stderr, "rill-sessiond: restarting %s after exit\n",
                            process->name);
                } else {
                    fprintf(stderr, "rill-sessiond: %s repeatedly failed\n",
                            process->name);
                    if(process->required) {
                        exit_status = 1;
                        stop_requested = 1;
                    }
                }
            } else if(process->body) {
                exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                stop_requested = 1;
            }
            break;
        }
    }
    reap_groups(0);
    if(!logout_pending && !stop_requested && !finishing) {
        for(guint i = 0; i < processes->len; i++) {
            SessionProcess *process = g_ptr_array_index(processes, i);
            if(process->retry_at > 0 && process->retry_at <= now_ms())
                start_process(process);
        }
    }
}

static char **
property_arguments(SessionClient *client, const char *name)
{
    SmProp *property = g_hash_table_lookup(client->properties, name);
    if(property == NULL || strcmp(property->type, SmLISTofARRAY8) != 0 ||
       property->num_vals <= 0 || property->num_vals > 1024)
        return NULL;
    char **arguments = g_new0(char *, (gsize)property->num_vals + 1);
    for(int i = 0; i < property->num_vals; i++) {
        if(property->vals[i].length < 0 || property->vals[i].length > 65536) {
            g_strfreev(arguments);
            return NULL;
        }
        arguments[i] = g_strndup(property->vals[i].value,
                                 (gsize)property->vals[i].length);
    }
    return arguments;
}

static int
restart_style(SessionClient *client)
{
    SmProp *property = g_hash_table_lookup(client->properties, SmRestartStyleHint);
    if(property != NULL && strcmp(property->type, SmCARD8) == 0 &&
       property->num_vals == 1 && property->vals[0].length == 1)
        return *(unsigned char *)property->vals[0].value;
    return SmRestartIfRunning;
}

static char *
command_key(char **arguments)
{
    if(arguments == NULL || arguments[0] == NULL)
        return NULL;
    char *resolved = g_find_program_in_path(arguments[0]);
    char **copy = g_strdupv(arguments);
    if(resolved != NULL) {
        g_free(copy[0]);
        copy[0] = resolved;
    }
    GVariant *value = g_variant_ref_sink(g_variant_new_strv((const char *const *)copy, -1));
    char *key = g_variant_print(value, TRUE);
    g_variant_unref(value);
    g_strfreev(copy);
    return key;
}

static int
managed_program(const char *command)
{
    char **arguments = NULL;
    if(command == NULL || !g_shell_parse_argv(command, NULL, &arguments, NULL))
        return 0;
    char *resolved = g_find_program_in_path(arguments[0]);
    int matched = 0;
    for(guint i = 0; i < processes->len && !matched; i++) {
        SessionProcess *process = g_ptr_array_index(processes, i);
        if(process->argv == NULL || !(process->restart || process->required || process->body))
            continue;
        char *other = g_find_program_in_path(process->argv[0]);
        int same = strcmp(arguments[0], process->argv[0]) == 0 ||
                   (resolved != NULL && other != NULL && strcmp(resolved, other) == 0);
        g_free(other);
        if(!same)
            continue;
        int index = 1;
        matched = 1;
        for(int j = 1; process->argv[j] != NULL; j++) {
            /* Foreground switches change supervision, not service identity. */
            if(strcmp(process->argv[j], "--no-daemon") == 0 ||
               strcmp(process->argv[j], "-no-splash") == 0)
                continue;
            if(arguments[index] == NULL || strcmp(process->argv[j], arguments[index]) != 0) {
                matched = 0;
                break;
            }
            index++;
        }
    }
    char *key = command_key(arguments);
    if(key != NULL && g_hash_table_contains(restored_programs, key))
        matched = 1;
    g_free(key);
    g_free(resolved);
    g_strfreev(arguments);
    return matched;
}

static int
managed_arguments(char **arguments)
{
    for(guint i = 0; i < processes->len; i++) {
        SessionProcess *process = g_ptr_array_index(processes, i);
        if(process->argv == NULL ||
           !(process->restart || process->required || process->body))
            continue;
        int index = 0;
        while(process->argv[index] != NULL && arguments[index] != NULL &&
              strcmp(process->argv[index], arguments[index]) == 0)
            index++;
        if(process->argv[index] == NULL)
            return 1;
    }
    return 0;
}

static void
save_session(void)
{
    if(state_file == NULL)
        return;
    GKeyFile *file = g_key_file_new();
    int count = 0;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        SessionClient *client = &clients[i];
        if(client->connection == NULL || client->closing ||
           restart_style(client) == SmRestartNever)
            continue;
        char **arguments = property_arguments(client, SmRestartCommand);
        if(arguments == NULL)
            continue;
        if(!managed_arguments(arguments)) {
            char *group = g_strdup_printf("Client %d", count++);
            g_key_file_set_string_list(file, group, "Command",
                                      (const char *const *)arguments,
                                      g_strv_length(arguments));
            SmProp *cwd = g_hash_table_lookup(client->properties, SmCurrentDirectory);
            if(cwd != NULL && cwd->num_vals == 1 &&
               strcmp(cwd->type, SmARRAY8) == 0 && cwd->vals[0].length > 0) {
                char *directory = g_strndup(cwd->vals[0].value,
                                            (gsize)cwd->vals[0].length);
                g_key_file_set_string(file, group, "Directory", directory);
                g_free(directory);
            }
            g_free(group);
        }
        g_strfreev(arguments);
    }
    g_key_file_set_integer(file, "Session", "Version", 1);
    gsize length;
    char *contents = g_key_file_to_data(file, &length, NULL);
    char *directory = g_path_get_dirname(state_file);
    GError *error = NULL;
    if(g_mkdir_with_parents(directory, 0700) != 0 ||
       !g_file_set_contents(state_file, contents, (gssize)length, &error))
        fprintf(stderr, "rill-sessiond: cannot save session: %s\n",
                error != NULL ? error->message : g_strerror(errno));
    g_clear_error(&error);
    g_free(directory);
    g_free(contents);
    g_key_file_unref(file);
}

static void
remove_client(SessionClient *client)
{
    if(interacting == client)
        interacting = NULL;
    SmsCleanUp(client->connection);
    g_hash_table_unref(client->properties);
    free(client->id);
    memset(client, 0, sizeof(*client));
}

static Status
register_client(SmsConn connection, SmPointer data, char *previous_id)
{
    SessionClient *client = data;
    if(previous_id != NULL) {
        for(int i = 0; i < MAX_CLIENTS; i++) {
            if(clients[i].connection != NULL &&
               g_strcmp0(clients[i].id, previous_id) == 0) {
                free(previous_id);
                return 0;
            }
        }
    }
    client->id = previous_id != NULL ? previous_id : SmsGenerateClientID(connection);
    if(client->id == NULL || !SmsRegisterClientReply(connection, client->id))
        return 0;
    if(previous_id == NULL) {
        client->saving = 1;
        SmsSaveYourself(connection, SmSaveLocal, False, SmInteractStyleNone, False);
    }
    IceFlush(client->ice);
    return 1;
}

static void
save_request(SmsConn connection, SmPointer data, int type, Bool shutdown,
             int style, Bool fast, Bool global)
{
    (void)connection;
    (void)data;
    (void)type;
    (void)style;
    (void)fast;
    (void)global;
    begin_save(shutdown);
}

static void
save_done(SmsConn connection, SmPointer data, Bool success)
{
    SessionClient *client = data;
    if(!client->saving)
        return;
    client->saving = 0;
    client->saved = 1;
    if(!success && logout_pending)
        cancel_logout();
    else if(!logout_pending && !checkpoint_pending) {
        SmsSaveComplete(connection);
        IceFlush(client->ice);
    }
}

static void
interact_request(SmsConn connection, SmPointer data, int dialog_type)
{
    (void)connection;
    (void)dialog_type;
    SessionClient *client = data;
    if(logout_pending || checkpoint_pending)
        client->wants_interaction = 1;
}

static void
interact_done(SmsConn connection, SmPointer data, Bool cancel)
{
    (void)connection;
    if(interacting != data)
        return;
    interacting = NULL;
    if(cancel && logout_pending)
        cancel_logout();
}

static void
phase2_request(SmsConn connection, SmPointer data)
{
    (void)connection;
    SessionClient *client = data;
    client->phase2 = 1;
}

static void
close_connection(SmsConn connection, SmPointer data, int count, char **reasons)
{
    (void)connection;
    SessionClient *client = data;
    SmFreeReasons(count, reasons);
    client->closing = 1;
}

static void
set_properties(SmsConn connection, SmPointer data, int count, SmProp **properties)
{
    (void)connection;
    SessionClient *client = data;
    for(int i = 0; i < count; i++)
        g_hash_table_replace(client->properties, g_strdup(properties[i]->name),
                             properties[i]);
    free(properties);
}

static void
delete_properties(SmsConn connection, SmPointer data, int count, char **names)
{
    (void)connection;
    SessionClient *client = data;
    for(int i = 0; i < count; i++) {
        g_hash_table_remove(client->properties, names[i]);
        free(names[i]);
    }
    free(names);
}

static void
get_properties(SmsConn connection, SmPointer data)
{
    SessionClient *client = data;
    GList *values = g_hash_table_get_values(client->properties);
    int count = (int)g_list_length(values);
    SmProp **properties = g_new(SmProp *, count);
    int index = 0;
    for(GList *value = values; value != NULL; value = value->next)
        properties[index++] = value->data;
    SmsReturnProperties(connection, count, properties);
    IceFlush(client->ice);
    g_free(properties);
    g_list_free(values);
}

static Status
new_client(SmsConn connection, SmPointer data, unsigned long *mask,
           SmsCallbacks *callbacks, char **reason)
{
    (void)data;
    if(logout_pending || finishing) {
        *reason = strdup("session is ending");
        return 0;
    }
    SessionClient *client = NULL;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i].connection == NULL) {
            client = &clients[i];
            break;
        }
    }
    if(client == NULL) {
        *reason = strdup("too many session clients");
        return 0;
    }
    client->connection = connection;
    client->ice = SmsGetIceConnection(connection);
    client->properties = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)SmFreeProperty);
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->register_client.callback = register_client;
    callbacks->register_client.manager_data = client;
    callbacks->interact_request.callback = interact_request;
    callbacks->interact_request.manager_data = client;
    callbacks->interact_done.callback = interact_done;
    callbacks->interact_done.manager_data = client;
    callbacks->save_yourself_request.callback = save_request;
    callbacks->save_yourself_request.manager_data = client;
    callbacks->save_yourself_phase2_request.callback = phase2_request;
    callbacks->save_yourself_phase2_request.manager_data = client;
    callbacks->save_yourself_done.callback = save_done;
    callbacks->save_yourself_done.manager_data = client;
    callbacks->close_connection.callback = close_connection;
    callbacks->close_connection.manager_data = client;
    callbacks->set_properties.callback = set_properties;
    callbacks->set_properties.manager_data = client;
    callbacks->delete_properties.callback = delete_properties;
    callbacks->delete_properties.manager_data = client;
    callbacks->get_properties.callback = get_properties;
    callbacks->get_properties.manager_data = client;
    *mask = SmsRegisterClientProcMask | SmsInteractRequestProcMask |
            SmsInteractDoneProcMask | SmsSaveYourselfRequestProcMask |
            SmsSaveYourselfP2RequestProcMask | SmsSaveYourselfDoneProcMask |
            SmsCloseConnectionProcMask | SmsSetPropertiesProcMask |
            SmsDeletePropertiesProcMask | SmsGetPropertiesProcMask;
    return 1;
}

static void
begin_save(int shutdown)
{
    if(logout_pending || checkpoint_pending)
        return;
    logout_pending = shutdown;
    checkpoint_pending = !shutdown;
    save_deadline = now_ms() + 30000;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        SessionClient *client = &clients[i];
        if(client->connection == NULL || client->closing)
            continue;
        client->saving = 1;
        client->saved = 0;
        client->phase2 = 0;
        SmsSaveYourself(client->connection, SmSaveBoth, shutdown,
                        SmInteractStyleAny, False);
        IceFlush(client->ice);
    }
}

static void
cancel_logout(void)
{
    fprintf(stderr, "rill-sessiond: logout cancelled; applications remain running\n");
    logout_pending = 0;
    power_action = NULL;
    checkpoint_pending = 0;
    interacting = NULL;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        SessionClient *client = &clients[i];
        if(client->connection == NULL || client->closing)
            continue;
        client->saving = 0;
        client->phase2 = 0;
        client->wants_interaction = 0;
        SmsShutdownCancelled(client->connection);
        IceFlush(client->ice);
    }
    /* A service may have exited while saving; resume its supervision. */
    for(guint i = 0; i < processes->len; i++) {
        SessionProcess *process = g_ptr_array_index(processes, i);
        if(process->restart && process->pid == 0 && process->retry_at == 0 &&
           process->failures <= MAX_RESTARTS)
            process->retry_at = now_ms() + 500;
    }
}

static int
request_power(void)
{
    if(power_action == NULL)
        return 1;
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    GVariant *reply = NULL;
    if(bus != NULL) {
        reply = g_dbus_connection_call_sync(bus, "org.freedesktop.login1",
            "/org/freedesktop/login1", "org.freedesktop.login1.Manager", power_action,
            g_variant_new("(b)", TRUE), NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
        g_object_unref(bus);
    }
    if(reply == NULL) {
        fprintf(stderr, "rill-sessiond: power request failed: %s\n",
                error ? error->message : "no system bus");
        g_clear_error(&error);
        return 0;
    }
    g_variant_unref(reply);
    return 1;
}

static void
advance_save(void)
{
    if(!logout_pending && !checkpoint_pending)
        return;
    int ready = 1;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        SessionClient *client = &clients[i];
        if(client->connection == NULL || client->closing)
            continue;
        if(client->wants_interaction && interacting == NULL) {
            client->wants_interaction = 0;
            interacting = client;
            SmsInteract(client->connection);
            IceFlush(client->ice);
        }
        if(!client->saved && !client->phase2)
            ready = 0;
    }
    if(ready && interacting == NULL) {
        for(int i = 0; i < MAX_CLIENTS; i++) {
            SessionClient *client = &clients[i];
            if(client->connection != NULL && !client->closing && client->phase2) {
                client->phase2 = 0;
                SmsSaveYourselfPhase2(client->connection);
                IceFlush(client->ice);
                ready = 0;
            }
        }
        if(ready) {
            save_session();
            if(logout_pending && !request_power()) {
                cancel_logout();
                return;
            }
            for(int i = 0; i < MAX_CLIENTS; i++) {
                SessionClient *client = &clients[i];
                if(client->connection == NULL || client->closing)
                    continue;
                if(logout_pending)
                    SmsDie(client->connection);
                else
                    SmsSaveComplete(client->connection);
                IceFlush(client->ice);
            }
            if(logout_pending)
                finishing = 1;
            checkpoint_pending = 0;
            return;
        }
    }
    /* Never discard unsaved work merely because a client stopped answering.
     * While a client is showing its save dialog there is no time limit. */
    if(interacting != NULL)
        save_deadline = now_ms() + 30000;
    else if(now_ms() >= save_deadline) {
        if(logout_pending)
            cancel_logout();
        else {
            checkpoint_pending = 0;
            fprintf(stderr, "rill-sessiond: session save timed out\n");
        }
    }
}

static void
autostart_pid(GDesktopAppInfo *app, GPid pid, gpointer data)
{
    const char *kind = data != NULL ? data : "autostart";
    SessionProcess *process = add_process(g_app_info_get_name(G_APP_INFO(app)),
                                          NULL, NULL, 0, 0, 0);
    process->pid = pid;
    fprintf(stderr, "rill-sessiond: %s %ld: %s\n", kind, (long)pid, process->name);
}

static int
matches_desktop(GKeyFile *file, const char *key)
{
    char **values = g_key_file_get_string_list(file, "Desktop Entry", key, NULL, NULL);
    const char *current = g_getenv("XDG_CURRENT_DESKTOP");
    char **desktops = g_strsplit(current != NULL ? current : "Rill:XFCE", ":", -1);
    int match = 0;
    if(values != NULL) {
        for(int i = 0; values[i] != NULL; i++)
            for(int j = 0; desktops[j] != NULL; j++)
                if(strcmp(values[i], desktops[j]) == 0)
                    match = 1;
    }
    g_strfreev(values);
    g_strfreev(desktops);
    return match;
}

static void
run_autostart(void)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for(guint i = 0; i < autostart_dirs->len; i++) {
        const char *directory = g_ptr_array_index(autostart_dirs, i);
        GDir *dir = g_dir_open(directory, 0, NULL);
        if(dir == NULL)
            continue;
        const char *name;
        while((name = g_dir_read_name(dir)) != NULL) {
            if(!g_str_has_suffix(name, ".desktop") || g_hash_table_contains(seen, name))
                continue;
            /* Even Hidden or invalid user entries shadow system entries. */
            g_hash_table_add(seen, g_strdup(name));
            char *path = g_build_filename(directory, name, NULL);
            GKeyFile *file = g_key_file_new();
            GDesktopAppInfo *app = NULL;
            if(g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL) &&
               !g_key_file_get_boolean(file, "Desktop Entry", "Hidden", NULL) &&
               (!g_key_file_has_key(file, "Desktop Entry", "OnlyShowIn", NULL) ||
                matches_desktop(file, "OnlyShowIn")) &&
               !matches_desktop(file, "NotShowIn"))
                app = g_desktop_app_info_new_from_filename(path);
            if(app != NULL) {
                const char *command = g_app_info_get_commandline(G_APP_INFO(app));
                if(command != NULL && !managed_program(command)) {
                    GError *error = NULL;
                    if(!g_desktop_app_info_launch_uris_as_manager(app, NULL, NULL,
                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                            child_setup, NULL, autostart_pid, NULL, &error)) {
                        fprintf(stderr, "rill-sessiond: autostart %s: %s\n",
                                name, error->message);
                        g_clear_error(&error);
                    }
                }
                g_object_unref(app);
            }
            g_key_file_unref(file);
            g_free(path);
        }
        g_dir_close(dir);
    }
    g_hash_table_unref(seen);
}

static int
load_processes(const char *path, int restore)
{
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;
    if(!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error)) {
        int ok = restore && g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
        if(!ok)
            fprintf(stderr, "rill-sessiond: %s: %s\n", path, error->message);
        g_clear_error(&error);
        g_key_file_unref(file);
        return ok;
    }
    char **groups = g_key_file_get_groups(file, NULL);
    for(int i = 0; groups[i] != NULL; i++) {
        if(strcmp(groups[i], "Session") == 0)
            continue;
        char **argv = g_key_file_get_string_list(file, groups[i], "Command", NULL, NULL);
        char *directory = g_key_file_get_string(file, groups[i], "Directory", NULL);
        if(argv != NULL && argv[0] != NULL && argv[0][0] != '\0' &&
           (!restore || !managed_arguments(argv))) {
            SessionProcess *process = add_process(groups[i], argv, directory,
                !restore && g_key_file_get_boolean(file, groups[i], "Restart", NULL),
                !restore && g_key_file_get_boolean(file, groups[i], "Required", NULL), 0);
            if(restore)
                g_hash_table_add(restored_programs, command_key(argv));
            start_process(process);
        }
        g_strfreev(argv);
        g_free(directory);
    }
    g_strfreev(groups);
    g_key_file_unref(file);
    return 1;
}

static int
setup_runtime(void)
{
    const char *base = g_getenv("XDG_RUNTIME_DIR");
    if(base == NULL || base[0] != '/')
        base = g_get_tmp_dir();
    runtime_directory = g_build_filename(base, "rill-session-XXXXXX", NULL);
    if(g_mkdtemp(runtime_directory) == NULL)
        return -1;
    control_path = g_build_filename(runtime_directory, "control", NULL);
    authority_path = g_build_filename(runtime_directory, "ICEauthority", NULL);
    if(mkfifo(control_path, 0600) != 0)
        return -1;
    setenv("RILL_SESSION_CONTROL", control_path, 1);
    setenv("ICEAUTHORITY", authority_path, 1);
    int fd = open(control_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    return fd >= 0 ? fd : -1;
}

static int
setup_ice(void)
{
    char error[256] = "";
    if(!SmsInitialize("rill", "Rill Session", new_client, NULL, reject_host_auth,
                       sizeof(error), error))
        return 0;
    IceListenObj *all = NULL;
    int count = 0;
    if(!IceListenForConnections(&count, &all, sizeof(error), error)) {
        fprintf(stderr, "rill-sessiond: %s\n", error);
        return 0;
    }
    auth_cookie = IceGenerateMagicCookie(16);
    FILE *file = fopen(authority_path, "wb");
    if(file == NULL || auth_cookie == NULL) {
        if(file != NULL)
            fclose(file);
        return 0;
    }
    GString *addresses = g_string_new(NULL);
    int ok = 1;
    for(int i = 0; i < count; i++) {
        char *address = IceGetListenConnectionString(all[i]);
        if(address != NULL && listener_count < MAX_LISTENERS &&
           (g_str_has_prefix(address, "local/") ||
            g_str_has_prefix(address, "unix/") || g_str_has_prefix(address, "local:"))) {
            listeners[listener_count++] = all[i];
            IceSetHostBasedAuthProc(all[i], reject_host_auth);
            if(addresses->len != 0)
                g_string_append_c(addresses, ',');
            g_string_append(addresses, address);
            for(int protocol = 0; protocol < 2; protocol++) {
                IceAuthDataEntry *entry = &auth_entries[auth_count++];
                entry->protocol_name = protocol == 0 ? "ICE" : "XSMP";
                entry->network_id = g_strdup(address);
                entry->auth_name = "MIT-MAGIC-COOKIE-1";
                entry->auth_data_length = 16;
                entry->auth_data = auth_cookie;
                IceAuthFileEntry output = {entry->protocol_name, 0, "",
                    entry->network_id, entry->auth_name, 16, auth_cookie};
                if(!IceWriteAuthFileEntry(file, &output))
                    ok = 0;
            }
        } else
            close(IceGetListenConnectionNumber(all[i]));
        free(address);
    }
    free(all);
    if(fclose(file) != 0)
        ok = 0;
    IceSetPaAuthData(auth_count, auth_entries);
    setenv("SESSION_MANAGER", addresses->str, 1);
    g_string_free(addresses, TRUE);
    return ok && listener_count > 0;
}

static void
publish_activation_environment(void)
{
    /* Update this session's bus only. Importing DISPLAY into the shared user
       systemd manager would redirect programs from another login session. */
    if(g_getenv("DBUS_SESSION_BUS_ADDRESS") == NULL)
        return;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if(bus == NULL)
        return;
    GVariantBuilder values;
    g_variant_builder_init(&values, G_VARIANT_TYPE("a{ss}"));
    const char *names[] = {"SESSION_MANAGER", "ICEAUTHORITY", "RILL_SESSION_CONTROL"};
    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *value = g_getenv(names[i]);
        if(value != NULL)
            g_variant_builder_add(&values, "{ss}", names[i], value);
    }
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "UpdateActivationEnvironment",
        g_variant_new("(a{ss})", &values), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if(reply != NULL)
        g_variant_unref(reply);
    g_object_unref(bus);
}

static void
launch_application(const char *request)
{
    if(logout_pending || finishing)
        return;
    GError *error = NULL;
    GVariant *value = NULL;
    GDesktopAppInfo *desktop_app = NULL;
    GAppInfo *handler = NULL;
    GList *uris = NULL;
    char *uri = NULL;
    if(g_str_has_prefix(request, "launch ")) {
        value = g_variant_parse(G_VARIANT_TYPE_STRING_ARRAY, request + 7, NULL, NULL, &error);
        if(value != NULL) {
            char **arguments = g_variant_dup_strv(value, NULL);
            if(arguments[0] != NULL && arguments[0][0] && g_strv_length(arguments) <= 128)
                start_process(add_process(arguments[0], arguments, NULL, 0, 0, 0));
            g_strfreev(arguments);
        }
    } else if(g_str_has_prefix(request, "desktop ")) {
        value = g_variant_parse(G_VARIANT_TYPE_STRING, request + 8, NULL, NULL, &error);
        if(value != NULL)
            desktop_app = g_desktop_app_info_new_from_filename(g_variant_get_string(value, NULL));
    } else if(g_str_has_prefix(request, "open ")) {
        value = g_variant_parse(G_VARIANT_TYPE_STRING, request + 5, NULL, NULL, &error);
        if(value != NULL) {
            GFile *file = g_file_new_for_commandline_arg(g_variant_get_string(value, NULL));
            handler = g_file_query_default_handler(file, NULL, &error);
            uri = g_file_get_uri(file);
            g_object_unref(file);
            if(G_IS_DESKTOP_APP_INFO(handler)) {
                desktop_app = g_object_ref(G_DESKTOP_APP_INFO(handler));
                uris = g_list_append(NULL, uri);
            }
        }
    }
    if(desktop_app != NULL) {
        g_desktop_app_info_launch_uris_as_manager(desktop_app, uris, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, child_setup, NULL,
            autostart_pid, "application", &error);
        g_object_unref(desktop_app);
    }
    if(error != NULL) {
        fprintf(stderr, "rill-sessiond: application launch failed: %s\n", error->message);
        g_error_free(error);
    }
    if(value != NULL)
        g_variant_unref(value);
    g_clear_object(&handler);
    g_list_free(uris);
    g_free(uri);
}

static void
process_control(int fd)
{
    static char buffer[4096];
    static size_t used;
    static int overflow;
    char bytes[256];
    ssize_t count;
    while((count = read(fd, bytes, sizeof(bytes))) > 0) {
        for(ssize_t i = 0; i < count; i++) {
            if(bytes[i] == '\n') {
                buffer[used] = '\0';
                if(overflow) {
                    used = 0;
                    overflow = 0;
                    continue;
                }
                if((strcmp(buffer, "logout") == 0 || strcmp(buffer, "restart") == 0 ||
                    strcmp(buffer, "shutdown") == 0) && !logout_pending && !checkpoint_pending) {
                    power_action = strcmp(buffer, "restart") == 0 ? "Reboot" :
                                   strcmp(buffer, "shutdown") == 0 ? "PowerOff" : NULL;
                    begin_save(1);
                }
                else if(strcmp(buffer, "save") == 0)
                    begin_save(0);
                else if(strcmp(buffer, "cancel") == 0 && logout_pending)
                    cancel_logout();
                else
                    launch_application(buffer);
                used = 0;
            } else if(used + 1 < sizeof(buffer))
                buffer[used++] = bytes[i];
            else
                overflow = 1;
        }
    }
}

static void
poll_session(int control_fd)
{
    struct pollfd fds[MAX_LISTENERS + MAX_CLIENTS + 1];
    int slots[MAX_LISTENERS + MAX_CLIENTS + 1];
    int count = 0;
    for(int i = 0; i < listener_count; i++) {
        slots[count] = i;
        fds[count++] = (struct pollfd){IceGetListenConnectionNumber(listeners[i]), POLLIN, 0};
    }
    int first_connection = count;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(connections[i] != NULL) {
            slots[count] = i;
            fds[count++] = (struct pollfd){IceConnectionNumber(connections[i]), POLLIN, 0};
        }
    }
    int control_index = count;
    fds[count++] = (struct pollfd){control_fd, POLLIN, 0};
    poll(fds, (nfds_t)count, 100);
    for(int i = 0; i < count; i++) {
        if(!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        if(i < first_connection) {
            IceAcceptStatus status;
            IceConn connection = IceAcceptConnection(listeners[slots[i]], &status);
            if(connection == NULL)
                continue;
            int slot;
            for(slot = 0; slot < MAX_CLIENTS && connections[slot] != NULL; slot++)
                ;
            if(slot < MAX_CLIENTS)
                connections[slot] = connection;
            else
                IceCloseConnection(connection);
        } else if(i == control_index)
            process_control(control_fd);
        else {
            IceConn connection = connections[slots[i]];
            IceProcessMessages(connection, NULL, NULL);
        }
    }
    for(int i = 0; i < MAX_CLIENTS; i++) {
        IceConn connection = connections[i];
        if(connection == NULL)
            continue;
        IceConnectStatus status = IceConnectionStatus(connection);
        int close_it = status == IceConnectRejected || status == IceConnectIOError;
        for(int j = 0; j < MAX_CLIENTS; j++) {
            if(clients[j].connection != NULL && clients[j].ice == connection &&
               (close_it || clients[j].closing)) {
                remove_client(&clients[j]);
                close_it = 1;
            }
        }
        if(close_it) {
            IceSetShutdownNegotiation(connection, False);
            IceCloseConnection(connection);
            connections[i] = NULL;
        }
    }
    while(g_main_context_iteration(NULL, FALSE))
        ;
}

static void
stop_processes(void)
{
    finishing = 1;
    for(guint i = 0; i < processes->len; i++) {
        SessionProcess *process = g_ptr_array_index(processes, i);
        if(process->pid > 0)
            kill(-process->pid, SIGTERM);
    }
    gint64 deadline = now_ms() + 2000;
    while(now_ms() < deadline) {
        reap_processes();
        int alive = retired_groups->len > 0;
        for(guint i = 0; i < processes->len; i++) {
            SessionProcess *process = g_ptr_array_index(processes, i);
            alive |= process->pid > 0;
        }
        if(!alive)
            break;
        g_usleep(20000);
    }
    for(guint i = 0; i < processes->len; i++) {
        SessionProcess *process = g_ptr_array_index(processes, i);
        if(process->pid > 0) {
            kill(-process->pid, SIGKILL);
            waitpid(process->pid, NULL, 0);
        }
        g_free(process->name);
        g_strfreev(process->argv);
        g_free(process->directory);
        g_free(process);
    }
    reap_groups(1);
    g_array_unref(retired_groups);
    g_ptr_array_unref(processes);
}

int
main(int argc, char **argv)
{
    int body_index = argc;
    int default_autostart = 1;
    int restart_body = 0;
    int restore = 0;
    const char *services_file = NULL;
    umask(0077);
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    IceSetIOErrorHandler(ice_error);
    processes = g_ptr_array_new();
    retired_groups = g_array_new(FALSE, FALSE, sizeof(RetiredGroup));
    autostart_dirs = g_ptr_array_new_with_free_func(g_free);
    restored_programs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--autostart-dir") == 0 && i + 1 < argc) {
            g_ptr_array_add(autostart_dirs, g_strdup(argv[++i]));
            default_autostart = 0;
        } else if(g_str_has_prefix(argv[i], "--autostart-dir=")) {
            g_ptr_array_add(autostart_dirs, g_strdup(argv[i] + 16));
            default_autostart = 0;
        } else if(strcmp(argv[i], "--no-default-autostart") == 0)
            default_autostart = 0;
        else if(strcmp(argv[i], "--restart-body") == 0)
            restart_body = 1;
        else if(strcmp(argv[i], "--restore") == 0)
            restore = 1;
        else if(strcmp(argv[i], "--state-file") == 0 && i + 1 < argc)
            state_file = argv[++i];
        else if(strcmp(argv[i], "--services") == 0 && i + 1 < argc)
            services_file = argv[++i];
        else if(strcmp(argv[i], "--") == 0) {
            body_index = i + 1;
            break;
        } else {
            fprintf(stderr, "rill-sessiond: unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
    int control_fd = setup_runtime();
    if(control_fd < 0 || !setup_ice()) {
        fprintf(stderr, "rill-sessiond: could not create private session endpoints\n");
        return 1;
    }
    publish_activation_environment();
    printf("control: %s\n", control_path);
    fflush(stdout);
    SessionProcess *body = NULL;
    if(body_index < argc)
        body = add_process("desktop", &argv[body_index], NULL, restart_body, 1, 1);
    if(services_file != NULL && !load_processes(services_file, 0)) {
        exit_status = 1;
        stop_requested = 1;
    }
    if(!stop_requested && body != NULL)
        start_process(body);
    if(!stop_requested && restore && state_file != NULL)
        load_processes(state_file, 1);
    if(default_autostart) {
        g_ptr_array_add(autostart_dirs,
                        g_build_filename(g_get_user_config_dir(), "autostart", NULL));
        const char *const *directories = g_get_system_config_dirs();
        for(int i = 0; directories[i] != NULL; i++)
            g_ptr_array_add(autostart_dirs,
                            g_build_filename(directories[i], "autostart", NULL));
    }
    if(!stop_requested)
        run_autostart();
    while(!stop_requested && !finishing) {
        reap_processes();
        poll_session(control_fd);
        advance_save();
    }
    /* Give cooperative clients a chance to act on Die before cleaning up
     * only the process groups this manager actually launched. */
    if(finishing)
        g_usleep(100000);
    stop_processes();
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i].connection != NULL)
            remove_client(&clients[i]);
    }
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(connections[i] != NULL) {
            IceSetShutdownNegotiation(connections[i], False);
            IceCloseConnection(connections[i]);
        }
    }
    close(control_fd);
    unlink(control_path);
    unlink(authority_path);
    rmdir(runtime_directory);
    g_free(control_path);
    g_free(authority_path);
    g_free(runtime_directory);
    g_ptr_array_unref(autostart_dirs);
    g_hash_table_unref(restored_programs);
    for(int i = 0; i < auth_count; i++)
        g_free(auth_entries[i].network_id);
    free(auth_cookie);
    return exit_status;
}
