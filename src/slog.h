#ifndef SLOG_H
#define SLOG_H

#include <stddef.h>
#include "names.h"

/*
 * slog.h — Always-on session logging.
 *
 * Each Clean click creates a new TSV in logs/ next to the exe.
 * Columns: raw, cleaned, flags, notes, ai_used, ai_input, ai_output
 */

/* Open a new session log file.  Returns 1 on success, 0 on failure.
 * Failure is non-fatal — the clean operation continues without logging. */
int slog_open(void);

/* Log one cleaned name.  ai_input/ai_output may be NULL if AI was not used. */
void slog_row(const NameResult *nr,
              const char *ai_input,
              const char *ai_output);

/* Write summary line and close the log file. */
void slog_close(int n_cleaned, int n_flagged, int n_trust);

#endif /* SLOG_H */
