#ifndef ZERO_CC_DRIVER_DRIVER_ASM_H
#define ZERO_CC_DRIVER_DRIVER_ASM_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"

static inline void emit_asm_data_sections(ZeroCCCompiler *comp) {
    if (!comp->asm_out) return;

    /* Emit .rodata if present */
    if (comp->rodata_section.length > 0) {
        fprintf(comp->asm_out, "\t.section\t.rodata\n");
        if (comp->isa == ISA_AARCH64) {
            fprintf(comp->asm_out, "\t.align\t3\n");
        } else {
            fprintf(comp->asm_out, "\t.p2align\t3\n");
        }
        fprintf(comp->asm_out, ".Lrodata:\n");
        const uint8_t *ro = (const uint8_t *)comp->rodata_section.data;
        for (size_t i = 0; i < comp->rodata_section.length; i++) {
            fprintf(comp->asm_out, "\t.byte\t0x%02x\n", ro[i]);
        }
    }

    /* Emit .data / global variables if present */
    bool has_data = false;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        if (comp->global_vars[i].name[0] && !comp->global_vars[i].is_tls && !comp->global_vars[i].is_bss &&
            !comp->global_vars[i].alias_target[0]) {
            has_data = true;
            break;
        }
    }
    if (has_data || comp->data_section.length > 0) {
        fprintf(comp->asm_out, "\t.section\t.data\n");
        fprintf(comp->asm_out, ".Lzc_data_start:\n");
        const uint8_t *data_buf = (const uint8_t *)comp->data_section.data;
        size_t data_len = comp->data_section.length;
        size_t cur_offset = 0;

        for (size_t i = 0; i < comp->global_var_count; i++) {
            ZeroCCGlobalVar *gv = &comp->global_vars[i];
            if (!gv->name[0]) continue;
            /* GNU alias entry: no storage of its own, no symbol to emit */
            if (gv->alias_target[0]) continue;
            /* TLS variables live in .tdata/.tbss (emitted below), not .data */
            if (gv->is_tls) continue;

            /* Fill any alignment padding gap before this variable */
            if (gv->data_offset > cur_offset && cur_offset < data_len) {
                size_t pad = (gv->data_offset <= data_len) ? (gv->data_offset - cur_offset) : (data_len - cur_offset);
                for (size_t b = 0; b < pad; b++) {
                    fprintf(comp->asm_out, "\t.byte\t0x%02x\n", data_buf[cur_offset + b]);
                }
                cur_offset += pad;
            }

            /* Alignment directive */
            size_t align = gv->type.align ? gv->type.align : 8;
            if (comp->isa == ISA_AARCH64) {
                int p2 = 0;
                while ((1ULL << p2) < align) p2++;
                fprintf(comp->asm_out, "\t.align\t%d\n", p2);
            } else {
                int p2 = 0;
                while ((1ULL << p2) < align) p2++;
                fprintf(comp->asm_out, "\t.p2align\t%d\n", p2);
            }

            /* Visibility / symbol label */
            if (!gv->is_static) {
                fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
            }
            fprintf(comp->asm_out, "%s:\n", gv->name);

            /* Emit bytes / relocations */
            size_t var_end = gv->data_offset + gv->size;
            size_t byte_pos = gv->data_offset;

            while (byte_pos < var_end) {
                /* Check if there is a 64-bit pointer relocation at byte_pos */
                ZeroCCDataReloc *matching_reloc = NULL;
                for (size_t r = 0; r < comp->data_reloc_count; r++) {
                    if (comp->data_relocs[r].data_patch_offset == byte_pos) {
                        matching_reloc = &comp->data_relocs[r];
                        break;
                    }
                }

                if (matching_reloc && byte_pos + 8 <= var_end) {
                    if (matching_reloc->target_kind == 0) {
                        fprintf(comp->asm_out, "\t.quad\t.Lrodata+%zu\n",
                                matching_reloc->rodata_offset + matching_reloc->target_data_offset);
                    } else if (matching_reloc->target_kind == 1 &&
                               matching_reloc->target_global_idx >= 0 &&
                               matching_reloc->target_global_idx < (int)comp->global_var_count) {
                        const char *tgt_name = comp->global_vars[matching_reloc->target_global_idx].name;
                        /* target_data_offset = gv->data_offset + extra: the
                         * extra (member/element offset) becomes a symbol
                         * addend, e.g. `.quad arr+16` (const-addr-expr-1).
                         * The (size_t)-1 sentinel means "the target's own
                         * storage base" - no addend. */
                        size_t extra = (matching_reloc->target_data_offset == (size_t)-1) ? 0 :
                                       matching_reloc->target_data_offset -
                                       comp->global_vars[matching_reloc->target_global_idx].data_offset;
                        if (extra > 0)
                            fprintf(comp->asm_out, "\t.quad\t%s+%zu\n", tgt_name, extra);
                        else
                            fprintf(comp->asm_out, "\t.quad\t%s\n", tgt_name);
                    } else if (matching_reloc->target_kind == 2 &&
                               matching_reloc->target_extern_name[0] != '\0') {
                        /* External symbol (undefined in this TU). */
                        fprintf(comp->asm_out, "\t.quad\t%s\n",
                                matching_reloc->target_extern_name);
                    } else if (matching_reloc->target_kind == 3 &&
                               matching_reloc->target_extern_name[0] != '\0') {
                        /* Function symbol - a function pointer initializer
                         * (`int (*fp)(void) = foo;` / `{&f}`) needs a
                         * reloc against the function, not a literal 0. */
                        fprintf(comp->asm_out, "\t.quad\t%s\n",
                                matching_reloc->target_extern_name);
                    } else if (matching_reloc->target_kind == 5 &&
                               matching_reloc->target_extern_name[0] != '\0') {
                        /* Code label symbol (&&label) */
                        fprintf(comp->asm_out, "\t.quad\t%s\n",
                                matching_reloc->target_extern_name);
                    } else {
                        fprintf(comp->asm_out, "\t.quad\t0\n");
                    }
                    byte_pos += 8;
                } else if (byte_pos < data_len) {
                    fprintf(comp->asm_out, "\t.byte\t0x%02x\n", data_buf[byte_pos]);
                    byte_pos++;
                } else {
                    fprintf(comp->asm_out, "\t.byte\t0x00\n");
                    byte_pos++;
                }
            }
            cur_offset = var_end;
        }

        /* Emit any remaining trailing data bytes */
        if (cur_offset < data_len) {
            for (size_t b = cur_offset; b < data_len; b++) {
                /* Kind-4 patch (address of an in-TU static local): emit a
                 * section-relative reloc instead of the raw placeholder. */
                ZeroCCDataReloc *mr4 = NULL;
                for (size_t r = 0; r < comp->data_reloc_count; r++) {
                    if (comp->data_relocs[r].target_kind == 4 &&
                        comp->data_relocs[r].data_patch_offset == b) {
                        mr4 = &comp->data_relocs[r];
                        break;
                    }
                }
                if (mr4 && b + 8 <= data_len) {
                    fprintf(comp->asm_out, "\t.quad\t.Lzc_data_start+%zu\n",
                            mr4->target_data_offset);
                    b += 7;
                    continue;
                }
                fprintf(comp->asm_out, "\t.byte\t0x%02x\n", data_buf[b]);
            }
        }
    }

    /* Emit .bss (zero-initialized globals above threshold) */
    bool has_bss = false;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        if (comp->global_vars[i].name[0] && comp->global_vars[i].is_bss && !comp->global_vars[i].is_tls &&
            !comp->global_vars[i].alias_target[0]) {
            has_bss = true;
            break;
        }
    }
    if (has_bss) {
        if (comp->isa == ISA_AARCH64) {
            fprintf(comp->asm_out, "\t.section\t.bss,\"aw\",@nobits\n");
        } else {
            fprintf(comp->asm_out, "\t.section\t.bss\n");
        }
        for (size_t i = 0; i < comp->global_var_count; i++) {
            ZeroCCGlobalVar *gv = &comp->global_vars[i];
            if (!gv->name[0] || gv->is_tls || !gv->is_bss || gv->alias_target[0]) continue;

            size_t align = gv->type.align ? gv->type.align : 8;
            if (comp->isa == ISA_AARCH64) {
                int p2 = 0;
                while ((1ULL << p2) < align) p2++;
                fprintf(comp->asm_out, "\t.align\t%d\n", p2);
            } else {
                int p2 = 0;
                while ((1ULL << p2) < align) p2++;
                fprintf(comp->asm_out, "\t.p2align\t%d\n", p2);
            }
            if (!gv->is_static) {
                fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
            }
            fprintf(comp->asm_out, "%s:\n", gv->name);
            fprintf(comp->asm_out, "\t.zero\t%zu\n", gv->size);
        }
    }

    /* Emit the static TLS block (initial-exec, x86-64): .tdata holds the
     * initialized variables (packed image in tls_data_section), .tbss the
     * uninitialized ones.  In-block offsets come from the TLS backend
     * (tls_backend.c); the code references them via name@tpoff. */
    if (comp->tls_data_section.length > 0 || comp->tls_bss_size > 0) {
        const uint8_t *tdata = (const uint8_t *)comp->tls_data_section.data;
        size_t tdata_len = comp->tls_data_section.length;
        size_t emitted = 0;
        fprintf(comp->asm_out, "\t.section\t.tdata,\"awT\",@progbits\n");
        for (size_t i = 0; i < comp->global_var_count; i++) {
            ZeroCCGlobalVar *gv = &comp->global_vars[i];
            if (!gv->is_tls || !gv->name[0]) continue;
            if (gv->data_offset >= tdata_len) continue;
            if (gv->data_offset > emitted) {
                for (size_t b = emitted; b < gv->data_offset && b < tdata_len; b++) {
                    fprintf(comp->asm_out, "\t.byte\t0x%02x\n", tdata[b]);
                }
                emitted = gv->data_offset;
            }
            size_t talign = gv->type.align ? gv->type.align : 8;
            int tp2 = 0;
            while ((1u << tp2) < talign) tp2++;
            fprintf(comp->asm_out, "\t.p2align\t%d\n", tp2);
            if (!gv->is_static) {
                fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
            }
            fprintf(comp->asm_out, "%s:\n", gv->name);
            for (size_t b = 0; b < gv->size && emitted + b < tdata_len; b++) {
                fprintf(comp->asm_out, "\t.byte\t0x%02x\n", tdata[emitted + b]);
            }
            emitted += gv->size;
        }
        if (comp->tls_bss_size > 0) {
            size_t tbss_off = 0;
            fprintf(comp->asm_out, "\t.section\t.tbss,\"aw\",@nobits\n");
            for (size_t i = 0; i < comp->global_var_count; i++) {
                ZeroCCGlobalVar *gv = &comp->global_vars[i];
                if (!gv->is_tls || !gv->name[0]) continue;
                if (gv->data_offset < tdata_len) continue;
                size_t boff = gv->data_offset - tdata_len;
                if (boff > tbss_off) {
                    fprintf(comp->asm_out, "\t.space\t%zu\n", boff - tbss_off);
                    tbss_off = boff;
                }
                size_t talign = gv->type.align ? gv->type.align : 8;
                int tp2 = 0;
                while ((1u << tp2) < talign) tp2++;
                fprintf(comp->asm_out, "\t.p2align\t%d\n", tp2);
                if (!gv->is_static) {
                    fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
                }
                fprintf(comp->asm_out, "%s:\n", gv->name);
                fprintf(comp->asm_out, "\t.space\t%zu\n", gv->size);
                tbss_off += gv->size;
            }
        }
    }

    fprintf(comp->asm_out, "\t.text\n");
    fclose(comp->asm_out);
    comp->asm_out = NULL;
}

#endif /* ZERO_CC_DRIVER_DRIVER_ASM_H */
