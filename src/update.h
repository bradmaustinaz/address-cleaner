#ifndef UPDATE_H
#define UPDATE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* =========================================================================
 * Application version + release repo
 *
 * APP_VERSION must match the tag pushed to GitHub (without the leading 'v').
 * Bump this string on every release so the update checker knows what version
 * the running binary is.
 * ====================================================================== */

#define APP_VERSION    "0.4.4"
#define APP_REPO_OWNER "bradmaustinaz"
#define APP_REPO_NAME  "address-cleaner"
#define APP_EXE_NAME   "nameclean.exe"

/* =========================================================================
 * UpdateInfo
 *
 * Heap-allocated by the background update thread, posted to the main window
 * as lParam of WM_APP_UPDATE_AVAIL.  The main window (gui.c) owns the
 * pointer and must free() it after calling update_show_dialog().
 * ====================================================================== */

typedef struct {
    char tag[64];         /* fetched tag, e.g. "v0.4.0"            */
    char asset_url[512];  /* direct .exe download URL, or "" if none */
} UpdateInfo;

/* WM_APP_UPDATE_AVAIL
 *   Posted to the main HWND when a newer release is found.
 *   wParam = 0
 *   lParam = (LPARAM)(UpdateInfo *)  — caller must free() after use
 */
#define WM_APP_UPDATE_AVAIL  (WM_APP + 20)

/* =========================================================================
 * Public API
 * ====================================================================== */

/*
 * update_check_async: spin a background thread that checks
 * api.github.com/repos/…/releases/latest.  If a newer version is found,
 * posts WM_APP_UPDATE_AVAIL to hwnd.  Returns immediately; all errors are
 * silent (network blocked at work, no internet, etc.).
 */
void update_check_async(HWND hwnd);

/*
 * update_show_dialog: called from gui.c when WM_APP_UPDATE_AVAIL arrives.
 * Shows a modal notification dialog with three choices:
 *   - Download & Install → downloads nameclean.exe, writes a restart batch,
 *     exits; batch relaunches the new binary after this process ends.
 *   - Open Release Page  → opens the GitHub releases page in the browser.
 *   - Skip               → dismisses the dialog, app continues normally.
 * Blocks until the user makes a choice (nested message loop).
 */
void update_show_dialog(HWND parent, UpdateInfo *info);

#endif /* UPDATE_H */
