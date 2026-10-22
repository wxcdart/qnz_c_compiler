#ifndef ZERO_CC_CORE_MULTI_TU_H
#define ZERO_CC_CORE_MULTI_TU_H
/*
 * multi_tu.h - Multi-translation-unit merge for the JIT driver (T1).
 *
 * The JIT execution model runs ONE ZeroCCCompiler image: text+rodata on an
 * RX page, .data on an RW page, with every relocation resolved by name
 * (zero_cc_find_function / find_global_var / dlsym). Multi-file `--run`
 * therefore compiles each input into its own ZeroCCCompiler and merges
 * them sequentially into the first one before execution:
 *
 *   zero_cc_tu_merge(base, tu, err, errsz)
 *
 * Merging `tu` into `base`:
 *   - .text/.rodata/.data are appended (16/8/8-aligned) so every offset
 *     inside `tu` shifts by a fixed per-section base
 *   - `tu`'s function table is appended with re-based text_offset; a
 *     duplicate *definition* is an error (linker "multiple definition")
 *   - call/data relocations shift by the text base; string relocations
 *     shift both fields; .data pointer initializers shift by the data
 *     base (and their rodata targets by the rodata base)
 *
 * Global-var registry: the process-wide table (global_vars.h) is reset at
 * the START of every compilation, so the driver must snapshot the merged
 * registry before compiling each next TU and re-append it (re-based by the
 * data base) after the merge - the merge itself does NOT touch the
 * registry. See the multi-TU loop in main.c (compile_jit).
 *
 * Cross-TU resolution is then free: a call or &global reference from TU1
 * to TU0's symbol was compiled as an external (the registry was reset
 * before TU1's parse), and at patch time the name lookup finds the merged
 * table / registry entry. Internal call relocations need no per-reloc
 * retargeting - execute_jit resolves them by name against the merged
 * function table (which prefers definitions over prototypes).
 *
 * Known limitations (documented, out of T1 scope):
 *   - `static` file-scope globals share the bare-name registry, so two
 *     TUs each defining `static int x` would alias (linkage rules not
 *     modeled); the duplicate check only applies to functions.
 *   - _Thread_local storage is per-compiler and not merged.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zero_cc/core/buffers.h"
#include "zero_cc/core/compiler_state.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/types/symbol_mgmt.h"

static inline size_t zero_cc_align_up(size_t v, size_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Append len zero bytes to a section buffer (alignment padding). */
static inline int zero_cc_buf_append_zeros(ZeroCCBuffer *b, size_t len) {
    if (len == 0) return 0;
    zero_cc_buf_ensure(b, len);
    memset(b->data + b->length, 0, len);
    b->length += len;
    return 0;
}

/*
 * Merge compiled TU `tu` into `base`. On success returns 0; on failure
 * returns -1 with a message in err (no partial state is left visible -
 * the caller must destroy both compilers either way). The caller is
 * responsible for the global-var registry (see file header).
 */
