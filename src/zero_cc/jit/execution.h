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

static inline int zero_cc_execute_jit(ZeroCCCompiler *c, void *context) {
    if (!c || c->text_section.length == 0) return -1;

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__) || defined(__posix) || defined(__MACH__) || defined(_WIN32)
    size_t page_sz = (c->text_section.length + c->rodata_section.length + 4095) & ~4095;
    void *mem = zero_mmap_alloc_rw(page_sz);
    if (mem == MAP_FAILED || !mem) return -1;

    /* Patch internal function calls and string literal RIP offsets */
    for (size_t i = 0; i < c->reloc_count; i++) {
        if (c->relocs[i].is_call) {
            ZeroCCFunction *target_fn = zero_cc_find_function(c, c->relocs[i].target_symbol);
            if (target_fn) {
                int32_t rel = (int32_t)(target_fn->text_offset - (c->relocs[i].text_patch_offset + 4));
                memcpy(c->text_section.data + c->relocs[i].text_patch_offset, &rel, 4);
            }
        }
    }
    for (size_t i = 0; i < c->str_reloc_count; i++) {
        int32_t rel = (int32_t)((c->text_section.length + c->str_relocs[i].rodata_offset) - (c->str_relocs[i].text_patch_offset + 4));
        memcpy(c->text_section.data + c->str_relocs[i].text_patch_offset, &rel, 4);
    }

    memcpy(mem, c->text_section.data, c->text_section.length);
    if (c->rodata_section.length > 0) {
        memcpy((char*)mem + c->text_section.length, c->rodata_section.data, c->rodata_section.length);
    }

    if (zero_mmap_make_rx(mem, page_sz) != 0) {
        zero_mmap_free(mem, page_sz);
        return -1;
    }

    /* Find 'main' entry point */
    ZeroCCFunction *main_fn = zero_cc_find_function(c, "main");
    size_t main_offset = main_fn ? main_fn->text_offset : 0;

    int (*jit_main)(void *) = (int (*)(void *))((char*)mem + main_offset);
    int result = jit_main(context);
    zero_mmap_free(mem, page_sz);
    return result;
#else
    (void)context;
    return 0;
#endif
}

/* --------------------------------------------------------------------------
 *  5. ELF64 EMITTERS
 *  -------------------------------------------------------------------------- */
#if defined(__has_include) && __has_include(<elf.h>)
#include <elf.h>
#else
#include "zero_cc/zero_elf.h"
#endif

#ifdef __linux__


#endif /* ZERO_CC_JIT_EXECUTION_H */
