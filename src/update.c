#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"

/* =========================================================================
 * Internal WM_APP messages (download progress window only)
 * ====================================================================== */

#define WM_UPD_PROGRESS  (WM_APP + 21) /* wParam = percent 0-100           */
#define WM_UPD_DONE      (WM_APP + 22) /* wParam = 1 success / 0 failure   */

/* =========================================================================
 * Window / control IDs
 * ====================================================================== */

#define NOTIF_CLASS  "NameCleanUpdater"
#define DL_CLASS     "NameCleanDL"
#define NOTIF_W      430
#define NOTIF_H      175
#define DL_W         360
#define DL_H         120

#define IDC_UPD_DL    501
#define IDC_UPD_WEB   502
#define IDC_UPD_SKIP  503
#define IDC_DL_CANCEL 504
#define IDC_DL_PROG   505
#define IDC_DL_LABEL  506

/* =========================================================================
 * Shared state between notification dialog and download thread
 * ====================================================================== */

static volatile LONG s_upd_choice   = 0;   /* set by notif WM_COMMAND     */
static volatile LONG s_dl_cancel    = 0;   /* set by DL cancel button      */
static char          s_dl_url[512]  = {0};
static char          s_dl_dest[MAX_PATH + 32] = {0};

/* =========================================================================
 * Helpers: get exe directory (same logic as llm.c / setup.c)
 * ====================================================================== */

static void get_exe_dir(char *buf, size_t bufsz)
{
    GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
    char *sep = strrchr(buf, '\\');
    if (sep) sep[1] = '\0';
    else     buf[0] = '\0';
}

/* =========================================================================
 * Minimal JSON string extractor (adapted from llm.c)
 *
 * Finds the first occurrence of "key":"<value>" in json and writes the
 * unescaped value into out (NUL-terminated).  Returns 1 on success.
 * ====================================================================== */