static inline int zero_cc_tu_merge(ZeroCCCompiler *base, ZeroCCCompiler *tu,
                                   char *err, size_t errsz) {
    if (!base || !tu) {
        if (err && errsz) snprintf(err, errsz, "internal: null merge input");
        return -1;
    }

    /* Duplicate-definition check BEFORE mutating anything. A prototype in
     * `base` + a definition in `tu` (the universal-header pattern) is the
     * expected cross-TU shape and merges cleanly; two definitions are a
     * multiple-definition error. */
    for (size_t i = 0; i < tu->function_count; i++) {
        if (tu->functions[i].is_prototype) continue;
        ZeroCCFunction *prev = zero_cc_find_function(base, tu->functions[i].name);
        if (prev && !prev->is_prototype) {
            snprintf(err, errsz, "multiple definition of '%s' (first in an earlier input file)",
                     tu->functions[i].name);
            return -1;
        }
    }

    /* Capacity headroom for the merged tables. */
    if (base->function_count + tu->function_count > ZERO_CC_MAX_FUNCTIONS) {
        snprintf(err, errsz, "too many functions for multi-TU image (max %d)", ZERO_CC_MAX_FUNCTIONS);
        return -1;
    }
    if (base->reloc_count + tu->reloc_count > ZERO_CC_MAX_RELOCS) {
        snprintf(err, errsz, "too many relocations for multi-TU image (max %d)", ZERO_CC_MAX_RELOCS);
        return -1;
    }
    if (base->str_reloc_count + tu->str_reloc_count > ZERO_CC_MAX_STR_RELOCS) {
        snprintf(err, errsz, "too many string relocations for multi-TU image (max %d)", ZERO_CC_MAX_STR_RELOCS);
        return -1;
    }
    if (base->data_reloc_count + tu->data_reloc_count > ZERO_CC_MAX_DATA_RELOCS) {
        snprintf(err, errsz, "too many data relocations for multi-TU image (max %d)", ZERO_CC_MAX_DATA_RELOCS);
        return -1;
    }

    size_t text_base = zero_cc_align_up(base->text_section.length, 16);
    size_t rodata_base = zero_cc_align_up(base->rodata_section.length, 8);
    size_t data_base = zero_cc_align_up(base->data_section.length, 8);

    /* Alignment padding + section append. */
    if (zero_cc_buf_append_zeros(&base->text_section, text_base - base->text_section.length) != 0 ||
        zero_cc_buf_append_zeros(&base->rodata_section, rodata_base - base->rodata_section.length) != 0 ||
        zero_cc_buf_append_zeros(&base->data_section, data_base - base->data_section.length) != 0) {
        snprintf(err, errsz, "out of memory appending sections");
        return -1;
    }
    zero_cc_buf_put_bytes(&base->text_section, tu->text_section.data, tu->text_section.length);
    zero_cc_buf_put_bytes(&base->rodata_section, tu->rodata_section.data, tu->rodata_section.length);
    zero_cc_buf_put_bytes(&base->data_section, tu->data_section.data, tu->data_section.length);

    /* Functions: re-base text_offset and append. */
    for (size_t i = 0; i < tu->function_count; i++) {
        base->functions[base->function_count] = tu->functions[i];
        base->functions[base->function_count].text_offset += text_base;
        base->function_count++;
    }

    /* Call/data relocations: shift the patch site by the text base. The
     * target symbol is resolved by name at patch time, so no retargeting
     * is needed. */
    for (size_t i = 0; i < tu->reloc_count; i++) {
        base->relocs[base->reloc_count] = tu->relocs[i];
        base->relocs[base->reloc_count].text_patch_offset += text_base;
        base->reloc_count++;
    }

    /* String-literal relocations: patch site by text base, rodata target
     * by rodata base. */
    for (size_t i = 0; i < tu->str_reloc_count; i++) {
        base->str_relocs[base->str_reloc_count] = tu->str_relocs[i];
        base->str_relocs[base->str_reloc_count].text_patch_offset += text_base;
        base->str_relocs[base->str_reloc_count].rodata_offset += rodata_base;
        base->str_reloc_count++;
    }

    /* .data pointer initializers: patch site by data base; rodata targets
     * by rodata base, global-storage targets by data base. */
    for (size_t i = 0; i < tu->data_reloc_count; i++) {
        base->data_relocs[base->data_reloc_count] = tu->data_relocs[i];
        base->data_relocs[base->data_reloc_count].data_patch_offset += data_base;
        if (base->data_relocs[base->data_reloc_count].target_kind == 0) {
            base->data_relocs[base->data_reloc_count].rodata_offset += rodata_base;
        } else {
            base->data_relocs[base->data_reloc_count].target_data_offset += data_base;
        }
        base->data_reloc_count++;
    }

    return 0;
}

#endif /* ZERO_CC_CORE_MULTI_TU_H */
