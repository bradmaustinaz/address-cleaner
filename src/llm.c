#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include "llm.h"

/* =========================================================================
 * Configuration
 * ====================================================================== */

#define LLM_PORT         8765
#define POLL_MAX_TRIES   120    /* 120 × 500 ms = 60 s max startup wait   */
#define POLL_INTERVAL_MS 500    /* ms between /health polls               */

/* =========================================================================
 * State
 *
 *  g_state:  -1 = no model / failed
 *             0 = loading (initial value)
 *             1 = ready
 *
 * Written only by startup_thread via InterlockedExchange.
 * Read by main thread via volatile load (x86/x64 aligned 32-bit reads
 * are inherently atomic).
 * ====================================================================== */

static HANDLE          g_proc    = NULL; /* llama-server.exe process handle */
static HANDLE          g_thread  = NULL; /* background startup thread        */
static volatile LONG   g_state   = 0;
static HINTERNET       g_session = NULL; /* shared WinHTTP session           */
static char            g_model_path[MAX_PATH] = {0};

/* =========================================================================
 * Internal helpers: locate files next to the exe
 * ====================================================================== */

static void get_exe_dir(char *buf, size_t bufsz)
{
    GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
    char *sep = strrchr(buf, '\\');
    if (sep) sep[1] = '\0';
    else     buf[0] = '\0';
}

/* Find the first *.gguf in dir.  Returns 1 on success. */
static int find_model(const char *dir, char *out, size_t outsz)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s*.gguf", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    snprintf(out, outsz, "%s%s", dir, fd.cFileName);
    FindClose(h);
    return 1;
}

/* =========================================================================
 * Internal helpers: WinHTTP wrappers
 * ====================================================================== */

static HINTERNET open_connect(void)
{
    return WinHttpConnect(g_session, L"127.0.0.1", LLM_PORT, 0);
}

/* GET path; returns HTTP status code (e.g. 200) or 0 on error. */
static int http_get_status(const char *path)
{
    HINTERNET hConn = open_connect();
    if (!hConn) return 0;

    wchar_t wpath[256];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 256);

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wpath,
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    int status = 0;
    if (hReq) {
        if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               NULL, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, NULL)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
        }
        WinHttpCloseHandle(hReq);
    }
    WinHttpCloseHandle(hConn);
    return status;
}

/*
 * POST JSON body to path.  Fills resp (NUL-terminated) and returns 1 on
 * HTTP 200, 0 on any error.
 */
static int http_post_json(const char *path, const char *body,
                          char *resp, size_t resp_sz)
{
    HINTERNET hConn = open_connect();
    if (!hConn) return 0;

    wchar_t wpath[256];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 256);

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", wpath,
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    int ok = 0;
    if (hReq) {
        static const WCHAR *hdr = L"Content-Type: application/json\r\n";
        DWORD blen = (DWORD)strlen(body);
        if (WinHttpSendRequest(hReq, hdr, (DWORD)-1L,
                               (LPVOID)body, blen, blen, 0) &&
            WinHttpReceiveResponse(hReq, NULL)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            if (code == 200 && resp && resp_sz > 0) {
                size_t pos = 0;
                DWORD n;
                char chunk[4096];
                while (WinHttpReadData(hReq, chunk, sizeof(chunk)-1, &n) && n > 0) {
                    if (pos + n < resp_sz - 1) {
                        memcpy(resp + pos, chunk, n);
                        pos += n;
                    }
                }
                resp[pos] = '\0';
                ok = 1;
            }
        }
        WinHttpCloseHandle(hReq);
    }
    WinHttpCloseHandle(hConn);
    return ok;
}

/* =========================================================================
 * Minimal JSON string extractor
 *
 * Finds the first occurrence of "key":"<value>" in json and writes
 * the unescaped value into out.  Returns 1 on success.
 * ====================================================================== */

