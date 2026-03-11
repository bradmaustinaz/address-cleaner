#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "names.h"
#include "rules.h"
#include "llm.h"

/* List-level uppercase flag: set by the caller (gui.c) when every name
 * in the current batch is already entirely uppercase.  Supplements the
 * per-name input_all_upper check inside name_clean. */
static int g_list_all_upper = 0;

void name_set_list_all_upper(int flag)
{
    g_list_all_upper = !!flag;
}

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

/* =========================================================================
 * Truncated first-name prefix table
 *
 * These are uppercase prefixes that are unambiguously incomplete forms of
 * common US first names — they are not valid standalone names in their own
 * right.  When the first word of a cleaned name (or the word immediately
 * after '&') matches one of these prefixes and is followed by at least one
 * more word, NAME_FLAG_NEEDS_AI is set so the LLM can correct the typo
 * (e.g. "MICHAE & HEIDI WILLIAMS" → "Michael & Heidi Williams").
 * ====================================================================== */

static const char *trunc_first_name_prefixes[] = {
    "MICHAE",   /* Michael   */ "MICHA",    /* Michael   */
    "BARBAR",   /* Barbara   */ "BARBA",    /* Barbara   */
    "RICHAR",   /* Richard   */
    "JENNIF",   /* Jennifer  */
    "ELIZAB",   /* Elizabeth */
    "WILLIA",   /* William   */
    "PATRIC",   /* Patricia / Patrick (PATRIC alone is not a US name) */
    "ROBER",    /* Robert    */
    "CHARLE",   /* Charles   */
    "SUSA",     /* Susan     */
    "MARGAR",   /* Margaret  */
    "DOROTH",   /* Dorothy   */
    "MATTHE",   /* Matthew   */
    "ANTHON",   /* Anthony   */
    "CATHERI",  /* Catherine */ "KATHERI",  /* Katherine */
    "KATHLE",   /* Kathleen  */
    "BENJAM",   /* Benjamin  */
    "TIMOTH",   /* Timothy   */
    "BEVERL",   /* Beverly   */
    "DEBOR",    /* Deborah   */
    "VIRGINI",  /* Virginia  */
    "LAWREN",   /* Lawrence  */
    "MARILY",   /* Marilyn   */
    "THERES",   /* Theresa / Teresa */
    "SHIRL",    /* Shirley   */
    "HAROL",    /* Harold    */
    "RAYMON",   /* Raymond   */
    "PHILLI",   /* Phillip / Philip */
    "DOUGL",    /* Douglas   */
    "JONATH",   /* Jonathan  */
    "CHARLOT",  /* Charlotte */
    "GERALDI",  /* Geraldine */
    "CHRISTIN", /* Christine / Christina */
    "STEPHANI", /* Stephanie */
    "JACQUEL",  /* Jacqueline */
    "SAMANTH",  /* Samantha  */
    "FREDERI",  /* Frederick / Frederica */
    "THEODO",   /* Theodore / Theodora */
    "LEONAR",   /* Leonard   */
    "TERREN",   /* Terrence  */
    "LORRAI",   /* Lorraine  */
    "CONSTAN",  /* Constance / Constantine */
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

/* Returns 1 if the word (any case, length wlen) is a known truncated first-name
 * prefix — i.e. it's listed in trunc_first_name_prefixes[]. */
static int is_trunc_first_name(const char *word, size_t wlen)
{
    if (wlen < 4 || wlen > 9) return 0;
    char upper[16] = {0};
    size_t k;
    for (k = 0; k < wlen && k < 15; k++)
        upper[k] = (char)toupper((unsigned char)word[k]);
    upper[k] = '\0';
    for (int i = 0; trunc_first_name_prefixes[i]; i++)
        if (strcmp(upper, trunc_first_name_prefixes[i]) == 0)
            return 1;
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

            /* Mac prefix: MacDonald.
             * Require wlen >= 6 to avoid false positives on common words
             * like MACON (5), MACE (4), MACHO (5), MACRO (5). Shortest
             * real Mac-surname: MacKay (6).
             * Also skip common English words that start with "MAC" but
             * are not surnames (e.g. MACHINE, MACRAME). */
            if (wlen > 5 && orig[0]=='M' && orig[1]=='A' && orig[2]=='C'
                         && isupper((unsigned char)orig[3])) {
                static const char *mac_non_names[] = {
                    "machine", "machines", "machete", "machetes",
                    "macrame", "macaroni", "macabre", "macadam",
                    "machination", "machinations", "machinist",
                    NULL
                };
                int is_blocked = 0;
                for (int bi = 0; mac_non_names[bi]; bi++)
                    if (str_ieq(lower, mac_non_names[bi]))
                        { is_blocked = 1; break; }
                if (!is_blocked)
                    word_start[3] = (char)toupper((unsigned char)word_start[3]);
            }

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
    result->raw[NAME_MAX_LEN - 1] = '\0'; /* explicit: don't rely solely on memset */

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

    /* Detect whether the original input is already entirely uppercase.
     * When it is — or when the caller flagged the whole list as uppercase
     * via name_set_list_all_upper — we preserve that casing and skip
     * title-case conversion so the output matches the input convention. */
    int input_all_upper = g_list_all_upper;
    if (!input_all_upper) {
        input_all_upper = 1;
        for (const char *cp = start; *cp; cp++) {
            if (isalpha((unsigned char)*cp) && !isupper((unsigned char)*cp)) {
                input_all_upper = 0;
                break;
            }
        }
    }

    {
        size_t slen = strlen(start);
        if (slen >= NAME_MAX_LEN) slen = NAME_MAX_LEN - 1;
        memcpy(result->cleaned, start, slen);
        result->cleaned[slen] = '\0';
    }
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
            /* Short single-word results (≤3 chars, e.g. "HEB") are
             * likely initials — let the AI decide the correct form
             * rather than blindly reverting to trust language. */
            size_t clen = strlen(result->cleaned);
            if (clen <= 3) {
                result->flags |= NAME_FLAG_NEEDS_AI;
            } else {
                /* Longer single words (e.g. "MAXWELL") mean trust
                 * stripping ate everything but the surname.  Revert to
                 * the full original (cleaned of whitespace/typos) so
                 * the trust name stays intact on the mailing label
                 * (e.g. "MAXWELL LIVING TRUST" → "Maxwell Living Trust"). */
                size_t slen2 = strlen(start);
                if (slen2 >= NAME_MAX_LEN) slen2 = NAME_MAX_LEN - 1;
                memcpy(result->cleaned, start, slen2);
                result->cleaned[slen2] = '\0';
                for (char *up = result->cleaned; *up; up++)
                    *up = (char)toupper((unsigned char)*up);
                result->flags |= NAME_FLAG_TRUST_KEPT;
                result->flags &= ~NAME_FLAG_WAS_TRUST;
            }
        }

        /* Deterministic Last-First-Initial reorder.
         * When the cleaned name has no '&', is 2–5 words, and ends with
         * a single alpha character (e.g. "SIRAKIS DEREK M"), it is in
         * reversed Last First Initial format.  Move the first word to the
         * end: "DEREK M SIRAKIS".  NEEDS_AI stays set so the AI still runs
         * afterward to catch typos in the already-reordered form. */
        if (!(result->flags & NAME_FLAG_EMPTY) &&
            !strchr(result->cleaned, '&')) {
            const char *ws[8], *we[8]; /* word start / end pointers */
            int nw = 0;
            const char *q2 = result->cleaned;
            while (*q2 && nw < 8) {
                while (*q2 && isspace((unsigned char)*q2)) q2++;
                if (!*q2) break;
                ws[nw] = q2;
                while (*q2 && !isspace((unsigned char)*q2)) q2++;
                we[nw] = q2;
                nw++;
            }
            if (nw >= 3 && nw <= 5) {
                size_t lwlen = (size_t)(we[nw-1] - ws[nw-1]);
                if (lwlen == 1 && isalpha((unsigned char)ws[nw-1][0])) {
                    /* Rebuild: words[1..nw-1] + " " + words[0] */
                    char reord[NAME_MAX_LEN];
                    size_t rpos = 0;
                    for (int wi = 1; wi < nw; wi++) {
                        if (wi > 1 && rpos < NAME_MAX_LEN - 1)
                            reord[rpos++] = ' ';
                        size_t wl = (size_t)(we[wi] - ws[wi]);
                        if (rpos + wl < NAME_MAX_LEN - 1) {
                            memcpy(reord + rpos, ws[wi], wl);
                            rpos += wl;
                        }
                    }
                    if (rpos < NAME_MAX_LEN - 1) reord[rpos++] = ' ';
                    size_t w0l = (size_t)(we[0] - ws[0]);
                    if (rpos + w0l < NAME_MAX_LEN - 1) {
                        memcpy(reord + rpos, ws[0], w0l);
                        rpos += w0l;
                    }
                    reord[rpos] = '\0';
                    strncpy(result->cleaned, reord, NAME_MAX_LEN - 1);
                    result->cleaned[NAME_MAX_LEN - 1] = '\0';
                }
            }
        }

        /* Detect 2–4 char all-consonant words that title-case will render
         * incorrectly (e.g. "KJ" → "Kj", "JLS" → "Jls").  These are
         * initials or abbreviations that the AI can fix.
         * Skip when input is all-uppercase — title case is not applied,
         * so these words already render correctly in uppercase. */
        if (!input_all_upper && !(result->flags & NAME_FLAG_NEEDS_AI)) {
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
                            up[k]=='O'||up[k]=='U') has_vowel = 1;
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

        /* Detect truncated first names that survive rules_apply unchanged
         * because they look like ordinary words (e.g. "MICHAE" from "Michael",
         * "BARBAR" from "Barbara").  Walk word-0 and every word right after
         * '&' — both are first-name positions.  Only flag when the candidate
         * is followed by at least one more word (confirming it is a first
         * name, not a lone surname).  Works regardless of input_all_upper
         * because the truncation is a data error, not a casing question. */
        if (!(result->flags & NAME_FLAG_NEEDS_AI)) {
            const char *p3  = result->cleaned;
            int chk_next    = 1; /* word-0 is always a first-name candidate */
            while (*p3 && !(result->flags & NAME_FLAG_NEEDS_AI)) {
                while (*p3 && isspace((unsigned char)*p3)) p3++;
                if (!*p3) break;
                const char *fw = p3;
                while (*p3 && !isspace((unsigned char)*p3)) p3++;
                size_t fwlen = (size_t)(p3 - fw);
                int is_amp   = (fwlen == 1 && fw[0] == '&');

                if (chk_next && !is_amp) {
                    /* Only flag if at least one more token follows this word */
                    const char *peek = p3;
                    while (*peek && isspace((unsigned char)*peek)) peek++;
                    if (*peek && is_trunc_first_name(fw, fwlen))
                        result->flags |= NAME_FLAG_NEEDS_AI;
                }
                /* The word immediately after '&' is the next candidate */
                chk_next = is_amp;
            }
        }

        if (!input_all_upper)
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
        /* Save what we're sending to the LLM for session logging */
        strncpy(result->ai_input, result->cleaned, NAME_MAX_LEN - 1);
        result->ai_input[NAME_MAX_LEN - 1] = '\0';

        char ai_out[NAME_MAX_LEN];
        if (llm_clean_name(result->raw, result->cleaned,
                           ai_out, sizeof(ai_out)) && *ai_out) {
            /* Save raw LLM response for session logging */
            strncpy(result->ai_output, ai_out, NAME_MAX_LEN - 1);
            result->ai_output[NAME_MAX_LEN - 1] = '\0';

            /* Reject AI output that reintroduces trust language the
             * rules engine already stripped (LLM hallucination). */
            int ai_ok = 1;
            if (result->flags & NAME_FLAG_WAS_TRUST) {
                /* Case-insensitive search for "trust" as a whole word */
                for (const char *tp = ai_out; *tp; tp++) {
                    if (tolower((unsigned char)tp[0]) == 't' &&
                        tolower((unsigned char)tp[1]) == 'r' &&
                        tolower((unsigned char)tp[2]) == 'u' &&
                        tolower((unsigned char)tp[3]) == 's' &&
                        tolower((unsigned char)tp[4]) == 't') {
                        /* Check word boundaries */
                        int lb = (tp == ai_out) ||
                                 !isalnum((unsigned char)*(tp - 1));
                        int rb = !tp[5] ||
                                 !isalnum((unsigned char)tp[5]);
                        if (lb && rb) { ai_ok = 0; break; }
                    }
                    if (!tp[1] || !tp[2] || !tp[3] || !tp[4]) break;
                }
            }
            if (ai_ok) {
                /* When the input is all-uppercase, force the AI output back
                 * to uppercase so the output convention matches the input. */
                if (input_all_upper) {
                    for (char *ap = ai_out; *ap; ap++)
                        *ap = (char)toupper((unsigned char)*ap);
                }

                strncpy(result->cleaned, ai_out, NAME_MAX_LEN - 1);
                result->cleaned[NAME_MAX_LEN - 1] = '\0';
                result->flags |= NAME_FLAG_WAS_AI;
            }
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

    if (flags & NAME_FLAG_TRUST_KEPT)  APPEND("trust name kept");
    else if (flags & NAME_FLAG_WAS_TRUST)  APPEND("trust stripped");
    if (flags & NAME_FLAG_WAS_ET_AL)  APPEND("et al removed");
    if (flags & NAME_FLAG_WAS_CO)     APPEND("c/o removed");
    if (flags & NAME_FLAG_WAS_AI)     APPEND("AI cleaned");
    if (flags & NAME_FLAG_SUSPICIOUS) APPEND("NEEDS REVIEW");
    if (flags & NAME_FLAG_EMPTY)      APPEND("empty result");

#undef APPEND
}