static int extract_json_str(const char *json, const char *key,
                             char *out, size_t outsz)
{
    if (!json || !key || !out || !outsz) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && i < outsz - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 'r':  out[i++] = '\r'; break;
            case 't':  out[i++] = '\t'; break;
            default:   out[i++] = *p;   break;
            }
        } else if (*p == '"') {
            break;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

/* Find the first occurrence of substring "needle" in the release JSON that
 * contains ".exe" and looks like a browser_download_url value.
 * Writes the URL into out. Returns 1 on success. */
static int extract_exe_asset_url(const char *json, char *out, size_t outsz)
{
    /* Search for each occurrence of browser_download_url */
    const char *p = json;
    while ((p = strstr(p, "\"browser_download_url\":")) != NULL) {
        p += strlen("\"browser_download_url\":");
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') continue;
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        size_t urllen = (size_t)(p - start);
        /* Check if the URL ends with .exe (case-insensitive last 4 chars) */
        if (urllen >= 4 && urllen < outsz) {
            const char *end4 = start + urllen - 4;
            if (_strnicmp(end4, ".exe", 4) == 0) {
                memcpy(out, start, urllen);
                out[urllen] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Version comparison
 *
 * Strips leading 'v', parses major.minor.patch (stops at '-' or '\0').
 * Returns 1 if fetched_tag is strictly newer than current.
 * Special case: if numeric parts are equal AND current has a pre-release
 * suffix ('-alpha'/'-beta'/'-rc') but fetched doesn't, fetched counts as
 * newer (stable release beats pre-release with same number).
 * ====================================================================== */

static void parse_ver(const char *s, int *maj, int *min, int *pat, int *prerel)
{
    if (*s == 'v' || *s == 'V') s++;
    *maj = *min = *pat = 0;
    *prerel = 0;
    sscanf(s, "%d.%d.%d", maj, min, pat);
    /* pre-release suffix? */
    while (*s && *s != '-') s++;
    *prerel = (*s == '-') ? 1 : 0;
}

static int is_newer_version(const char *current, const char *fetched_tag)
{
    int cmaj, cmin, cpat, cpre;
    int fmaj, fmin, fpat, fpre;
    parse_ver(current,     &cmaj, &cmin, &cpat, &cpre);
    parse_ver(fetched_tag, &fmaj, &fmin, &fpat, &fpre);

    long cv = cmaj * 10000L + cmin * 100L + cpat;
    long fv = fmaj * 10000L + fmin * 100L + fpat;

    if (fv > cv) return 1;
    if (fv < cv) return 0;
    /* Same numeric version: stable beats pre-release */
    return (cpre && !fpre) ? 1 : 0;
}

/* =========================================================================
 * HTTPS fetch to GitHub releases API
 * ====================================================================== */

static int fetch_latest_release(char *tag_out, size_t tag_sz,
                                 char *asset_url_out, size_t asset_sz)
{
    int ok = 0;
    tag_out[0]       = '\0';
    asset_url_out[0] = '\0';

    HINTERNET hSession = WinHttpOpen(
        L"nameclean/" APP_VERSION L" (Windows)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    /* Short timeouts — if the API is blocked we fail fast */
    WinHttpSetTimeouts(hSession, 8000, 8000, 15000, 15000);

    HINTERNET hConn = WinHttpConnect(hSession, L"api.github.com",
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSession); return 0; }

    wchar_t path[256];
    _snwprintf(path, 256,
               L"/repos/" APP_REPO_OWNER L"/" APP_REPO_NAME L"/releases/latest",
               (void *)0);
    /* The macro expansions produce wide string literals; use directly: */
    HINTERNET hReq = WinHttpOpenRequest(
        hConn, L"GET",
        L"/repos/" L"" APP_REPO_OWNER L"/" L"" APP_REPO_NAME L"/releases/latest",
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) goto cleanup_conn;

    /* GitHub API requires Accept + User-Agent headers */
    static const WCHAR *hdrs =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(hReq, hdrs, (DWORD)-1L, NULL, 0, 0, 0))
        goto cleanup_req;
    if (!WinHttpReceiveResponse(hReq, NULL))
        goto cleanup_req;

    /* Check status code (200 = found, 404 = no releases yet) */
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) goto cleanup_req;

    /* Read response body (GitHub JSON is typically 3–15 KB) */
    char *body = malloc(65536);
    if (!body) goto cleanup_req;
    size_t pos = 0;
    DWORD n;
    while (WinHttpReadData(hReq, body + pos,
                           (DWORD)(65535 - pos), &n) && n > 0)
        pos += n;
    body[pos] = '\0';

    /* Parse tag_name */
    if (!extract_json_str(body, "tag_name", tag_out, tag_sz)) {
        free(body);
        goto cleanup_req;
    }
    /* Parse exe asset URL (optional — "" if no binary attached yet) */
    extract_exe_asset_url(body, asset_url_out, asset_sz);
    free(body);
    ok = 1;

cleanup_req:
    WinHttpCloseHandle(hReq);
cleanup_conn:
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return ok;
}

/* =========================================================================
 * Background update check thread
 * ====================================================================== */

static DWORD WINAPI update_thread(LPVOID param)
{
    HWND hwnd = (HWND)param;
    char tag[64]        = {0};
    char asset_url[512] = {0};

    if (!fetch_latest_release(tag, sizeof(tag), asset_url, sizeof(asset_url)))
        return 0;

    if (!is_newer_version(APP_VERSION, tag))
        return 0;

    UpdateInfo *info = (UpdateInfo *)malloc(sizeof(UpdateInfo));
    if (!info) return 0;
    snprintf(info->tag,       sizeof(info->tag),       "%s", tag);
    snprintf(info->asset_url, sizeof(info->asset_url), "%s", asset_url);

    PostMessage(hwnd, WM_APP_UPDATE_AVAIL, 0, (LPARAM)info);
    return 0;
}

void update_check_async(HWND hwnd)
{
    HANDLE t = CreateThread(NULL, 0, update_thread, (LPVOID)hwnd, 0, NULL);
    if (t) CloseHandle(t);
}

/* =========================================================================
 * Download progress window
 * ====================================================================== */

static LRESULT CALLBACK dl_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExA(0, "STATIC",
            "Downloading update\x85",      /* \x85 = CP1252 ellipsis */
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            14, 14, DL_W - 32, 20,
            hwnd, (HMENU)IDC_DL_LABEL, NULL, NULL);
        CreateWindowExA(0, PROGRESS_CLASSA, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            14, 42, DL_W - 32, 18,
            hwnd, (HMENU)IDC_DL_PROG, NULL, NULL);
        HWND hprog = GetDlgItem(hwnd, IDC_DL_PROG);
        SendMessageA(hprog, PBM_SETRANGE32, 0, 100);
        CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            DL_W / 2 - 40, 72, 80, 28,
            hwnd, (HMENU)IDC_DL_CANCEL, NULL, NULL);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_DL_CANCEL)
            InterlockedExchange(&s_dl_cancel, 1);
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&s_dl_cancel, 1);
        return 0;
    case WM_UPD_PROGRESS:
        SendDlgItemMessageA(hwnd, IDC_DL_PROG, PBM_SETPOS, wp, 0);
        return 0;
    case WM_UPD_DONE:
        DestroyWindow(hwnd);
        PostQuitMessage((int)wp); /* 1 = success, 0 = fail/cancel */
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* =========================================================================
 * Download background thread
 *
 * Downloads s_dl_url to s_dl_dest, posting WM_UPD_PROGRESS to hwnd.
 * Posts WM_UPD_DONE(1) on success, WM_UPD_DONE(0) on failure/cancel.
 * ====================================================================== */

