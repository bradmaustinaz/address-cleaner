#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include "setup.h"

/* =========================================================================
 * Constants
 * ====================================================================== */

#define SETUP_W          460
#define SETUP_WIN_H      180
#define SETUP_CLASS      "NameCleanSetup"

#define IDC_SETUP_LABEL  301
#define IDC_SETUP_SUB    302
#define IDC_SETUP_PROG   303
#define IDC_SETUP_BTN    304

#define WM_SETUP_STATUS     (WM_APP + 10)
#define WM_SETUP_SUBSTATUS  (WM_APP + 11)
#define WM_SETUP_PROGRESS   (WM_APP + 12)
#define WM_SETUP_DONE       (WM_APP + 13)

#define MODEL_URL       "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"
#define MODEL_FILENAME  "qwen2.5-3b-instruct-q4_k_m.gguf"

#define FINETUNE_MODEL_URL \
    "https://huggingface.co/bradmaustinaz/Realty-Mailing-Address-Q4_K_M-GGUF/resolve/main/realty-mailing-address-q4_k_m.gguf"
#define FINETUNE_MODEL_FILENAME  "realty-mailing-address-q4_k_m.gguf"

static const wchar_t GITHUB_RELEASE_URL[] =
    L"https://api.github.com/repos/ggml-org/llama.cpp/releases/latest";

/* =========================================================================
 * Dialog state
 * ====================================================================== */

static HWND          s_hLabel    = NULL;
static HWND          s_hSub      = NULL;
static HWND          s_hProg     = NULL;
static HWND          s_hBtn      = NULL;
static HFONT         s_hBoldFont = NULL;
static volatile LONG s_cancel      = 0;
static volatile LONG s_result      = 0;
static volatile LONG s_skip_model  = 0;
static char          s_substatus[256];
static char          s_chosen_model_url[512]      = {0};
static char          s_chosen_model_filename[256] = {0};

/* =========================================================================
 * File helpers
 * ====================================================================== */

static void setup_get_exe_dir(char *buf, size_t bufsz)
{
    GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
    char *sep = strrchr(buf, '\\');
    if (sep) sep[1] = '\0';
    else     buf[0] = '\0';
}

static void setup_get_ai_dir(char *buf, size_t bufsz)
{
    setup_get_exe_dir(buf, bufsz);
    size_t len = strlen(buf);
    snprintf(buf + len, bufsz - len, "ai\\");
}

static int has_server(void)
{
    char ai[MAX_PATH];
    setup_get_ai_dir(ai, sizeof(ai));
    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "%sllama-server.exe", ai);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int has_model(void)
{
    char ai[MAX_PATH];
    setup_get_ai_dir(ai, sizeof(ai));
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof(pattern), "%s*.gguf", ai);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

/* =========================================================================
 * Model picker dialog — plain Win32, no comctl32 v6 required
 * Returns: 1=fine-tuned, 2=qwen2.5, 3=browse (fills out_path), 0=skip
 * ====================================================================== */

#define PICKER_CLASS   "NameCleanModelPicker"
#define PICKER_W       440
#define PICKER_H       230
#define IDC_PK_FINE    401
#define IDC_PK_QWEN    402
#define IDC_PK_BROWSE  403
#define IDC_PK_SKIP    404

static volatile int s_picker_choice = 0;

