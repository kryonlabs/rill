/* Bridge between Rill's .kry UI sources (app/*.kry) and the shell's C state.
 * See include/rill_kry_ui.h for the contract; main.c provides the helpers
 * declared in rill_kry_internal.h and calls RillKrySetContext() at startup. */
#include "rill_kry_ui.h"
#include "rill_kry_internal.h"
#include "../kryon/include/kryon.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

RillShellState *kry_shell;
static RillVisualState *kry_visuals;
static const RillPlatformServices *kry_platform;
static int kry_run_cursor;
static int kry_run_focus;
static int kry_rebinding = -1;
static char kry_text_buffer[160];

void
RillKrySetContext(RillShellState *shell, RillVisualState *visuals,
                  const RillPlatformServices *platform)
{
    kry_shell = shell;
    kry_visuals = visuals;
    kry_platform = platform;
}

int
RillKryButton(float x, float y, float width, float height,
              const char *label, int active)
{
    return draw_settings_button((Rectangle){x, y, width, height}, label, active);
}

void
RillKryTextFit(float x, float y, float width, const char *text,
               int font, int class_name)
{
    draw_text_fit((TextProps){.bounds = {x, y, width, 0}, .text = text,
                              .font = font, .class_name = class_name,
                              .wrap = TextWrapNone});
}

void
RillKryStatus(const char *message)
{
    if(kry_shell != NULL)
        RillShellSetStatus(kry_shell, message);
}

char *
RillKryRunText(void)
{
    return kry_visuals != NULL ? kry_visuals->run_input : kry_text_buffer;
}

int
RillKryRunTextCap(void)
{
    return (int)(kry_visuals != NULL ? sizeof(kry_visuals->run_input) :
                 sizeof(kry_text_buffer));
}

int *
RillKryRunCursor(void)
{
    return &kry_run_cursor;
}

int *
RillKryRunFocus(void)
{
    return &kry_run_focus;
}

static const RillLauncher *
kry_run_match(int index)
{
    return kry_shell != NULL && kry_visuals != NULL ?
        run_match_launcher(kry_shell, kry_visuals->run_input, index) : NULL;
}

int
RillKryRunMatchCount(void)
{
    int count = 0;
    for(int i = 0; i < 6; i++)
        if(kry_run_match(i) != NULL)
            count++;
    return count;
}

const char *
RillKryRunMatchName(int index)
{
    const RillLauncher *match = kry_run_match(index);
    return match != NULL ? match->name : "";
}

const char *
RillKryRunMatchDescription(int index)
{
    const RillLauncher *match = kry_run_match(index);
    return match != NULL ? match->description : "";
}

int
RillKryRunHistoryCount(void)
{
    const char *history = RillSettingsGet(&rill_kry_settings(), "run-history", "");
    int count = 0;
    while(*history != '\0' && count < 4) {
        const char *end = strchr(history, '|');
        if(end == history)
            break;
        count++;
        if(end == NULL)
            break;
        history = end + 1;
    }
    return count;
}

const char *
RillKryRunHistoryItem(int index)
{
    static char item[128];
    const char *history = RillSettingsGet(rill_kry_settings(), "run-history", "");
    for(int i = 0; i <= index && history[0] != '\0'; i++) {
        const char *end = strchr(history, '|');
        size_t size = end != NULL ? (size_t)(end - history) : strlen(history);
        if(size == 0)
            return "";
        if(i == index) {
            if(size > sizeof(item) - 1)
                size = sizeof(item) - 1;
            memcpy(item, history, size);
            item[size] = '\0';
            return item;
        }
        if(end == NULL)
            break;
        history = end + 1;
    }
    return "";
}

int
RillKryRunLaunchMatch(int index)
{
    int ok = run_execute(kry_shell, kry_platform, kry_run_match(index),
                         kry_visuals != NULL ? kry_visuals->run_input : "");
    if(ok)
        rill_stop_requested = 1;
    return ok;
}

int
RillKryRunLaunchText(void)
{
    int ok = run_execute(kry_shell, kry_platform, kry_run_match(0),
                         kry_visuals != NULL ? kry_visuals->run_input : "");
    if(ok)
        rill_stop_requested = 1;
    return ok;
}

void
RillKryRunClose(void)
{
    rill_stop_requested = 1;
}

const char *
RillKryThemeName(void)
{
    return kry_visuals != NULL ? kry_visuals->system_theme_name : "";
}

const char *
RillKryFontName(void)
{
    return kry_visuals != NULL ? kry_visuals->system_font_name : "";
}
