#ifndef SPLASH_H
#define SPLASH_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * splash.h — AI startup splash screen and shared logo loader.
 *
 * Call splash_load_logo() early in startup (before showing any window) to
 * load config\logo.png if present.  It is idempotent — safe to call more
 * than once.  The resulting HBITMAP is shared by the splash and main window.
 *
 * Call splash_run() after llm_init() if the AI is loading.  It blocks
 * (runs its own message loop) until the sidecar becomes ready, fails, or
 * hits the 180-second timeout, then returns.
 *
 * Main window should not be shown until splash_run() returns.
 */

/* Load config\logo.png next to the exe.  No-op if not found or already loaded. */
void splash_load_logo(void);

/*
 * Return the loaded logo HBITMAP (or NULL if none).
 * out_w / out_h receive the original pixel dimensions of the PNG.
 * The HBITMAP remains valid for the lifetime of the process.
 */
HBITMAP splash_get_logo(int *out_w, int *out_h);

/* Same as splash_get_logo() but composited against COLOR_BTNFACE — use this
 * when drawing onto the main window background. */
HBITMAP splash_get_logo_for_window(int *out_w, int *out_h);

/* Show the AI startup splash and block until startup resolves. */
void splash_run(HINSTANCE hInst);

/* Free the logo HBITMAPs.  Call once during shutdown (after gui cleanup). */
void splash_cleanup(void);

#endif /* SPLASH_H */