static LRESULT CALLBACK picker_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_CREATE: {
        HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int bw = PICKER_W - 40, bh = 34, x = 20, gap = 40;

        HWND hLbl = CreateWindowExA(0, "STATIC",
            "No AI model found. Select an option:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, 14, bw, 20, hwnd, NULL, NULL, NULL);
        SendMessageA(hLbl, WM_SETFONT, (WPARAM)hf, FALSE);

        static const char *labels[] = {
            "Download Fine-Tuned Model (Recommended, ~2 GB)",
            "Download Qwen2.5-3B  (general-purpose, ~2 GB)",
            "Browse for local .gguf file...",
            "Skip  (run without AI)",
        };
        static const int ids[] = {
            IDC_PK_FINE, IDC_PK_QWEN, IDC_PK_BROWSE, IDC_PK_SKIP
        };
        for (int i = 0; i < 4; i++) {
            HWND hb = CreateWindowExA(0, "BUTTON", labels[i],
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x, 44 + i * gap, bw, bh,
                hwnd, (HMENU)(size_t)ids[i], NULL, NULL);
            SendMessageA(hb, WM_SETFONT, (WPARAM)hf, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        s_picker_choice = LOWORD(wp);
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        s_picker_choice = IDC_PK_SKIP;
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int pick_model_source(char *out_path, size_t out_path_sz)
{
    s_picker_choice = IDC_PK_SKIP;

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = picker_wnd_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = PICKER_CLASS;
    RegisterClassExA(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, PICKER_CLASS,
        "Address Cleaner \x97 Select AI Model",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sx - PICKER_W) / 2, (sy - PICKER_H) / 2,
        PICKER_W, PICKER_H, NULL, NULL, NULL, NULL);
    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (s_picker_choice == IDC_PK_FINE)  return 1;
    if (s_picker_choice == IDC_PK_QWEN)  return 2;
    if (s_picker_choice != IDC_PK_BROWSE) return 0;

    /* Browse */
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "GGUF Models (*.gguf)\0*.gguf\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = out_path;
    ofn.nMaxFile    = (DWORD)out_path_sz;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = "Select GGUF Model File";
    return GetOpenFileNameA(&ofn) ? 3 : 0;
}

/* =========================================================================
 * GPU detection via PowerShell
 * ====================================================================== */

static void detect_gpu(char *mode, size_t modesz)
{
    strncpy(mode, "cpu", modesz);
    mode[modesz - 1] = '\0';

    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);

    char ps_path[MAX_PATH + 32], out_path[MAX_PATH + 32];
    snprintf(ps_path, sizeof(ps_path), "%ssetup_gpu.ps1", tmpdir);
    snprintf(out_path, sizeof(out_path), "%ssetup_gpu.txt", tmpdir);

    FILE *f = fopen(ps_path, "w");
    if (!f) return;
    fprintf(f,
        "$gpus = (Get-CimInstance Win32_VideoController).Name -join ' '\n"
        "if ($gpus -match 'NVIDIA') { Set-Content -Path '%s' -Value 'cuda' }\n"
        "elseif ($gpus -match 'Radeon') { Set-Content -Path '%s' -Value 'vulkan' }\n"
        "elseif ($gpus -match 'AMD') { Set-Content -Path '%s' -Value 'vulkan' }\n"
        "else { Set-Content -Path '%s' -Value 'cpu' }\n",
        out_path, out_path, out_path, out_path);
    fclose(f);

    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof(cmd),
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps_path);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    DeleteFileA(ps_path);

    FILE *rf = fopen(out_path, "r");
    if (rf) {
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), rf)) {
            /* Trim whitespace */
            char *p = buf;
            int len = (int)strlen(p);
            while (len > 0 && (p[len-1]==' '||p[len-1]=='\r'||p[len-1]=='\n'))
                p[--len] = '\0';
            if (strcmp(p, "cuda") == 0 || strcmp(p, "vulkan") == 0)
                snprintf(mode, modesz, "%s", p);
        }
        fclose(rf);
    }
    DeleteFileA(out_path);
}

/* =========================================================================
 * WinHTTP helpers
 * ====================================================================== */

/*
 * Download a URL into a malloc'd buffer.  Caller must free().
 * Returns byte count, or 0 on failure.
 */