static int extract_json_string(const char *json, const char *key,
                                char *out, size_t outlen)
{
    if (!json || !key || !out || !outlen) return 0;

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;

    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++; /* skip opening quote */

    size_t i = 0;
    while (*p && i < outlen - 1) {
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
            break; /* closing quote */
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

/* =========================================================================
 * Prompt builder
 *
 * Uses Qwen2.5 chat template (im_start / im_end tokens).
 * Each user turn shows:
 *   Original: <ALL-CAPS raw input>
 *   Cleaned:  <rules-cleaned, title-cased form>
 * The assistant replies with the fully corrected name.
 *
 * Few-shot examples cover:
 *   - Reversed Last-First-Initial format
 *   - Obvious first-name typo
 *   - All-caps initials that title-case lowered (KJ, JLS, ...)
 *   - Compound & names (preserve the ampersand)
 * ====================================================================== */

static void build_prompt(const char *raw, const char *cleaned,
                         char *buf, size_t bufsz)
{
    snprintf(buf, bufsz,
        "<|im_start|>system\n"
        "You clean US real estate property owner names for mailing labels.\n"
        "Rules: fix typos; correct reversed Last-First to First Last; "
        "keep abbreviations/initials uppercase when the original shows them "
        "in ALL CAPS (e.g. KJ, JLS, HEB); preserve & between names; "
        "remove incomplete trailing trust language.\n"
        "Reply with ONLY the corrected name on one line, nothing else.\n"
        "<|im_end|>\n"
        "<|im_start|>user\n"
        "Original: JOHNSON MARY K\n"
        "Cleaned: Johnson Mary K\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n"
        "Mary K Johnson\n"
        "<|im_end|>\n"
        "<|im_start|>user\n"
        "Original: MICHAE WILLIAMS\n"
        "Cleaned: Michae Williams\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n"
        "Michael Williams\n"
        "<|im_end|>\n"
        "<|im_start|>user\n"
        "Original: KJ POLLAK FAMILY TRUST\n"
        "Cleaned: Kj Pollak Family\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n"
        "KJ Pollak Family\n"
        "<|im_end|>\n"
        "<|im_start|>user\n"
        "Original: BARBARA TROILO SURVIIVO\n"
        "Cleaned: Barbara Troilo Surviivo\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n"
        "Barbara Troilo\n"
        "<|im_end|>\n"
        "<|im_start|>user\n"
        "Original: %s\n"
        "Cleaned: %s\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n",
        raw, cleaned);
}

/* Escape a string for embedding inside a JSON double-quoted value. */
static void json_escape(const char *src, char *dst, size_t dstsz)
{
    size_t i = 0;
    while (*src && i < dstsz - 3) {
        switch (*src) {
        case '"':  dst[i++] = '\\'; dst[i++] = '"';  break;
        case '\\': dst[i++] = '\\'; dst[i++] = '\\'; break;
        case '\n': dst[i++] = '\\'; dst[i++] = 'n';  break;
        case '\r': dst[i++] = '\\'; dst[i++] = 'r';  break;
        case '\t': dst[i++] = '\\'; dst[i++] = 't';  break;
        default:   dst[i++] = *src; break;
        }
        src++;
    }
    dst[i] = '\0';
}

/* =========================================================================
 * Background startup thread
 * ====================================================================== */

static DWORD WINAPI startup_thread(LPVOID param)
{
    (void)param;

    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    /* Verify llama-server.exe exists */
    char server_exe[MAX_PATH];
    snprintf(server_exe, sizeof(server_exe), "%sllama-server.exe", exe_dir);
    if (GetFileAttributesA(server_exe) == INVALID_FILE_ATTRIBUTES) {
        InterlockedExchange(&g_state, -1);
        return 0;
    }

    /* Find *.gguf model */
    if (!find_model(exe_dir, g_model_path, sizeof(g_model_path))) {
        InterlockedExchange(&g_state, -1);
        return 0;
    }

    /* Build command line */
    char cmd[MAX_PATH * 2 + 256];
    snprintf(cmd, sizeof(cmd),
        "\"%s\""
        " --model \"%s\""
        " --port %d"
        " --ctx-size 1024"
        " --n-predict 48"
        " -ngl 99"       /* offload all layers to GPU */
        " --threads 4"
        " --log-disable",
        server_exe, g_model_path, LLM_PORT);

    /* Launch server process, suppressing its console window */
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        InterlockedExchange(&g_state, -1);
        return 0;
    }
    g_proc = pi.hProcess;
    CloseHandle(pi.hThread);

    /* Open the shared WinHTTP session */
    g_session = WinHttpOpen(L"AddressCleaner/1.0",
                            WINHTTP_ACCESS_TYPE_NO_PROXY,
                            WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) {
        InterlockedExchange(&g_state, -1);
        return 0;
    }

    /* Short timeouts for health polling */
    WinHttpSetTimeouts(g_session, 1000, 1000, 2000, 2000);

    /* Poll /health until the server is ready */
    for (int i = 0; i < POLL_MAX_TRIES; i++) {
        Sleep(POLL_INTERVAL_MS);

        if (http_get_status("/health") == 200) {
            /* Longer timeouts for actual inference */
            WinHttpSetTimeouts(g_session, 5000, 5000, 60000, 60000);
            InterlockedExchange(&g_state, 1);
            return 0;
        }

        /* Bail early if the process crashed */
        DWORD exit_code;
        if (GetExitCodeProcess(g_proc, &exit_code) &&
            exit_code != STILL_ACTIVE) {
            InterlockedExchange(&g_state, -1);
            return 0;
        }
    }

    /* Timed out */
    InterlockedExchange(&g_state, -1);
    return 0;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void llm_init(void)
{
    /* Quick pre-check: if there's no server exe, don't even start a thread */
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    char server_exe[MAX_PATH];
    snprintf(server_exe, sizeof(server_exe), "%sllama-server.exe", exe_dir);
    if (GetFileAttributesA(server_exe) == INVALID_FILE_ATTRIBUTES) {
        InterlockedExchange(&g_state, -1);
        return;
    }

    g_thread = CreateThread(NULL, 0, startup_thread, NULL, 0, NULL);
}

void llm_shutdown(void)
{
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000); /* let startup thread finish */
        /* TerminateThread is generally unsafe but acceptable here: the process
         * exits immediately after llm_shutdown(), so any transient lock state
         * is irrelevant. */
        TerminateThread(g_thread, 0);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_proc) {
        TerminateProcess(g_proc, 0);
        CloseHandle(g_proc);
        g_proc = NULL;
    }
    if (g_session) {
        WinHttpCloseHandle(g_session);
        g_session = NULL;
    }
}

