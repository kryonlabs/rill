#ifndef RILL_PANEL_H
#define RILL_PANEL_H

typedef enum RillPanelPluginKind {
    RILL_PANEL_SEPARATOR,
    RILL_PANEL_MENU,
    RILL_PANEL_LAUNCHER,
    RILL_PANEL_TASK_LIST,
    RILL_PANEL_WORKSPACES,
    RILL_PANEL_TRAY,
    RILL_PANEL_LANGUAGE,
    RILL_PANEL_CLOCK,
    RILL_PANEL_RESOURCE
} RillPanelPluginKind;

typedef struct RillPanelPlugin {
    RillPanelPluginKind kind;
    char id[64];
    char label[96];
    char launcher_id[64];
    int menu_id;
    int width;
    int advance;
    int variant;
} RillPanelPlugin;

const RillPanelPlugin *RillPanelDefaultLeft(int *count);
const RillPanelPlugin *RillPanelDefaultRight(int *count);
const char *RillPanelPluginKindName(RillPanelPluginKind kind);
int RillPanelPluginKindFromName(const char *name, RillPanelPluginKind *out);

/* Versioned text configuration; failed loads leave the caller's layout intact. */
int RillPanelLoad(const char *path, RillPanelPlugin *left, int *left_count,
                  RillPanelPlugin *right, int *right_count, int cap);
int RillPanelSave(const char *path, const RillPanelPlugin *left, int left_count,
                  const RillPanelPlugin *right, int right_count);

#endif