static DWORD download_to_memory(const wchar_t *url, char **out_buf)
{
    *out_buf = NULL;

    URL_COMPONENTSW uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048], extra[512];
    uc.lpszHostName     = host;  uc.dwHostNameLength  = 256;
    uc.lpszUrlPath      = path;  uc.dwUrlPathLength   = 2048;
    uc.lpszExtraInfo    = extra; uc.dwExtraInfoLength = 512;

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;

    wchar_t full_path[2560];
    wcscpy(full_path, path);
    wcscat(full_path, extra);

    HINTERNET hSession = WinHttpOpen(L"AddressCleaner-Setup/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    WinHttpSetTimeouts(hSession, 5000, 5000, 15000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", full_path,
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto fail_connect;

    WinHttpAddRequestHeaders(hRequest,
        L"User-Agent: AddressCleaner-Setup/1.0\r\nAccept: application/json\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
        goto fail_request;

    /* Read into growing buffer */
    size_t cap = 65536, total = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) goto fail_request;

    DWORD n;
    while (WinHttpReadData(hRequest, buf + total,
                            (DWORD)(cap - total - 1), &n) && n > 0) {
        total += n;
        if (total + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; total = 0; break; }
            buf = nb;
        }
    }
    if (buf && total > 0) buf[total] = '\0';

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    *out_buf = buf;
    return (DWORD)total;

fail_request:
    WinHttpCloseHandle(hRequest);
fail_connect:
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}

/*
 * Download a URL to a local file with progress updates posted to hwnd.
 * Checks s_cancel between chunks.  Returns 1 on success, 0 on failure.
 */
static int download_to_file(const char *url_a, const char *filepath,
                            HWND hwnd)
{
    wchar_t url_w[2048];
    MultiByteToWideChar(CP_ACP, 0, url_a, -1, url_w, 2048);

    URL_COMPONENTSW uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048], extra[512];
    uc.lpszHostName     = host;  uc.dwHostNameLength  = 256;
    uc.lpszUrlPath      = path;  uc.dwUrlPathLength   = 2048;
    uc.lpszExtraInfo    = extra; uc.dwExtraInfoLength = 512;

    if (!WinHttpCrackUrl(url_w, 0, 0, &uc)) return 0;

    wchar_t full_path[2560];
    wcscpy(full_path, path);
    wcscat(full_path, extra);

    HINTERNET hSession = WinHttpOpen(L"AddressCleaner-Setup/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    /* Generous timeouts for large file downloads */
    WinHttpSetTimeouts(hSession, 10000, 10000, 300000, 300000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", full_path,
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto fail_connect;

    WinHttpAddRequestHeaders(hRequest,
        L"User-Agent: AddressCleaner-Setup/1.0\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
        goto fail_request;

    /* Content-Length for progress (may be 0 if server doesn't provide it) */
    DWORD content_len = 0, sz = sizeof(content_len);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &content_len, &sz, WINHTTP_NO_HEADER_INDEX);

    HANDLE hFile = CreateFileA(filepath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) goto fail_request;

    ULONGLONG total = 0;
    DWORD n;
    char chunk[16384];
    int ok = 1;

    while (WinHttpReadData(hRequest, chunk, sizeof(chunk), &n) && n > 0) {
        if (s_cancel) { ok = 0; break; }

        DWORD written;
        if (!WriteFile(hFile, chunk, n, &written, NULL) || written != n) {
            ok = 0;
            break;
        }
        total += n;

        if (content_len > 0 && hwnd) {
            int pct = (int)(total * 100 / content_len);
            PostMessage(hwnd, WM_SETUP_PROGRESS, pct, 0);
            snprintf(s_substatus, sizeof(s_substatus),
                     "%u MB / %u MB",
                     (unsigned)(total >> 20),
                     (unsigned)(content_len >> 20));
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
        } else if (hwnd) {
            snprintf(s_substatus, sizeof(s_substatus),
                     "%u MB downloaded", (unsigned)(total >> 20));
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
        }
    }

    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok || (content_len > 0 && total < content_len)) {
        DeleteFileA(filepath);
        return 0;
    }
    return 1;

fail_request:
    WinHttpCloseHandle(hRequest);
fail_connect:
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}

/* =========================================================================
 * GitHub release URL finder
 *
 * Searches the JSON for the first browser_download_url containing the
 * given pattern (e.g. "bin-win-cpu-x64").
 * ====================================================================== */