int llm_is_ready(void)
{
    return (int)g_state == 1;
}

const char *llm_status_str(void)
{
    switch ((int)g_state) {
    case  1: return "AI: Ready";
    case -1: return "AI: No model";
    default: return "AI: Loading...";
    }
}

int llm_clean_name(const char *raw, const char *rules_result,
                   char *out, size_t outlen)
{
    if (!llm_is_ready() || !raw || !out || !outlen) return 0;

    /* Build prompt with both raw (for casing context) and rules_result */
    const char *cleaned = (rules_result && *rules_result) ? rules_result : raw;
    char prompt[4096];
    build_prompt(raw, cleaned, prompt, sizeof(prompt));

    /* Escape prompt for embedding in JSON string */
    char escaped[8192];
    json_escape(prompt, escaped, sizeof(escaped));

    /* Build /completion JSON body */
    char body[9216];
    snprintf(body, sizeof(body),
        "{"
        "\"prompt\":\"%s\","
        "\"n_predict\":48,"
        "\"temperature\":0.1,"
        "\"stop\":[\"\\n\",\"<|im_end|>\"]"
        "}",
        escaped);

    char resp[16384];
    if (!http_post_json("/completion", body, resp, sizeof(resp)))
        return 0;

    char content[512];
    if (!extract_json_string(resp, "content", content, sizeof(content)))
        return 0;

    /* Trim leading/trailing whitespace */
    char *p = content;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int len = (int)strlen(p);
    while (len > 0 && (p[len-1]==' '||p[len-1]=='\t'||p[len-1]=='\n'||p[len-1]=='\r'))
        p[--len] = '\0';

    /* Sanity checks: reject obvious bad output */
    if (!*p)                          return 0; /* empty */
    if (*p == '{' || *p == '[')       return 0; /* JSON leaked into output */
    if (len > 128)                    return 0; /* hallucination paragraph */
    if (strstr(p, "<|im_start|>"))    return 0; /* prompt template leak */
    if (strstr(p, "<|im_end|>"))      return 0;

    snprintf(out, outlen, "%s", p);
    return 1;
}
