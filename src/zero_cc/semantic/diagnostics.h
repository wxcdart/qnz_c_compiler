#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "zero_cc/common/arena.h"

/* Forward declaration */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/** Diagnostic severity level */
typedef enum {
    DIAG_NOTE = 0,
    DIAG_WARNING = 1,
    DIAG_ERROR = 2,
} DiagSeverity;

/** Macro expansion context for diagnostic notes */
typedef struct DiagMacroContext {
    const char *macro_name;
    const char *file;
    uint32_t    line, col;
    struct DiagMacroContext *prev;
} DiagMacroContext;

typedef struct {
    const char *file;           // source filename (e.g. "<stdin>")
    char       *msg;            // allocated in tmp arena
    uint32_t    line, col;
    DiagSeverity severity;      // note, warning, error
    DiagMacroContext *macro_ctx; // macro expansion chain
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

/** Report a formatted diagnostic at the given source location using default filename (legacy: error). */
void diag_report(DiagBuf *buf, uint32_t line, uint32_t col, const char *fmt, ...);

/** Report a formatted diagnostic with explicit filename at the given source location (legacy: error). */
void diag_report_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col, const char *fmt, ...);

/** Report an error diagnostic with explicit filename. */
void diag_report_error_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                             DiagMacroContext *macro_ctx, const char *fmt, ...);

/** Report a warning diagnostic with explicit filename. */
void diag_report_warning_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                               DiagMacroContext *macro_ctx, const char *fmt, ...);

/** Report a note diagnostic with explicit filename. */
void diag_report_note_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                            DiagMacroContext *macro_ctx, const char *fmt, ...);

/** Report an error diagnostic at the given source location using default filename. */
void diag_report_error(DiagBuf *buf, uint32_t line, uint32_t col,
                        DiagMacroContext *macro_ctx, const char *fmt, ...);

/** Report a warning diagnostic at the given source location using default filename. */
void diag_report_warning(DiagBuf *buf, uint32_t line, uint32_t col,
                          DiagMacroContext *macro_ctx, const char *fmt, ...);

/** Report a note diagnostic at the given source location using default filename. */
void diag_report_note(DiagBuf *buf, uint32_t line, uint32_t col,
                       DiagMacroContext *macro_ctx, const char *fmt, ...);

/** Return the number of error diagnostics recorded. */
size_t diag_error_count(const DiagBuf *buf);

/** Return the number of warning diagnostics recorded. */
size_t diag_warning_count(const DiagBuf *buf);

/** Return true if any error diagnostics have been recorded. */
bool diag_has_errors(const DiagBuf *buf);

/** Return true if any diagnostics (errors or warnings) have been recorded. */
bool diag_has_any(const DiagBuf *buf);

/** Print all diagnostics to stream with standardized file:line:col caret formatting. */
void diag_print_stream(const DiagBuf *buf, FILE *out);

/** Print all diagnostics to stdout with file:line:col caret formatting. */
void diag_print(const DiagBuf *buf);

/** Create a new macro context node for tracking macro expansions. */
DiagMacroContext *diag_macro_context_new(Arena *tmp, const char *macro_name,
                                          const char *file, uint32_t line, uint32_t col,
                                          DiagMacroContext *prev);

/** Print all diagnostics to stderr with file:line:col caret formatting. */
void diag_print_stderr(const DiagBuf *buf);

/** Sync compiler's had_error and error_msg with diagnostics buffer. */
void diag_sync_compiler(ZeroCCCompiler *c);

/** Report an error diagnostic at the compiler's current token position. */
void diag_report_error_at_token(ZeroCCCompiler *c, const char *fmt, ...);

/** Report a warning diagnostic at the compiler's current token position. */
void diag_report_warning_at_token(ZeroCCCompiler *c, const char *fmt, ...);

/** Merge source diagnostics buffer into destination buffer. */
void diag_merge(DiagBuf *dest, const DiagBuf *src);

/** Check if a warning is disabled by #pragma GCC diagnostic. */
bool diag_is_warning_disabled(const ZeroCCCompiler *c, const char *warn_name);

/* ---------------- "did you mean" support ---------------- */

/**
 * Classic Levenshtein edit distance with early exit at 3
 * (only distances <= 2 are of interest; 3 means "too far").
 */
int diag_levenshtein(const char *a, size_t alen, const char *b, size_t blen);

/**
 * Find the closest candidate to `name` for a "did you mean" suggestion.
 * A candidate qualifies when: distance <= 2 AND distance <= max(1,
 * name_len/2) (short names get a tighter leash so 2-letter typos don't
 * match everything).  Ties (two candidates at the best distance) return
 * NULL — an ambiguous suggestion is worse than none.
 * Returns the winning candidate pointer or NULL.
 */
const char *diag_best_match(const char *name, size_t name_len,
                            const char *const *candidates, size_t n);

/** Report a warning diagnostic with warning name for #pragma GCC diagnostic support. */
void diag_report_warning_named(ZeroCCCompiler *c, uint32_t line, uint32_t col,
                                DiagMacroContext *macro_ctx, const char *warn_name, const char *fmt, ...);

/** Report a warning diagnostic at the compiler's current token position with warning name. */
void diag_report_warning_at_token_named(ZeroCCCompiler *c, const char *warn_name, const char *fmt, ...);