static int find_release_url(const char *json, const char *pattern,
                            char *url, size_t urlsz)
{
    const char *key = "\"browser_download_url\":";
    const char *p = json;

    while ((p = strstr(p, key)) != NULL) {
        p += strlen(key);
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') continue;
        p++;

        const char *end = strchr(p, '"');
        if (!end) continue;

        size_t len = (size_t)(end - p);
        if (len >= urlsz) { p = end + 1; continue; }

        /* Check if this URL contains our pattern */
        /* Use a temp buffer for the strstr check */
        char tmp[2048];
        if (len >= sizeof(tmp)) { p = end + 1; continue; }
        memcpy(tmp, p, len);
        tmp[len] = '\0';

        if (strstr(tmp, pattern) && strstr(tmp, ".zip")) {
            memcpy(url, p, len);
            url[len] = '\0';
            return 1;
        }

        p = end + 1;
    }
    return 0;
}

/* =========================================================================
 * Zip extraction via PowerShell
 * ====================================================================== */

static int extract_zip(const char *zip_path, const char *dest_dir)
{
    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd),
        "powershell.exe -NoProfile -Command "
        "\"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"",
        zip_path, dest_dir);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;

    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exitcode = 1;
    GetExitCodeProcess(pi.hProcess, &exitcode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitcode == 0;
}

/* =========================================================================
 * Copy server files from extracted dir into ai\
 * ====================================================================== */

