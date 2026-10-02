#ifndef ZERO_CC_JIT_EXECUTION_H
#define ZERO_CC_JIT_EXECUTION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "zero_cc/core.h"
#include "zero_cc/types_lexer.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
#include "../types/global_vars.h"
#include "../platform/mmap_adapter.h"

static inline int zero_cc_execute_jit(ZeroCCCompiler *c, void *context) {
    if (!c || c->text_section.length == 0) return -1;

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__) || defined(__posix) || defined(__MACH__) || defined(_WIN32)
    size_t page_sz = zero_get_page_size();

    /* W^X memory partitioning:
     * Code and rodata are mapped into code_page_sz and transitioned to RX (read+exec, no write).
     * Mutable data (.data) is mapped on a separate page boundary and remains RW (read+write, no exec).
     */
    size_t code_len = c->text_section.length + c->rodata_section.length;
    size_t code_page_sz = (code_len + page_sz - 1) & ~(page_sz - 1);
    if (code_page_sz == 0) code_page_sz = page_sz;

    size_t data_len = c->data_section.length;
    size_t data_page_sz = (data_len + page_sz - 1) & ~(page_sz - 1);
    size_t total_sz = code_page_sz + data_page_sz;

    void *mem = zero_mmap_alloc_rw(total_sz);
    if (!mem) return -1;

    size_t data_offset = code_page_sz;

    /* Patch internal function calls and string literal RIP offsets */
    for (size_t i = 0; i < c->reloc_count; i++) {
        if (c->relocs[i].is_call) {
            ZeroCCFunction *target_fn = zero_cc_find_function(c, c->relocs[i].target_symbol);
            if (target_fn) {
                int32_t rel = (int32_t)(target_fn->text_offset - (c->relocs[i].text_patch_offset + 4));
                memcpy(c->text_section.data + c->relocs[i].text_patch_offset, &rel, 4);
            }
        } else if (strncmp(c->relocs[i].target_symbol, "__data_", 7) == 0) {
            /* Data reference (global variable) - calculate RIP-relative offset to data_offset */
            char var_name[128];
            strcpy(var_name, c->relocs[i].target_symbol + 7);
            ZeroCCGlobalVar *gv = find_global_var(var_name);
            if (gv) {
                size_t target_addr = data_offset + gv->data_offset;
                int32_t rel = (int32_t)(target_addr - (c->relocs[i].text_patch_offset + 4));
                memcpy(c->text_section.data + c->relocs[i].text_patch_offset, &rel, 4);
            }
        } else {
            /* Address patch for external function indirect calls */
            void *host_fn = zero_cc_resolve_symbol(c->relocs[i].target_symbol);
            if (host_fn) {
                memcpy(c->text_section.data + c->relocs[i].text_patch_offset, &host_fn, 8);
            }
        }
    }
    for (size_t i = 0; i < c->str_reloc_count; i++) {
        int32_t rel = (int32_t)((c->text_section.length + c->str_relocs[i].rodata_offset) - (c->str_relocs[i].text_patch_offset + 4));
        memcpy(c->text_section.data + c->str_relocs[i].text_patch_offset, &rel, 4);
    }

    /* Copy text section */
    memcpy(mem, c->text_section.data, c->text_section.length);
    /* Copy rodata section */
    if (c->rodata_section.length > 0) {
        memcpy((char*)mem + c->text_section.length, c->rodata_section.data, c->rodata_section.length);
    }
    /* Copy data section (global variables) */
    if (c->data_section.length > 0) {
        memcpy((char*)mem + data_offset, c->data_section.data, c->data_section.length);
    }

    /* Strict W^X Transition:
     * Transition code pages to Read+Execute (NOT writable).
     * Data pages remain Read+Write (NOT executable). */
    if (zero_mmap_make_rx(mem, code_page_sz) != 0) {
        zero_mmap_free(mem, total_sz);
        return -1;
    }

    /* Find 'main' entry point */
    ZeroCCFunction *main_fn = zero_cc_find_function(c, "main");
    size_t main_offset = main_fn ? main_fn->text_offset : 0;

    int (*jit_main)(void *) = (int (*)(void *))((char*)mem + main_offset);
    int result = jit_main(context);
    zero_mmap_free(mem, total_sz);
    return result;
#else
    (void)context;
    return 0;
#endif
}

#endif /* ZERO_CC_JIT_EXECUTION_H */
