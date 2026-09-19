#ifndef RILL_KRY_INTERNAL_H
#define RILL_KRY_INTERNAL_H

/* Private glue between main.c and the .kry UI bridge (src/rill_kry_bridge.c).
 * main.c owns the shell state; the bridge reads it through these exports. */

#include "rill_platform.h"
#include "rill_shell.h"

void rill_settings_persist(RillShellState *shell);
int draw_settings_button(Rectangle bounds, const char *label, int active);
void draw_text_fit(TextProps props);
const RillLauncher *run_match_launcher(const RillShellState *shell,
                                       const char *text, int index);
int run_execute(RillShellState *shell, const RillPlatformServices *platform,
                const RillLauncher *match, const char *text);
void apply_wallpaper(RillShellState *shell, RillVisualState *visuals,
                     const char *path, int persist);
void load_panel_entries(RillVisualState *visuals,
                        const RillPlatformServices *platform);
void serialize_panel_config(const RillVisualState *visuals, char *json, int size);
int import_xfce_panel(RillVisualState *visuals,
                      const RillPlatformServices *platform);
void load_input_settings(RillVisualState *visuals,
                         const RillPlatformServices *platform);
void load_wm_keys(RillVisualState *visuals);
void save_wm_keys(RillVisualState *visuals);
const char *wm_key_name(int key);
extern int rill_stop_requested;

/* Set by main() once the platform services and shell state exist. */
void RillKrySetContext(RillShellState *shell, RillVisualState *visuals,
                       const RillPlatformServices *platform);

#endif