/* GCC inlines this into setup_thread and then computes worst-case path
 * lengths using the caller's buffer sizes, producing false-positive
 * -Wformat-truncation warnings.  The buffers are correct; suppress. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static int copy_server_files(const char *src_dir, const char *ai_dir)
{
    /* The zip may contain files directly or in a single subdirectory */
    char actual_src[MAX_PATH * 2];
    strncpy(actual_src, src_dir, sizeof(actual_src));
    actual_src[sizeof(actual_src) - 1] = '\0';

    char server_test[MAX_PATH + 32];
    snprintf(server_test, sizeof(server_test), "%s\\llama-server.exe", src_dir);

    if (GetFileAttributesA(server_test) == INVALID_FILE_ATTRIBUTES) {
        /* Search one level of subdirectories */
        char search[MAX_PATH + 8];
        snprintf(search, sizeof(search), "%s\\*", src_dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;

                char sub[MAX_PATH * 2 + 32];
                snprintf(sub, sizeof(sub), "%s\\%s\\llama-server.exe",
                         src_dir, fd.cFileName);
                if (GetFileAttributesA(sub) != INVALID_FILE_ATTRIBUTES) {
                    snprintf(actual_src, sizeof(actual_src), "%s\\%s",
                             src_dir, fd.cFileName);
                    break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

    /* Copy llama-server.exe */
    char src_file[MAX_PATH * 2], dst_file[MAX_PATH + 32];
    snprintf(src_file, sizeof(src_file), "%s\\llama-server.exe", actual_src);
    snprintf(dst_file, sizeof(dst_file), "%sllama-server.exe", ai_dir);
    if (!CopyFileA(src_file, dst_file, FALSE)) return 0;

    /* Copy all DLLs */
    char dll_search[MAX_PATH * 2 + 16];
    snprintf(dll_search, sizeof(dll_search), "%s\\*.dll", actual_src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(dll_search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            snprintf(src_file, sizeof(src_file), "%s\\%s", actual_src, fd.cFileName);
            snprintf(dst_file, sizeof(dst_file), "%s%s",   ai_dir,     fd.cFileName);
            CopyFileA(src_file, dst_file, FALSE);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    return 1;
}
#pragma GCC diagnostic pop

/* Recursively delete a directory tree */
static void delete_directory(const char *dir)
{
    /* SHFileOperation needs double-null terminated path */
    char buf[MAX_PATH + 2];
    ZeroMemory(buf, sizeof(buf));
    strncpy(buf, dir, MAX_PATH);

    SHFILEOPSTRUCTA fo;
    ZeroMemory(&fo, sizeof(fo));
    fo.wFunc  = FO_DELETE;
    fo.pFrom  = buf;
    fo.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationA(&fo);
}

/* =========================================================================
 * Setup thread — orchestrates the full download pipeline
 * ====================================================================== */

static DWORD WINAPI setup_thread(LPVOID param)
{
    HWND hwnd = (HWND)param;
    int need_server = !has_server();
    int need_model  = !has_model();

    char ai_dir[MAX_PATH];
    setup_get_ai_dir(ai_dir, sizeof(ai_dir));
    CreateDirectoryA(ai_dir, NULL);

    if (need_server) {
        /* --- GPU detection --- */
        PostMessage(hwnd, WM_SETUP_STATUS, 0, (LPARAM)"Detecting GPU...");
        s_substatus[0] = '\0';
        PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);

        char mode[16];
        detect_gpu(mode, sizeof(mode));
        if (s_cancel) goto done;

        if (strcmp(mode, "cuda") == 0)
            snprintf(s_substatus, sizeof(s_substatus), "NVIDIA GPU detected");
        else if (strcmp(mode, "vulkan") == 0)
            snprintf(s_substatus, sizeof(s_substatus), "AMD GPU detected");
        else
            snprintf(s_substatus, sizeof(s_substatus), "Using CPU build");
        PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
        Sleep(800); /* Brief pause so user can read GPU result */

        /* --- Fetch GitHub release JSON --- */
        PostMessage(hwnd, WM_SETUP_STATUS, 0,
                    (LPARAM)"Fetching latest llama.cpp release...");
        PostMessage(hwnd, WM_SETUP_PROGRESS, 0, 0);

        char *json = NULL;
        DWORD json_len = download_to_memory(GITHUB_RELEASE_URL, &json);
        if (!json || json_len == 0) {
            PostMessage(hwnd, WM_SETUP_STATUS, 0,
                (LPARAM)"Cannot reach GitHub. Check connection or firewall.");
            snprintf(s_substatus, sizeof(s_substatus),
                "Download llama.cpp manually \x97 see README.");
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
            Sleep(4000);
            goto done;
        }
        if (s_cancel) { free(json); goto done; }

        /* --- Find matching download URL --- */
        const char *pattern;
        if (strcmp(mode, "cuda") == 0)        pattern = "bin-win-cuda-12";
        else if (strcmp(mode, "vulkan") == 0)  pattern = "bin-win-vulkan-x64";
        else                                   pattern = "bin-win-cpu-x64";

        char download_url[2048] = {0};
        if (!find_release_url(json, pattern, download_url, sizeof(download_url))) {
            free(json);
            PostMessage(hwnd, WM_SETUP_STATUS, 0,
                (LPARAM)"No matching llama.cpp build found.");
            snprintf(s_substatus, sizeof(s_substatus),
                "Pattern: %s \x97 download manually.", pattern);
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
            Sleep(4000);
            goto done;
        }
        free(json);
        if (s_cancel) goto done;

        /* --- Download zip --- */
        PostMessage(hwnd, WM_SETUP_STATUS, 0,
                    (LPARAM)"Downloading llama.cpp...");
        PostMessage(hwnd, WM_SETUP_PROGRESS, 0, 0);

        char tmpdir[MAX_PATH];
        GetTempPathA(MAX_PATH, tmpdir);
        char zip_path[MAX_PATH + 32];
        snprintf(zip_path, sizeof(zip_path), "%sllama_setup.zip", tmpdir);

        if (!download_to_file(download_url, zip_path, hwnd)) {
            PostMessage(hwnd, WM_SETUP_STATUS, 0,
                (LPARAM)"Download failed. Check connection or try later.");
            snprintf(s_substatus, sizeof(s_substatus),
                "You can download manually \x97 see README.");
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
            Sleep(4000);
            goto done;
        }
        if (s_cancel) { DeleteFileA(zip_path); goto done; }

        /* --- Extract zip --- */
        PostMessage(hwnd, WM_SETUP_STATUS, 0,
                    (LPARAM)"Extracting llama.cpp...");
        s_substatus[0] = '\0';
        PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
        PostMessage(hwnd, WM_SETUP_PROGRESS, 0, 0);

        char extract_dir[MAX_PATH + 32];
        snprintf(extract_dir, sizeof(extract_dir), "%sllama_setup", tmpdir);

        if (!extract_zip(zip_path, extract_dir)) {
            DeleteFileA(zip_path);
            PostMessage(hwnd, WM_SETUP_STATUS, 0,
                (LPARAM)"Extraction failed (PowerShell may be blocked).");
            snprintf(s_substatus, sizeof(s_substatus),
                "Extract the zip manually into the ai\\ folder.");
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
            Sleep(4000);
            goto done;
        }
        DeleteFileA(zip_path);
        if (s_cancel) { delete_directory(extract_dir); goto done; }

        /* --- Copy files into ai\ --- */
        PostMessage(hwnd, WM_SETUP_STATUS, 0,
                    (LPARAM)"Installing llama-server...");

        if (!copy_server_files(extract_dir, ai_dir)) {
            PostMessage(hwnd, WM_SETUP_STATUS, 0,
                (LPARAM)"Could not copy files into ai\\ folder.");
            snprintf(s_substatus, sizeof(s_substatus),
                "Check folder permissions or copy manually.");
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
            delete_directory(extract_dir);
            Sleep(4000);
            goto done;
        }
        delete_directory(extract_dir);
        if (s_cancel) goto done;
    }

    if (need_model && !s_cancel && !s_skip_model) {
        /* --- Download model (URL chosen by pick_model_source on main thread) --- */
        const char *model_url  = s_chosen_model_url[0]
                                 ? s_chosen_model_url  : MODEL_URL;
        const char *model_file = s_chosen_model_filename[0]
                                 ? s_chosen_model_filename : MODEL_FILENAME;

        PostMessage(hwnd, WM_SETUP_STATUS, 0,
                    (LPARAM)"Downloading AI model (~2 GB)...");
        PostMessage(hwnd, WM_SETUP_PROGRESS, 0, 0);

        char model_path[MAX_PATH + 260];
        snprintf(model_path, sizeof(model_path), "%s%s", ai_dir, model_file);

        if (!download_to_file(model_url, model_path, hwnd)) {
            PostMessage(hwnd, WM_SETUP_STATUS, 0,
                (LPARAM)"Model download failed.");
            snprintf(s_substatus, sizeof(s_substatus),
                "Download manually from huggingface.co \x97 see README.");
            PostMessage(hwnd, WM_SETUP_SUBSTATUS, 0, 0);
            Sleep(4000);
            goto done;
        }
    }

    if (!s_cancel)
        InterlockedExchange(&s_result, 1);

done:
    PostMessage(hwnd, WM_SETUP_DONE, (WPARAM)s_result, 0);
    return 0;
}

/* =========================================================================
 * Dialog window procedure
 * ====================================================================== */

static LRESULT CALLBACK setup_wnd_proc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        NONCLIENTMETRICSA ncm;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfWeight = FW_BOLD;
        ncm.lfMessageFont.lfHeight = -14;
        s_hBoldFont = CreateFontIndirectA(&ncm.lfMessageFont);

        HFONT hGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        s_hLabel = CreateWindowExA(0, "STATIC", "Preparing setup...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 16, SETUP_W - 50, 22,
            hwnd, (HMENU)IDC_SETUP_LABEL, NULL, NULL);

        s_hSub = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 40, SETUP_W - 50, 18,
            hwnd, (HMENU)IDC_SETUP_SUB, NULL, NULL);

        s_hProg = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            20, 68, SETUP_W - 50, 18,
            hwnd, (HMENU)IDC_SETUP_PROG, NULL, NULL);
        SendMessageA(s_hProg, PBM_SETRANGE32, 0, 100);

        s_hBtn = CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            (SETUP_W - 90) / 2, 100, 80, 26,
            hwnd, (HMENU)IDC_SETUP_BTN, NULL, NULL);

        SendMessageA(s_hLabel, WM_SETFONT, (WPARAM)s_hBoldFont, FALSE);
        SendMessageA(s_hSub,   WM_SETFONT, (WPARAM)hGui, FALSE);
        SendMessageA(s_hBtn,   WM_SETFONT, (WPARAM)hGui, FALSE);

        /* Kick off the background download thread */
        CreateThread(NULL, 0, setup_thread, (LPVOID)hwnd, 0, NULL);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SETUP_BTN) {
            InterlockedExchange(&s_cancel, 1);
            EnableWindow(s_hBtn, FALSE);
            SetWindowTextA(s_hLabel, "Cancelling...");
        }
        return 0;

    case WM_SETUP_STATUS:
        SetWindowTextA(s_hLabel, (const char *)lp);
        return 0;

    case WM_SETUP_SUBSTATUS:
        SetWindowTextA(s_hSub, s_substatus);
        return 0;

    case WM_SETUP_PROGRESS:
        SendMessageA(s_hProg, PBM_SETPOS, wp, 0);
        return 0;

    case WM_SETUP_DONE:
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            InterlockedExchange(&s_cancel, 1);
            EnableWindow(s_hBtn, FALSE);
            SetWindowTextA(s_hLabel, "Cancelling...");
            return 0;
        }
        break;

    case WM_DESTROY:
        if (s_hBoldFont) { DeleteObject(s_hBoldFont); s_hBoldFont = NULL; }
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

