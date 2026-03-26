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
    "LLC", "LLP", "LP", "INC", "CORP", "LTD",
    "PC",  "PLC", "NA", "FSB", "PLLC", "DBA",
    "USA", "US",  "PO", "TNT",
    "CO",  /* Company / Colorado */
    "NV",  /* Nevada (already used for entity names) */
    /* US state abbreviations — only codes that don't collide with
     * common surnames or prepositions (excluded: AL, DE, HI, ID, IN,
     * LA, MA, ME, MI, MO, OH, OK, OR, PA, VA) */
    "AK", "AZ", "AR", "CA", "CT", "DC", "FL", "GA",
    "IA", "IL", "KS", "KY", "MD", "MN", "MS", "MT",
    "NC", "ND", "NE", "NH", "NJ", "NM", "NY",
    "RI", "SC", "SD", "TN", "TX", "UT", "VT",
    "WA", "WV", "WI", "WY",
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

/* Case-insensitive comparison of n bytes (like strncasecmp) */
static int icase_ncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
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

        /* --- 2b. All-consonant words 3+ chars → likely acronym, keep UPPER.
         * Vowels: A E I O U Y.  Catches collapsed acronyms (FKH, MDK, BRN,
         * NNN, KJS).  2-char consonant-only words (NG, ST) are too ambiguous
         * — those are handled by AI flagging in rules_apply instead. */
        if (!emit && wlen >= 3) {
            int all_consonant = 1;
            for (i = 0; i < wlen; i++) {
                char ch = (char)toupper((unsigned char)word_start[i]);
                if (ch=='A'||ch=='E'||ch=='I'||ch=='O'||ch=='U'||ch=='Y'
                    || !isalpha((unsigned char)word_start[i])) {
                    all_consonant = 0; break;
                }
            }
            if (all_consonant) {
                for (i = 0; i < wlen; i++)
                    word_start[i] = (char)toupper((unsigned char)word_start[i]);
                emit = word_start;
            }
        }

        /* --- 3. Single-letter word = initial, always uppercase ---------- */
        if (!emit && wlen == 1 && isalpha((unsigned char)lower[0])) {
            word_start[0] = (char)toupper((unsigned char)word_start[0]);
            emit = word_start;
        }

        /* --- 4. Lower exception (articles/prepositions) ----------------
         * Only lowercase when the word is NOT the last word in the string.
         * Final-position "An", "To", etc. are likely surnames (e.g.
         * "Amy An", "Anh To") and should stay title-cased. */
        if (!emit && word_idx > 0 && in_table(lower, lower_words)) {
            /* Check if this is the last word: `saved` holds the char that
             * was at *p before NUL-termination.  If it's NUL, this is the
             * last word.  If it's whitespace, peek past it for more text. */
            int is_last = (saved == '\0');
            if (!is_last) {
                const char *ahead = p + 1;  /* skip past the saved char */
                while (*ahead && isspace((unsigned char)*ahead)) ahead++;
                is_last = (*ahead == '\0');
            }
            if (!is_last) {
                /* Not the last word → lowercase */
                memcpy(word_start, lower, wlen + 1);
                emit = word_start;
            }
            /* else: last word → fall through to title case (surname) */
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
        } else {
            *p = saved;
            break;  /* buffer full — stop rather than skip words */
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

    snprintf(result->raw, NAME_MAX_LEN, "%s", raw);

    char work[NAME_MAX_LEN];
    snprintf(work, NAME_MAX_LEN, "%s", raw);

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
                    /* Skip reorder when the trailing letter is a roman
                     * numeral (I, V, X) and this was a trust entity —
                     * it's a series/version, not a person's initial.
                     * e.g. "CROSS INVESTMENT I" is not "Last First I". */
                    char last_ch = (char)toupper((unsigned char)ws[nw-1][0]);
                    if ((result->flags & NAME_FLAG_WAS_TRUST) &&
                        (last_ch == 'I' || last_ch == 'V' || last_ch == 'X')) {
                        goto skip_reorder;
                    }
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
                    snprintf(result->cleaned, NAME_MAX_LEN, "%s", reord);
                skip_reorder: ;
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
        snprintf(result->cleaned, NAME_MAX_LEN, "Current Resident");
    }

    /* AI fallback: for names the rule engine flagged as needing review,
     * ask the LLM for a second opinion (only when the server is ready). */
    if ((result->flags & (NAME_FLAG_NEEDS_AI | NAME_FLAG_SUSPICIOUS)) &&
        !(result->flags & NAME_FLAG_EMPTY) &&
        llm_is_ready()) {
        /* Save what we're sending to the LLM for session logging */
        snprintf(result->ai_input, NAME_MAX_LEN, "%s", result->cleaned);

        char ai_out[NAME_MAX_LEN];
        if (llm_clean_name(result->raw, result->cleaned,
                           ai_out, sizeof(ai_out)) && *ai_out) {
            /* Save raw LLM response for session logging */
            snprintf(result->ai_output, NAME_MAX_LEN, "%s", ai_out);

            /* ── Deterministic post-AI validation ──────────────────
             *
             * Check for common AI failures before accepting the output.
             * Flags: VFAIL_TRUST     = reintroduced trust language
             *        VFAIL_SURNAME   = changed a word from the original
             *        VFAIL_ACRONYM   = lowercased a consonant-only word
             *        VFAIL_SPLIT     = different word count (split/merged)
             *
             * If any flag is set, escalate to AI validation pass. */
            int vflags = 0;
            #define VFAIL_TRUST   (1 << 0)
            #define VFAIL_SURNAME (1 << 1)
            #define VFAIL_ACRONYM (1 << 2)
            #define VFAIL_SPLIT   (1 << 3)

            /* Check 1: Trust language — reject if AI added trust-related
             * words that weren't in the rules-engine output (ai_input).
             * This catches both reintroduction (AI adds "Trust") and
             * fragment addition (AI adds "Fam" back after it was stripped). */
            {
                static const char *trust_frags[] = {
                    "TRUST", "TRUSTEE", "TRS", "TR",
                    "RLT", "FMTR", "FTR", "FAM",
                    "REVOCABLE", "IRREVOCABLE",
                    NULL
                };
                const char *op = ai_out;
                while (*op && !(vflags & VFAIL_TRUST)) {
                    while (*op && isspace((unsigned char)*op)) op++;
                    if (!*op) break;
                    const char *ow = op;
                    while (*op && !isspace((unsigned char)*op)) op++;
                    size_t olen = (size_t)(op - ow);
                    for (int tf = 0; trust_frags[tf]; tf++) {
                        size_t tlen = strlen(trust_frags[tf]);
                        if (olen != tlen) continue;
                        if (icase_ncmp(ow, trust_frags[tf], tlen) != 0) continue;
                        /* Found trust word in AI output — was it in ai_input? */
                        int in_input = 0;
                        const char *ip2 = result->ai_input;
                        while (*ip2) {
                            while (*ip2 && isspace((unsigned char)*ip2)) ip2++;
                            if (!*ip2) break;
                            const char *iw2 = ip2;
                            while (*ip2 && !isspace((unsigned char)*ip2)) ip2++;
                            size_t ilen2 = (size_t)(ip2 - iw2);
                            if (ilen2 == tlen &&
                                icase_ncmp(iw2, trust_frags[tf], tlen) == 0) {
                                in_input = 1; break;
                            }
                        }
                        if (!in_input) { vflags |= VFAIL_TRUST; break; }
                    }
                }
            }

            /* Check 2: Word count changed (split/merge) */
            {
                int wc_in = 0, wc_out = 0;
                for (const char *q = result->ai_input; *q; ) {
                    while (*q && isspace((unsigned char)*q)) q++;
                    if (!*q) break;
                    wc_in++;
                    while (*q && !isspace((unsigned char)*q)) q++;
                }
                for (const char *q = ai_out; *q; ) {
                    while (*q && isspace((unsigned char)*q)) q++;
                    if (!*q) break;
                    wc_out++;
                    while (*q && !isspace((unsigned char)*q)) q++;
                }
                if (wc_in != wc_out) vflags |= VFAIL_SPLIT;
            }

            /* Check 3: Consonant-only uppercase word lowercased by AI */
            {
                /* Tokenize ai_input and ai_out, compare word-by-word */
                const char *ip = result->ai_input, *op = ai_out;
                while (*ip && *op) {
                    while (*ip && isspace((unsigned char)*ip)) ip++;
                    while (*op && isspace((unsigned char)*op)) op++;
                    if (!*ip || !*op) break;
                    const char *iw = ip, *ow = op;
                    while (*ip && !isspace((unsigned char)*ip)) ip++;
                    while (*op && !isspace((unsigned char)*op)) op++;
                    size_t ilen = (size_t)(ip - iw);
                    size_t olen = (size_t)(op - ow);

                    if (ilen == olen && ilen >= 2) {
                        /* Check if input word is all-uppercase and should
                         * stay that way: either consonant-only (FKH, MDK)
                         * or in the upper_words table (INC, LLC, LP). */
                        int should_be_upper = 0;

                        /* All-uppercase consonant-only (3+ chars) */
                        if (ilen >= 3) {
                            int all_upper_cons = 1;
                            for (size_t k = 0; k < ilen; k++) {
                                char ch = (char)toupper((unsigned char)iw[k]);
                                if (!isupper((unsigned char)iw[k]) ||
                                    ch=='A'||ch=='E'||ch=='I'||
                                    ch=='O'||ch=='U'||ch=='Y') {
                                    all_upper_cons = 0; break;
                                }
                            }
                            if (all_upper_cons) should_be_upper = 1;
                        }

                        /* In upper_words table (INC, LLC, CORP, etc.) */
                        if (!should_be_upper) {
                            char uword[16];
                            if (ilen < sizeof(uword)) {
                                for (size_t k = 0; k < ilen; k++)
                                    uword[k] = (char)toupper((unsigned char)iw[k]);
                                uword[ilen] = '\0';
                                if (in_table(uword, upper_words))
                                    should_be_upper = 1;
                            }
                        }

                        if (should_be_upper) {
                            /* AI lowercased it? */
                            int ai_changed = 0;
                            for (size_t k = 0; k < ilen; k++) {
                                if (iw[k] != ow[k]) { ai_changed = 1; break; }
                            }
                            if (ai_changed) vflags |= VFAIL_ACRONYM;
                        }
                    }
                }
            }

            /* Check 4: Surname changed — a word in ai_input that appears
             * in the original raw (case-insensitive) was changed by AI */
            if (!(vflags & VFAIL_SPLIT)) {
                const char *ip = result->ai_input, *op = ai_out;
                while (*ip && *op) {
                    while (*ip && isspace((unsigned char)*ip)) ip++;
                    while (*op && isspace((unsigned char)*op)) op++;
                    if (!*ip || !*op) break;
                    const char *iw = ip, *ow = op;
                    while (*ip && !isspace((unsigned char)*ip)) ip++;
                    while (*op && !isspace((unsigned char)*op)) op++;
                    size_t ilen = (size_t)(ip - iw);
                    size_t olen = (size_t)(op - ow);

                    /* Words differ? */
                    if (ilen != olen ||
                        icase_ncmp(iw, ow, ilen) != 0) {
                        /* Check if the input word (case-insensitive)
                         * appears in the original raw input */
                        char iword[64];
                        if (ilen < sizeof(iword)) {
                            memcpy(iword, iw, ilen);
                            iword[ilen] = '\0';
                            /* Search raw for this word */
                            const char *rp = result->raw;
                            while (*rp) {
                                while (*rp && isspace((unsigned char)*rp)) rp++;
                                if (!*rp) break;
                                const char *rw = rp;
                                while (*rp && !isspace((unsigned char)*rp)) rp++;
                                size_t rlen = (size_t)(rp - rw);
                                if (rlen == ilen &&
                                    icase_ncmp(rw, iword, ilen) == 0) {
                                    /* Word was in original → AI changed it */
                                    vflags |= VFAIL_SURNAME;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            /* ── Decision: accept, reject, or escalate to AI validator ─── */
            int ai_ok = 1;

            if (vflags & (VFAIL_TRUST | VFAIL_SURNAME | VFAIL_ACRONYM)) {
                /* Hard reject — trust language reintroduced, AI changed
                 * a name from the original record, or AI lowercased an
                 * acronym/abbreviation the rules engine correctly uppercased. */
                ai_ok = 0;
            } else if (vflags & VFAIL_SPLIT) {
                /* Uncertain — escalate to AI validation pass */
                char validated[NAME_MAX_LEN];
                if (llm_validate(result->raw, result->ai_input,
                                 ai_out, validated, sizeof(validated))
                    && *validated) {
                    snprintf(ai_out, sizeof(ai_out), "%s", validated);
                    /* ai_ok stays 1 — use the validated output */
                } else {
                    /* Validation call failed — fall back to rules output */
                    ai_ok = 0;
                }
            }

            if (ai_ok) {
                /* When the input is all-uppercase, force the AI output back
                 * to uppercase so the output convention matches the input. */
                if (input_all_upper) {
                    for (char *ap = ai_out; *ap; ap++)
                        *ap = (char)toupper((unsigned char)*ap);
                }

                snprintf(result->cleaned, NAME_MAX_LEN, "%s", ai_out);
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
