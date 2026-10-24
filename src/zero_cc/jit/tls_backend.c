/* tls_backend.c – Static TLS backend for _Thread_local (x86-64)
 *
 * Static TLS model (initial-exec), single-threaded-safe for both backends:
 *
 * - AOT (ELF / raw binary): the program's static TLS block is the classic
 *   `.tdata` (initialized) + `.tbss` (uninitialized) pair.  `declare`
 *   records the variable; `finalize` (called after all top-level
 *   declarations) packs the block, writes the .tdata contents and patches
 *   each variable's registered data_offset to its in-block offset.  The
 *   ELF writer then emits the sections, one symbol per variable and
 *   R_X86_64_TPOFF64 relocations for OP_TLS_LEA accesses.
 *
 * - JIT (--run): there is no thread pointer to speak of, so the TLS block
 *   is simply appended to the data section.  Each variable is registered
 *   as an ordinary global at its data-section offset; codegen uses
 *   OP_MOV_GLOBAL, and the JIT's existing data-relocation patching makes
 *   the accesses work.
 *
 * Loads/stores of the variable go through the address (OP_TLS_LEA /
 * OP_MOV_GLOBAL) and the ordinary load/store IR ops, so narrow widths
 * (char/short) and float members work without TLS-specific codegen.
 */

#include "zero_cc/jit/backends.h"
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include <string.h>

/* Forward declaration */
extern const ZeroCCTLSBackend zero_cc_default_tls_backend;

/* Record a TLS variable for finalization (AOT) / register storage (JIT).
 * Returns the variable's byte offset within the TLS block, or -1 on error. */
static int tls_declare(ZeroCCCompiler *c, const char *name,
                       const ZeroCCType *type, int64_t init_value, bool has_init)
{
    if (c->isa != ISA_X86_64) {
        diag_report_error_at_token(c,
            "_Thread_local: only the x86-64 target implements TLS "
            "(.tdata/.tbss block emission is x86-64-specific)");
        return -1;
    }
    size_t size = type->size;
    size_t align = type->align;
    if (size < 1 || size > 8) {
        diag_report_error_at_token(c,
            "unsupported _Thread_local type (integral or pointer scalars up to 8 bytes only)");
        return -1;
    }
    if (align < 1) align = 1;

    /* Record for finalization (AOT packing) and legacy thread_locals[]
     * lookup (const_eval type queries). */
    if (c->tls_var_count < 64) {
        ZeroCCTLSVar *t = &c->tls_vars[c->tls_var_count++];
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        t->size = size;
        t->align = align;
        t->has_init = has_init;
        t->init = init_value;
    }
    if (c->thread_local_count < 64) {
        strncpy(c->thread_locals[c->thread_local_count].name, name,
                sizeof(c->thread_locals[0].name) - 1);
        c->thread_locals[c->thread_local_count].value = init_value;
        c->thread_locals[c->thread_local_count].type = *type;
        c->thread_local_count++;
    }

    if (c->jit_mode) {
        /* TLS block == data section: append the variable (aligned),
         * write its initial value (or zero for tbss-style variables). */
        size_t off = (c->data_section.length + align - 1) & ~(align - 1);
        for (size_t i = c->data_section.length; i < off; i++) {
            zero_cc_emit_u8(&c->data_section, 0);
        }
        for (size_t b = 0; b < size; b++) {
            zero_cc_emit_u8(&c->data_section,
                has_init ? (uint8_t)((uint64_t)init_value >> (8 * b)) : 0);
        }
        return (int)off;
    }

    /* AOT: the final in-block offset is computed in finalize(); the
     * placeholder 0 is patched onto the registered global there. */
    return 0;
}

/* Pack the TLS block: .tdata = initialized variables (declaration order,
 * aligned), .tbss = uninitialized variables, aligned relative to the end
 * of .tdata.  Patches each variable's registered data_offset to its
 * in-block offset and builds the .tdata contents. */
static void tls_finalize(ZeroCCCompiler *c)
{
    if (c->jit_mode) return;

    /* .tdata/.tbss section alignment: the largest variable alignment.
     * Keeping it small (never above 8) avoids inter-section padding in
     * the combined .tdata block, which linkers count inconsistently for
     * TPOFF32 relocation resolution. */
    size_t max_align = 1;
    for (size_t i = 0; i < c->tls_var_count; i++) {
        if (c->tls_vars[i].align > max_align) max_align = c->tls_vars[i].align;
    }
    if (max_align > 8) max_align = 8;
    c->tls_block_align = max_align;

    size_t tdata_len = 0;
    for (size_t i = 0; i < c->tls_var_count; i++) {
        const ZeroCCTLSVar *t = &c->tls_vars[i];
        if (!t->has_init) continue;
        tdata_len = (tdata_len + t->align - 1) & ~(t->align - 1);
        ZeroCCGlobalVar *gv = find_global_var(t->name);
        if (gv) gv->data_offset = tdata_len;
        tdata_len += t->size;
    }

    size_t tbss_len = 0;
    for (size_t i = 0; i < c->tls_var_count; i++) {
        const ZeroCCTLSVar *t = &c->tls_vars[i];
        if (t->has_init) continue;
        size_t total = tdata_len + tbss_len;
        total = (total + t->align - 1) & ~(t->align - 1);
        ZeroCCGlobalVar *gv = find_global_var(t->name);
        if (gv) gv->data_offset = total;
        tbss_len = (total - tdata_len) + t->size;
    }
    c->tls_bss_size = tbss_len;

    /* .tdata contents (initialized variables, packed with the same
     * alignment as above; the buffer is append-only, so zero-pad first). */
    for (size_t i = 0; i < c->tls_var_count; i++) {
        const ZeroCCTLSVar *t = &c->tls_vars[i];
        if (!t->has_init) continue;
        size_t total = c->tls_data_section.length;
        total = (total + t->align - 1) & ~(t->align - 1);
        for (size_t p = c->tls_data_section.length; p < total; p++) {
            zero_cc_emit_u8(&c->tls_data_section, 0);
        }
        for (size_t b = total; b < total + t->size; b++) {
            uint64_t v = (uint64_t)t->init;
            zero_cc_emit_u8(&c->tls_data_section, (uint8_t)(v >> (8 * (b - total))));
        }
    }
}

/* Setter for TLS backend - NULL resets to default */
void zero_cc_set_tls_backend(struct ZeroCCCompiler *c, const ZeroCCTLSBackend *b)
{
    c->tls_backend = b ? b : &zero_cc_default_tls_backend;
}

/* Export the default TLS backend */
const ZeroCCTLSBackend zero_cc_default_tls_backend = {
    .declare = tls_declare,
    .finalize = tls_finalize,
};
