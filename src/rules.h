#ifndef RULES_H
#define RULES_H

#include <stddef.h>

/*
 * rules.h — Pattern-based name cleaning rules engine.
 *
 * Rules are applied to an uppercased working copy of the name.
 * Each rule removes a specific pattern of real-estate / trust language.
 * Extend the rule tables in rules.c to add new patterns.
 */

/* Flags returned by rules_apply() — bitmask */
#define NAME_FLAG_WAS_TRUST    (1 << 0)  /* Trust language stripped        */
#define NAME_FLAG_WAS_ET_AL    (1 << 1)  /* Et al / et ux / et vir removed */
#define NAME_FLAG_WAS_CO       (1 << 2)  /* C/O or "in care of" removed    */
#define NAME_FLAG_SUSPICIOUS   (1 << 3)  /* Ambiguous — flag for review    */
#define NAME_FLAG_EMPTY        (1 << 4)  /* Nothing left after cleaning    */
#define NAME_FLAG_NEEDS_AI     (1 << 5)  /* Pattern that benefits from AI  */
#define NAME_FLAG_WAS_AI       (1 << 6)  /* AI fallback was applied        */

/*
 * rules_apply: Apply all name-cleaning rules to the uppercase string s.
 * s is modified in-place. slen is the buffer capacity.
 * Returns a bitmask of NAME_FLAG_* indicating what was stripped.
 */
int rules_apply(char *s, size_t slen);

/*
 * Debug logging — only available in DEBUG builds.
 * Call rules_debug_open() before a clean run and rules_debug_close() after.
 * rules_apply() will write a step-by-step trace to the log file.
 */
#ifdef DEBUG
void rules_debug_open(const char *path);
void rules_debug_close(void);
#endif

#endif /* RULES_H */
