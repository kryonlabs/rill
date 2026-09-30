#ifndef RILL_RUN_H
#define RILL_RUN_H

#include "rill_shell.h"

const RillLauncher *RillRunMatchLauncher(const RillShellState *, const char *, int);
int RillRunHistoryItem(const RillSettings *, int, char *, int);
void RillRunRecordHistory(RillSettings *, const char *);
int RillRunExecute(RillShellState *, const RillPlatformServices *, RillSettings *,
                   RillSettings *, const char *, const RillLauncher *, const char *);

#endif
