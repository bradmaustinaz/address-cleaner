#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rules.h"
#include "names.h"

/* Ensure RULES_BUF_MAX stays in sync with NAME_MAX_LEN */
#if RULES_BUF_MAX != NAME_MAX_LEN
#error "RULES_BUF_MAX and NAME_MAX_LEN must be equal"
#endif

/* =========================================================================
 * Optional step-by-step debug log  (compiled in only for DEBUG builds)
 *
 * Usage: call rules_debug_open("path/to/nameclean_debug.log") before a
 * clean run, then rules_debug_close() when done.  rules_apply() will write
 * one section per name showing every rule that changed the string.
 * ====================================================================== */
#ifdef DEBUG
static FILE *g_dbg = NULL;

void rules_debug_open(const char *path)
{
    if (g_dbg) fclose(g_dbg);
    g_dbg = fopen(path, "w");
}

void rules_debug_close(void)
{
    if (g_dbg) { fclose(g_dbg); g_dbg = NULL; }
}

static void dlog(const char *step, const char *s)
{
    if (g_dbg)
        fprintf(g_dbg, "  %-30s \"%s\"\n", step, s);
}
#else
#define dlog(step, s) ((void)0)
#endif

/* =========================================================================
 * Low-level string helpers
 * ====================================================================== */

/* Case-insensitive strncmp (portable, avoids _strnicmp vs strncasecmp) */
static int icase_ncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca)      return 0;
    }
    return 0;
}

/* True if c is a word-boundary character (non-alphanumeric, or NUL) */
static int is_boundary(char c)
{
    return !c || (!isalnum((unsigned char)c) && c != '\'');
}

/*
 * find_word: find pattern in s case-insensitively, requiring word boundaries
 * on both sides. Returns pointer to match start in s, or NULL.
 */
static char *find_word(const char *s, const char *pattern)
{
    size_t plen = strlen(pattern);
    if (!plen) return NULL;

    for (const char *p = s; *p; p++) {
        if (icase_ncmp(p, pattern, plen) != 0) continue;
        /* Left boundary: must be start of string or a boundary char */
        if (p > s && !is_boundary(*(p - 1))) continue;
        /* Right boundary */
        if (!is_boundary(p[plen])) continue;
        return (char *)p;
    }
    return NULL;
}

/* Remove n bytes at offset pos in s (shifts tail left) */
static void str_erase(char *s, size_t pos, size_t n)
{
    size_t len = strlen(s);
    if (pos >= len) return;
    if (pos + n > len) n = len - pos;
    memmove(s + pos, s + pos + n, len - pos - n + 1);
}

/* Trim trailing whitespace, commas, and ampersands */
static void rtrim(char *s)
{
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (isspace((unsigned char)s[i]) || s[i] == ',' || s[i] == '&'))
        s[i--] = '\0';
}

/* Trim leading whitespace, return pointer into s */
static char *ltrim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    return s;
}

/* Collapse runs of whitespace to a single space */
static void collapse_spaces(char *s)
{
    char *r = s, *w = s;
    int in_sp = 0;
    while (*r) {
        if (isspace((unsigned char)*r)) {
            if (!in_sp) { *w++ = ' '; in_sp = 1; }
        } else {
            *w++ = *r;
            in_sp = 0;
        }
        r++;
    }
    *w = '\0';
}

/* Case-insensitive full string equality */
static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/*
 * all_words_in: returns 1 if every word in 'needle' appears as a whole
 * word in 'haystack' (case-insensitive, uses find_word for boundary match).
 */
static int all_words_in(const char *haystack, const char *needle)
{
    char tmp[RULES_BUF_MAX];
    snprintf(tmp, sizeof(tmp), "%s", needle);

    char *p = tmp;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *ws = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char saved = *p;
        *p = '\0';
        if (!find_word(haystack, ws)) { *p = saved; return 0; }
        *p = saved;
        if (saved) p++;
    }
    return 1;
}

/* =========================================================================
 * Step 1: Strip C/O prefix
 * ====================================================================== */

