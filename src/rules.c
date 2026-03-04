#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rules.h"

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
    char tmp[512];
    strncpy(tmp, needle, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

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

        char right_name[512];
        strncpy(right_name, after, sizeof(right_name) - 1);
        right_name[sizeof(right_name) - 1] = '\0';

        /* Quick-strip trust keywords from the right-side copy so we are
         * left with just the person name for comparison. */
        static const char *tkw[] = {
            "REVOCABLE LIVING TRUST", "IRREVOCABLE LIVING TRUST",
            "REVOCABLE INTER VIVOS TRUST", "IRREVOCABLE INTER VIVOS TRUST",
            "INTER VIVOS TRUST", "LIVING TRUST",
            "IRREVOCABLE TRUST", "REVOCABLE TRUST",
            "FAMILY TRUST", "LAND TRUST", "BLIND TRUST",
            "MARITAL TRUST", "BYPASS TRUST", "DYNASTY TRUST",
            "TRUST", NULL
        };
        for (int ki = 0; tkw[ki]; ki++) {
            char *tp = find_word(right_name, tkw[ki]);
            if (tp) { *tp = '\0'; break; }
        }
        rtrim(right_name);

        /* Build the left-side name for comparison. */
        char left[512];
        size_t llen = (size_t)(last_amp - s);
        if (llen < sizeof(left)) {
            memcpy(left, s, llen);
            left[llen] = '\0';
            rtrim(left);

            /* If the right-side person name is a superset of the left name,
             * use it — it is the same person with more detail. */
            if (*right_name && all_words_in(right_name, left)) {
                size_t rlen = strlen(right_name);
                memmove(s, right_name, rlen + 1);
                *flags |= NAME_FLAG_WAS_TRUST;
                return 1;
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
 * name (no trust language), decide which to keep:
 *
 *   "Tssk Trust & Susan Shelton"                → "Susan Shelton"
 *   "Demarino Trust & Christine Demarino"       → "Christine Demarino"
 *   "The Cimarrons Living Trust & Terri Cutting"→ "Terri Cutting"
 *   "Melody And Michael Morgan Family Trust
 *      & Melody Morgan"                         → keep left (left is richer)
 *   "The Ardell And Kari Witt Family Trust
 *      & Ardell Witt Jr"                        → keep left (left has more names)
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
    char left[512];
    size_t llen = (size_t)(amp - s);
    if (llen >= sizeof(left)) return 0;
    memcpy(left, s, llen);
    left[llen] = '\0';
    if (!find_word(left, "TRUST")) return 0;

    /* Right side must NOT contain trust language */
    if (find_word(after_amp, "TRUST")) return 0;

    /* Build and trim the right side */
    char right[512];
    strncpy(right, after_amp, sizeof(right) - 1);
    right[sizeof(right) - 1] = '\0';
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
    static const char *tkw[] = {
        "REVOCABLE LIVING TRUST", "IRREVOCABLE LIVING TRUST",
        "REVOCABLE INTER VIVOS TRUST", "IRREVOCABLE INTER VIVOS TRUST",
        "INTER VIVOS TRUST", "LIVING TRUST",
        "IRREVOCABLE TRUST", "REVOCABLE TRUST",
        "FAMILY TRUST", "LAND TRUST", "BLIND TRUST",
        "MARITAL TRUST", "BYPASS TRUST", "DYNASTY TRUST",
        "TRUST", NULL
    };
    for (int ki = 0; tkw[ki]; ki++) {
        char *tp = find_word(left, tkw[ki]);
        if (tp) { *tp = '\0'; break; }
    }
    rtrim(left);

    /* Decision 1: right's words ⊆ left_base → left is richer, let
     * strip_trust_suffix handle it normally */
    if (*left && all_words_in(left, right)) return 0;

    /* Decision 2: left_base's words ⊆ right → right is the person name */
    if (*left && all_words_in(right, left)) {
        memmove(s, right, strlen(right) + 1);
        *flags |= NAME_FLAG_WAS_TRUST;
        return 1;
    }

    /* Decision 3: left_base has more words → left is more informative
     * (e.g. "Ardell and Kari Witt" vs just "Ardell Witt Jr") */
    int lwords = 0;
    { char *lp = left;
      while (*lp) {
          while (isspace((unsigned char)*lp)) lp++;
          if (!*lp) break;
          lwords++;
          while (*lp && !isspace((unsigned char)*lp)) lp++;
      }
    }
    if (lwords > rwords) return 0;

    /* Default: use right (person name is more useful than trust acronym) */
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
    { "REVOCABLE LIVING TRUST",              0 },
    { "IRREVOCABLE LIVING TRUST",            0 },
    { "REVOCABLE INTER VIVOS TRUST",         0 },
    { "IRREVOCABLE INTER VIVOS TRUST",       0 },
    { "INTER VIVOS TRUST",                   0 },
    { "CHARITABLE REMAINDER UNITRUST",       0 },
    { "CHARITABLE REMAINDER TRUST",          0 },
    { "QUALIFIED PERSONAL RESIDENCE TRUST",  0 },
    { "GENERATION SKIPPING TRUST",           0 },
    { "GENERATION-SKIPPING TRUST",           0 },
    { "ASSET PROTECTION TRUST",              0 },
    { "SPECIAL NEEDS TRUST",                 0 },
    { "DISCRETIONARY TRUST",                 0 },
    { "SPENDTHRIFT TRUST",                   0 },
    { "TESTAMENTARY TRUST",                  0 },
    { "IRREVOCABLE TRUST",                   0 },
    { "REVOCABLE TRUST",                     0 },
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
    /* Catch-all — must be last */
    { "TRUST",                               0 },
    { NULL, 0 }
};

static int strip_trust_suffix(char *s)
{
    for (int i = 0; trust_suffixes[i].pattern; i++) {
        char *p = find_word(s, trust_suffixes[i].pattern);
        if (p) {
            if (trust_suffixes[i].keep_prefix > 0) {
                /* Erase only the suffix portion (e.g. " TRUST" from
                 * "FAMILY TRUST"), preserving anything that follows
                 * (e.g. "& co-owner after the trust name").
                 * Old behaviour null-terminated here which silently
                 * dropped everything past the match. */
                int kp = trust_suffixes[i].keep_prefix;
                size_t pattern_len = strlen(trust_suffixes[i].pattern);
                str_erase(s, (size_t)(p - s) + (size_t)kp,
                          pattern_len - (size_t)kp);
            } else {
                *p = '\0';
            }
            rtrim(s);
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Step 3b: Strip truncated trust-type suffixes (field-cutoff in source data)
 *
 * County records are sometimes truncated mid-word at a column boundary:
 *   "Gregory Neil Hunter Living Tru"   (TRUST → TRU)
 *   "Alfredo Ontiveros Revocable Tr"   (TRUST → TR)
 *
 * Patterns are matched ONLY at the END of the string (suffix-anchored)
 * so they cannot fire in the middle of a legitimate name.
 * ====================================================================== */

static int strip_truncated_suffix(char *s)
{
    static const char *patterns[] = {
        /*
         * Compound-phrase truncations — longest / most-specific first so that
         * a long-prefix match fires before a shorter suffix-only pattern does.
         */

        /* IRREVOCABLE INTER VIVOS TRUST — all truncation points */
        "IRREVOCABLE INTER VIVOS TRUS", "IRREVOCABLE INTER VIVOS TRU",
        "IRREVOCABLE INTER VIVOS TR",   "IRREVOCABLE INTER VIVOS T",
        "IRREVOCABLE INTER VIVOS",
        "IRREVOCABLE INTER VIVO",       "IRREVOCABLE INTER VIV",
        "IRREVOCABLE INTER VI",         "IRREVOCABLE INTER V",
        "IRREVOCABLE INTER",

        /* REVOCABLE INTER VIVOS TRUST — all truncation points */
        "REVOCABLE INTER VIVOS TRUS", "REVOCABLE INTER VIVOS TRU",
        "REVOCABLE INTER VIVOS TR",   "REVOCABLE INTER VIVOS T",
        "REVOCABLE INTER VIVOS",
        "REVOCABLE INTER VIVO",       "REVOCABLE INTER VIV",
        "REVOCABLE INTER VI",         "REVOCABLE INTER V",
        "REVOCABLE INTER",

        /* INTER VIVOS TRUST — all truncation points */
        "INTER VIVOS TRUS", "INTER VIVOS TRU", "INTER VIVOS TR",
        "INTER VIVOS T",    "INTER VIVOS",
        "INTER VIVO",       "INTER VIV",
        "INTER VI",         "INTER V",

        /* IRREVOCABLE LIVING TRUST — all truncation points */
        "IRREVOCABLE LIVING TRUS", "IRREVOCABLE LIVING TRU",
        "IRREVOCABLE LIVING TR",   "IRREVOCABLE LIVING T",
        "IRREVOCABLE LIVING",
        "IRREVOCABLE LIVIN",       "IRREVOCABLE LIVI",
        "IRREVOCABLE LIV",         "IRREVOCABLE LI",
        "IRREVOCABLE L",

        /* REVOCABLE LIVING TRUST — all truncation points */
        "REVOCABLE LIVING TRUS", "REVOCABLE LIVING TRU",
        "REVOCABLE LIVING TR",   "REVOCABLE LIVING T",
        "REVOCABLE LIVING",
        "REVOCABLE LIVIN",       "REVOCABLE LIVI",
        "REVOCABLE LIV",         "REVOCABLE LI",
        "REVOCABLE L",

        /* IRREVOCABLE TRUST — truncated within TRUST */
        "IRREVOCABLE TRUS", "IRREVOCABLE TRU", "IRREVOCABLE TR", "IRREVOCABLE T",

        /* REVOCABLE TRUST — truncated within TRUST */
        "REVOCABLE TRUS", "REVOCABLE TRU", "REVOCABLE TR", "REVOCABLE T",

        /* LIVING TRUST — truncated within TRUST */
        "LIVING TRUS", "LIVING TRU", "LIVING TR", "LIVING T",

        /* FAMILY TRUST — truncated (FAMILY alone is preserved by strip_trust_suffix) */
        "FAMILY TRUS", "FAMILY TRU", "FAMILY TR", "FAMILY T",

        /* LAND TRUST — truncated */
        "LAND TRUS", "LAND TRU", "LAND TR", "LAND T",

        /* CHARITABLE REMAINDER UNITRUST — all truncation points */
        "CHARITABLE REMAINDER UNITRUS", "CHARITABLE REMAINDER UNITRU",
        "CHARITABLE REMAINDER UNITR",   "CHARITABLE REMAINDER UNITU",
        "CHARITABLE REMAINDER UNIT",    "CHARITABLE REMAINDER UNI",
        "CHARITABLE REMAINDER UN",      "CHARITABLE REMAINDER U",
        /* CHARITABLE REMAINDER TRUST — all truncation points */
        "CHARITABLE REMAINDER TRUS", "CHARITABLE REMAINDER TRU",
        "CHARITABLE REMAINDER TR",   "CHARITABLE REMAINDER T",
        "CHARITABLE REMAINDER",      "CHARITABLE REMAINDE",
        "CHARITABLE REMAIND",        "CHARITABLE REMAIN",
        "CHARITABLE REMAI",          "CHARITABLE REMA",
        "CHARITABLE REM",            "CHARITABLE RE",
        "CHARITABLE R",              "CHARITABLE",

        /* GENERATION SKIPPING TRUST — all truncation points */
        "GENERATION SKIPPING TRUS", "GENERATION SKIPPING TRU",
        "GENERATION SKIPPING TR",   "GENERATION SKIPPING T",
        "GENERATION SKIPPING",      "GENERATION SKIPPIN",
        "GENERATION SKIPPI",        "GENERATION SKIPP",
        "GENERATION SKIP",

        /* QUALIFIED PERSONAL RESIDENCE TRUST — all truncation points */
        "QUALIFIED PERSONAL RESIDENCE TRUS", "QUALIFIED PERSONAL RESIDENCE TRU",
        "QUALIFIED PERSONAL RESIDENCE TR",   "QUALIFIED PERSONAL RESIDENCE T",
        "QUALIFIED PERSONAL RESIDENCE",      "QUALIFIED PERSONAL RESIDENC",
        "QUALIFIED PERSONAL RESIDEN",        "QUALIFIED PERSONAL RESIDE",
        "QUALIFIED PERSONAL RESID",          "QUALIFIED PERSONAL RESI",
        "QUALIFIED PERSONAL RES",            "QUALIFIED PERSONAL RE",
        "QUALIFIED PERSONAL R",              "QUALIFIED PERSONAL",
        "QUALIFIED PERSONA",                 "QUALIFIED PERSON",

        /* ASSET PROTECTION TRUST — all truncation points */
        "ASSET PROTECTION TRUS", "ASSET PROTECTION TRU",
        "ASSET PROTECTION TR",   "ASSET PROTECTION T",
        "ASSET PROTECTION",      "ASSET PROTECTIO",
        "ASSET PROTECTI",        "ASSET PROTECT",
        "ASSET PROTEC",          "ASSET PROTE",
        "ASSET PROT",

        /* SPECIAL NEEDS TRUST — truncated */
        "SPECIAL NEEDS TRUS", "SPECIAL NEEDS TRU",
        "SPECIAL NEEDS TR",   "SPECIAL NEEDS T",
        "SPECIAL NEEDS",      "SPECIAL NEED",

        /* SPENDTHRIFT TRUST — all truncation points */
        "SPENDTHRIFT TRUS", "SPENDTHRIFT TRU", "SPENDTHRIFT TR", "SPENDTHRIFT T",
        "SPENDTHRIFT",      "SPENDTHRIF",      "SPENDTHRI",
        "SPENDTHR",         "SPENDTH",         "SPENDT",

        /* TESTAMENTARY TRUST — all truncation points */
        "TESTAMENTARY TRUS", "TESTAMENTARY TRU", "TESTAMENTARY TR", "TESTAMENTARY T",
        "TESTAMENTARY",      "TESTAMENTAR",      "TESTAMENTA",
        "TESTAMENT",         "TESTAMEN",         "TESTAME",

        /* DISCRETIONARY TRUST — all truncation points */
        "DISCRETIONARY TRUS", "DISCRETIONARY TRU", "DISCRETIONARY TR", "DISCRETIONARY T",
        "DISCRETIONARY",      "DISCRETIONAR",      "DISCRETIONA",
        "DISCRETION",         "DISCRETIO",         "DISCRETI",

        /* CREDIT SHELTER TRUST — all truncation points */
        "CREDIT SHELTER TRUS", "CREDIT SHELTER TRU", "CREDIT SHELTER TR", "CREDIT SHELTER T",
        "CREDIT SHELTER",      "CREDIT SHELTE",      "CREDIT SHELT",
        "CREDIT SHEL",         "CREDIT SHE",         "CREDIT SH",

        /* DYNASTY TRUST — all truncation points */
        "DYNASTY TRUS", "DYNASTY TRU", "DYNASTY TR", "DYNASTY T",
        "DYNASTY",      "DYNAST",

        /* MARITAL TRUST — truncated */
        "MARITAL TRUS", "MARITAL TRU", "MARITAL TR", "MARITAL T",

        /* BYPASS TRUST — truncated */
        "BYPASS TRUS", "BYPASS TRU", "BYPASS TR", "BYPASS T", "BYPAS",

        /* QTIP TRUST — truncated */
        "QTIP TRUS", "QTIP TRU", "QTIP TR", "QTIP T",

        /*
         * Single-word truncations and full-word non-name trust terms.
         * None of these are components of a legitimate person or business name.
         */

        /* IRREVOCABLE — full word at column boundary, then each truncation */
        "IRREVOCABLE",
        "IRREVOCABL", "IRREVOCAB", "IRREVOCA", "IRREVOC", "IRREVO", "IRREV",

        /* REVOCABLE — full word at column boundary, then each truncation */
        "REVOCABLE",
        "REVOCABL",  "REVOCAB",  "REVOCA",  "REVOC",

        /* TRUSTEE truncated (full TRUST caught by trust_suffixes catch-all) */
        "TRUSTE",

        /* TRUST truncated */
        "TRUS", "TRU",

        /* LIVING truncated below 6 chars (LIVING alone → special case below) */
        "LIVIN", "LIVI",

        /*
         * Mangled / misspelled SURVIVOR(S) — data-entry corruption of
         * "Survivor's Trust" / "Survivors Trust" at the end of the name.
         * Ordered longest-to-shortest within each corruption family.
         */
        "SURVIVIORS", "SURVIVIOR",   /* extra I inserted (surviv-I-or)   */
        "SURVIIVORS", "SURVIIVOS",   /* doubled II                        */
        "SURVIIVO",                  /* doubled II, short form            */
        "SURVIVIOS",  "SURVIVIO",    /* IO ending corruption              */
        "SURVIVRS",                  /* dropped O                         */
        "SURVIVOS",   "SURVIVO",     /* transposition / cutoff            */
        "SURVIVORS",                 /* full word — "SURVIVORS TRUST" cut */
        "SURVIV",                    /* truncated at 6 chars              */
        "SURVI",                     /* truncated at 5 chars              */
        NULL
    };

    size_t slen = strlen(s);
    for (int i = 0; patterns[i]; i++) {
        size_t plen = strlen(patterns[i]);
        if (plen >= slen) continue;          /* must have a preceding name word */
        const char *pos = s + slen - plen;
        if (!is_boundary(*(pos - 1))) continue;  /* word boundary on the left */
        if (icase_ncmp(pos, patterns[i], plen) != 0) continue;
        *(char *)pos = '\0';
        rtrim(s);
        return 1;
    }

    /* Special case: "LIVING" alone at end of a 3+ word name.
     * This happens when the source data truncated "LIVING TRUST" at the
     * column boundary leaving only the first word:
     *   "Marie Valente & The Marie Anne Elisabeth Valente Living"
     * We only strip it if there are 3+ words to avoid touching the rare
     * "John Living" two-word personal name. */
    {
        size_t slen2 = strlen(s);
        const size_t llen = 6; /* strlen("LIVING") */
        if (slen2 > llen) {
            const char *pos2 = s + slen2 - llen;
            if (is_boundary(*(pos2 - 1)) &&
                icase_ncmp(pos2, "LIVING", llen) == 0) {
                int wc = 0;
                for (const char *p = s; *p; ) {
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (!*p) break;
                    wc++;
                    while (*p && !isspace((unsigned char)*p)) p++;
                }
                if (wc >= 3) {
                    *(char *)pos2 = '\0';
                    rtrim(s);
                    return 1;
                }
            }
        }
    }

    return 0;
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
    for (int i = 0; trustee_phrases[i]; i++) {
        char *p = find_word(s, trustee_phrases[i]);
        if (p) {
            str_erase(s, (size_t)(p - s), strlen(trustee_phrases[i]));
            rtrim(s);
            return 1;
        }
    }
    return 0;
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
    for (int i = 0; junk_table[i].pattern; i++) {
        char *p = find_word(s, junk_table[i].pattern);
        if (p) {
            str_erase(s, (size_t)(p - s), strlen(junk_table[i].pattern));
            *flags |= junk_table[i].flag;
            matched = 1;
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
    char out[512];
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
            char run[512];
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
                if (nwlen == 1 && isupper((unsigned char)*nw)) {
                    run[rlen++] = *nw;
                    p = scan;
                } else {
                    scan = sp; /* back up — this token is not part of the run */
                    break;
                }
            }

            if (rlen >= 2) {
                if (oi > 0) out[oi++] = ' ';
                memcpy(out + oi, run, rlen);
                oi += rlen;
            } else {
                if (oi > 0) out[oi++] = ' ';
                out[oi++] = *wstart;
            }
        } else {
            if (oi > 0) out[oi++] = ' ';
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

    char left[512];
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
                char stem[512];
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

    char left[512], right[512];

    size_t llen = (size_t)(amp - s);
    if (llen >= sizeof(left)) return 0;
    memcpy(left, s, llen);
    left[llen] = '\0';
    { int i = (int)strlen(left) - 1;
      while (i >= 0 && isspace((unsigned char)left[i])) left[i--] = '\0'; }

    char *rp = amp + 1;
    while (isspace((unsigned char)*rp)) rp++;
    strncpy(right, rp, sizeof(right) - 1);
    right[sizeof(right) - 1] = '\0';
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
    if (strip_truncated_suffix(s)) { flags |= NAME_FLAG_WAS_TRUST; dlog("strip_truncated_suffix", s); }
    if (strip_trustee(s))          { flags |= NAME_FLAG_WAS_TRUST; dlog("strip_trustee", s); }

    /* After trust stripping, there may be residual "OF THE" fragments */
    {
        char *p;
        int changed = 0;
        while ((p = find_word(s, "OF THE")) != NULL)
            { str_erase(s, (size_t)(p - s), 6); changed = 1; }
        while ((p = find_word(s, "OF")) != NULL && *(p + 2) == '\0')
            { str_erase(s, (size_t)(p - s), 2); changed = 1; }
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
        char  scan[512];
        strncpy(scan, s, sizeof(scan) - 1);
        scan[sizeof(scan) - 1] = '\0';
        char *sp = scan;
        while (*sp && wc < 8) {
            while (*sp && isspace((unsigned char)*sp)) sp++;
            if (!*sp) break;
            wptr[wc++] = sp;
            while (*sp && !isspace((unsigned char)*sp)) sp++;
            if (*sp) *sp++ = '\0';
        }
        if (wc >= 2 && wc <= 4) {
            const char *last = wptr[wc - 1];
            if (strlen(last) == 1 && isalpha((unsigned char)last[0]))
                flags |= NAME_FLAG_NEEDS_AI;
        }
    }

    return flags;
}
