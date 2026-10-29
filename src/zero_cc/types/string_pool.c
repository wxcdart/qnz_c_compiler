#include "zero_cc/types/symbols.h"
#include <stdlib.h>
#include <string.h>

const char *zero_cc_intern_string_len(ZeroCCCompiler *c, const char *s, size_t len) {
    if (!c || !s) return s;
    size_t bytes = len + 1; /* content + terminating NUL */
    ZeroCCStrChunk *ch = c->str_chunks;
    if (!ch || ch->cap - ch->len < bytes) {
        size_t ncap = (bytes < 512) ? 512 : bytes * 2;
        char *data = (char *)malloc(ncap);
        if (!data) return s;
        ZeroCCStrChunk *nch = (ZeroCCStrChunk *)malloc(sizeof(*nch));
        if (!nch) { free(data); return s; }
        nch->data = data;
        nch->len = 0;
        nch->cap = ncap;
        nch->next = c->str_chunks;
        c->str_chunks = nch;
        ch = nch;
    }
    memcpy(ch->data + ch->len, s, bytes);
    const char *p = ch->data + ch->len;
    ch->len += bytes;
    return p;
}

const char *zero_cc_intern_string(ZeroCCCompiler *c, const char *s) {
    return zero_cc_intern_string_len(c, s, strlen(s));
}

void zero_cc_free_string_pool(ZeroCCCompiler *c) {
    if (!c) return;
    ZeroCCStrChunk *ch = c->str_chunks;
    while (ch) {
        ZeroCCStrChunk *nx = ch->next;
        free(ch->data);
        free(ch);
        ch = nx;
    }
    c->str_chunks = NULL;
}
