#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include "splash.h"
#include "llm.h"

/* =========================================================================
 * Layout constants
 *
 * Logo file:  config\logo.png  (next to the exe)
 * Recommended logo size: 621 × 100 px (any size is accepted and scaled to
 * fit within 360 × 60 px while preserving aspect ratio).
 * PNG transparency is composited onto white before display.
 *
 * Without logo  →  400 × 150 px splash
 * With logo     →  400 × 230 px splash (logo at top, controls shifted down)
 * ====================================================================== */

#define SPLASH_W          400
#define SPLASH_H_BASE     150
#define SPLASH_H_LOGO     230
#define LOGO_Y            10      /* top margin for logo                    */
#define LOGO_MAX_W        360     /* max display width  (px)                */
#define LOGO_MAX_H        60      /* max display height (px)                */
#define LOGO_OFFSET       80      /* extra vertical shift of controls       */

#define SPLASH_CLASS      "NameCleanSplash"
#define SPLASH_TIMER_ID   1
#define SPLASH_TIMER_MS   500          /* poll interval (ms)                */
#define SPLASH_TIMEOUT_TICKS  360      /* 360 × 500ms = 180s                */
#define SPLASH_CLOSE_DELAY_MS 2000     /* show failure text for 2s          */

#define IDC_SPLASH_LABEL  201
#define IDC_SPLASH_SUB    202
#define IDC_SPLASH_PROG   203

/* =========================================================================
 * GDI+ flat-API via dynamic loading
 *
 * We never link against gdiplus.lib; we resolve everything at runtime so
 * the binary has zero additional link-time dependencies.
 * ====================================================================== */

typedef struct {
    UINT32    GdiplusVersion;
    ULONG_PTR DebugEventCallback;
    BOOL      SuppressBackgroundThread;
    BOOL      SuppressExternalCodecs;
} GdipStartupInput;

typedef void *GpImage;

typedef int (WINAPI *PFN_GdiplusStartup)         (ULONG_PTR*, const GdipStartupInput*, void*);
typedef void(WINAPI *PFN_GdiplusShutdown)         (ULONG_PTR);
typedef int (WINAPI *PFN_GdipLoadImageFromFile)   (const WCHAR*, GpImage*);
typedef int (WINAPI *PFN_GdipGetImageWidth)       (GpImage, UINT*);
typedef int (WINAPI *PFN_GdipGetImageHeight)      (GpImage, UINT*);
typedef int (WINAPI *PFN_GdipCreateHBITMAPFromBitmap)(GpImage, HBITMAP*, DWORD);
typedef int (WINAPI *PFN_GdipDisposeImage)        (GpImage);

/* =========================================================================
 * State
 * ====================================================================== */

static HWND  g_hLabel    = NULL;
static HWND  g_hSub      = NULL;
static HWND  g_hProg     = NULL;
static HFONT g_hBoldFont = NULL;
static int   g_ticks     = 0;
static int   g_closing   = 0;

static HBITMAP g_hLogo       = NULL; /* composited on white — for splash      */
static HBITMAP g_hLogoWindow = NULL; /* composited on COLOR_BTNFACE — for GUI */
static int     g_logo_w  = 0;        /* original pixel dims of the loaded PNG */
static int     g_logo_h  = 0;
static int     g_logo_off = 0;       /* LOGO_OFFSET when logo present, else 0 */
static int     g_splash_h = SPLASH_H_BASE;

/* =========================================================================
 * Logo loader  (PNG via GDI+, fully dynamic — no gdiplus.lib needed)
 * ====================================================================== */

