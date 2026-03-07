#ifndef GUI_H
#define GUI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* -------------------------------------------------------------------------
 * Control IDs
 * ---------------------------------------------------------------------- */
#define IDC_INPUT        101   /* Multiline input edit (paste from Excel)  */
#define IDC_OUTPUT       102   /* Multiline output edit (read-only)         */
#define IDC_BTN_CLEAN    104   /* Primary "Clean" button                    */
#define IDC_BTN_COPY     105   /* "Copy Output" button                      */
#define IDC_BTN_CLEAR    106   /* "Clear" button                            */
#define IDC_STATUS       107   /* Status bar at bottom                      */

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/* Register window class and create the main window.
 * Returns the window handle, or NULL on failure. */
HWND gui_create_window(HINSTANCE hInst);

/* Standard Win32 message loop. Returns exit code. */
int gui_run(void);

/* Window procedure — exported so WinMain can register it */
LRESULT CALLBACK gui_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

#endif /* GUI_H */
