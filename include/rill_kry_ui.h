#ifndef RILL_KRY_UI_H
#define RILL_KRY_UI_H

/* Bridge between Rill's .kry UI sources (app/*.kry) and the shell's C state.
 * Implemented in src/main.c, where the shell/platform state lives; the .kry
 * sources #import this header and call these functions directly. */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Shared drawing primitives (pixel-identical with the C shell) ---- */
int RillKryButton(float x, float y, float width, float height,
                  const char *label, int active);
void RillKryTextFit(float x, float y, float width, const char *text,
                    int font, int class_name);
void RillKryStatus(const char *message);

/* ---- Run dialog ---- */
char *RillKryRunText(void);
int RillKryRunTextCap(void);
int *RillKryRunCursor(void);
int *RillKryRunFocus(void);
int RillKryRunMatchCount(void);
const char *RillKryRunMatchName(int index);
const char *RillKryRunMatchDescription(int index);
int RillKryRunHistoryCount(void);
const char *RillKryRunHistoryItem(int index);
int RillKryRunLaunchMatch(int index);
int RillKryRunLaunchText(void);
void RillKryRunClose(void);

/* ---- Settings: appearance and desktop ---- */
const char *RillKryThemeName(void);
const char *RillKryFontName(void);
int RillKryWallpaperCount(void);
const char *RillKryWallpaperPath(int index);
const char *RillKryWallpaperCurrent(void);
void RillKryWallpaperApply(const char *path);
void RillKryWallpaperSystem(void);
int RillKryWallpaperSlideshow(void);
void RillKryWallpaperToggleSlideshow(void);
const char *RillKryClockSample(int index);
int RillKryClockFormatIs(int index);
void RillKryClockFormatSet(int index);
int RillKryPanelHeight(void);
void RillKryPanelHeightChange(int delta);

/* ---- Settings: system categories ---- */
const char *RillKrySystemLabel(int index);
int RillKrySystemOpen(int index);
const char *RillKrySettingsError(void);
const char *RillKryDiagnostics(void);

/* ---- Settings: panels.json editor ---- */
int RillKryPanelEntryCount(void);
const char *RillKryPanelEntryId(int index);
const char *RillKryPanelEntryOutput(int index);
int RillKryPanelEntryEdge(int index);
int RillKryPanelEntrySize(int index);
int RillKryPanelEntryAutohide(int index);
int RillKryPanelEntryDeskbar(int index);
void RillKryPanelEntryCycleEdge(int index);
void RillKryPanelEntrySizeChange(int index, int delta);
void RillKryPanelEntryToggleAutohide(int index);
void RillKryPanelEntryToggleDeskbar(int index);
void RillKryPanelEntryRemove(int index);
void RillKryPanelEntryAdd(void);
int RillKryPanelOutputCount(void);
const char *RillKryPanelOutputName(int index);
void RillKryPanelEntryCycleOutput(int index);
void RillKryPanelsSave(void);
void RillKryPanelsImportXfce(void);
const char *RillKryPanelError(void);

/* ---- Settings: display, input and shortcuts ---- */
int RillKryDisplayOutputCount(void);
const char *RillKryDisplayOutputName(int index);
int RillKryDisplayModeCount(int output_index);
const char *RillKryDisplayModeLabel(int index);
void RillKryDisplayApply(int output_index, int mode_index);
int RillKryDisplayConfirmRemaining(void);
void RillKryDisplayKeep(void);
void RillKryDisplayRevert(void);
const char *RillKryDisplayError(void);
int RillKryKeyboardDelay(void);
int RillKryKeyboardRate(void);
void RillKryKeyboardChange(int delay_delta, int rate_delta);
int RillKryMouseNumerator(void);
int RillKryMouseDenominator(void);
int RillKryMouseThreshold(void);
void RillKryMouseChange(int numerator_delta, int denominator_delta,
                        int threshold_delta);
int RillKryWmActionCount(void);
const char *RillKryWmActionName(int index);
const char *RillKryWmActionBinding(int index);
void RillKryWmCaptureBegin(int index);
int RillKryWmCaptureUpdate(void);
void RillKryWmKeysSave(void);
void RillKryWmKeysReset(void);
const char *RillKryInputError(void);
/* "edge" or "edge, output" for panel placement text. */
const char *RillKryTextJoin(const char *edge, const char *output);

#ifdef __cplusplus
}
#endif

#endif
