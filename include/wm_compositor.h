#ifndef WM_COMPOSITOR_H
#define WM_COMPOSITOR_H
#include <X11/Xlib.h>
int CompositorStart(Display *display, Window root, Window owner);
void CompositorEvent(XEvent *event);
void CompositorPaint(void);
void CompositorStop(void);
#endif
