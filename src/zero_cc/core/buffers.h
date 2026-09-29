#ifndef ZERO_CC_CORE_BUFFERS_H
#define ZERO_CC_CORE_BUFFERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} ZeroCCBuffer;

static inline void zero_cc_buf_init(ZeroCCBuffer *b, size_t initial_cap) {
    b->capacity = initial_cap ? initial_cap : 256;
    b->length = 0;
    b->data = (char *)malloc(b->capacity);
    if (b->data) b->data[0] = '\0';
}

static inline void zero_cc_buf_free(ZeroCCBuffer *b) {
    if (b->data) {
        free(b->data);
        b->data = NULL;
    }
    b->capacity = 0;
    b->length = 0;
}

static inline void zero_cc_buf_ensure(ZeroCCBuffer *b, size_t extra) {
    if (b->length + extra + 1 >= b->capacity) {
        size_t new_cap = (b->capacity * 2 > b->length + extra + 1) ? b->capacity * 2 : b->length + extra + 256;
        b->data = (char *)realloc(b->data, new_cap);
        b->capacity = new_cap;
    }
}

static inline void zero_cc_buf_putc(ZeroCCBuffer *b, char c) {
    zero_cc_buf_ensure(b, 1);
    b->data[b->length++] = c;
    b->data[b->length] = '\0';
}

static inline void zero_cc_buf_puts(ZeroCCBuffer *b, const char *s) {
    if (!s) return;
    size_t len = strlen(s);
    zero_cc_buf_ensure(b, len);
    memcpy(b->data + b->length, s, len);
    b->length += len;
    b->data[b->length] = '\0';
}

static inline void zero_cc_buf_put_bytes(ZeroCCBuffer *b, const void *bytes, size_t len) {
    if (!bytes || !len) return;
    zero_cc_buf_ensure(b, len);
    memcpy(b->data + b->length, bytes, len);
    b->length += len;
}

/* --------------------------------------------------------------------------
 *  SharpArena Lesson #2: Dedicated Unmanaged Pointer Stack (ZeroPtrStack)
 *  Bypasses heap tracking for AST, Tokenizer, and Expression evaluation
 * -------------------------------------------------------------------------- */
typedef struct {
    void **ptrs;
    size_t capacity;
    size_t top;
} ZeroPtrStack;

static inline void zero_ptr_stack_init(ZeroPtrStack *st, size_t initial_cap) {
    st->capacity = initial_cap ? initial_cap : 128;
    st->top = 0;
    st->ptrs = (void **)malloc(st->capacity * sizeof(void *));
}

static inline void zero_ptr_stack_destroy(ZeroPtrStack *st) {
    if (st && st->ptrs) {
        free(st->ptrs);
        st->ptrs = NULL;
        st->capacity = 0;
        st->top = 0;
    }
}

static inline void zero_ptr_stack_push(ZeroPtrStack *st, void *ptr) {
    if (st->top >= st->capacity) {
        size_t new_cap = st->capacity * 2;
        st->ptrs = (void **)realloc(st->ptrs, new_cap * sizeof(void *));
        st->capacity = new_cap;
    }
    st->ptrs[st->top++] = ptr;
}

static inline void* zero_ptr_stack_pop(ZeroPtrStack *st) {
    if (!st || st->top == 0) return NULL;
    return st->ptrs[--st->top];
}

static inline void* zero_ptr_stack_peek(const ZeroPtrStack *st) {
    if (!st || st->top == 0) return NULL;
    return st->ptrs[st->top - 1];
}

static inline void zero_ptr_stack_clear(ZeroPtrStack *st) {
    if (st) st->top = 0;
}


#endif /* ZERO_CC_CORE_BUFFERS_H */
