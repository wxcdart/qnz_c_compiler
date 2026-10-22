/* Tokenization and Utility Functions */
#ifndef ZERO_CC_LEXER_TOKENIZE_H
#define ZERO_CC_LEXER_TOKENIZE_H

#include "zero_cc/types/symbols.h"
#include "zero_cc/lexer/next.h"
#include "zero_cc/core/preprocessor.h"

/* Main tokenization function */
static inline void zero_cc_tokenize_c99(const char *src, ZeroCCCompiler *c) {
    if (!src || !c) return;

    /* Initialize pack value from preprocessor */
    c->default_pack = zero_cc_get_pack_value();
    c->pack_ev_n = 0;
    int pack_stack[32]; int pack_depth = 0; int cur_pack = 0;

    /* Initialize diagnostic pragma state from preprocessor */
    c->diag_pragma_state.warning_as_error = zero_cc_global_diag_state.warning_as_error;
    c->diag_pragma_state.disabled_warning_count = zero_cc_global_diag_state.disabled_warning_count;
    for (int i = 0; i < zero_cc_global_diag_state.disabled_warning_count && i < 64; i++) {
        c->diag_pragma_state.disabled_warnings[i] = zero_cc_global_diag_state.disabled_warnings[i];
        zero_cc_global_diag_state.disabled_warnings[i] = NULL; /* Transfer ownership */
    }

    ZeroCCLexer lx;
    zero_cc_lexer_init(&lx, src);

    size_t cap = 1024;
    c->tokens = (ZeroCCToken *)malloc(cap * sizeof(ZeroCCToken));
    if (!c->tokens) {
        const char *file = c->diag_buf.default_file ? c->diag_buf.default_file : "<stdin>";
        diag_report_error_file(&c->diag_buf, file, 0, 0, NULL, "Failed to allocate token buffer");
        return;
    }
    c->token_count = 0;
    c->token_pos = 0;

    while (1) {
        if (c->token_count + 1 >= cap) {
            cap *= 2;
            ZeroCCToken *new_tokens = (ZeroCCToken *)realloc(c->tokens, cap * sizeof(ZeroCCToken));
            if (!new_tokens) {
                const char *file = c->diag_buf.default_file ? c->diag_buf.default_file : "<stdin>";
                diag_report_error_file(&c->diag_buf, file, 0, 0, NULL, "Failed to grow token buffer");
                break;
            }
            c->tokens = new_tokens;
        }

        ZeroCCToken *tok = &c->tokens[c->token_count];
        int ret = zero_cc_lexer_next(&lx, tok);
        if (ret == 1) {
            c->token_count++;
            break;  /* EOF */
        }
        if (ret == -1) {      /* Error */
            c->had_error = 1;
            if (lx.err_msg[0]) {
                snprintf(c->error_msg, sizeof(c->error_msg), "%s", lx.err_msg);
            } else {
                snprintf(c->error_msg, sizeof(c->error_msg), "Lexer error at line %zu, col %zu", lx.line, lx.col);
            }
            const char *file = c->diag_buf.default_file ? c->diag_buf.default_file : "<stdin>";
            if (c->diag_buf.tmp_arena) {
                diag_report_error_file(&c->diag_buf, file, (uint32_t)lx.line, (uint32_t)lx.col, NULL, "%s", c->error_msg);
            } else {
                fprintf(stderr, "%s:%zu:%zu: error: %s\n", file, lx.line, lx.col, c->error_msg);
            }
            break;
        }

        /* #pragma pack: record position-based event, drop from token stream */
        if (tok->type == TK_HASH && tok->src) {
            const char *q = tok->src + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (strncmp(q, "pragma", 6) == 0) {
                q += 6;
                while (*q == ' ' || *q == '\t') q++;
                if (strncmp(q, "pack", 4) == 0 && !isalnum((unsigned char)q[4]) && q[4] != '_') {
                    q += 4;
                    const char *eol = q;
                    while (*eol && *eol != '\n') eol++;
                    while (q < eol && (*q == ' ' || *q == '\t' || *q == '(')) q++;
                    bool changed = true;
                    if (strncmp(q, "push", 4) == 0) {
                        q += 4;
                        if (pack_depth < 32) pack_stack[pack_depth++] = cur_pack;
                        while (q < eol && (*q == ' ' || *q == ',' || *q == '\t')) q++;
                        if (*q >= '0' && *q <= '9') cur_pack = (int)strtol(q, NULL, 10);
                    } else if (strncmp(q, "pop", 3) == 0) {
                        if (pack_depth > 0) cur_pack = pack_stack[--pack_depth];
                    } else if (*q >= '0' && *q <= '9') {
                        cur_pack = (int)strtol(q, NULL, 10);
                    } else {
                        cur_pack = 0; /* plain pack() / pack: reset */
                    }
                    if (changed && c->pack_ev_n < 256) {
                        c->pack_ev_tok[c->pack_ev_n] = c->token_count;
                        c->pack_ev_val[c->pack_ev_n++] = cur_pack;
                    }
                    lx.cur = eol; /* newline handled by the lexer */
                    lx.offset += (size_t)(eol - tok->src);
                    continue;
                } else {
                    /* Any other pragma forwarded from preprocessor is dropped */
                    const char *eol = q;
                    while (*eol && *eol != '\n') eol++;
                    lx.cur = eol;
                    lx.offset += (size_t)(eol - tok->src);
                    continue;
                }
            }
        }
        c->token_count++;
        if (tok->type == TK_EOF) break;
    }
}

/* Free token memory */
static inline void zero_cc_token_free(ZeroCCToken *tok) {
    if (tok && tok->allocated_str) {
        free(tok->allocated_str);
        tok->allocated_str = NULL;
    }
}

/* Free all compiler tokens */
static inline void zero_cc_compiler_free_tokens(ZeroCCCompiler *c) {
    if (!c) return;
    if (c->tokens) {
        for (size_t i = 0; i < c->token_count; i++) {
            zero_cc_token_free(&c->tokens[i]);
        }
        free(c->tokens);
        c->tokens = NULL;
    }
    c->token_count = 0;
    c->token_pos = 0;
}

#endif /* ZERO_CC_LEXER_TOKENIZE_H */