static int strip_co(char *s, int *flags)
{
    static const char *prefixes[] = { "C/O ", "C/O\t", "IN CARE OF ", NULL };
    for (int i = 0; prefixes[i]; i++) {
        size_t n = strlen(prefixes[i]);
        if (icase_ncmp(s, prefixes[i], n) == 0) {
            memmove(s, s + n, strlen(s + n) + 1);
            *flags |= NAME_FLAG_WAS_CO;
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Step 2: Strip date clause (everything from keyword to end of string)
 *
 * Real estate trust names append dates like:
 *   "JOHN SMITH LIVING TRUST DATED JANUARY 1 2020"
 *   "JOHN SMITH REVOCABLE TRUST DTD 01/01/2020"
 *   "JOHN SMITH FAMILY TRUST U/D/T 1/1/20"
 * ====================================================================== */

static const char *date_keywords[] = {
    "AS AMENDED", "AMENDED AND RESTATED", "AMENDED", "RESTATED",
    "DATED", "DTD", "U/D/T", "UTD", "U/T/D",
    "ESTABLISHED", "CREATED",
    NULL
};

static int strip_date_clause(char *s)
{
    for (int i = 0; date_keywords[i]; i++) {
        const char *kw  = date_keywords[i];
        size_t      kwl = strlen(kw);

        for (char *p = s; *p; p++) {
            if (icase_ncmp(p, kw, kwl) != 0) continue;
            /* Word-boundary check */
            if (p > s && (isalnum((unsigned char)*(p-1)) || *(p-1) == '/')) continue;
            char next = p[kwl];
            if (isalnum((unsigned char)next) || next == '/') continue;
            /* Truncate here */
            *p = '\0';
            rtrim(s);
            return 1;
        }
    }
    return 0;
}

/* Quick-strip trust keywords for name extraction (used by strip_and_trust
 * and extract_cotenant).  Longest-match first, "TRUST" catch-all last.
 * This must stay in sync with trust_suffixes[] below. */
static const char *trust_keywords_quick[] = {
    "AMENDED AND RESTATED TRUST",
    "RESTATEMENT OF TRUST",
    "REVOCABLE LIVING TRUST", "IRREVOCABLE LIVING TRUST",
    "LEGACY LIVING TRUST", "HERITAGE LIVING TRUST",
    "REVOCABLE INTER VIVOS TRUST", "IRREVOCABLE INTER VIVOS TRUST",
    "INTER VIVOS TRUST", "LIVING TRUST",
    "CHARITABLE REMAINDER UNITRUST", "CHARITABLE REMAINDER TRUST",
    "CHARITABLE LEAD TRUST",
    "GENERATION SKIPPING TRUST", "GENERATION-SKIPPING TRUST",
    "ASSET PROTECTION TRUST", "SUPPLEMENTAL NEEDS TRUST",
    "SPECIAL NEEDS TRUST",
    "DISCRETIONARY TRUST", "SPENDTHRIFT TRUST", "TESTAMENTARY TRUST",
    "IRREVOCABLE TRUST", "REVOCABLE TRUST",
    "SURVIVORS TRUST", "SURVIVOR'S TRUST",
    "EXEMPT TRUST", "LEGACY TRUST", "HERITAGE TRUST",
    "FAMILY TRUST", "LAND TRUST", "BLIND TRUST",
    "MARITAL TRUST", "BYPASS TRUST",
    "CREDIT SHELTER TRUST", "QTIP TRUST", "DYNASTY TRUST",
    "TRUST", NULL
};

/* =========================================================================
 * Step 2b: Strip "[NAME] & [TRUST...]" — when an ampersand is followed
 * anywhere by trust language, strip from the last such ampersand to end.
 *
 *   "Charles Smith & The Smith Family Trust" → "Charles Smith"
 *   "Bob Marley & The Rasta's Trust"         → "Bob Marley"
 * ====================================================================== */

static int strip_and_trust(char *s, int *flags)
{
    char *last_amp = NULL;

    for (char *p = s; *p; p++) {
        if (*p != '&') continue;
        if (p == s) continue; /* nothing before & */

        /* Only strip when "THE" immediately follows the &.
         * This distinguishes "S & C Boege" (& is part of the entity name)
         * from "Smith & The Smith Family Trust" (& separates a person from
         * a separately-named trust). */
        char *after = p + 1;
        while (isspace((unsigned char)*after)) after++;
        if (icase_ncmp(after, "THE ", 4) != 0) continue;

        if (find_word(p, "TRUST"))
            last_amp = p;
    }

    if (!last_amp) return 0;

    /* Before truncating, try to recover the person name from the right side.
     * "Nancy Bower & The Nancy A Bower Living Trust" → keep "Nancy A Bower"
     * rather than just "Nancy Bower". */
    {
        char *after = last_amp + 1;
        while (isspace((unsigned char)*after)) after++;
        if (icase_ncmp(after, "THE ", 4) == 0) after += 4;
        while (isspace((unsigned char)*after)) after++;

        char right_name[RULES_BUF_MAX];
        snprintf(right_name, sizeof(right_name), "%s", after);

        /* Quick-strip trust keywords from the right-side copy so we are
         * left with just the person name for comparison. */
        for (int ki = 0; trust_keywords_quick[ki]; ki++) {
            char *tp = find_word(right_name, trust_keywords_quick[ki]);
            if (tp) { *tp = '\0'; break; }
        }
        rtrim(right_name);

        /* Build the left-side name for comparison. */
        char left[RULES_BUF_MAX];
        size_t llen = (size_t)(last_amp - s);
        if (llen < sizeof(left)) {
            memcpy(left, s, llen);
            left[llen] = '\0';
            rtrim(left);

            /* If the right-side person name is a superset of the left name,
             * use it — it is the same person with more detail.
             * However, if right has extra words beyond left, verify they
             * are legitimate name parts (single-letter initials or short
             * suffixes like JR/SR/II/III).  Extra multi-letter words that
             * aren't recognizable name parts are likely trust-language
             * residue or typos (e.g. "LIVUING" from "LIVING TRUST").
             * In that case, prefer the cleaner left-side name. */
            if (*right_name && all_words_in(right_name, left)) {
                /* Check if right has extra words that look suspicious */
                int use_right = 1;
                if (!all_words_in(left, right_name)) {
                    /* right is a strict superset — validate extra words */
                    char rtmp[RULES_BUF_MAX];
                    snprintf(rtmp, sizeof(rtmp), "%s", right_name);
                    char *rp = rtmp;
                    while (*rp) {
                        while (isspace((unsigned char)*rp)) rp++;
                        if (!*rp) break;
                        char *ws = rp;
                        while (*rp && !isspace((unsigned char)*rp)) rp++;
                        char sv = *rp; *rp = '\0';
                        size_t wl = strlen(ws);
                        /* If this word is already in left, it's fine */
                        if (!find_word(left, ws)) {
                            /* Extra word: accept single-letter initials
                             * and common suffixes; reject anything else */
                            if (wl > 1) {
                                char upper_w[64];
                                for (size_t ui = 0; ui < wl && ui < 63; ui++)
                                    upper_w[ui] = (char)toupper((unsigned char)ws[ui]);
                                upper_w[wl < 63 ? wl : 63] = '\0';
                                if (strcmp(upper_w, "JR") != 0 &&
                                    strcmp(upper_w, "SR") != 0 &&
                                    strcmp(upper_w, "II") != 0 &&
                                    strcmp(upper_w, "III") != 0 &&
                                    strcmp(upper_w, "IV") != 0) {
                                    use_right = 0;
                                    *rp = sv;
                                    break;
                                }
                            }
                        }
                        *rp = sv;
                        if (sv) rp++;
                    }
                }
                if (use_right) {
                    size_t rlen = strlen(right_name);
                    memmove(s, right_name, rlen + 1);
                    *flags |= NAME_FLAG_WAS_TRUST;
                    return 1;
                }
            }
        }
    }

    /* Default: simple truncation at & */
    *last_amp = '\0';
    rtrim(s);
    *flags |= NAME_FLAG_WAS_TRUST;
    return 1;
}

/* =========================================================================
 * Step 2c: Extract co-tenant from "TRUST_LEFT & PERSON_RIGHT"
 *
 * When the left side is a trust entity and the right side is a plain person
 * name (no trust language), keep the person (right side) — it is always
 * the more useful mail label.
 *
 *   "Tssk Trust & Susan Shelton"                → "Susan Shelton"
 *   "Demarino Trust & Christine Demarino"       → "Christine Demarino"
 *   "The Cimarrons Living Trust & Terri Cutting"→ "Terri Cutting"
 *   "Melody And Michael Morgan Family Trust
 *      & Melody Morgan"                         → "Melody Morgan"
 *   "The Ardell And Kari Witt Family Trust
 *      & Ardell Witt Jr"                        → "Ardell Witt Jr"
 * ====================================================================== */

static int extract_cotenant(char *s, int *flags)
{
    char *amp = strchr(s, '&');
    if (!amp || amp == s) return 0;

    /* "& THE ..." is already handled by strip_and_trust */
    char *after_amp = amp + 1;
    while (isspace((unsigned char)*after_amp)) after_amp++;
    if (icase_ncmp(after_amp, "THE ", 4) == 0) return 0;

    /* Left side must contain trust language */
    char left[RULES_BUF_MAX];
    size_t llen = (size_t)(amp - s);
    if (llen >= sizeof(left)) return 0;
    memcpy(left, s, llen);
    left[llen] = '\0';
    if (!find_word(left, "TRUST")) return 0;

    /* Right side must NOT contain trust language */
    if (find_word(after_amp, "TRUST")) return 0;

    /* Build and trim the right side */
    char right[RULES_BUF_MAX];
    snprintf(right, sizeof(right), "%s", after_amp);
    { int i = (int)strlen(right) - 1;
      while (i >= 0 && (isspace((unsigned char)right[i])
                        || right[i] == ',' || right[i] == '&'))
          right[i--] = '\0';
    }
    if (!*right) return 0;

    /* Right must have at least 2 words (a real person name) */
    int rwords = 0;
    { char *rp = right;
      while (*rp) {
          while (isspace((unsigned char)*rp)) rp++;
          if (!*rp) break;
          rwords++;
          while (*rp && !isspace((unsigned char)*rp)) rp++;
      }
    }
    if (rwords < 2) return 0;

    /* Quick-strip trust keywords from left → entity base name for comparison */
    for (int ki = 0; trust_keywords_quick[ki]; ki++) {
        char *tp = find_word(left, trust_keywords_quick[ki]);
        if (tp) { *tp = '\0'; break; }
    }
    rtrim(left);

    /* Decision 1: right's words ⊆ left_base → person is specifically named
     * on the deed, use the person name for the mail label.
     * e.g. "Melody And Michael Morgan Family Trust & Melody Morgan"
     *       → "Melody Morgan" (not "Melody and Michael Morgan Family") */
    if (*left && all_words_in(left, right)) {
        memmove(s, right, strlen(right) + 1);
        *flags |= NAME_FLAG_WAS_TRUST;
        return 1;
    }

    /* Decision 2: left_base's words ⊆ right → right is the person name */
    if (*left && all_words_in(right, left)) {
        memmove(s, right, strlen(right) + 1);
        *flags |= NAME_FLAG_WAS_TRUST;
        return 1;
    }

    /* Default: use right (person name is more useful than trust entity) */
    memmove(s, right, strlen(right) + 1);
    *flags |= NAME_FLAG_WAS_TRUST;
    return 1;
}

/* =========================================================================
 * Step 3: Strip trust type suffixes
 *
 * keep_prefix: chars to retain from the start of the match.
 *   0          → remove the entire phrase  (e.g. "REVOCABLE TRUST" → gone)
 *   non-zero   → keep that many chars      (e.g. "FAMILY TRUST" → "FAMILY")
 *
 * Listed longest-to-shortest so multi-word forms match before shorter ones.
 * "TRUST" alone is last — it's the catch-all.
 * ====================================================================== */

typedef struct { const char *pattern; int keep_prefix; } TrustSuffix;

static const TrustSuffix trust_suffixes[] = {
    /* Most specific first, keep_prefix=0 means remove entirely */
    { "AMENDED AND RESTATED TRUST",          0 },
    { "RESTATEMENT OF TRUST",                0 },
    { "REVOCABLE LIVING TRUST",              0 },
    { "IRREVOCABLE LIVING TRUST",            0 },
    { "LEGACY LIVING TRUST",                 0 },
    { "HERITAGE LIVING TRUST",               0 },
    { "REVOCABLE INTER VIVOS TRUST",         0 },
    { "IRREVOCABLE INTER VIVOS TRUST",       0 },
    { "INTER VIVOS TRUST",                   0 },
    { "CHARITABLE REMAINDER UNITRUST",       0 },
    { "CHARITABLE REMAINDER TRUST",          0 },
    { "QUALIFIED PERSONAL RESIDENCE TRUST",  0 },
    { "GENERATION SKIPPING TRUST",           0 },
    { "GENERATION-SKIPPING TRUST",           0 },
    { "ASSET PROTECTION TRUST",              0 },
    { "SUPPLEMENTAL NEEDS TRUST",            0 },
    { "SPECIAL NEEDS TRUST",                 0 },
    { "DISCRETIONARY TRUST",                 0 },
    { "SPENDTHRIFT TRUST",                   0 },
    { "TESTAMENTARY TRUST",                  0 },
    { "IRREVOCABLE TRUST",                   0 },
    { "REVOCABLE TRUST",                     0 },
    { "SURVIVORS TRUST",                     0 },
    { "SURVIVOR'S TRUST",                    0 },
    { "EXEMPT TRUST",                        0 },
    { "LEGACY TRUST",                        0 },
    { "HERITAGE TRUST",                      0 },
    { "LIVING TRUST",                        0 },
    /* FAMILY TRUST: keep "FAMILY", strip " TRUST" */
    { "FAMILY TRUST",                        6 }, /* strlen("FAMILY") == 6 */
    { "LAND TRUST",                          0 },
    { "BLIND TRUST",                         0 },
    { "MARITAL TRUST",                       0 },
    { "BYPASS TRUST",                        0 },
    { "CREDIT SHELTER TRUST",               0 },
    { "QTIP TRUST",                          0 },
    { "DYNASTY TRUST",                       0 },
    /* Abbreviated trust types (common estate planning acronyms) */
    { "GRAT",                                0 },
    { "GRUT",                                0 },
    { "CRUT",                                0 },
    { "CRAT",                                0 },
    { "QPRT",                                0 },
    { "ILIT",                                0 },
    /* Catch-all — must be last */
    { "TRUST",                               0 },
    { NULL, 0 }
};

static int strip_trust_suffix(char *s)
{
    int changed = 0;
    int safety = 10;  /* prevent infinite loops on pathological input */

    while (safety-- > 0) {
        int found = 0;
        for (int i = 0; trust_suffixes[i].pattern; i++) {
            char *p = find_word(s, trust_suffixes[i].pattern);
            if (!p) continue;

            size_t pattern_len = strlen(trust_suffixes[i].pattern);
            if (trust_suffixes[i].keep_prefix > 0) {
                /* Erase only the suffix portion (e.g. " TRUST" from
                 * "FAMILY TRUST"), preserving anything that follows. */
                int kp = trust_suffixes[i].keep_prefix;
                str_erase(s, (size_t)(p - s) + (size_t)kp,
                          pattern_len - (size_t)kp);
            } else {
                /* Erase the pattern and any preceding whitespace,
                 * preserving content that follows (e.g. "& co-owner").
                 * This is safer than null-termination which silently
                 * dropped everything past the match. */
                char *erase_start = p;
                while (erase_start > s &&
                       isspace((unsigned char)*(erase_start - 1)))
                    erase_start--;
                size_t erase_len = (size_t)(p - erase_start) + pattern_len;
                str_erase(s, (size_t)(erase_start - s), erase_len);
            }
            rtrim(s);
            found = 1;
            changed = 1;
            break;  /* restart scan — removal may expose new matches */
        }
        if (!found) break;
    }
    return changed;
}

/* =========================================================================
 * Step 3a: Strip trailing "FAMILY" when preceded by first names
 *
 * After strip_trust_suffix keeps "FAMILY" from "FAMILY TRUST", check whether
 * the preceding text is just a surname (keep FAMILY) or a full person name
 * (strip FAMILY — it is noise, not a useful label).
 *
 *   "DONAHUE FAMILY"                            → keep (1 surname)
 *   "LIZ JOU FAMILY"                            → strip → "LIZ JOU"
 *   "MAURICE G & H PATRICIA HARTMAN FAMILY"     → strip → "MAURICE G ..."
 * ====================================================================== */

static int strip_stale_family(char *s)
{
    size_t slen = strlen(s);
    const size_t flen = 6; /* strlen("FAMILY") */
    if (slen <= flen) return 0;

    char *fam = s + slen - flen;
    if (!is_boundary(*(fam - 1))) return 0;
    if (icase_ncmp(fam, "FAMILY", flen) != 0) return 0;

    /* Count "real" words before FAMILY (skip single-letter initials and &) */
    int real_words = 0;
    char *p = s;
    while (p < fam) {
        while (p < fam && (isspace((unsigned char)*p) || *p == '&')) p++;
        if (p >= fam) break;
        char *ws = p;
        while (p < fam && !isspace((unsigned char)*p) && *p != '&') p++;
        size_t wlen = (size_t)(p - ws);
        if (wlen > 1) real_words++;  /* skip single-letter initials */
    }

    if (real_words <= 1) return 0;  /* "DONAHUE FAMILY" — keep */

    /* Strip " FAMILY" (also remove the space before it) */
    char *cut = fam - 1;
    *cut = '\0';
    rtrim(s);
    return 1;
}

/* =========================================================================
 * Step 3b: Strip truncated trust-type suffixes (field-cutoff in source data)
 *
 * County records are sometimes truncated mid-word at a column boundary:
 *   "Gregory Neil Hunter Living Tru"   (TRUST → TRU)
 *   "Alfredo Ontiveros Revocable Tr"   (TRUST → TR)
 *
 * Uses a vocabulary-based approach with iterative right-to-left stripping
 * plus a forward anchor scan, instead of exhaustive compound patterns.
 * This automatically handles ANY combination of trust vocabulary at ANY
 * truncation point.  Adding a new trust type = one vocabulary entry.
 *
 * Two-tier system prevents false positives:
 *   Tier 1 ("anchor"): always stripped (IRREVOCABLE, REVOCABLE, etc.)
 *   Tier 2 ("context"): only stripped after a tier-1 word is found
 *
 * Phase 1: Iterative right-to-left stripping of vocab words/prefixes
 * Phase 2: Forward anchor scan for short trailing fragments
 * Phase 3: Fuzzy SURVIVOR matching (any word starting with SURVI)
 * Phase 4: Standalone LIVING at end of 3+ word name
 * ====================================================================== */

/*
 * Two-tier trust-vocabulary table (used by strip_truncated_suffix).
 *
 * Instead of listing every possible truncation of every compound trust
 * phrase (~170 patterns), we define individual vocabulary words and strip
 * them iteratively from the end of the string, right-to-left.
 *
 * Tier 1 ("anchor") words are NEVER legitimate as the last word of a
 * person or business name — they are always stripped when found at the
 * end of the string.
 *
 * Tier 2 ("context") words COULD be surnames (e.g., Land) so they are
 * only stripped after a Tier 1 word has already been removed.
 *
 * Each entry stores the full word and a minimum prefix length for
 * truncated matching.  If trunc_only is set, the full word is NOT
 * matched here (it is handled by another rule in the pipeline).
 */

typedef struct {
    const char *word;
    int         wlen;       /* strlen(word), cached              */
    int         min_prefix; /* minimum chars for truncated match */
    int         trunc_only; /* 1 = skip full-word match          */
    int         tier;       /* 1 = anchor, 2 = context           */
} TrustVocab;

static const TrustVocab trunc_vocab[] = {
    /* ── Tier 1: anchor words (always safe to strip) ──────────────
     *
     * These words NEVER appear as legitimate person/business name
     * endings in real-estate ownership records.  They are always
     * stripped when found as the last word (or truncated prefix
     * thereof) in the string. */

    /* Core trust modifiers */
    {"IRREVOCABLE",    11, 5, 0, 1},
    {"REVOCABLE",       9, 5, 0, 1},
    {"CHARITABLE",     10, 6, 0, 1},
    {"TESTAMENTARY",   12, 6, 0, 1},
    {"SPENDTHRIFT",    11, 6, 0, 1},
    {"DISCRETIONARY",  13, 8, 0, 1},
    {"UNITRUST",        8, 5, 0, 1},
    {"QTIP",            4, 4, 0, 1},
    {"VIVOS",           5, 3, 0, 1},
    {"GRANTOR",         7, 6, 0, 1},
    {"QUALIFIED",       9, 5, 0, 1},
    {"DYNASTY",         7, 6, 0, 1},
    {"MARITAL",         7, 7, 0, 1},
    {"BYPASS",          6, 5, 0, 1},

    /* Compound-phrase components — never surnames */
    {"INTER",           5, 5, 0, 1},
    {"REMAINDER",       9, 6, 0, 1},
    {"GENERATION",     10, 6, 0, 1},
    {"SKIPPING",        8, 5, 0, 1},
    {"PERSONAL",        8, 5, 0, 1},
    {"RESIDENCE",       9, 6, 0, 1},
    {"ASSET",           5, 5, 0, 1},
    {"PROTECTION",     10, 6, 0, 1},
    {"SPECIAL",         7, 7, 0, 1},
    {"NEEDS",           5, 5, 0, 1},
    {"CREDIT",          6, 6, 0, 1},
    {"SHELTER",         7, 5, 0, 1},

    /* Trust descriptor words — never surnames */
    {"LEGACY",          6, 6, 0, 1},
    {"HERITAGE",        8, 6, 0, 1},
    {"EXEMPT",          6, 6, 0, 1},

    /* TRUST / TRUSTEE — full words handled by strip_trust_suffix
     * and strip_trustee, so only match truncated forms here. */
    {"TRUST",           5, 3, 1, 1},  /* TRU, TRUS           */
    {"TRUSTEE",         7, 6, 1, 1},  /* TRUSTE              */

    /* LIVING — truncated forms LIVI/LIVIN are tier 1 (never names).
     * LIV (3 chars) could be a Scandinavian name so min_prefix=4.
     * Full "LIVING" has its own special-case handler below. */
    {"LIVING",          6, 4, 1, 1},

    /* ── Tier 2: context words (only strip after a tier-1 match) ──
     *
     * These words COULD appear in legitimate business names (e.g.,
     * "COMMUNITY BANK LLC") so they are only stripped once trust
     * context is established by a tier-1 match. */

    /* FAMILY / LAND — full words handled elsewhere (strip_trust_suffix,
     * strip_stale_family), so only truncated forms match here. */
    {"FAMILY",          6, 3, 1, 2},
    {"LAND",            4, 4, 1, 2},

    /* Less common trust types that overlap with business vocabulary */
    {"RETAINED",        8, 6, 0, 2},
    {"ANNUITY",         7, 6, 0, 2},
    {"SUPPLEMENTAL",   12, 6, 0, 2},
    {"INSURANCE",       9, 6, 0, 2},
    {"NOMINEE",         7, 5, 0, 2},
    {"COMMUNITY",       9, 6, 0, 2},
    {"PROPERTY",        8, 6, 0, 2},
    {"HOMESTEAD",       9, 6, 0, 2},
    {"BLIND",           5, 5, 0, 2},
    {"EDUCATION",       9, 6, 0, 2},
    {"MEDICAID",        8, 6, 0, 2},

    {NULL, 0, 0, 0, 0}
};

/*
 * is_trunc_vocab_prefix: returns 1 if the word (of length n) is a prefix
 * of ANY trust vocabulary word.  Used by Phase 2 to verify that
 * short trailing fragments are plausibly truncated trust words.
 */
static int is_trunc_vocab_prefix(const char *w, size_t n)
{
    for (int i = 0; trunc_vocab[i].word; i++) {
        if ((int)n <= trunc_vocab[i].wlen &&
            icase_ncmp(w, trunc_vocab[i].word, n) == 0)
            return 1;
    }
    return 0;
}

static int strip_truncated_suffix(char *s)
{
    int stripped_any = 0;
    int in_trust_ctx = 0;   /* set once a tier-1 word is found */

    /* ── Phase 1: Right-to-left iterative stripping ──────────────
     *
     * Repeatedly strip trust vocabulary words (full or truncated to
     * ≥ min_prefix chars) from the end of the string.  Tier-2 words
     * are only stripped after a tier-1 anchor has been removed. */
    for (;;) {
        size_t slen = strlen(s);
        if (slen == 0) break;

        int found = 0;
        int max_tier = in_trust_ctx ? 2 : 1;

        for (int i = 0; trunc_vocab[i].word; i++) {
            if (trunc_vocab[i].tier > max_tier) continue;

            int wlen = trunc_vocab[i].wlen;
            int minp = trunc_vocab[i].min_prefix;

            /* Try match lengths from full word down to min_prefix */
            for (int trylen = wlen; trylen >= minp; trylen--) {
                /* Skip full-word length if trunc_only is set */
                if (trylen == wlen && trunc_vocab[i].trunc_only) continue;

                if ((size_t)trylen >= slen) continue; /* must leave preceding content */
                char *pos = s + slen - trylen;
                if (!is_boundary(*(pos - 1))) continue;  /* word boundary on left */

                if (icase_ncmp(pos, trunc_vocab[i].word, (size_t)trylen) != 0) continue;

                /* Match — truncate here */
                *pos = '\0';
                rtrim(s);
                found = 1;
                stripped_any = 1;
                if (trunc_vocab[i].tier == 1) in_trust_ctx = 1;
                break;
            }
            if (found) break;
        }
        if (!found) break;
    }

    /* ── Phase 2: Forward anchor scan ────────────────────────────
     *
     * Phase 1 can fail when the rightmost word is too short for any
     * vocabulary entry's min_prefix (e.g., "REVOCABLE T" — the "T"
     * is only 1 char but is clearly truncated TRUST).
     *
     * Phase 2 scans for the leftmost tier-1 anchor word (full match,
     * not the first word) and verifies that ALL trailing words are
     * either full trust-vocabulary matches or short prefixes of one.
     * If verified, everything from the anchor onward is stripped.
     *
     *   "SMITH IRREVOCABLE LIV"
     *   → anchor IRREVOCABLE at word 1, trailing "LIV" is prefix of LIVING
     *   → strip "IRREVOCABLE LIV" → "SMITH"
     *
     *   "SMITH CHARITABLE RE"
     *   → anchor CHARITABLE at word 1, trailing "RE" is prefix of REMAINDER
     *   → strip "CHARITABLE RE" → "SMITH"
     */
    {
        /* Tokenize: collect start/length of each word (max 32 words) */
        struct { char *start; int len; } words[32];
        int nwords = 0;
        char *p = s;
        while (*p && nwords < 32) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            words[nwords].start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            words[nwords].len = (int)(p - words[nwords].start);
            nwords++;
        }

        /* Search for leftmost tier-1 anchor (skip word 0 = the person name) */
        for (int wi = 1; wi < nwords; wi++) {
            int is_anchor = 0;
            for (int vi = 0; trunc_vocab[vi].word; vi++) {
                if (trunc_vocab[vi].tier != 1) continue;
                if (words[wi].len == trunc_vocab[vi].wlen &&
                    icase_ncmp(words[wi].start, trunc_vocab[vi].word,
                               (size_t)trunc_vocab[vi].wlen) == 0) {
                    is_anchor = 1;
                    break;
                }
            }
            if (!is_anchor) continue;

            /* Verify trailing words (wi+1 .. nwords-1) are trust vocab
             * or short prefixes of trust vocab words. */
            int all_ok = 1;
            for (int tj = wi + 1; tj < nwords; tj++) {
                if (!is_trunc_vocab_prefix(words[tj].start,
                                     (size_t)words[tj].len)) {
                    all_ok = 0;
                    break;
                }
            }
            if (!all_ok) continue;

            /* Strip everything from anchor word onward */
            *words[wi].start = '\0';
            rtrim(s);
            stripped_any = 1;
            break;
        }
    }

    /* ── Phase 3: Fuzzy SURVIVOR matching ────────────────────────
     *
     * Any word starting with "SURVI" (5+ chars) at the end of the
     * string is mangled/truncated survivor language.  This catches
     * all data-entry corruption variants (SURVIVIOR, SURVIIVO,
     * SURVIVIOS, etc.) plus any future ones automatically. */
    if (!stripped_any) {
        size_t slen = strlen(s);
        if (slen > 5) {
            char *end = s + slen;
            char *wp = end;
            while (wp > s && !isspace((unsigned char)*(wp - 1))) wp--;
            size_t wlen = (size_t)(end - wp);
            if (wlen >= 5 && wp > s && is_boundary(*(wp - 1)) &&
                icase_ncmp(wp, "SURVI", 5) == 0) {
                *wp = '\0';
                rtrim(s);
                stripped_any = 1;
            }
        }
    }

    /* ── Phase 4: "LIVING" alone at end of a 3+ word name ────────
     *
     * Source data truncated "LIVING TRUST" at the column boundary,
     * leaving only "LIVING".  We only strip if 3+ words to avoid
     * touching the rare "John Living" personal name. */
    if (!stripped_any) {
        size_t slen2 = strlen(s);
        const size_t llen = 6; /* strlen("LIVING") */
        if (slen2 > llen) {
            char *pos2 = s + slen2 - llen;
            if (is_boundary(*(pos2 - 1)) &&
                icase_ncmp(pos2, "LIVING", llen) == 0) {
                int wc = 0;
                for (char *p2 = s; *p2; ) {
                    while (*p2 && isspace((unsigned char)*p2)) p2++;
                    if (!*p2) break;
                    wc++;
                    while (*p2 && !isspace((unsigned char)*p2)) p2++;
                }
                if (wc >= 3) {
                    *pos2 = '\0';
                    rtrim(s);
                    stripped_any = 1;
                }
            }
        }
    }

    return stripped_any;
}

/* =========================================================================
 * Step 4: Strip trustee language
 * ====================================================================== */

static const char *trustee_phrases[] = {
    /* Most specific first */
    "AS SUCCESSOR CO-TRUSTEE",
    "AS SUCCESSOR TRUSTEE",
    "AS CO-TRUSTEE",
    "AS TRUSTEE OF THE",
    "AS TRUSTEE",
    "SUCCESSOR CO-TRUSTEE",
    "SUCCESSOR TRUSTEE",
    "CO-TRUSTEE",
    "TRUSTEE",
    NULL
};

static int strip_trustee(char *s)
{
    int stripped = 0;
    int safety = 0;
    for (;;) {
        int found = 0;
        for (int i = 0; trustee_phrases[i]; i++) {
            char *p = find_word(s, trustee_phrases[i]);
            if (p) {
                str_erase(s, (size_t)(p - s), strlen(trustee_phrases[i]));
                rtrim(s);
                stripped = 1;
                found = 1;
                break; /* restart scan from longest phrase */
            }
        }
        if (!found || ++safety > 10) break;
    }
    return stripped;
}

/* =========================================================================
 * Step 5: Strip leading "THE"
 *
 * Skipped when "FAMILY" is still present in the string — in that case
 * "The Smith Family" is the intended output, not "Smith Family".
 * ====================================================================== */

static int strip_the_prefix(char *s)
{
    /* Preserve "The Smith Family" (standalone family trust name).
     * But when an & is also present ("The X Family & Person"), The is
     * just noise from the trust syntax — strip it there. */
    if (find_word(s, "FAMILY") && !strchr(s, '&')) return 0;

    if (icase_ncmp(s, "THE ", 4) == 0) {
        memmove(s, s + 4, strlen(s + 4) + 1);
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Step 6: Strip junk words/phrases
 * ====================================================================== */

typedef struct { const char *pattern; int flag; } JunkEntry;

static const JunkEntry junk_table[] = {
    /* Real estate deed language */
    { "ET AL.",         NAME_FLAG_WAS_ET_AL  },
    { "ET AL",          NAME_FLAG_WAS_ET_AL  },
    { "ETAL",           NAME_FLAG_WAS_ET_AL  },
    { "ET UX.",         NAME_FLAG_WAS_ET_AL  },
    { "ET UX",          NAME_FLAG_WAS_ET_AL  },
    { "ET VIR.",        NAME_FLAG_WAS_ET_AL  },
    { "ET VIR",         NAME_FLAG_WAS_ET_AL  },
    /* Needs-review patterns — stripped but flagged */
    { "DECEASED",       NAME_FLAG_SUSPICIOUS },
    { "DEC.",           NAME_FLAG_SUSPICIOUS },
    { "ESTATE OF",      NAME_FLAG_SUSPICIOUS },
    { "HEIRS OF",       NAME_FLAG_SUSPICIOUS },
    { "DEVISEES OF",    NAME_FLAG_SUSPICIOUS },
    { "PERSONAL REPRESENTATIVE", NAME_FLAG_SUSPICIOUS },
    { "PR OF",          NAME_FLAG_SUSPICIOUS },
    /* Data-source placeholder values — flag and clear */
    { "NOT AVAILABLE FROM THE DATA SOURCE", NAME_FLAG_SUSPICIOUS },
    { "NOT AVAILABLE",  NAME_FLAG_SUSPICIOUS },
    { "INFORMATION NOT AVAILABLE",          NAME_FLAG_SUSPICIOUS },
    { "UNKNOWN OWNER",  NAME_FLAG_SUSPICIOUS },
    { "UNKNOWN",        NAME_FLAG_SUSPICIOUS },
    { NULL, 0 }
};

static int strip_junk(char *s, int *flags)
{
    int matched = 0;
    int safety = 0;
    for (int i = 0; junk_table[i].pattern; i++) {
        char *p = find_word(s, junk_table[i].pattern);
        if (p) {
            str_erase(s, (size_t)(p - s), strlen(junk_table[i].pattern));
            *flags |= junk_table[i].flag;
            matched = 1;
            if (++safety > 20) break;
            i = -1; /* restart — removal may expose new matches */
        }
    }
    return matched;
}

/* =========================================================================
 * Step 7: Strip embedded street addresses from business entity names.
 *
 * Pattern: [Name words] [3+ digit number] [direction or street suffix] ...
 *   "Debrie Ivey 4660 W Elgin St LLC"  → "Debrie Ivey"
 *
 * Detection: first word is always skipped (must be part of the name).
 * A qualifying number must be followed within 3 words by a known
 * directional (N/S/E/W/NE...) or street type (ST/AVE/BLVD/DR...).
 * ====================================================================== */

static int is_addr_indicator(const char *word, size_t wlen)
{
    static const char *indicators[] = {
        /* Directionals */
        "N", "S", "E", "W", "NE", "NW", "SE", "SW",
        "NORTH", "SOUTH", "EAST", "WEST",
        /* Street type abbreviations */
        "ST", "AVE", "BLVD", "DR", "RD", "LN", "CT",
        "WAY", "PL", "PKWY", "HWY", "CIR", "TRL", "LOOP",
        /* Street type full words */
        "STREET", "AVENUE", "BOULEVARD", "DRIVE", "ROAD",
        "LANE", "COURT", "PLACE", "PARKWAY", "HIGHWAY",
        "CIRCLE", "TRAIL",
        NULL
    };
    if (wlen == 0 || wlen >= 16) return 0;
    char upper[16];
    for (size_t i = 0; i < wlen; i++)
        upper[i] = (char)toupper((unsigned char)word[i]);
    upper[wlen] = '\0';
    for (int i = 0; indicators[i]; i++)
        if (strcmp(upper, indicators[i]) == 0) return 1;
    return 0;
}

static int strip_address_in_name(char *s)
{
    char *p = s;

    /* Skip the first word — it is always part of the owner name */
    while (*p && !isspace((unsigned char)*p)) p++;

    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char *word_start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t wlen = (size_t)(p - word_start);

        /* Check if this word is a 3+ digit number */
        int all_digits = 1;
        for (size_t i = 0; i < wlen; i++) {
            if (!isdigit((unsigned char)word_start[i])) { all_digits = 0; break; }
        }

        if (all_digits && wlen >= 3) {
            /* Look ahead up to 3 words for an address indicator */
            char *scan = p;
            int found = 0;
            for (int wi = 0; wi < 3 && *scan; wi++) {
                while (isspace((unsigned char)*scan)) scan++;
                if (!*scan) break;
                char *ws = scan;
                while (*scan && !isspace((unsigned char)*scan)) scan++;
                if (is_addr_indicator(ws, (size_t)(scan - ws))) { found = 1; break; }
            }
            if (found) {
                *word_start = '\0';
                rtrim(s);
                return 1;
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Step 7b: Collapse space-separated single-letter acronyms
 *
 * Source data sometimes stores acronyms with spaces between letters:
 *   "T N T Trust"  →  "TNT"  (after trust strip: "T N T" → "TNT")
 *
 * Only consecutive uppercase single-letter tokens are collapsed.
 * Non-letter tokens (& , . etc.) break the run, so "J & L" is untouched.
 * ====================================================================== */

static void collapse_spaced_acronym(char *s)
{
    char out[RULES_BUF_MAX];
    size_t oi = 0;
    char *p = s;

    while (*p) {
        /* Skip inter-word whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* Read next token */
        char *wstart = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t wlen = (size_t)(p - wstart);

        if (wlen == 1 && isupper((unsigned char)*wstart)) {
            /* Try to grow a run of consecutive single-uppercase tokens */
            char run[RULES_BUF_MAX];
            size_t rlen = 0;
            run[rlen++] = *wstart;

            char *scan = p;
            while (1) {
                char *sp = scan;
                while (*scan && isspace((unsigned char)*scan)) scan++;
                if (!*scan) break;
                char *nw = scan;
                while (*scan && !isspace((unsigned char)*scan)) scan++;
                size_t nwlen = (size_t)(scan - nw);
                if (nwlen == 1 && isupper((unsigned char)*nw)
                    && rlen < sizeof(run) - 1) {
                    run[rlen++] = *nw;
                    p = scan;
                } else {
                    scan = sp; /* back up — this token is not part of the run */
                    break;
                }
            }

            if (rlen >= 3) {
                if (oi > 0 && oi < sizeof(out) - 1) out[oi++] = ' ';
                if (oi + rlen < sizeof(out)) {
                    memcpy(out + oi, run, rlen);
                    oi += rlen;
                }
            } else {
                /* Short run (1–2 letters): emit each letter separately
                 * so that "D C" stays as "D C", not just "D". */
                for (size_t ri = 0; ri < rlen; ri++) {
                    if (oi > 0 && oi < sizeof(out) - 1) out[oi++] = ' ';
                    if (oi < sizeof(out) - 1) out[oi++] = run[ri];
                }
            }
        } else {
            if (oi > 0 && oi < sizeof(out) - 1) out[oi++] = ' ';
            if (oi + wlen < sizeof(out)) {
                memcpy(out + oi, wstart, wlen);
                oi += wlen;
            }
        }
    }

    out[oi] = '\0';
    memcpy(s, out, oi + 1);
}

/* =========================================================================
 * Steps 8–9: Same-person deduplication and couple combination
 * ====================================================================== */

/*
 * deduplicate_and: "LEFT & RIGHT" where every word in LEFT also appears in
 * RIGHT (same person listed twice after trust stripping) → keep RIGHT.
 *
 *   "MONICA SCOTT & MONICA CORNELIA SCOTT" → "MONICA CORNELIA SCOTT"
 */
static int deduplicate_and(char *s)
{
    char *amp = strchr(s, '&');
    if (!amp) return 0;

    char left[RULES_BUF_MAX];
    size_t llen = (size_t)(amp - s);
    if (llen >= sizeof(left)) return 0;
    memcpy(left, s, llen);
    left[llen] = '\0';
    rtrim(left);

    char *right = amp + 1;
    while (isspace((unsigned char)*right)) right++;
    if (!*left || !*right) return 0;

    /* Case 1: every word in LEFT appears in RIGHT → RIGHT is the fuller name */
    if (all_words_in(right, left)) {
        /* Strip spurious trailing " FAMILY" that was left behind when
         * strip_trust_suffix kept the "FAMILY" prefix of "FAMILY TRUST"
         * (e.g. "Rebecca Fulton & Rebecca S Fulton Family Trust" →
         *  after trust strip: "Rebecca Fulton & Rebecca S Fulton Family" →
         *  deduplicate keeps RIGHT "Rebecca S Fulton Family" → strip FAMILY
         *  → "Rebecca S Fulton").
         * Only fire when LEFT itself did not contain FAMILY (avoids
         * touching "John & Mary Smith Family" which is legitimately named). */
        {
            size_t rlen = strlen(right);
            const size_t flen = 7; /* strlen(" FAMILY") */
            if (rlen > flen &&
                icase_ncmp(right + rlen - flen, " FAMILY", flen) == 0 &&
                !find_word(left, "FAMILY")) {
                right[rlen - flen] = '\0';
            }
        }
        memmove(s, right, strlen(right) + 1);
        return 1;
    }

    /* Case 1b: every word in RIGHT appears in LEFT → LEFT is the fuller name.
     * "John Vizzard III & John Vizzard" → keep "John Vizzard III" */
    if (all_words_in(left, right)) {
        *amp = '\0';
        rtrim(s);
        return 1;
    }

    /* Cases 2 & 2b: RIGHT is a single word — either an exact match for a
     * word already in LEFT, or a plural/apostrophe-dropped form of one
     * (e.g. "Lynn Kepford & Kepford" or "Luvilla Sturgill & Luvillas").
     * Drop the & fragment and keep LEFT. */
    {
        char *rp = right;
        int rwords = 0;
        while (*rp) {
            while (isspace((unsigned char)*rp)) rp++;
            if (!*rp) break;
            rwords++;
            while (*rp && !isspace((unsigned char)*rp)) rp++;
        }

        if (rwords == 1) {
            /* Case 2: exact word match */
            if (all_words_in(left, right)) {
                *amp = '\0';
                rtrim(s);
                return 1;
            }

            /* Case 2b: right word is LEFT-word + trailing "S"
             * (handles "Luvillas" from "Luvilla's Trust" with apostrophe
             * stripped in the source data — the boundary check would fail
             * for an exact match, so we test the stem). */
            size_t rwlen = strlen(right);
            if (rwlen >= 4 &&
                tolower((unsigned char)right[rwlen - 1]) == 's') {
                char stem[RULES_BUF_MAX];
                memcpy(stem, right, rwlen - 1);
                stem[rwlen - 1] = '\0';
                if (find_word(left, stem)) {
                    *amp = '\0';
                    rtrim(s);
                    return 1;
                }
            }
        }
    }

    return 0;
}

/*
 * combine_couple: "FIRST_A LAST & FIRST_B LAST" (shared last name, both
 * sides exactly two words) → "FIRST_A & FIRST_B LAST".
 *
 *   "JOE SCHMO & HILLARY SCHMO" → "JOE & HILLARY SCHMO"
 */
static int combine_couple(char *s, size_t slen)
{
    char *amp = strchr(s, '&');
    if (!amp) return 0;

    char left[RULES_BUF_MAX], right[RULES_BUF_MAX];

    size_t llen = (size_t)(amp - s);
    if (llen >= sizeof(left)) return 0;
    memcpy(left, s, llen);
    left[llen] = '\0';
    { int i = (int)strlen(left) - 1;
      while (i >= 0 && isspace((unsigned char)left[i])) left[i--] = '\0'; }

    char *rp = amp + 1;
    while (isspace((unsigned char)*rp)) rp++;
    snprintf(right, sizeof(right), "%s", rp);
    { int i = (int)strlen(right) - 1;
      while (i >= 0 && isspace((unsigned char)right[i])) right[i--] = '\0'; }

    if (!*left || !*right) return 0;

    /* Tokenize left — must be exactly 2 words */
    char *lw[32]; int lwc = 0;
    { char *p = left;
      while (*p && lwc < 32) {
          while (isspace((unsigned char)*p)) p++;
          if (!*p) break;
          lw[lwc++] = p;
          while (*p && !isspace((unsigned char)*p)) p++;
          if (*p) *p++ = '\0';
      }
    }
    if (lwc != 2) return 0;

    /* Tokenize right — must be exactly 2 words */
    char *rw[32]; int rwc = 0;
    { char *p = right;
      while (*p && rwc < 32) {
          while (isspace((unsigned char)*p)) p++;
          if (!*p) break;
          rw[rwc++] = p;
          while (*p && !isspace((unsigned char)*p)) p++;
          if (*p) *p++ = '\0';
      }
    }
    if (rwc != 2) return 0;

    /* Last names must match */
    if (!str_ieq(lw[1], rw[1])) return 0;

    /* Build "FIRST_A & FIRST_B LAST" */
    int written = snprintf(s, slen, "%s & %s %s", lw[0], rw[0], rw[1]);
    return written > 0;
}

/* =========================================================================
 * rules_apply: Main entry point
 * ====================================================================== */

int rules_apply(char *s, size_t slen)
{
    if (!s || !*s) return NAME_FLAG_EMPTY;

    int flags = 0;

    /* Uppercase the working string */
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);

#ifdef DEBUG
    if (g_dbg) fprintf(g_dbg, "\nIN:  \"%s\"\n", s);
#endif

    /* Apply rules in order */
    if (strip_co(s, &flags))            dlog("strip_co", s);
    if (strip_and_trust(s, &flags))     dlog("strip_and_trust", s);
    if (extract_cotenant(s, &flags))    dlog("extract_cotenant", s);

    if (strip_date_clause(s))      { flags |= NAME_FLAG_WAS_TRUST; dlog("strip_date_clause", s); }
    if (strip_trust_suffix(s))     { flags |= NAME_FLAG_WAS_TRUST; dlog("strip_trust_suffix", s); }
    if (strip_stale_family(s))     dlog("strip_stale_family", s);
    if (strip_truncated_suffix(s)) { flags |= NAME_FLAG_WAS_TRUST; dlog("strip_truncated_suffix", s); }
    if (strip_trustee(s))          { flags |= NAME_FLAG_WAS_TRUST; dlog("strip_trustee", s); }

    /* After trust stripping, there may be residual "OF THE" fragments */
    {
        char *p;
        int changed = 0;
        while ((p = find_word(s, "OF THE")) != NULL)
            { str_erase(s, (size_t)(p - s), 6); changed = 1; }
        /* Strip trailing "OF" — scan backwards to find the one at the end */
        {
            size_t sl = strlen(s);
            while (sl >= 2) {
                /* Check if the last word is "OF" */
                const char *end = s + sl;
                const char *candidate = end - 2;
                if (icase_ncmp(candidate, "OF", 2) == 0 &&
                    (candidate == s || is_boundary(*(candidate - 1))) &&
                    *(candidate + 2) == '\0') {
                    str_erase(s, (size_t)(candidate - s), 2);
                    rtrim(s);
                    sl = strlen(s);
                    changed = 1;
                } else {
                    break;
                }
            }
        }
        if (changed) dlog("strip_of_the", s);
    }

    if (strip_the_prefix(s))           dlog("strip_the_prefix (1)", s);
    if (strip_junk(s, &flags))         dlog("strip_junk", s);
    if (strip_address_in_name(s))      dlog("strip_address_in_name", s);
    collapse_spaced_acronym(s); /* "T N T" → "TNT"                          */
    if (deduplicate_and(s))            dlog("deduplicate_and", s);
    if (strip_the_prefix(s))           dlog("strip_the_prefix (2)", s);
    if (combine_couple(s, slen))       dlog("combine_couple", s);

    /* Final cleanup */
    collapse_spaces(s);
    rtrim(s);
    char *trimmed = ltrim(s);
    if (trimmed != s) memmove(s, trimmed, strlen(trimmed) + 1);

    if (!*s) flags |= NAME_FLAG_EMPTY;

#ifdef DEBUG
    if (g_dbg) fprintf(g_dbg, "OUT: \"%s\"\n", s);
#endif

    /* -----------------------------------------------------------------------
     * Post-clean AI detection
     *
     * Flag patterns that the rule engine cannot fix but a small LLM can:
     *   - 2–4 word name ending with a single letter → likely reversed format
     *     e.g. "Sirakis Derek M"  (Last First Initial)
     * --------------------------------------------------------------------- */
    if (!(flags & NAME_FLAG_EMPTY)) {
        char *wptr[8];
        int   wc = 0;
        char  scan[RULES_BUF_MAX];
        snprintf(scan, sizeof(scan), "%s", s);
        char *sp = scan;
        while (*sp && wc < 8) {
            while (*sp && isspace((unsigned char)*sp)) sp++;
            if (!*sp) break;
            wptr[wc++] = sp;
            while (*sp && !isspace((unsigned char)*sp)) sp++;
            if (*sp) *sp++ = '\0';
        }
        if (wc >= 3 && wc <= 4) {
            const char *last = wptr[wc - 1];
            if (strlen(last) == 1 && isalpha((unsigned char)last[0]))
                flags |= NAME_FLAG_NEEDS_AI;
        }
    }

    return flags;
}