int setup_needed(void)
{
    return !has_server() || !has_model();
}

int setup_run(HINSTANCE hInst)
{
    int need_server = !has_server();
    int need_model  = !has_model();

    /* Reset shared state */
    InterlockedExchange(&s_cancel,     0);
    InterlockedExchange(&s_result,     0);
    InterlockedExchange(&s_skip_model, 0);
    s_chosen_model_url[0]      = '\0';
    s_chosen_model_filename[0] = '\0';
    s_substatus[0]             = '\0';

    /* --- If server is missing, ask permission before downloading --- */
    if (need_server) {
        const char *msg = need_model
            ? "AI components (llama-server and model) were not found.\n\n"
              "Would you like to download them now?\n"
              "Total download: ~2 GB\n\n"
              "Click No to continue in rules-only mode (no AI).\n"
              "You can also set up manually \x97 see README."
            : "The AI engine (llama-server) was not found.\n\n"
              "Would you like to download it now?\n\n"
              "Click No to continue in rules-only mode (no AI).";
        if (MessageBoxA(NULL, msg, "Address Cleaner \x97 AI Setup",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
    }

    /* --- Model picker (when model is needed) --- */
    if (need_model) {
        char browse_path[MAX_PATH] = {0};
        int src = pick_model_source(browse_path, sizeof(browse_path));

        if (src == 0) {          /* Skip */
            if (!need_server) return 0;
            InterlockedExchange(&s_skip_model, 1);

        } else if (src == 3) {   /* Browse — copy file immediately */
            char ai_dir[MAX_PATH];
            setup_get_ai_dir(ai_dir, sizeof(ai_dir));
            CreateDirectoryA(ai_dir, NULL);
            char *fname = strrchr(browse_path, '\\');
            char dest[MAX_PATH + 260];
            snprintf(dest, sizeof(dest), "%s%s",
                     ai_dir, fname ? fname + 1 : browse_path);
            CopyFileA(browse_path, dest, FALSE);
            if (!need_server) return 1;  /* model handled, nothing else to do */
            InterlockedExchange(&s_skip_model, 1);

        } else if (src == 1) {   /* Fine-tuned */
            strncpy(s_chosen_model_url,      FINETUNE_MODEL_URL,
                    sizeof(s_chosen_model_url) - 1);
            strncpy(s_chosen_model_filename, FINETUNE_MODEL_FILENAME,
                    sizeof(s_chosen_model_filename) - 1);

        } else {                 /* Qwen2.5 (src == 2) */
            strncpy(s_chosen_model_url,      MODEL_URL,
                    sizeof(s_chosen_model_url) - 1);
            strncpy(s_chosen_model_filename, MODEL_FILENAME,
                    sizeof(s_chosen_model_filename) - 1);
        }
    }

    /* Register window class */
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = setup_wnd_proc;
    wc.hInstance      = hInst;
    wc.hbrBackground  = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName  = SETUP_CLASS;
    RegisterClassExA(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x  = (sx - SETUP_W) / 2;
    int y  = (sy - SETUP_WIN_H) / 2;

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST,
        SETUP_CLASS, "Address Cleaner \x97 AI Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, SETUP_W, SETUP_WIN_H,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* Modal message loop — exits when setup_thread posts WM_SETUP_DONE */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)s_result;
}
