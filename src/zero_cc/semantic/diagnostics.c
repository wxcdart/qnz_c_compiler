#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/types/symbols.h"
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
    buf->default_file = "<stdin>";
}

/** Set default source filename for diagnostics. */
void diag_set_file(DiagBuf *buf, const char *file) {
    if (buf) {
        buf->default_file = file ? file : "<stdin>";
    }
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

/** Internal helper to report a diagnostic with severity and macro context. */
static void diag_report_internal(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                                  DiagSeverity severity, DiagMacroContext *macro_ctx,
                                  const char *fmt, va_list ap) {
    if (!buf || !fmt) return;
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0) return;
    char *msg = (char *)arena_alloc(buf->tmp_arena, (size_t)needed + 1);
    if (!msg) return;
    vsnprintf(msg, (size_t)needed + 1, fmt, ap);
    diag_ensure_capacity(buf);
    DiagEntry *e = &buf->entries[buf->count];
    e->file = file ? file : (buf->default_file ? buf->default_file : "<stdin>");
    e->msg = msg;
    e->line = line;
    e->col = col;
    e->severity = severity;
    e->macro_ctx = macro_ctx;
    buf->count++;
}

/** Report an error diagnostic with explicit filename. */
void diag_report_error_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                             DiagMacroContext *macro_ctx, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, file, line, col, DIAG_ERROR, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Report a warning diagnostic with explicit filename. */
void diag_report_warning_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                               DiagMacroContext *macro_ctx, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, file, line, col, DIAG_WARNING, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Report a note diagnostic with explicit filename. */
void diag_report_note_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col,
                            DiagMacroContext *macro_ctx, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, file, line, col, DIAG_NOTE, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Report an error diagnostic at the given source location using default filename. */