typedef struct { HWND hwnd; } DlThreadParam;

static DWORD WINAPI dl_thread(LPVOID param)
{
    DlThreadParam *dp = (DlThreadParam *)param;
    HWND hwnd = dp->hwnd;
    free(dp);

    int ok = 0;

    /* Parse URL into host + path for WinHTTP */
    /* asset URLs are always https://github.com/... or similar */
    char host[256] = {0};
    char path_part[512] = {0};
    const char *url = s_dl_url;
    int use_ssl = 0;

    /* Strip scheme */
    if (_strnicmp(url, "https://", 8) == 0) {
        url += 8;
        use_ssl = 1;
    } else if (_strnicmp(url, "http://", 7) == 0) {
        url += 7;
    }
    /* Split host / path */
    const char *slash = strchr(url, '/');
    if (!slash) goto done;
    size_t hlen = (size_t)(slash - url);
    if (hlen >= sizeof(host)) goto done;
    memcpy(host, url, hlen);
    host[hlen] = '\0';
    strncpy(path_part, slash, sizeof(path_part) - 1);

    {
        /* Convert host to wide */
        wchar_t whost[256];
        MultiByteToWideChar(CP_ACP, 0, host, -1, whost, 256);
        wchar_t wpath[512];
        MultiByteToWideChar(CP_ACP, 0, path_part, -1, wpath, 512);

        HINTERNET hSess = WinHttpOpen(
            L"nameclean-updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSess) goto done;
        WinHttpSetTimeouts(hSess, 10000, 10000, 30000, 60000);

        HINTERNET hConn = WinHttpConnect(hSess, whost,
            use_ssl ? INTERNET_DEFAULT_HTTPS_PORT
                    : INTERNET_DEFAULT_HTTP_PORT, 0);
        if (!hConn) { WinHttpCloseHandle(hSess); goto done; }

        DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wpath,
            NULL, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); goto done; }

        /* GitHub release redirects — follow up to 10 redirects */
        DWORD opt = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &opt, sizeof(opt));

        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                NULL, 0, 0, 0) ||
            !WinHttpReceiveResponse(hReq, NULL)) {
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConn);
            WinHttpCloseHandle(hSess);
            goto done;
        }

        /* Content-Length for progress */
        DWORD content_len = 0;
        DWORD cl_sz = sizeof(content_len);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &content_len, &cl_sz, WINHTTP_NO_HEADER_INDEX);

        HANDLE hFile = CreateFileA(s_dl_dest, GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConn);
            WinHttpCloseHandle(hSess);
            goto done;
        }

        char chunk[16384];
        DWORD n, total = 0;
        ok = 1;
        while (WinHttpReadData(hReq, chunk, sizeof(chunk), &n) && n > 0) {
            if (s_dl_cancel) { ok = 0; break; }
            DWORD written;
            if (!WriteFile(hFile, chunk, n, &written, NULL) || written != n)
                { ok = 0; break; }
            total += n;
            if (content_len > 0 && hwnd) {
                int pct = (int)(total * 100UL / content_len);
                PostMessage(hwnd, WM_UPD_PROGRESS, (WPARAM)pct, 0);
            }
        }

        CloseHandle(hFile);
        if (!ok) DeleteFileA(s_dl_dest); /* remove partial download */

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
    }

