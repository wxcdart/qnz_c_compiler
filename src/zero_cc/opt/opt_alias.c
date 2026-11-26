#include "zero_cc/opt/opt_alias.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/type_system.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/core/core.h"

/* =========================================================================
 * Sprint 236: Type-Based Alias Analysis (TBAA - Tier 3, Item #15)
 *
 * Implements ISO C99 6.5p7 strict aliasing rules:
 * - Two pointers of incompatible types cannot alias.
 * - Character pointers (char, uint8_t, signed char) can alias any type.
 * - Restrict qualifiers with distinct groups cannot alias.
 * - Distinct stack slots and disjoint globals cannot alias.
 * ========================================================================= */

TBAATypeClass zero_cc_tbaa_class_from_op(int op) {
    switch (op) {
        case OP_STORE_BYTE:
        case OP_LOAD_BYTE:
            return TBAA_CHAR;
        case OP_STORE_16:
        case OP_LOAD_16:
            return TBAA_INT16;
        case OP_STORE_32:
        case OP_LOAD_32:
            return TBAA_INT32;
        case OP_STORE_64:
        case OP_STORE64:
        case OP_LOAD64:
        case OP_DEREF:
            return TBAA_INT64;
        case OP_VSTORESS:
        case OP_VLOADSS:
            return TBAA_FLOAT;
        case OP_FSTORE:
        case OP_FLOAD:
        case OP_VSTORESD:
        case OP_VLOADSD:
            return TBAA_DOUBLE;
        default:
            return TBAA_UNKNOWN;
    }
}

bool zero_cc_tbaa_can_alias(TBAATypeClass t1, TBAATypeClass t2) {
    if (t1 == TBAA_UNKNOWN || t2 == TBAA_UNKNOWN) {
        return true;
    }
    /* C99 6.5p7: Character types can alias any object */
    if (t1 == TBAA_CHAR || t2 == TBAA_CHAR) {
        return true;
    }
    return t1 == t2;
}

bool zero_cc_tbaa_types_can_alias(const ZeroCCType *t1, const ZeroCCType *t2) {
    if (!t1 || !t2) return true;
    if (t1->tag == TYPE_VOID || t2->tag == TYPE_VOID) return true;

    /* Character types alias everything */
    if (t1->tag == TYPE_INT8 || t1->tag == TYPE_UINT8 ||
        t2->tag == TYPE_INT8 || t2->tag == TYPE_UINT8) {
        return true;
    }

    /* Aggregate/union can alias its members */
    if (t1->tag == TYPE_STRUCT || t1->tag == TYPE_UNION ||
        t2->tag == TYPE_STRUCT || t2->tag == TYPE_UNION) {
        return true;
    }

    /* Pointers can alias other pointers (void*, T*, etc.) */
    if (t1->tag == TYPE_PTR && t2->tag == TYPE_PTR) {
        if (!t1->base || !t2->base) return true;
        return zero_cc_tbaa_types_can_alias(t1->base, t2->base);
    }

    /* Compatible scalar types (signed and unsigned forms can alias per 6.5p7) */
    if (t1->size != t2->size) return false;

    bool t1_is_fp = (t1->tag == TYPE_FLOAT || t1->tag == TYPE_DOUBLE);
    bool t2_is_fp = (t2->tag == TYPE_FLOAT || t2->tag == TYPE_DOUBLE);
    if (t1_is_fp != t2_is_fp) return false;

    return true;
}

/* Helper to trace pointer temporary back to base and offset */
typedef struct {
    int base_kind; /* 0 = unknown, 1 = stack_slot, 2 = global, 3 = base_temp */
    int64_t slot_or_disp;
    char global_name[64];
    uint32_t base_temp;
} PtrOrigin;

