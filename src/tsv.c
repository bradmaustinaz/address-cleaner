#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsv.h"

int tsv_parse_row(const char *line, TsvRow *row)
{
    if (!line || !row) return -1;

    memset(row, 0, sizeof(*row));

    /* Copy line into internal buffer, stripping trailing CR/LF */
    size_t len = strlen(line);
    if (len >= TSV_MAX_LINE) len = TSV_MAX_LINE - 1;
    memcpy(row->buf, line, len);
    row->buf[len] = '\0';

    /* Strip trailing CR and LF */
    while (len > 0 && (row->buf[len-1] == '\r' || row->buf[len-1] == '\n')) {
        row->buf[--len] = '\0';
    }

    /* Split on tab characters */
    char *p = row->buf;
    row->count = 0;
    while (row->count < TSV_MAX_FIELDS) {
        row->fields[row->count++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }

    return 0;
}

TsvTable *tsv_read(FILE *fp)
{
    TsvTable *table = calloc(1, sizeof(TsvTable));
    if (!table) return NULL;

    table->capacity = 64;
    table->rows = malloc(table->capacity * sizeof(TsvRow *));
    if (!table->rows) { free(table); return NULL; }

    char line[TSV_MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip completely blank lines */
        if (line[0] == '\n' || line[0] == '\r') continue;

        TsvRow *row = malloc(sizeof(TsvRow));
        if (!row) break;

        if (tsv_parse_row(line, row) != 0) { free(row); continue; }

        /* Grow table if needed */
        if (table->count >= table->capacity) {
            table->capacity *= 2;
            TsvRow **tmp = realloc(table->rows, table->capacity * sizeof(TsvRow *));
            if (!tmp) { free(row); break; }
            table->rows = tmp;
        }

        table->rows[table->count++] = row;
    }

    return table;
}

void tsv_write_row(FILE *fp, TsvRow *row)
{
    for (int i = 0; i < row->count; i++) {
        if (i > 0) fputc('\t', fp);
        fputs(row->fields[i] ? row->fields[i] : "", fp);
    }
    fputc('\n', fp);
}

void tsv_write(FILE *fp, TsvTable *table)
{
    for (int i = 0; i < table->count; i++) {
        tsv_write_row(fp, table->rows[i]);
    }
}

void tsv_table_free(TsvTable *table)
{
    if (!table) return;
    for (int i = 0; i < table->count; i++) {
        free(table->rows[i]);
    }
    free(table->rows);
    free(table);
}

const char *tsv_field(const TsvRow *row, int idx)
{
    if (!row || idx < 0 || idx >= row->count) return "";
    return row->fields[idx] ? row->fields[idx] : "";
}

void tsv_set_field(TsvRow *row, int idx, const char *value)
{
    if (!row || idx < 0 || idx >= TSV_MAX_FIELDS) return;

    /* Fields beyond current count: extend */
    if (idx >= row->count) {
        for (int i = row->count; i <= idx; i++) {
            row->fields[i] = (char *)"";
        }
        row->count = idx + 1;
    }

    /* Store directly — caller is responsible for value lifetime.
     * For cleaned output, value will typically point into a static
     * or cleaner-owned buffer that persists for the row lifetime. */
    row->fields[idx] = (char *)value;
}
