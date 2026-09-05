#ifndef RILL_WAYLAND_H
#define RILL_WAYLAND_H

#include "rill_platform.h"

int RillWaylandSession(void);
int RillWaylandListTasks(RillTask *out, int cap);
int RillWaylandFocusTask(int id);
int RillWaylandCloseTask(int id);
int RillWaylandAvailable(void);
void RillWaylandShutdown(void);

#endif
