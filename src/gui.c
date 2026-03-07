#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "tsv.h"
#include "names.h"
#include "rules.h"
#include "llm.h"
#include "slog.h"
#include "splash.h"

/* =========================================================================
 * Layout constants
 * ====================================================================== */
#define MARGIN      8
#define TOOLBAR_H   38
#define BTN_H       26
#define BTN_W       88

#define CLASS_NAME    "NameCleanWnd"
#define WINDOW_TITLE  "Name Cleaner"

/* =========================================================================
 * Globals
 * ====================================================================== */
static HWND g_hwnd;
static HWND g_hInput, g_hOutput;
static HWND g_hBtnClean, g_hBtnCopy, g_hBtnClear;
static HWND g_hStatus;
static HFONT g_hGuiFont, g_hMonoFont;

/* Logo (optional — loaded from config\logo.png by splash_load_logo) */
static HBITMAP g_hLogoGui  = NULL;
static int     g_logo_src_w = 0;
static int     g_logo_src_h = 0;

/* =========================================================================
 * Helpers
 * ====================================================================== */

#define AI_STATUS_W  130   /* width of the right AI-status panel in pixels */
#define TIMER_AI_ID    1   /* WM_TIMER id for AI status polling             */

static void set_status(const char *msg)
{
    /* Part 0: main message text */
    SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)msg);
}

static void update_ai_status(void)
{
    /* Part 1: AI status */
    SendMessage(g_hStatus, SB_SETTEXT, 1, (LPARAM)llm_status_str());
}

static void setup_status_parts(void)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    /* Part 0 gets everything except the last AI_STATUS_W pixels;
     * part 1 (value -1) stretches to fill the rest. */
    INT parts[2] = { rc.right - AI_STATUS_W, -1 };
    SendMessage(g_hStatus, SB_SETPARTS, 2, (LPARAM)parts);
    update_ai_status();
}

/* =========================================================================
 * Core clean operation
 * ====================================================================== */

static void do_clean(void)
{

    /* --- Get input text ------------------------------------------------ */
    int input_len = GetWindowTextLength(g_hInput);
    if (input_len <= 0) {
        set_status("Nothing to clean — paste names in the left pane.");
        return;
    }

    char *input = malloc((size_t)input_len + 2);
    if (!input) { set_status("Out of memory."); return; }
    GetWindowText(g_hInput, input, input_len + 1);

    /* --- Allocate output buffer ---------------------------------------- */
    size_t out_cap = (size_t)input_len * 2 + 1024;
    char  *out_buf = malloc(out_cap);
    if (!out_buf) { free(input); set_status("Out of memory."); return; }
    size_t out_pos = 0;

    /* --- Open debug log (DEBUG builds only) ---------------------------- */
#ifdef DEBUG
    {
        char log_dir[MAX_PATH];
        GetModuleFileNameA(NULL, log_dir, MAX_PATH);
        char *sep = strrchr(log_dir, '\\');
        if (sep) *(sep + 1) = '\0'; else log_dir[0] = '\0';
        char log_path[MAX_PATH];
        snprintf(log_path, sizeof(log_path), "%snameclean_debug.log", log_dir);
        rules_debug_open(log_path);
    }
#endif

    /* --- Open session log (always on) ---------------------------------- */
    slog_open();

    /* --- Process line by line ------------------------------------------ */
    int n_cleaned = 0;
    int n_flagged = 0;
    int n_trust   = 0;

    char *line = input;
    while (line && *line) {
        /* Find end of line */
        char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        char saved = *eol;
        if (*eol) *eol = '\0';

        if (*line == '\0') goto next_line;

        /* Parse TSV row and clean field 0 */
        TsvRow row;
        tsv_parse_row(line, &row);

        NameResult nr;
        name_clean(tsv_field(&row, 0), &nr);
        slog_row(&nr, nr.ai_input[0] ? nr.ai_input : NULL,
                      nr.ai_output[0] ? nr.ai_output : NULL);

        /* Grow output buffer if needed */
        size_t needed = out_pos + (size_t)input_len + 512;
        if (needed > out_cap) {
            out_cap = needed * 2;
            char *tmp = realloc(out_buf, out_cap);
            if (!tmp) { free(out_buf); free(input); set_status("Out of memory."); return; }
            out_buf = tmp;
        }

        /* Write field 0 as cleaned name, pass remaining fields through */
        for (int i = 0; i < row.count; i++) {
            if (i > 0) out_buf[out_pos++] = '\t';
            const char *f = (i == 0) ? nr.cleaned : tsv_field(&row, i);
            size_t fl = strlen(f);
            memcpy(out_buf + out_pos, f, fl);
            out_pos += fl;
        }

        /* Windows line ending for correct Excel paste */
        out_buf[out_pos++] = '\r';
        out_buf[out_pos++] = '\n';

        n_cleaned++;
        if (nr.flags & NAME_FLAG_SUSPICIOUS) n_flagged++;
        if (nr.flags & NAME_FLAG_WAS_TRUST)  n_trust++;

next_line:
        *eol = saved;
        line = eol;
        if (*line == '\r') line++;
        if (*line == '\n') line++;
    }

    out_buf[out_pos] = '\0';
    SetWindowText(g_hOutput, out_buf);

    /* --- Close logs and report ----------------------------------------- */
    slog_close(n_cleaned, n_flagged, n_trust);
#ifdef DEBUG
    rules_debug_close();
#endif

    char status[256];
    snprintf(status, sizeof(status),
#ifdef DEBUG
             "%d rows cleaned  |  %d trusts stripped  |  %d flagged  |  debug log written",
#else
             "%d rows cleaned  |  %d trusts stripped  |  %d flagged for review",
#endif
             n_cleaned, n_trust, n_flagged);
    set_status(status);

    free(out_buf);
    free(input);
}

