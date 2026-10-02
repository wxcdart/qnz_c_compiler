#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../utils/arena.h"

typedef struct {
    const char *file;   // source filename (e.g. "<stdin>")
    char       *msg;    // allocated in tmp arena
    uint32_t    line, col;
} DiagEntry;

typedef struct {
    DiagEntry  *entries;   // array of entries allocated in tmp arena
    size_t      count;
    size_t      capacity;
    Arena      *tmp_arena; // arena that owns message strings and entries
    const char *default_file; // default source filename
} DiagBuf;

/** Initialise a DiagBuf to use the supplied temporary arena. */
void diag_init(DiagBuf *buf, Arena *tmp);

/** Set default source filename for diagnostics. */
void diag_set_file(DiagBuf *buf, const char *file);

/** Report a formatted diagnostic at the given source location using default filename. */
void diag_report(DiagBuf *buf, uint32_t line, uint32_t col, const char *fmt, ...);

/** Report a formatted diagnostic with explicit filename at the given source location. */
void diag_report_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col, const char *fmt, ...);

/** Return true if any diagnostics have been recorded. */
bool diag_has_errors(const DiagBuf *buf);

/** Print all diagnostics to stream with file:line:col caret formatting. */
void diag_print_stream(const DiagBuf *buf, FILE *out);

/** Print all diagnostics to stdout with file:line:col caret formatting. */
void diag_print(const DiagBuf *buf);
