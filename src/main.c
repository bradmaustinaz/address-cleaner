/*
 * Address Cleaner — Win32 GUI application entry point.
 *
 * Workflow:
 *   1. Paste tab-separated rows from Excel into the left pane.
 *   2. Click "Clean". Cleaned rows appear in the right pane.
 *   3. Click "Copy" and paste back into Excel.
 *
 * Cleaning pipeline (rules.c → names.c → llm.c):
 *   - Strip trust language (Living Trust, Revocable Trust, Trustee, etc.)
 *   - Strip date clauses (DATED, DTD, U/D/T, ...)
 *   - Strip Et Al / Et Ux / Et Vir / C/O prefix
 *   - Title-case with LLC/INC/LP etc. preserved uppercase
 *   - Optional AI pass for edge cases (reversed names, typos, initials)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include "gui.h"
#include "llm.h"
#include "setup.h"
#include "splash.h"
#include "update.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR cmdLine, int nShow)
{
    (void)hPrev;
    (void)cmdLine;
    (void)nShow;

    /* Initialize common controls (status bar, etc.) */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    /* Load logo before creating the main window so WM_CREATE can fetch it */
    splash_load_logo();

    HWND hwnd = gui_create_window(hInst);
    if (!hwnd) {
        MessageBox(NULL,
                   "Failed to create main window.",
                   "Name Cleaner", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Download AI dependencies if missing (asks user first) */
    if (setup_needed())
        setup_run(hInst);

    /* Start LLM sidecar in background (no-op if llama-server.exe not found) */
    llm_init();

    /* If the sidecar is loading, show a splash screen while it starts up.
     * If no model was found, llm_init() already set state to -1 so we skip. */
    if (llm_is_loading()) {
        splash_run(hInst);
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    /* Check for a newer GitHub release in the background.
     * Posts WM_APP_UPDATE_AVAIL to hwnd if an update is found; the main
     * message loop (gui_run) handles it.  Silent on any network error. */
    update_check_async(hwnd);

    return gui_run();
}
