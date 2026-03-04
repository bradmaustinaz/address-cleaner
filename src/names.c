#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "names.h"
#include "rules.h"
#include "llm.h"

/* =========================================================================
 * Exact-form table
 *
 * Words that must be rendered in a specific canonical form — checked
 * before generic title-case logic.  Keys are lowercase; values are the
 * exact string to emit (may be longer than the key, e.g. adds a period).
 * ====================================================================== */

typedef struct { const char *key; const char *canonical; } ExactForm;

static const ExactForm exact_forms[] = {
    /* Personal titles */
    {"mr",    "Mr."},   {"mr.",   "Mr."},
    {"mrs",   "Mrs."},  {"mrs.",  "Mrs."},
    {"ms",    "Ms."},   {"ms.",   "Ms."},
    {"miss",  "Miss"},
    {"dr",    "Dr."},   {"dr.",   "Dr."},
    {"rev",   "Rev."},  {"rev.",  "Rev."},
    {"prof",  "Prof."}, {"prof.", "Prof."},
    {"hon",   "Hon."},  {"hon.",  "Hon."},
    /* Military / professional titles */
    {"capt",  "Capt."}, {"capt.", "Capt."},
    {"lt",    "Lt."},   {"lt.",   "Lt."},
    {"maj",   "Maj."},  {"maj.",  "Maj."},
    {"col",   "Col."},  {"col.",  "Col."},
    {"gen",   "Gen."},  {"gen.",  "Gen."},
    {"sgt",   "Sgt."},  {"sgt.",  "Sgt."},
    {"cpl",   "Cpl."},  {"cpl.",  "Cpl."},
    {"pvt",   "Pvt."},  {"pvt.",  "Pvt."},
    /* Name suffixes */
    {"jr",    "Jr."},   {"jr.",   "Jr."},
    {"sr",    "Sr."},   {"sr.",   "Sr."},
    {"esq",   "Esq."}, {"esq.",  "Esq."},
    /* Roman numeral generational suffixes — stay uppercase */
    {"ii",    "II"},
    {"iii",   "III"},
    {"iv",    "IV"},
    {"vi",    "VI"},
    {"vii",   "VII"},
    {"viii",  "VIII"},
    {"ix",    "IX"},
    {"xi",    "XI"},
    {"xii",   "XII"},
    {NULL, NULL}
};

static const char *exact_form_lookup(const char *lower_key)
{
    for (int i = 0; exact_forms[i].key; i++)
        if (strcmp(exact_forms[i].key, lower_key) == 0)
            return exact_forms[i].canonical;
    return NULL;
}

/* =========================================================================
 * Upper / lower exception word lists
 * ====================================================================== */

/* Business entity abbreviations — always UPPERCASE */
static const char *upper_words[] = {
    "LLC", "LLP", "LP", "INC", "CORP", "LTD", "CO",
    "PC",  "PLC", "NA", "FSB", "PLLC", "DBA", "NV",
    "USA", "US",  "PO", "TNT",
    NULL
};

/* Articles / conjunctions / short prepositions — lowercase unless first word */
static const char *lower_words[] = {
    "a", "an", "the",
    "of", "in", "on", "at", "to", "by", "as",
    "for", "and", "or", "but", "nor", "so",
    NULL
};

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int in_table(const char *word, const char **table)
{
    for (int i = 0; table[i]; i++)
        if (str_ieq(word, table[i])) return 1;
    return 0;
}

/* =========================================================================
 * apply_title_case
 *
 * Builds the result into a temporary buffer so that exact-form substitutions
 * that change the string length (e.g. "MR" → "Mr.") work correctly.
 *
 * Priority order per word:
 *   1. Exact-form table  (Mr., Jr., II, III, ...)
 *   2. Business abbrev   (LLC, INC, ... → stay uppercase)
 *   3. Lower exceptions  (of, and, ... → lowercase unless first word)
 *   4. Standard title case with Mc/Mac/O'/hyphen handling
 * ====================================================================== */
