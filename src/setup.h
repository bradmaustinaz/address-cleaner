#ifndef SETUP_H
#define SETUP_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * setup.h — Automatic download of AI dependencies at first run.
 *
 * If ai\llama-server.exe or ai\*.gguf is missing, presents a dialog
 * offering to download them.  Downloads use WinHTTP (HTTPS) with
 * progress reporting.  On failure or cancel the app continues in
 * rules-only mode.
 */

/* Returns 1 if ai\llama-server.exe or ai\*.gguf is missing. */
int setup_needed(void);

/*
 * Show a confirmation dialog, then download missing files with a
 * progress window.  Returns 1 on success, 0 on failure/cancel.
 * The app should call llm_init() after this returns regardless —
 * llm_init() gracefully handles missing files.
 */
int setup_run(HINSTANCE hInst);

#endif /* SETUP_H */
