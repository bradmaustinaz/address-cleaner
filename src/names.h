#ifndef NAMES_H
#define NAMES_H

#include <stddef.h>
#include "rules.h"

#define NAME_MAX_LEN   512

/*
 * NameResult holds one cleaned name plus metadata about what changed.
 */
typedef struct {
    char raw[NAME_MAX_LEN];      /* Original input, unmodified          */
    char cleaned[NAME_MAX_LEN];  /* After all rules + capitalization    */
    int  flags;                  /* NAME_FLAG_* bitmask from rules.h    */
    char notes[256];             /* Human-readable summary of changes   */
    /* Session logging: populated only when AI fallback was invoked */
    char ai_input[NAME_MAX_LEN]; /* Rules-cleaned text sent to LLM     */
    char ai_output[NAME_MAX_LEN];/* Raw LLM response                   */
} NameResult;

/*
 * name_clean: Clean a raw name string.
 *   - Strips trust language, junk phrases
 *   - Applies title case with real-estate-aware exceptions
 *   - Populates result->cleaned, result->flags, result->notes
 * Returns 0 on success, -1 if input was empty.
 */
int name_clean(const char *raw, NameResult *result);

/*
 * name_flags_str: Build a short human-readable summary of a flags bitmask.
 * buf must be at least 256 bytes.
 */
void name_flags_str(int flags, char *buf, size_t buflen);

#endif /* NAMES_H */