static void apply_title_case(char *s)
{
    if (!*s) return;

    char result[NAME_MAX_LEN];
    size_t res_pos = 0;
    int word_idx = 0;
    char *p = s;

    while (*p) {
        /* Copy inter-word whitespace verbatim */
        while (*p && isspace((unsigned char)*p)) {
            if (res_pos < NAME_MAX_LEN - 1) result[res_pos++] = *p;
            p++;
        }
        if (!*p) break;

        /* Delimit word */
        char *word_start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char saved = *p;
        *p = '\0';

        size_t wlen = strlen(word_start);

        /* Build lowercase key */
        char lower[NAME_MAX_LEN];
        size_t i;
        for (i = 0; word_start[i] && i < sizeof(lower) - 1; i++)
            lower[i] = (char)tolower((unsigned char)word_start[i]);
        lower[i] = '\0';

        const char *emit = NULL; /* points to the string we'll write */

        /* --- 1. Exact form -------------------------------------------- */
        emit = exact_form_lookup(lower);

        /* --- 2. Business abbreviation (force uppercase, e.g. LLC) ------ */
        if (!emit && in_table(word_start, upper_words)) {
            for (i = 0; i < wlen; i++)
                word_start[i] = (char)toupper((unsigned char)word_start[i]);
            emit = word_start;
        }

        /* --- 3. Single-letter word = initial, always uppercase ---------- */
        if (!emit && wlen == 1 && isalpha((unsigned char)lower[0])) {
            word_start[0] = (char)toupper((unsigned char)word_start[0]);
            emit = word_start;
        }

        /* --- 4. Lower exception (articles/prepositions) ---------------- */
        if (!emit && word_idx > 0 && in_table(lower, lower_words)) {
            memcpy(word_start, lower, wlen + 1);
            emit = word_start;
        }

        /* --- 4. Standard title case ------------------------------------ */
        if (!emit) {
            /* Save original for Mac-prefix detection (original is ALL CAPS
             * from rules_apply, so isupper checks are reliable) */
            char orig[NAME_MAX_LEN];
            memcpy(orig, word_start, wlen + 1);

            /* Lowercase everything, then capitalize first char */
            memcpy(word_start, lower, wlen + 1);
            word_start[0] = (char)toupper((unsigned char)word_start[0]);

            /* Mc prefix: McCoy, McBride */
            if (wlen > 2 && orig[0]=='M' && orig[1]=='C' && isalpha((unsigned char)orig[2]))
                word_start[2] = (char)toupper((unsigned char)word_start[2]);

            /* Mac prefix: MacDonald — only when 4th char was uppercase in orig */
            if (wlen > 3 && orig[0]=='M' && orig[1]=='A' && orig[2]=='C'
                         && isupper((unsigned char)orig[3]))
                word_start[3] = (char)toupper((unsigned char)word_start[3]);

            /* O' prefix: O'Brien */
            if (wlen > 2 && orig[0]=='O' && orig[1]=='\''
                         && isalpha((unsigned char)orig[2]))
                word_start[2] = (char)toupper((unsigned char)word_start[2]);

            /* Hyphenated names: capitalize each segment */
            for (i = 1; i < wlen; i++)
                if (word_start[i] == '-' && i + 1 < wlen
                    && isalpha((unsigned char)word_start[i + 1]))
                    word_start[i + 1] = (char)toupper((unsigned char)word_start[i + 1]);

            emit = word_start;
        }

        /* Copy chosen form into result */
        size_t elen = strlen(emit);
        if (res_pos + elen < NAME_MAX_LEN - 1) {
            memcpy(result + res_pos, emit, elen);
            res_pos += elen;
        }

        *p = saved;
        word_idx++;
    }

    result[res_pos] = '\0';
    memcpy(s, result, res_pos + 1);
}

/* =========================================================================
 * name_clean
 * ====================================================================== */

