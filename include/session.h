#ifndef SESSION_H
#define SESSION_H
int SessionConnect(int argc, char **argv);
int SessionPoll(void);
void SessionDisconnect(void);
#endif
