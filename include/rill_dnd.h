#ifndef RILL_DND_H
#define RILL_DND_H

#include <stddef.h>

#ifdef KRYON_PLATFORM_PLAN9
/* 8c rejects the megabyte X11 drag structure; native Plan 9 has no XDND. */
#define RILL_DND_MAX_URIS 8
#define RILL_DND_URI_MAX 512
#else
#define RILL_DND_MAX_URIS 256
#define RILL_DND_URI_MAX 4096
#endif

typedef struct RillDndDrop {
    int x, y;   /* root coordinates of the drop */
    int move;   /* the source requested a move */
    int count;
    char uris[RILL_DND_MAX_URIS][RILL_DND_URI_MAX];
} RillDndDrop;

/* Target side: advertise `window` (an X11 Window id) as an XDND drop target
   through a proxy window owned by this module. Returns 1 on success. */
int RillDndTargetInit(unsigned long window);
/* Pump protocol traffic. When a drop has been received it is copied into
   *drop and 1 is returned; the caller answers with RillDndTargetFinish(). */
int RillDndTargetPoll(RillDndDrop *drop);
/* Tell the source whether the drop was handled. */
void RillDndTargetFinish(int accepted);
void RillDndTargetShutdown(void);

/* Source side: start dragging URIs (already "file://..." form). */
int RillDndSourceStart(const char *const *uris, int count);
/* Feed the pointer state. Returns 0 inactive, 1 dragging, 2 completed. */
int RillDndSourceUpdate(int x, int y, int button_down);
void RillDndSourceAbort(void);

#endif