void diag_report_error(DiagBuf *buf, uint32_t line, uint32_t col,
                        DiagMacroContext *macro_ctx, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, buf->default_file, line, col, DIAG_ERROR, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Report a warning diagnostic at the given source location using default filename. */
void diag_report_warning(DiagBuf *buf, uint32_t line, uint32_t col,
                          DiagMacroContext *macro_ctx, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, buf->default_file, line, col, DIAG_WARNING, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Report a note diagnostic at the given source location using default filename. */
void diag_report_note(DiagBuf *buf, uint32_t line, uint32_t col,
                       DiagMacroContext *macro_ctx, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, buf->default_file, line, col, DIAG_NOTE, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Legacy API: report diagnostic (treated as error for backward compat). */
void diag_report_file(DiagBuf *buf, const char *file, uint32_t line, uint32_t col, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, file, line, col, DIAG_ERROR, NULL, fmt, ap);
    va_end(ap);
}

/** Legacy API: report diagnostic (treated as error for backward compat). */
void diag_report(DiagBuf *buf, uint32_t line, uint32_t col, const char *fmt, ...) {
    if (!buf || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    diag_report_internal(buf, buf->default_file, line, col, DIAG_ERROR, NULL, fmt, ap);
    va_end(ap);
}

/** Return the number of error diagnostics recorded. */
size_t diag_error_count(const DiagBuf *buf) {
    if (!buf) return 0;
    size_t count = 0;
    for (size_t i = 0; i < buf->count; ++i) {
        if (buf->entries[i].severity == DIAG_ERROR) count++;
    }
    return count;
}

/** Return the number of warning diagnostics recorded. */
size_t diag_warning_count(const DiagBuf *buf) {
    if (!buf) return 0;
    size_t count = 0;
    for (size_t i = 0; i < buf->count; ++i) {
        if (buf->entries[i].severity == DIAG_WARNING) count++;
    }
    return count;
}

/** Return true if any error diagnostics have been recorded. */
bool diag_has_errors(const DiagBuf *buf) {
    return diag_error_count(buf) > 0;
}

/** Return true if any diagnostics (errors or warnings) have been recorded. */
bool diag_has_any(const DiagBuf *buf) {
    return buf && buf->count > 0;
}

/** Print all diagnostics to stream with standardized file:line:col caret formatting. */
void diag_print_stream(const DiagBuf *buf, FILE *out) {
    if (!buf || !out) return;
    for (size_t i = 0; i < buf->count; ++i) {
        const DiagEntry *e = &buf->entries[i];
        const char *file = e->file ? e->file : "<stdin>";
        const char *sev_str = (e->severity == DIAG_ERROR) ? "error" :
                              (e->severity == DIAG_WARNING) ? "warning" : "note";
        if (e->line > 0 && e->col > 0) {
            fprintf(out, "%s:%u:%u: %s: %s\n", file, e->line, e->col, sev_str, e->msg ? e->msg : "");
            fprintf(out, "    %*s^\n", (int)(e->col - 1), "");
        } else if (e->line > 0) {
            fprintf(out, "%s:%u: %s: %s\n", file, e->line, sev_str, e->msg ? e->msg : "");
        } else {
            fprintf(out, "%s: %s: %s\n", file, sev_str, e->msg ? e->msg : "");
        }
        /* Print macro expansion context if present */
        DiagMacroContext *ctx = e->macro_ctx;
        while (ctx) {
            const char *mfile = ctx->file ? ctx->file : "<stdin>";
            if (ctx->line > 0 && ctx->col > 0) {
                fprintf(out, "%s:%u:%u: note: in expansion of macro '%s'\n",
                        mfile, ctx->line, ctx->col, ctx->macro_name ? ctx->macro_name : "??");
                fprintf(out, "    %*s^\n", (int)(ctx->col - 1), "");
            } else if (ctx->line > 0) {
                fprintf(out, "%s:%u: note: in expansion of macro '%s'\n",
                        mfile, ctx->line, ctx->macro_name ? ctx->macro_name : "??");
            } else {
                fprintf(out, "%s: note: in expansion of macro '%s'\n",
                        mfile, ctx->macro_name ? ctx->macro_name : "??");
            }
            ctx = ctx->prev;
        }
    }
}

/** Print all diagnostics to stderr with file:line:col caret formatting. */
void diag_print_stderr(const DiagBuf *buf) {
    diag_print_stream(buf, stderr);
}

/** Print all diagnostics to stdout with file:line:col caret formatting. */
void diag_print(const DiagBuf *buf) {
    diag_print_stream(buf, stdout);
}

/** Sync compiler's had_error and error_msg with diagnostics buffer. */
void diag_sync_compiler(ZeroCCCompiler *c) {
    if (!c) return;
    /* If had_error was already set by legacy code, preserve it */
    bool legacy_had_error = c->had_error;
    c->had_error = diag_has_errors(&c->diag_buf) || legacy_had_error;
    if (c->had_error) {
        /* Copy first error message to error_msg for backward compat */
        for (size_t i = 0; i < c->diag_buf.count; ++i) {
            if (c->diag_buf.entries[i].severity == DIAG_ERROR) {
                const char *msg = c->diag_buf.entries[i].msg;
                if (msg) {
                    snprintf(c->error_msg, sizeof(c->error_msg), "%s", msg);
                    break;
                }
            }
        }
        /* If no diagnostics but legacy error, keep existing error_msg */
        if (legacy_had_error && c->diag_buf.count == 0) {
            /* error_msg already set by legacy code */
        }
    } else {
        c->error_msg[0] = '\0';
    }
}

/** Create a new macro context node for tracking macro expansions. */
DiagMacroContext *diag_macro_context_new(Arena *tmp, const char *macro_name,
                                          const char *file, uint32_t line, uint32_t col,
                                          DiagMacroContext *prev) {
    if (!tmp || !macro_name) return NULL;
    DiagMacroContext *ctx = (DiagMacroContext *)arena_alloc(tmp, sizeof(DiagMacroContext));
    if (!ctx) return NULL;
    ctx->macro_name = macro_name;
    ctx->file = file;
    ctx->line = line;
    ctx->col = col;
    ctx->prev = prev;
    return ctx;
}

/** Merge source diagnostics buffer into destination buffer. */
void diag_merge(DiagBuf *dest, const DiagBuf *src) {
    if (!dest || !src) return;
    for (size_t i = 0; i < src->count; ++i) {
        const DiagEntry *e = &src->entries[i];
        if (!e->msg) continue;  /* Skip entries with NULL message */
        char *msg_copy = (char *)arena_alloc(dest->tmp_arena, strlen(e->msg) + 1);
        if (!msg_copy) continue;
        strcpy(msg_copy, e->msg);
        diag_ensure_capacity(dest);
        DiagEntry *d = &dest->entries[dest->count];
        d->file = e->file;
        d->msg = msg_copy;
        d->line = e->line;
        d->col = e->col;
        d->severity = e->severity;
        d->macro_ctx = e->macro_ctx;
        dest->count++;
    }
}

/** Report an error diagnostic at the compiler's current token position. */
void diag_report_error_at_token(ZeroCCCompiler *c, const char *fmt, ...) {
    if (!c || !fmt) return;
    uint32_t line = 0, col = 0;
    const char *file = "<stdin>";
    if (c->token_pos < c->token_count) {
        line = c->tokens[c->token_pos].line;
        col = c->tokens[c->token_pos].col;
        if (c->tokens[c->token_pos].filename) {
            file = c->tokens[c->token_pos].filename;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    /* Format message once for both diagnostics and legacy error_msg */
    char tmp_buf[256];
    vsnprintf(tmp_buf, sizeof(tmp_buf), fmt, ap);
    va_end(ap);
    /* Append the offending token's text when available - file-scope and
     * declaration errors are much easier to triage with it. */
    if (c->token_pos < c->token_count) {
        const ZeroCCToken *tk = &c->tokens[c->token_pos];
        const char *ttext = (tk->type == TK_IDENT || tk->type == TK_NUM ||
                             tk->type == TK_STRING) ?
            (tk->allocated_str ? tk->allocated_str : tk->str_val) : NULL;
        if (ttext && ttext[0]) {
            char full_buf[320];
            snprintf(full_buf, sizeof(full_buf), "%s (near '%.24s')", tmp_buf, ttext);
            diag_report_error_file(&c->diag_buf, file, line, col, NULL, "%s", full_buf);
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "%s", full_buf);
            return;
        }
    }
    diag_report_error_file(&c->diag_buf, file, line, col, NULL, "%s", tmp_buf);
    /* Also set legacy fields for backward compat */
    c->had_error = 1;
    snprintf(c->error_msg, sizeof(c->error_msg), "%s", tmp_buf);
}

/** Report a warning diagnostic at the compiler's current token position. */
void diag_report_warning_at_token(ZeroCCCompiler *c, const char *fmt, ...) {
    if (!c || !fmt) return;
    uint32_t line = 0, col = 0;
    const char *file = "<stdin>";
    if (c->token_pos < c->token_count) {
        line = c->tokens[c->token_pos].line;
        col = c->tokens[c->token_pos].col;
        if (c->tokens[c->token_pos].filename) {
            file = c->tokens[c->token_pos].filename;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    diag_report_warning_file(&c->diag_buf, file, line, col, NULL, fmt, ap);
    va_end(ap);
}

/** Check if a warning is disabled by #pragma GCC diagnostic. */
bool diag_is_warning_disabled(const ZeroCCCompiler *c, const char *warn_name) {
    if (!c || !warn_name) return false;
    if (strncmp(warn_name, "-W", 2) == 0) warn_name += 2;
    for (int i = 0; i < c->diag_pragma_state.disabled_warning_count && i < 64; i++) {
        if (c->diag_pragma_state.disabled_warnings[i] &&
            strcmp(c->diag_pragma_state.disabled_warnings[i], warn_name) == 0) {
            return true;
        }
    }
    return false;
}

/** Report a warning diagnostic with warning name for #pragma GCC diagnostic support. */
void diag_report_warning_named(ZeroCCCompiler *c, uint32_t line, uint32_t col,
                                DiagMacroContext *macro_ctx, const char *warn_name, const char *fmt, ...) {
    if (!c || !fmt) return;
    /* Check if warning is disabled */
    if (warn_name && diag_is_warning_disabled(c, warn_name)) return;

    uint32_t ln = line, cl = col;
    const char *file = "<stdin>";
    if (c->token_pos < c->token_count) {
        ln = c->tokens[c->token_pos].line;
        cl = c->tokens[c->token_pos].col;
        if (c->tokens[c->token_pos].filename) {
            file = c->tokens[c->token_pos].filename;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    diag_report_warning_file(&c->diag_buf, file, ln, cl, macro_ctx, fmt, ap);
    va_end(ap);
}

/** Report a warning diagnostic at the compiler's current token position with warning name. */
void diag_report_warning_at_token_named(ZeroCCCompiler *c, const char *warn_name, const char *fmt, ...) {
    if (!c || !fmt) return;
    /* Check if warning is disabled */
    if (warn_name && diag_is_warning_disabled(c, warn_name)) return;

    uint32_t line = 0, col = 0;
    const char *file = "<stdin>";
    if (c->token_pos < c->token_count) {
        line = c->tokens[c->token_pos].line;
        col = c->tokens[c->token_pos].col;
        if (c->tokens[c->token_pos].filename) {
            file = c->tokens[c->token_pos].filename;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    diag_report_warning_file(&c->diag_buf, file, line, col, NULL, fmt, ap);
    va_end(ap);
}