static void trace_ptr_origin(ZeroCCCompiler *c, uint32_t temp, PtrOrigin *out) {
    memset(out, 0, sizeof(*out));
    if (temp == TEMP_NONE) return;

    out->base_temp = temp;
    out->base_kind = 3;

    int cur = (int)temp;
    int limit = 16;
    while (limit-- > 0 && cur >= 0) {
        int def = -1;
        for (int i = (int)ir_get_count(c) - 1; i >= 0; i--) {
            Instr *ins = ir_get(c, i);
            if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
            if ((int)ins->temp_dest == cur) { def = i; break; }
        }
        if (def < 0) break;
        Instr *ins = ir_get(c, def);
        if (!ins) break;

        if (ins->op == OP_LEA) {
            out->base_kind = 1; /* Stack slot */
            out->slot_or_disp += (ins->imm64 ? ins->imm64 : ins->imm);
            break;
        } else if (ins->op == OP_LEA_RIP) {
            out->base_kind = 2; /* Rodata */
            snprintf(out->global_name, sizeof(out->global_name), ".Lrodata");
            out->slot_or_disp += ins->imm64;
            break;
        } else if (ins->op == OP_MOV_GLOBAL) {
            out->base_kind = 2; /* Global symbol */
            const char *sym = (const char *)(intptr_t)ins->imm64;
            if (sym && (uintptr_t)sym > 0x10000) {
                snprintf(out->global_name, sizeof(out->global_name), "%s", sym);
            }
            break;
        } else if (ins->op == OP_MOV_REG) {
            cur = (int)ins->temp_src1;
        } else if (ins->op == OP_ADD) {
            int d2 = -1;
            for (int i = def - 1; i >= 0; i--) {
                Instr *di = ir_get(c, i);
                if (di && di->temp_dest == ins->temp_src2) { d2 = i; break; }
            }
            if (d2 >= 0) {
                Instr *di = ir_get(c, d2);
                if (di && di->op == OP_MOV_IMM) {
                    out->slot_or_disp += (di->imm64 ? di->imm64 : di->imm);
                    cur = (int)ins->temp_src1;
                    continue;
                }
            }
            break;
        } else {
            break;
        }
    }
}

AliasResult zero_cc_alias_query(ZeroCCCompiler *c,
                                int op1, uint32_t addr1, uint8_t group1,
                                int op2, uint32_t addr2, uint8_t group2) {
    /* 1. C99 restrict group separation */
    if (group1 != 0 && group2 != 0 && group1 != group2) {
        return ALIAS_NO;
    }

    /* 2. TBAA strict aliasing type check */
    TBAATypeClass t1 = zero_cc_tbaa_class_from_op(op1);
    TBAATypeClass t2 = zero_cc_tbaa_class_from_op(op2);
    if (!zero_cc_tbaa_can_alias(t1, t2)) {
        return ALIAS_NO;
    }

    /* 3. Pointer origin analysis */
    if (addr1 != TEMP_NONE && addr2 != TEMP_NONE) {
        if (addr1 == addr2) {
            return ALIAS_MUST;
        }

        PtrOrigin o1, o2;
        trace_ptr_origin(c, addr1, &o1);
        trace_ptr_origin(c, addr2, &o2);

        /* Distinct stack frame slots */
        if (o1.base_kind == 1 && o2.base_kind == 1) {
            if (o1.slot_or_disp != o2.slot_or_disp) {
                int64_t diff = o1.slot_or_disp - o2.slot_or_disp;
                if (diff < 0) diff = -diff;
                if (diff >= 8) {
                    return ALIAS_NO;
                }
            }
        }

        /* Distinct global variables */
        if (o1.base_kind == 2 && o2.base_kind == 2) {
            if (o1.global_name[0] && o2.global_name[0]) {
                if (strcmp(o1.global_name, o2.global_name) != 0) {
                    return ALIAS_NO;
                } else if (strcmp(o1.global_name, ".Lrodata") == 0) {
                    int64_t diff = o1.slot_or_disp - o2.slot_or_disp;
                    if (diff < 0) diff = -diff;
                    if (diff >= 8) {
                        return ALIAS_NO;
                    }
                }
            }
        }

        /* Stack vs Global */
        if ((o1.base_kind == 1 && o2.base_kind == 2) ||
            (o1.base_kind == 2 && o2.base_kind == 1)) {
            return ALIAS_NO;
        }
    }

    return ALIAS_MAYBE;
}
