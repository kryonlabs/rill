#ifndef RILL_APPLICATIONS_H
#define RILL_APPLICATIONS_H

#include "rill_shell.h"

const char *RillApplicationsCategoryName(int);
int RillTextContainsFold(const char *, const char *);
int RillApplicationsMatches(const RillShellState *, const RillLauncher *);
int RillApplicationsAt(const RillShellState *, int);
int RillApplicationsCount(const RillShellState *);
void RillApplicationsLoadRecent(RillShellState *, const RillSettings *);
void RillApplicationsStoreRecent(const RillShellState *, RillSettings *);
int RillApplicationsLaunch(RillShellState *, const RillPlatformServices *,
                           RillSettings *, RillSettings *, const char *, int);
int RillApplicationsOpen(RillShellState *, const RillPlatformServices *);

#endif
