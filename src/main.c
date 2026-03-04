/*
 * Name Cleaner — Win32 GUI application entry point.
 *
 * Workflow:
 *   1. Paste tab-separated rows from Excel into the top pane.
 *   2. Set "Name col" to the 0-based column index containing owner names.
 *   3. Click "Clean". Cleaned rows appear in the bottom pane.
 *   4. Click "Copy Output" and paste back into Excel.
 *
 * Cleaning pipeline (rules.c):
 *   - Strip trust language (Living Trust, Revocable Trust, Trustee, etc.)
 *   - Strip date clauses (DATED, DTD, U/D/T, ...)
 *   - Strip Et Al / Et Ux / Et Vir
 *   - Strip C/O prefix
 *   - Title-case with LLC/INC/LP etc. preserved uppercase
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include "gui.h"
#include "llm.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR cmdLine, int nShow)
{
    (void)hPrev;
    (void)cmdLine;
    (void)nShow;

    /* Initialize common controls (status bar, etc.) */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    HWND hwnd = gui_create_window(hInst);
    if (!hwnd) {
        MessageBox(NULL,
                   "Failed to create main window.",
                   "Name Cleaner", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Start LLM sidecar in background (no-op if llama-server.exe not found) */
    llm_init();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    return gui_run();
}