/* =========================================================================
 * Copy output to clipboard
 * ====================================================================== */

static void copy_to_clipboard(void)
{
    int len = GetWindowTextLength(g_hOutput);
    if (len <= 0) { set_status("Nothing in output to copy."); return; }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len + 1);
    if (!hMem) return;

    char *p = GlobalLock(hMem);
    GetWindowText(g_hOutput, p, len + 1);
    GlobalUnlock(hMem);

    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, hMem);
        CloseClipboard();
        set_status("Output copied to clipboard — paste into Excel.");
    } else {
        GlobalFree(hMem);
        set_status("Could not open clipboard.");
    }
}

/* =========================================================================
 * Layout
 *
 *  ┌─────────────────────────────────────────────────────┐
 *  │                          [Clear] [Copy] [Clean »]  │  toolbar
 *  ├──────────────────────────┬──────────────────────────┤
 *  │  Input (paste here)      │  Output (read-only)      │
 *  │                          │                          │
 *  └──────────────────────────┴──────────────────────────┘
 *  │ status bar                                          │
 * ====================================================================== */

static void layout_controls(void)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right;
    int H = rc.bottom;

    /* Status bar */
    SendMessage(g_hStatus, WM_SIZE, 0, 0);
    setup_status_parts();
    RECT sr;
    GetWindowRect(g_hStatus, &sr);
    int status_h = sr.bottom - sr.top;

    /* Toolbar at top */
    int toolbar_y = MARGIN;
    int btn_y = toolbar_y + (TOOLBAR_H - BTN_H) / 2;

    /* Buttons flush right */
    int bx = W - MARGIN - BTN_W;
    MoveWindow(g_hBtnClean, bx, btn_y, BTN_W, BTN_H, TRUE); bx -= BTN_W + MARGIN;
    MoveWindow(g_hBtnCopy,  bx, btn_y, BTN_W, BTN_H, TRUE); bx -= BTN_W + MARGIN;
    MoveWindow(g_hBtnClear, bx, btn_y, BTN_W, BTN_H, TRUE);

    /* Two side-by-side edit panes below toolbar */
    int pane_y = toolbar_y + TOOLBAR_H + MARGIN;
    int pane_h = H - pane_y - status_h - MARGIN;
    int half_w = (W - MARGIN * 3) / 2;

    MoveWindow(g_hInput,  MARGIN,              pane_y, half_w, pane_h, TRUE);
    MoveWindow(g_hOutput, MARGIN * 2 + half_w, pane_y, half_w, pane_h, TRUE);
}

/* =========================================================================
 * Control creation
 * ====================================================================== */

