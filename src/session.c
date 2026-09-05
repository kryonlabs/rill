#include "session.h"
#include <X11/SM/SMlib.h>
#include <X11/ICE/ICElib.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static SmcConn connection;
static char *client_id;
static int stopping;
static void save(SmcConn c, SmPointer data, int type, Bool shutdown, int style, Bool fast)
{
    (void)data; (void)type; (void)shutdown; (void)style; (void)fast;
    /* Panel state is persisted at the end of each UI frame. */
    SmcSaveYourselfDone(c, True);
}
static void die(SmcConn c, SmPointer data) { (void)c; (void)data; stopping = 1; }
static void complete(SmcConn c, SmPointer data) { (void)c; (void)data; }
static void cancelled(SmcConn c, SmPointer data) { (void)c; (void)data; }

int SessionConnect(int argc, char **argv)
{
    SmcCallbacks callbacks = {{save, NULL}, {die, NULL}, {complete, NULL}, {cancelled, NULL}};
    char error[256], pid[32];
    const char *previous = NULL;
    struct passwd *user = getpwuid(getuid());
    if(getenv("SESSION_MANAGER") == NULL) return 0;
    for(int i = 1; i + 1 < argc; i++)
        if(strcmp(argv[i], "--sm-client-id") == 0) previous = argv[i + 1];
    connection = SmcOpenConnection(NULL, NULL, 1, 0,
        SmcSaveYourselfProcMask | SmcDieProcMask | SmcSaveCompleteProcMask |
        SmcShutdownCancelledProcMask, &callbacks, previous, &client_id, sizeof(error), error);
    if(connection == NULL) { fprintf(stderr, "rill: session registration: %s\n", error); return 0; }
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    SmPropValue *args = calloc((size_t)argc + 2, sizeof(*args));
    if(args == NULL) { SessionDisconnect(); return 0; }
    int n = 0;
    for(int i = 0; i < argc; i++) {
        if(strcmp(argv[i], "--sm-client-id") == 0 && i + 1 < argc) { i++; continue; }
        args[n++] = (SmPropValue){(int)strlen(argv[i]), argv[i]};
    }
    args[n++] = (SmPropValue){14, "--sm-client-id"};
    args[n++] = (SmPropValue){(int)strlen(client_id), client_id};
    unsigned char restart = SmRestartImmediately;
    char *username = user != NULL ? user->pw_name : "";
    SmPropValue values[] = {{(int)strlen(argv[0]), argv[0]}, {(int)strlen(pid), pid},
                            {(int)strlen(username), username}, {1, &restart}};
    SmProp props[] = {{SmProgram, SmARRAY8, 1, &values[0]},
                      {SmProcessID, SmARRAY8, 1, &values[1]},
                      {SmUserID, SmARRAY8, 1, &values[2]},
                      {SmRestartStyleHint, SmCARD8, 1, &values[3]},
                      {SmRestartCommand, SmLISTofARRAY8, n, args}};
    SmProp *list[5];
    for(int i = 0; i < 5; i++) list[i] = &props[i];
    SmcSetProperties(connection, 5, list);
    free(args);
    return 1;
}

int SessionPoll(void)
{
    if(connection != NULL) {
        IceConn ice = SmcGetIceConnection(connection);
        struct pollfd fd = {IceConnectionNumber(ice), POLLIN, 0};
        if(poll(&fd, 1, 0) > 0) {
            if(fd.revents & (POLLERR | POLLHUP | POLLNVAL)) stopping = 1;
            else if(IceProcessMessages(ice, NULL, NULL) != IceProcessMessagesSuccess) stopping = 1;
        }
    }
    return !stopping;
}

void SessionDisconnect(void)
{
    if(connection != NULL) SmcCloseConnection(connection, 0, NULL);
    connection = NULL;
    free(client_id);
    client_id = NULL;
}
