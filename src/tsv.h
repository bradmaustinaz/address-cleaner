#ifndef TSV_H
#define TSV_H

#include <stddef.h>
#include <stdio.h>

#define TSV_MAX_FIELDS  64
#define TSV_MAX_FIELD   1024
#define TSV_MAX_LINE    (TSV_MAX_FIELDS * TSV_MAX_FIELD)

/* A single parsed row from TSV input */
typedef struct {
    char   *fields[TSV_MAX_FIELDS];
    int     count;
    char    buf[TSV_MAX_LINE]; /* owns the field string data */
} TsvRow;

/* A collection of rows (full paste buffer) */
typedef struct {
    TsvRow **rows;
    int      count;
    int      capacity;
} TsvTable;

/* Parse a single line of tab-separated text into a TsvRow.
 * Returns 0 on success, -1 on error. */
int tsv_parse_row(const char *line, TsvRow *row);

/* Read all rows from fp until EOF.
 * Caller must free with tsv_table_free(). */
TsvTable *tsv_read(FILE *fp);

/* Write a row to fp as tab-separated fields followed by a newline. */
void tsv_write_row(FILE *fp, TsvRow *row);

/* Write all rows in table to fp. */
void tsv_write(FILE *fp, TsvTable *table);

/* Free memory allocated by tsv_read(). */
void tsv_table_free(TsvTable *table);

/* Utility: get field by index, returns "" if out of range. */
const char *tsv_field(const TsvRow *row, int idx);

/* Utility: set a field value (must be within current count or next slot). */
void tsv_set_field(TsvRow *row, int idx, const char *value);

#endif /* TSV_H */