void splash_load_logo(void)
{
    if (g_hLogo) return;  /* already loaded — idempotent */

    /* Build path: <exe_dir>config\logo.png */
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *sep = strrchr(dir, '\\');
    if (sep) sep[1] = '\0'; else dir[0] = '\0';

    char path_a[MAX_PATH];
    snprintf(path_a, sizeof(path_a), "%sconfig\\logo.png", dir);

    if (GetFileAttributesA(path_a) == INVALID_FILE_ATTRIBUTES)
        return;  /* no config\logo.png — that's fine */

    /* Convert path to wide for GDI+ */
    WCHAR path_w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path_a, -1, path_w, MAX_PATH);

    /* Load gdiplus.dll dynamically */
    HMODULE hGdip = LoadLibraryA("gdiplus.dll");
    if (!hGdip) return;

    /* Cast through void* to avoid -Wcast-function-type on FARPROC → typed ptr */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    PFN_GdiplusStartup          pfnStart  = (PFN_GdiplusStartup)         GetProcAddress(hGdip, "GdiplusStartup");
    PFN_GdiplusShutdown         pfnStop   = (PFN_GdiplusShutdown)        GetProcAddress(hGdip, "GdiplusShutdown");
    PFN_GdipLoadImageFromFile   pfnLoad   = (PFN_GdipLoadImageFromFile)  GetProcAddress(hGdip, "GdipLoadImageFromFile");
    PFN_GdipGetImageWidth       pfnW      = (PFN_GdipGetImageWidth)      GetProcAddress(hGdip, "GdipGetImageWidth");
    PFN_GdipGetImageHeight      pfnH      = (PFN_GdipGetImageHeight)     GetProcAddress(hGdip, "GdipGetImageHeight");
    PFN_GdipCreateHBITMAPFromBitmap pfnBmp = (PFN_GdipCreateHBITMAPFromBitmap)GetProcAddress(hGdip, "GdipCreateHBITMAPFromBitmap");
    PFN_GdipDisposeImage        pfnFree   = (PFN_GdipDisposeImage)       GetProcAddress(hGdip, "GdipDisposeImage");
#pragma GCC diagnostic pop

    if (!pfnStart || !pfnStop || !pfnLoad || !pfnW || !pfnH || !pfnBmp || !pfnFree) {
        FreeLibrary(hGdip);
        return;
    }

    ULONG_PTR token = 0;
    GdipStartupInput si = { 1, 0, FALSE, FALSE };
    if (pfnStart(&token, &si, NULL) != 0) { FreeLibrary(hGdip); return; }

    GpImage *img = NULL;
    if (pfnLoad(path_w, (void**)&img) != 0) { pfnStop(token); FreeLibrary(hGdip); return; }

    UINT iw = 0, ih = 0;
    pfnW(img, &iw);
    pfnH(img, &ih);

    /* Create splash version: composited onto white */
    HBITMAP hbmp_splash = NULL;
    int ok_splash = pfnBmp(img, &hbmp_splash, 0xFFFFFFFF) == 0;

    /* Create main-window version: composited onto COLOR_BTNFACE */
    COLORREF cr = GetSysColor(COLOR_BTNFACE);
    DWORD gdip_bg = 0xFF000000
                  | ((DWORD)GetRValue(cr) << 16)
                  | ((DWORD)GetGValue(cr) <<  8)
                  |  (DWORD)GetBValue(cr);
    HBITMAP hbmp_window = NULL;
    int ok_window = pfnBmp(img, &hbmp_window, gdip_bg) == 0;

    pfnFree(img);
    pfnStop(token);
    FreeLibrary(hGdip);

    if ((ok_splash || ok_window) && iw > 0 && ih > 0) {
        g_hLogo       = ok_splash ? hbmp_splash : NULL;
        g_hLogoWindow = ok_window ? hbmp_window : NULL;
        g_logo_w  = (int)iw;
        g_logo_h  = (int)ih;
        g_logo_off = LOGO_OFFSET;
        g_splash_h = SPLASH_H_LOGO;
    }
}

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

static void do_close(HWND hwnd)
{
    KillTimer(hwnd, SPLASH_TIMER_ID);
    DestroyWindow(hwnd);
    PostQuitMessage(0);
}