int name_clean(const char *raw, NameResult *result)
{
    if (!result) return -1;
    memset(result, 0, sizeof(*result));

    if (!raw || !*raw) {
        result->flags = NAME_FLAG_EMPTY;
        return -1;
    }

    strncpy(result->raw, raw, NAME_MAX_LEN - 1);

    char work[NAME_MAX_LEN];
    strncpy(work, raw, NAME_MAX_LEN - 1);
    work[NAME_MAX_LEN - 1] = '\0';

    char *start = work;
    while (isspace((unsigned char)*start)) start++;
    int end = (int)strlen(start) - 1;
    while (end >= 0 && isspace((unsigned char)start[end])) start[end--] = '\0';

    if (!*start) {
        result->flags = NAME_FLAG_EMPTY;
        return -1;
    }

    memcpy(result->cleaned, start, NAME_MAX_LEN - 1);
    result->flags = rules_apply(result->cleaned, NAME_MAX_LEN);

    if (!(result->flags & NAME_FLAG_EMPTY)) {
        /* If trust stripping reduced the name to a single word
         * (e.g. "Maxwell Living Trust" → "MAXWELL"), the result is not a
         * usable mailing-label name.  Revert to the full original with title
         * case so the trust entity name itself becomes the label. */
        int wc = 0;
        for (const char *p = result->cleaned; *p; ) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            wc++;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
        if (wc == 1 && strchr(start, ' ') != NULL &&
            !in_table(result->cleaned, upper_words)) {
            /* Short single-word results (≤4 chars, e.g. "HEB", "KJ") are
             * likely a name or initials — let the AI decide the correct
             * form rather than blindly reverting to trust language. */
            if (strlen(result->cleaned) <= 4) {
                result->flags |= NAME_FLAG_NEEDS_AI;
            } else {
                /* Longer single words (e.g. "MAXWELL") are surnames —
                 * revert to the full original trust name so the mailing
                 * label shows a recognisable entity name. */
                strncpy(result->cleaned, start, NAME_MAX_LEN - 1);
                result->cleaned[NAME_MAX_LEN - 1] = '\0';
                for (char *p = result->cleaned; *p; p++)
                    *p = (char)toupper((unsigned char)*p);
                result->flags &= ~(NAME_FLAG_WAS_TRUST | NAME_FLAG_NEEDS_AI);
            }
        }

        /* Detect 2–4 char all-consonant words that title-case will render
         * incorrectly (e.g. "KJ" → "Kj", "JLS" → "Jls").  These are
         * initials or abbreviations that the AI can fix. */
        if (!(result->flags & NAME_FLAG_NEEDS_AI)) {
            for (const char *q = result->cleaned; *q; ) {
                while (*q && isspace((unsigned char)*q)) q++;
                if (!*q) break;
                const char *ws = q;
                while (*q && !isspace((unsigned char)*q)) q++;
                size_t wlen = (size_t)(q - ws);
                if (wlen >= 2 && wlen <= 4) {
                    int all_alpha = 1, has_vowel = 0;
                    char up[8] = {0}, lo[8] = {0};
                    for (size_t k = 0; k < wlen && k < 7; k++) {
                        unsigned char c = (unsigned char)ws[k];
                        if (!isalpha(c)) { all_alpha = 0; break; }
                        up[k] = (char)toupper(c);
                        lo[k] = (char)tolower(c);
                        if (up[k]=='A'||up[k]=='E'||up[k]=='I'||
                            up[k]=='O'||up[k]=='U'||up[k]=='Y') has_vowel = 1;
                    }
                    if (all_alpha && !has_vowel
                        && !in_table(up, upper_words)
                        && !exact_form_lookup(lo)) {
                        result->flags |= NAME_FLAG_NEEDS_AI;
                        break;
                    }
                }
            }
        }

        apply_title_case(result->cleaned);
    } else {
        /* rules_apply stripped everything (e.g. "Not Available From The
         * Data Source" → erased by the junk table → "").  Use the
         * standard mail-merge placeholder so the label still has content. */
        strncpy(result->cleaned, "Current Resident", NAME_MAX_LEN - 1);
        result->cleaned[NAME_MAX_LEN - 1] = '\0';
    }

    /* AI fallback: for names the rule engine flagged as needing review,
     * ask the LLM for a second opinion (only when the server is ready). */
    if ((result->flags & (NAME_FLAG_NEEDS_AI | NAME_FLAG_SUSPICIOUS)) &&
        !(result->flags & NAME_FLAG_EMPTY) &&
        llm_is_ready()) {
        char ai_out[NAME_MAX_LEN];
        if (llm_clean_name(result->raw, result->cleaned,
                           ai_out, sizeof(ai_out)) && *ai_out) {
            strncpy(result->cleaned, ai_out, NAME_MAX_LEN - 1);
            result->cleaned[NAME_MAX_LEN - 1] = '\0';
            result->flags |= NAME_FLAG_WAS_AI;
        }
    }

    name_flags_str(result->flags, result->notes, sizeof(result->notes));
    return 0;
}

void name_flags_str(int flags, char *buf, size_t buflen)
{
    if (!buf || !buflen) return;
    buf[0] = '\0';
    size_t pos = 0;

#define APPEND(msg) do {                                        \
    const char *_m = (msg);                                     \
    size_t _ml = strlen(_m);                                    \
    if (pos + _ml + 3 < buflen) {                               \
        if (pos > 0) { buf[pos++] = ';'; buf[pos++] = ' '; }   \
        memcpy(buf + pos, _m, _ml);                             \
        pos += _ml;                                             \
        buf[pos] = '\0';                                        \
    }                                                           \
} while (0)

    if (flags & NAME_FLAG_WAS_TRUST)  APPEND("trust stripped");
    if (flags & NAME_FLAG_WAS_ET_AL)  APPEND("et al removed");
    if (flags & NAME_FLAG_WAS_CO)     APPEND("c/o removed");
    if (flags & NAME_FLAG_WAS_AI)     APPEND("AI cleaned");
    if (flags & NAME_FLAG_SUSPICIOUS) APPEND("NEEDS REVIEW");
    if (flags & NAME_FLAG_EMPTY)      APPEND("empty result");

#undef APPEND
}
