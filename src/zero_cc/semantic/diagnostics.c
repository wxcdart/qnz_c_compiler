#include "diagnostics.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

/** Initialise a DiagBuf to use the supplied temporary arena. */
void diag_init(DiagBuf *buf, Arena *tmp) {
    if (!buf) return;
    buf->entries = NULL;
    buf->count = 0;
    buf->capacity = 0;
    buf->tmp_arena = tmp;
}

/** Ensure there is space for at least one more entry. */
static void diag_ensure_capacity(DiagBuf *buf) {
    if (buf->count < buf->capacity) return;
    size_t new_cap = buf->capacity ? buf->capacity * 2 : 8;
    DiagEntry *new_arr = (DiagEntry *)arena_alloc(buf->tmp_arena, new_cap * sizeof(DiagEntry));
    if (!new_arr) return; // OOM
    if (buf->entries) memcpy(new_arr, buf->entries, buf->count * sizeof(DiagEntry));
    buf->entries = new_arr;
    buf->capacity = new_cap;
}

/** Report a formatted diagnostic at the given source location. */
void diag_report(DiagBuf *buf, uint32_t line, uint32_t col, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) return;
    char *msg = (char *)arena_alloc(buf->tmp_arena, (size_t)needed + 1);
    if (!msg) return;
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    diag_ensure_capacity(buf);
    DiagEntry *e = &buf->entries[buf->count];
    e->msg = msg;
    e->line = line;
    e->col = col;
    buf->count++;
}

/** Return true if any diagnostics have been recorded. */
bool diag_has_errors(const DiagBuf *buf) {
    return buf && buf->count > 0;
}

/** Print all diagnostics to stdout (one per line). */
void diag_print(const DiagBuf *buf) {
    if (!buf) return;
    for (size_t i = 0; i < buf->count; ++i) {
        const DiagEntry *e = &buf->entries[i];
        printf("%u:%u: %s\n", e->line, e->col, e->msg);
    }
}