done:
    if (hwnd)
        PostMessage(hwnd, WM_UPD_DONE, (WPARAM)ok, 0);
    return 0;
}

/* =========================================================================
 * Batch-script restart helper
 *
 * Writes a small .bat to a local tmp\ folder that waits for this process
 * to exit, moves nameclean_update.exe over nameclean.exe, then relaunches.
 * ====================================================================== */

static void write_restart_batch(const char *update_path,
                                 const char *final_path,
                                 char *bat_out, size_t bat_sz)
{
    char temp_dir[MAX_PATH];
    get_exe_dir(temp_dir, sizeof(temp_dir));
    size_t len = strlen(temp_dir);
    snprintf(temp_dir + len, sizeof(temp_dir) - len, "tmp\\");
    CreateDirectoryA(temp_dir, NULL);
    snprintf(bat_out, bat_sz, "%snameclean_upd.bat", temp_dir);

    FILE *f = fopen(bat_out, "w");
    if (!f) { bat_out[0] = '\0'; return; }
    fprintf(f,
        "@echo off\r\n"
        "ping -n 3 127.0.0.1 > nul\r\n"
        "move /y \"%s\" \"%s\"\r\n"
        "start \"\" \"%s\"\r\n"
        "del \"%%~f0\"\r\n",
        update_path, final_path, final_path);
    fclose(f);
}

/* =========================================================================
 * do_download_and_install
 *
 * Shows a progress window, downloads the update, writes the restart batch,
 * then posts WM_CLOSE to parent to trigger a graceful app exit.
 * ====================================================================== */

static void do_download_and_install(HWND parent, const UpdateInfo *info)
{
    /* Compute paths */
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    char update_dest[MAX_PATH + 32];
    snprintf(update_dest, sizeof(update_dest), "%s" APP_EXE_NAME ".new.exe", exe_dir);

    char final_dest[MAX_PATH + 32];
    snprintf(final_dest, sizeof(final_dest), "%s" APP_EXE_NAME, exe_dir);

    /* Set shared state for download thread */
    snprintf(s_dl_url,  sizeof(s_dl_url),  "%s", info->asset_url);
    snprintf(s_dl_dest, sizeof(s_dl_dest), "%s", update_dest);
    InterlockedExchange(&s_dl_cancel, 0);

    /* Register and create the progress window */
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = dl_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = DL_CLASS;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassExA(&wc); /* ignore if already registered */

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND dlg = CreateWindowExA(WS_EX_TOPMOST, DL_CLASS,
        "Name Cleaner \x97 Downloading Update",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sw - DL_W) / 2, (sh - DL_H) / 2,
        DL_W, DL_H,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    if (!dlg) return;
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    /* Spawn download thread */
    DlThreadParam *dp = (DlThreadParam *)malloc(sizeof(DlThreadParam));
    if (!dp) { DestroyWindow(dlg); return; }
    dp->hwnd = dlg;
    HANDLE t = CreateThread(NULL, 0, dl_thread, dp, 0, NULL);
    if (!t) { free(dp); DestroyWindow(dlg); return; }
    CloseHandle(t);

    /* Modal message loop — exits when DL_CLASS posts WM_UPD_DONE → PostQuitMessage */
    MSG msg;
    int dl_ok = 0;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    /* PostQuitMessage wParam carries success flag */
    dl_ok = (msg.message == WM_QUIT) ? (int)msg.wParam : 0;

    if (!dl_ok || s_dl_cancel) {
        /* Download failed or cancelled */
        int go_web = MessageBoxA(parent,
            "The download failed or was cancelled.\r\n\r\n"
            "Open the release page in your browser instead?",
            "Update", MB_YESNO | MB_ICONINFORMATION);
        if (go_web == IDYES) {
            ShellExecuteA(NULL, "open",
                "https://github.com/" APP_REPO_OWNER
                "/" APP_REPO_NAME "/releases/latest",
                NULL, NULL, SW_SHOWNORMAL);
        }
        return;
    }

    /* Rename update file to final name via batch script */
    char bat_path[MAX_PATH + 32] = {0};
    write_restart_batch(update_dest, final_dest, bat_path, sizeof(bat_path));

    if (bat_path[0]) {
        ShellExecuteA(NULL, "open", bat_path, NULL, NULL, SW_HIDE);
    }

    /* Close the app — batch will relaunch the new binary */
    PostMessageA(parent, WM_CLOSE, 0, 0);
}

