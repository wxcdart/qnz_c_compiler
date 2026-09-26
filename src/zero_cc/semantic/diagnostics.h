#pragma once

#include <stdarg.h>
#include <stdint.h>
#include "../arena.h"
#include <stdbool.h>

typedef struct {
    char   *msg;    // allocated in tmp arena
    uint32_t line, col;
} DiagEntry;

typedef struct {
    DiagEntry *entries;   // array of entries allocated in tmp arena
    size_t     count;
    size_t     capacity;
    Arena     *tmp_arena; // arena that owns message strings and entries
} DiagBuf;

/** Initialise a DiagBuf to use the supplied temporary arena. */
void diag_init(DiagBuf *buf, Arena *tmp);
/** Report a formatted diagnostic at the given source location. */
void diag_report(DiagBuf *buf, uint32_t line, uint32_t col, const char *fmt, ...);
/** Return true if any diagnostics have been recorded. */
bool diag_has_errors(const DiagBuf *buf);
/** Print all diagnostics to stdout (one per line). */
void diag_print(const DiagBuf *buf);
