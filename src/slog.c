#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "slog.h"

static FILE *g_slog;

/* Write string to file, replacing \t \r \n with space to keep TSV intact */
static void write_field(FILE *f, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        if (*s == '\t' || *s == '\r' || *s == '\n')
            fputc(' ', f);
        else
            fputc(*s, f);
    }
}

int slog_open(void)
{
    if (g_slog) { fclose(g_slog); g_slog = NULL; }

    /* Get exe directory */
    char exe_dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    char *sep = strrchr(exe_dir, '\\');
    if (sep) sep[1] = '\0'; else exe_dir[0] = '\0';

    /* Create logs subdirectory */
    char logs_dir[MAX_PATH + 8];
    snprintf(logs_dir, sizeof(logs_dir), "%slogs", exe_dir);
    CreateDirectoryA(logs_dir, NULL); /* OK if already exists */

    /* Build timestamped filename — needs MAX_PATH + ~30 chars for the suffix */
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path),
             "%s\\session_%04d%02d%02d_%02d%02d%02d.tsv",
             logs_dir,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    g_slog = fopen(path, "w");
    if (!g_slog) return 0;

    fprintf(g_slog, "raw\tcleaned\tflags\tnotes\tai_used\tai_input\tai_output\n");
    return 1;
}

void slog_row(const NameResult *nr,
              const char *ai_input,
              const char *ai_output)
{
    if (!g_slog || !nr) return;

    write_field(g_slog, nr->raw);       fputc('\t', g_slog);
    write_field(g_slog, nr->cleaned);   fputc('\t', g_slog);
    fprintf(g_slog, "0x%02X", nr->flags); fputc('\t', g_slog);
    write_field(g_slog, nr->notes);     fputc('\t', g_slog);
    fprintf(g_slog, "%d", (nr->flags & NAME_FLAG_WAS_AI) ? 1 : 0);
    fputc('\t', g_slog);
    write_field(g_slog, ai_input  ? ai_input  : "");
    fputc('\t', g_slog);
    write_field(g_slog, ai_output ? ai_output : "");
    fputc('\n', g_slog);
    fflush(g_slog);
}

void slog_close(int n_cleaned, int n_flagged, int n_trust)
{
    if (!g_slog) return;
    fprintf(g_slog, "# %d cleaned, %d flagged, %d trusts\n",
            n_cleaned, n_flagged, n_trust);
    fclose(g_slog);
    g_slog = NULL;
}