/* =========================================================================
 * Notification dialog
 * ====================================================================== */

/* Stored by create param so the WM_COMMAND handler can access asset_url */
static const UpdateInfo *s_notif_info = NULL;

static LRESULT CALLBACK notif_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        /* Compose the message text using the info passed via CreateWindowEx */
        char caption[256];
        snprintf(caption, sizeof(caption),
            "Name Cleaner %s is available  \x96  you have v" APP_VERSION
            "\r\n\r\n"
            "Download and install now, or open the release page for details.",
            s_notif_info ? s_notif_info->tag : "(new version)");

        CreateWindowExA(0, "STATIC", caption,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            14, 14, NOTIF_W - 32, 72,
            hwnd, NULL, NULL, NULL);

        /* Buttons — evenly spaced at bottom */
        int bw = 126, bh = 30, by = NOTIF_H - 50;
        int gap = (NOTIF_W - 3 * bw - 28) / 2;
        int bx = 14;
        CreateWindowExA(0, "BUTTON", "Download && Install",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            bx, by, bw, bh,
            hwnd, (HMENU)IDC_UPD_DL, NULL, NULL);
        bx += bw + gap;
        CreateWindowExA(0, "BUTTON", "Open Release Page",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, by, bw, bh,
            hwnd, (HMENU)IDC_UPD_WEB, NULL, NULL);
        bx += bw + gap;
        CreateWindowExA(0, "BUTTON", "Skip",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, by, bw, bh,
            hwnd, (HMENU)IDC_UPD_SKIP, NULL, NULL);
        return 0;
    }
    case WM_COMMAND:
        InterlockedExchange(&s_upd_choice, (LONG)LOWORD(wp));
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&s_upd_choice, IDC_UPD_SKIP);
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* =========================================================================
 * update_show_dialog — public entry point called from gui.c
 * ====================================================================== */

void update_show_dialog(HWND parent, UpdateInfo *info)
{
    /* Register notification window class (once) */
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = notif_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = NOTIF_CLASS;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassExA(&wc); /* silently fails if already registered — that's fine */

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    s_notif_info = info;
    InterlockedExchange(&s_upd_choice, IDC_UPD_SKIP);

    HWND dlg = CreateWindowExA(WS_EX_TOPMOST, NOTIF_CLASS,
        "Update Available \x97 Name Cleaner",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sw - NOTIF_W) / 2, (sh - NOTIF_H) / 2,
        NOTIF_W, NOTIF_H,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    if (!dlg) return;
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    /* Modal message loop */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Dispatch based on user choice */
    LONG choice = s_upd_choice;
    s_notif_info = NULL;

    if (choice == IDC_UPD_SKIP) {
        return;
    } else if (choice == IDC_UPD_WEB) {
        ShellExecuteA(NULL, "open",
            "https://github.com/" APP_REPO_OWNER
            "/" APP_REPO_NAME "/releases/latest",
            NULL, NULL, SW_SHOWNORMAL);
        return;
    } else if (choice == IDC_UPD_DL) {
        if (info->asset_url[0]) {
            do_download_and_install(parent, info);
        } else {
            /* No binary attached — fall back to browser */
            MessageBoxA(parent,
                "No downloadable installer was found for this release.\r\n\r\n"
                "Opening the release page so you can download it manually.",
                "Update", MB_OK | MB_ICONINFORMATION);
            ShellExecuteA(NULL, "open",
                "https://github.com/" APP_REPO_OWNER
                "/" APP_REPO_NAME "/releases/latest",
                NULL, NULL, SW_SHOWNORMAL);
        }
    }
}