static void show_failure(HWND hwnd, const char *line1)
{
    SendMessage(g_hProg, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(g_hProg, SW_HIDE);
    SetWindowTextA(g_hLabel, line1);
    SetWindowTextA(g_hSub,   "Using rules-only mode.");
    g_closing = 1;
    KillTimer(hwnd, SPLASH_TIMER_ID);
    SetTimer(hwnd, SPLASH_TIMER_ID, SPLASH_CLOSE_DELAY_MS, NULL);
}

/* =========================================================================
 * Window procedure
 * ====================================================================== */

static LRESULT CALLBACK splash_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        /* Bold font for main label */
        NONCLIENTMETRICSA ncm;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfWeight = FW_BOLD;
        ncm.lfMessageFont.lfHeight = -16;
        g_hBoldFont = CreateFontIndirectA(&ncm.lfMessageFont);

        HFONT hGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int off = g_logo_off;  /* 0 if no logo, LOGO_OFFSET if logo */

        g_hLabel = CreateWindowExA(0, "STATIC", "Loading AI model...",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            10, 18 + off, SPLASH_W - 20, 28,
            hwnd, (HMENU)IDC_SPLASH_LABEL, NULL, NULL);

        g_hSub = CreateWindowExA(0, "STATIC",
            "This may take up to 2 minutes on first run",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            10, 54 + off, SPLASH_W - 20, 20,
            hwnd, (HMENU)IDC_SPLASH_SUB, NULL, NULL);

        g_hProg = CreateWindowExA(0, PROGRESS_CLASS, NULL,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            20, 86 + off, SPLASH_W - 40, 18,
            hwnd, (HMENU)IDC_SPLASH_PROG, NULL, NULL);

        SendMessage(g_hLabel, WM_SETFONT, (WPARAM)g_hBoldFont, FALSE);
        SendMessage(g_hSub,   WM_SETFONT, (WPARAM)hGui,        FALSE);
        SendMessage(g_hProg,  PBM_SETMARQUEE, TRUE, 40);

        SetTimer(hwnd, SPLASH_TIMER_ID, SPLASH_TIMER_MS, NULL);
        return 0;
    }

    case WM_TIMER:
        if (wp != SPLASH_TIMER_ID) break;

        if (g_closing) {
            do_close(hwnd);
            return 0;
        }
        if (llm_is_ready()) {
            do_close(hwnd);
            return 0;
        }
        g_ticks++;
        if (!llm_is_loading()) {
            show_failure(hwnd, "AI unavailable");
        } else if (g_ticks >= SPLASH_TIMEOUT_TICKS) {
            show_failure(hwnd, "AI timed out");
        }
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

        /* Draw logo if loaded */
        if (g_hLogo) {
            /* Scale to fit within LOGO_MAX_W × LOGO_MAX_H, preserving ratio */
            int dw = g_logo_w, dh = g_logo_h;
            if (dw > LOGO_MAX_W) { dh = MulDiv(dh, LOGO_MAX_W, dw); dw = LOGO_MAX_W; }
            if (dh > LOGO_MAX_H) { dw = MulDiv(dw, LOGO_MAX_H, dh); dh = LOGO_MAX_H; }
            int lx = (SPLASH_W - dw) / 2;
            int ly = LOGO_Y + (LOGO_MAX_H - dh) / 2;  /* vertically center within reserved area */

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hLogo);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, lx, ly, dw, dh,
                       hdcMem, 0, 0, g_logo_w, g_logo_h, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        return 1;
    }

    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);

    case WM_SYSCOMMAND:
        /* Block close button while loading */
        if ((wp & 0xFFF0) == SC_CLOSE) return 0;
        break;

    case WM_DESTROY:
        if (g_hBoldFont) { DeleteObject(g_hBoldFont); g_hBoldFont = NULL; }
        /* g_hLogo is intentionally NOT deleted here — the main window reuses it */
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

HBITMAP splash_get_logo(int *out_w, int *out_h)
{
    if (out_w) *out_w = g_logo_w;
    if (out_h) *out_h = g_logo_h;
    return g_hLogo;
}

HBITMAP splash_get_logo_for_window(int *out_w, int *out_h)
{
    if (out_w) *out_w = g_logo_w;
    if (out_h) *out_h = g_logo_h;
    return g_hLogoWindow ? g_hLogoWindow : g_hLogo;
}

void splash_run(HINSTANCE hInst)
{
    /* Check for config\logo.png and load it if present (idempotent) */
    splash_load_logo();

    /* Register window class */
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = splash_wnd_proc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = SPLASH_CLASS;
    RegisterClassExA(&wc);

    /* Center on primary monitor */
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x  = (sx - SPLASH_W)    / 2;
    int y  = (sy - g_splash_h)  / 2;

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST,
        SPLASH_CLASS, "Name Cleaner - Starting AI",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, SPLASH_W, g_splash_h,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* Run modal message loop until splash destroys itself */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