static void create_controls(HINSTANCE hInst)
{
    g_hGuiFont  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_hMonoFont = CreateFont(
        14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas"
    );
    if (!g_hMonoFont)
        g_hMonoFont = (HFONT)GetStockObject(ANSI_FIXED_FONT);

    DWORD ed = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL
             | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL;

    g_hInput = CreateWindowEx(
        0, "EDIT", "",
        ed,
        0, 0, 0, 0, g_hwnd, (HMENU)IDC_INPUT, hInst, NULL);

    g_hOutput = CreateWindowEx(
        0, "EDIT", "",
        ed | ES_READONLY,
        0, 0, 0, 0, g_hwnd, (HMENU)IDC_OUTPUT, hInst, NULL);

    SendMessage(g_hInput,  EM_LIMITTEXT, 0, 0);
    SendMessage(g_hOutput, EM_LIMITTEXT, 0, 0);

    g_hBtnClean = CreateWindowEx(
        0, "BUTTON", "Clean  \xBB",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, g_hwnd, (HMENU)IDC_BTN_CLEAN, hInst, NULL);

    g_hBtnCopy = CreateWindowEx(
        0, "BUTTON", "Copy Output",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, g_hwnd, (HMENU)IDC_BTN_COPY, hInst, NULL);

    g_hBtnClear = CreateWindowEx(
        0, "BUTTON", "Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, g_hwnd, (HMENU)IDC_BTN_CLEAR, hInst, NULL);

    g_hStatus = CreateWindowEx(
        0, STATUSCLASSNAME, NULL,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, g_hwnd, (HMENU)IDC_STATUS, hInst, NULL);

    /* Fonts */
    SendMessage(g_hInput,    WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
    SendMessage(g_hOutput,   WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
    SendMessage(g_hBtnClean, WM_SETFONT, (WPARAM)g_hGuiFont,  FALSE);
    SendMessage(g_hBtnCopy,  WM_SETFONT, (WPARAM)g_hGuiFont,  FALSE);
    SendMessage(g_hBtnClear, WM_SETFONT, (WPARAM)g_hGuiFont,  FALSE);

    set_status("Paste names in the left pane, then click Clean.");
    update_ai_status();

    /* Grab logo composited against COLOR_BTNFACE for correct rendering on the toolbar */
    g_hLogoGui = splash_get_logo_for_window(&g_logo_src_w, &g_logo_src_h);
}

/* =========================================================================
 * Window procedure
 * ====================================================================== */

LRESULT CALLBACK gui_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        g_hwnd = hwnd;
        create_controls(((CREATESTRUCT *)lp)->hInstance);
        layout_controls();
        /* Poll AI status every 500 ms until determined */
        SetTimer(hwnd, TIMER_AI_ID, 500, NULL);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_AI_ID) {
            update_ai_status();
            /* Stop polling once the state is final (ready or no model) */
            if (!llm_is_loading())
                KillTimer(hwnd, TIMER_AI_ID);
        }
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) layout_controls();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 520;
        mm->ptMinTrackSize.y = 300;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_CLEAN:  do_clean();          break;
        case IDC_BTN_COPY:   copy_to_clipboard(); break;
        case IDC_BTN_CLEAR:
            SetWindowText(g_hInput,  "");
            SetWindowText(g_hOutput, "");
            set_status("Cleared.");
            break;
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hLogoGui && g_logo_src_w > 0 && g_logo_src_h > 0) {
            /* Scale to fit within toolbar height, max 250px wide */
            int max_h = TOOLBAR_H - 8;
            int max_w = 250;
            int dh = max_h;
            int dw = MulDiv(g_logo_src_w, max_h, g_logo_src_h);
            if (dw > max_w) { dh = MulDiv(dh, max_w, dw); dw = max_w; }
            int lx = MARGIN;
            int ly = MARGIN + (TOOLBAR_H - dh) / 2;

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hLogoGui);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, lx, ly, dw, dh,
                       hdcMem, 0, 0, g_logo_src_w, g_logo_src_h, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_DESTROY:
        llm_shutdown();
        if (g_hMonoFont) DeleteObject(g_hMonoFont);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

HWND gui_create_window(HINSTANCE hInst)
{
    WNDCLASSEX wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = gui_wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wc)) return NULL;

    return CreateWindowEx(
        0, CLASS_NAME, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        900, 560,
        NULL, NULL, hInst, NULL
    );
}

int gui_run(void)
{
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
