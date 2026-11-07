#ifndef ZERO_CC_JIT_TYPE_INFO_H
#define ZERO_CC_JIT_TYPE_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <string.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"

/* Parse and Compile a Top-Level Function */

static inline TypeInfo *zero_cc_type_to_typeinfo(Arena *perm, ZeroCCType t) {
    /* Recursion & cycle/DAG memoization guard: a self-referential struct
     * (`struct S { struct S *next, *prev; }`) or shared struct graph
     * otherwise expands exponentially (B^(depth/2)).  We memoize converted
     * struct/union TypeInfo* records within each top-level call (and tie
     * recursive cycles directly to the in-progress TypeInfo* before
     * expanding members), turning O(B^d) into O(V + E). */
    typedef struct {
        Arena      *perm;
        const void *members;
        char        struct_name[128];
        size_t      size;
        size_t      member_count;
        uint8_t     tag;
        uint32_t    modifiers;
        TypeInfo   *ti;
    } ZCT2TIMemo;
    static int zc_t2ti_depth = 0;
    static ZCT2TIMemo zc_t2ti_memo[256];
    static int zc_t2ti_memo_cnt = 0;
    if (zc_t2ti_depth == 0) {
        zc_t2ti_memo_cnt = 0;
    }
    bool zc_t2ti_break = (zc_t2ti_depth >= 48);
    zc_t2ti_depth++;
    // Guard against uninitialized/corrupted types while allowing valid large arrays and structs.
    // NOTE: the threshold must comfortably exceed the largest legitimate struct.  zerocc's
    // own state struct (ZeroCCCompiler) is ~62 MB because it embeds large fixed arrays
    // (structs[], typedefs[], buffers, ...), so a 16 MB bound mis-flagged it as corrupt
    // and silently degraded pointers to it into pointer_to(int), breaking every member
    // access on `c->...`.  A genuinely uninitialized size is a random 64-bit value
    // (typically >1 TB), so a 1 GB bound still catches corruption while admitting real
    // structs and large user arrays.
    bool is_legit_large = ((t.tag == TYPE_STRUCT || t.tag == TYPE_UNION) && t.member_count <= 64) ||
                          (t.tag == TYPE_ARRAY && t.base != NULL);
    if (!is_legit_large && t.size > (1ULL * 1024 * 1024 * 1024)) {
        zc_t2ti_depth--;
        return type_int(perm);
    }
    uint32_t t_mods = 0;
    if (t.is_const) t_mods |= TYPE_MOD_CONST;
    if (t.is_volatile) t_mods |= TYPE_MOD_VOLATILE;
    if (t.is_restrict) t_mods |= TYPE_MOD_RESTRICT;
    if (t.is_atomic) t_mods |= TYPE_MOD_ATOMIC;
    TypeInfo *ti = NULL;
    /* C99 6.2.5: _Complex T -> a 2-element complex type; details.array
     * carries the element type (real part first, then imaginary). */
    if (t.is_complex) {
        ti = type_new(perm);
        if (!ti) {
            zc_t2ti_depth--;
            return type_int(perm);
        }
        ti->kind = TYPE_KIND_COMPLEX;
        ti->details.array.base = t.base ? zero_cc_type_to_typeinfo(perm, *t.base) : type_double(perm);
        ti->details.array.count = 2;
        zc_t2ti_depth--;
        return ti;
    }
    switch (t.tag) {
        case TYPE_VOID: ti = type_void(perm); break;
        case TYPE_BOOL: ti = type_bool(perm); break;
        case TYPE_INT8: ti = type_char(perm); break;
        case TYPE_UINT8: ti = type_uchar(perm); break;
        case TYPE_INT16: ti = type_short(perm); break;
        case TYPE_UINT16: ti = type_ushort(perm); break;
        case TYPE_INT32: ti = type_int(perm); break;
        case TYPE_UINT32: ti = type_uint(perm); break;
        case TYPE_INT64: ti = type_long(perm); break;
        case TYPE_UINT64: ti = type_ulong(perm); break;
        case TYPE_INT128: ti = type_int128(perm); break;
        case TYPE_UINT128: ti = type_uint128(perm); break;
        case TYPE_FLOAT: ti = type_float(perm); break;
        case TYPE_DOUBLE: ti = type_double(perm); if (t.is_long_double) ti->is_long_double = true; if (t.is_float128) ti->is_float128 = true; break;
        case TYPE_NULLPTR: ti = type_pointer_to(perm, type_void(perm)); break;
        case TYPE_PTR: {
            // FIX: Guard against NULL base pointer (incomplete types like FILE*)
            if (!t.base) {
                ti = type_pointer_to(perm, type_void(perm));
            } else if (t.is_func_ptr) {
                TypeInfo *ret_ti = zero_cc_type_to_typeinfo(perm, *t.base);
                /* Rebuild the pointee's declared parameter list when the
                 * function-pointer type carries it (parsed prototype);
                 * otherwise keep the old no-prototype (variadic) shape. */
                TypeInfo **fn_params = NULL;
                size_t fn_pcount = 0;
                bool fn_variadic = true;
                if (t.base->fn_params && t.base->fn_param_count > 0) {
                    fn_pcount = t.base->fn_param_count;
                    fn_params = (TypeInfo **)malloc(fn_pcount * sizeof(TypeInfo *));
                    if (fn_params) {
                        for (size_t i = 0; i < fn_pcount; i++)
                            fn_params[i] = zero_cc_type_to_typeinfo(perm, t.base->fn_params[i]);
                        fn_variadic = t.base->fn_is_variadic;
                    } else {
                        fn_params = NULL;
                        fn_pcount = 0;
                    }
                }
                TypeInfo *fn_ti = type_function(perm, ret_ti, fn_params, fn_pcount, fn_variadic);
                if (fn_params) free(fn_params);
                ti = type_pointer_to(perm, fn_ti);
            } else {
                TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
                ti = type_pointer_to(perm, base);
            }
            break;
        }
        case TYPE_ARRAY: {

            // FIX: Guard against NULL base pointer
            if (!t.base) {
                ti = type_array_of(perm, type_void(perm), t.array_len);
            } else {
                TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
                ti = type_array_of(perm, base, t.array_len);
            }
            if (ti) ti->details.array.is_vla = t.is_vla;
            break;
        }
        case TYPE_STRUCT:
        case TYPE_UNION: {
            if (t.members != NULL || t.struct_name[0] != '\0') {
                for (int m = 0; m < zc_t2ti_memo_cnt; m++) {
                    if (zc_t2ti_memo[m].perm == perm &&
                        zc_t2ti_memo[m].tag == (uint8_t)t.tag &&
                        zc_t2ti_memo[m].modifiers == t_mods &&
                        zc_t2ti_memo[m].size == t.size &&
                        zc_t2ti_memo[m].member_count == t.member_count &&
                        ((t.members != NULL && zc_t2ti_memo[m].members == t.members) ||
                         (t.struct_name[0] != '\0' &&
                          strncmp(zc_t2ti_memo[m].struct_name, t.struct_name, sizeof(zc_t2ti_memo[m].struct_name) - 1) == 0))) {
                        zc_t2ti_depth--;
                        return zc_t2ti_memo[m].ti;
                    }
                }
            }
            ti = type_new(perm);
            ti->kind = (t.tag == TYPE_UNION) ? TYPE_KIND_UNION : TYPE_KIND_STRUCT;
            if (t.struct_name[0] != '\0') {
                size_t tlen = strlen(t.struct_name);
                char *tag_copy = (char *)arena_alloc(perm, tlen + 1);
                if (tag_copy) {
                    memcpy(tag_copy, t.struct_name, tlen + 1);
                    ti->details.record.tag = tag_copy;
                } else {
                    ti->details.record.tag = NULL;
                }
            } else {
                ti->details.record.tag = NULL;
            }
            ti->details.record.size = t.size;
            ti->details.record.align = t.align;
            ti->details.record.is_vla_struct = t.is_vla_struct;
            ti->modifiers |= t_mods;
            if ((t.members != NULL || t.struct_name[0] != '\0') && zc_t2ti_memo_cnt < 256) {
                ZCT2TIMemo *me = &zc_t2ti_memo[zc_t2ti_memo_cnt++];
                me->perm = perm;
                me->members = t.members;
                snprintf(me->struct_name, sizeof(me->struct_name), "%s", t.struct_name);
                me->size = t.size;
                me->member_count = t.member_count;
                me->tag = (uint8_t)t.tag;
                me->modifiers = t_mods;
                me->ti = ti;
            }
            if (zc_t2ti_break) break; /* cycle guard: no member expansion */
            for (size_t i = 0; i < t.member_count; i++) {
                size_t nlen = strlen(t.members[i].name);
                char *name_copy = (char *)arena_alloc(perm, nlen + 1);
                if (name_copy) {
                    memcpy(name_copy, t.members[i].name, nlen + 1);
                }
                ZeroCCType mt;
                if (t.members[i].tag == TYPE_ARRAY) {
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = TYPE_ARRAY;
                    mt.size = t.members[i].size;
                    mt.align = t.members[i].align;
                    mt.base = t.members[i].base;
                    mt.array_len = (t.members[i].base && t.members[i].base->size > 0)
                                       ? (t.members[i].size / t.members[i].base->size)
                                       : 0;
                } else if (t.members[i].tag == TYPE_PTR) {
                    /* Pointer member: the member's own type is the pointer
                     * (C5). m->base is the pointee (may be NULL → ptr→void).
                     * A function-pointer member's base carries is_func (the
                     * function type); preserve it so the field's TypeInfo is
                     * pointer→function, not pointer→(return type) - without
                     * it `arr[i].f()` through a struct member loses the
                     * return type and an FP return is read from RAX
                     * (c-torture execute/ieee/fp-cmp-8). */
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = TYPE_PTR;
                    mt.size = t.members[i].size ? t.members[i].size : 8;
                    mt.align = t.members[i].align ? t.members[i].align : 8;
                    mt.base = t.members[i].base;
                    mt.is_func_ptr = t.members[i].is_func_ptr ||
                                     (t.members[i].base && t.members[i].base->is_func);
                } else if (t.members[i].tag == TYPE_USER_0) {
                    /* GNU vector member: the member's own type is the vector
                     * (base = the element type, size = total width). */
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = t.members[i].tag;
                    mt.size = t.members[i].size;
                    mt.align = t.members[i].align;
                    mt.base = t.members[i].base;
                } else if (t.members[i].base) {
                    mt = *t.members[i].base;
                    /* C99 6.7.2.3: a complex member keeps the complex
                     * marker (base = element type); unwrapping to the
                     * element would type `s.a` as plain double/float. */
                    if (t.members[i].is_complex) {
                        mt = zero_cc_type_complex(&mt);
                    }
                } else {
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = t.members[i].tag;
                    mt.size = t.members[i].size;
                    mt.align = t.members[i].align;
                    mt.base = NULL;
                    /* Complex scalar member (`_Complex double a;`): the
                     * member stores the doubled size but no base pointer;
                     * rebuild the complex type from the element. */
                    if (t.members[i].is_complex) {
                        ZeroCCType el;
                        memset(&el, 0, sizeof(el));
                        el.tag = t.members[i].tag;
                        el.size = t.members[i].size / 2;
                        el.align = t.members[i].align;
                        mt = zero_cc_type_complex(&el);
                    }
                }
                if (t.members[i].is_const) mt.is_const = true;
                if (t.members[i].is_volatile) mt.is_volatile = true;
                if (t.members[i].is_atomic) mt.is_atomic = true;
                TypeInfo *mti = zero_cc_type_to_typeinfo(perm, mt);
                const char *fname = name_copy ? name_copy : t.members[i].name;
                if (t.members[i].is_bitfield) {
                    type_add_field_ext(perm, ti, fname, t.members[i].offset, mti,
                                       true, t.members[i].bit_offset, t.members[i].bit_width,
                                       t.members[i].is_signed, t.members[i].is_flexible);
                    if (t.members[i].sso_be &&
                        ti->details.record.field_count > 0) {
                        ti->details.record.fields[ti->details.record.field_count - 1].sso_be = true;
                        ti->details.record.fields[ti->details.record.field_count - 1].sso_unit = t.members[i].sso_unit;
                    }
                } else {
                    type_add_field_ext(perm, ti, fname, t.members[i].offset, mti,
                                       false, 0, 0, false, t.members[i].is_flexible);
                }
            }
            break;
        }
        case TYPE_ENUM: {
            ti = type_new(perm);
            ti->kind = TYPE_KIND_ENUM;
            if (t.struct_name[0] != '\0') {
                size_t tlen = strlen(t.struct_name);
                char *tag_copy = (char *)arena_alloc(perm, tlen + 1);
                if (tag_copy) {
                    memcpy(tag_copy, t.struct_name, tlen + 1);
                    ti->details.record.tag = tag_copy;
                }
            }
            ti->details.record.size = 4;
            ti->details.record.align = 4;
            break;
        }
        case TYPE_USER_0: {  /* GNU vector type: base = element type */
            if (t.base) {
                TypeInfo *elem = zero_cc_type_to_typeinfo(perm, *t.base);
                ti = type_vector(perm, elem ? elem : type_int(perm), t.size);
            } else {
                ti = type_vector(perm, type_int(perm), t.size);
            }
            break;
        }
        default: ti = type_int(perm); break;
    }
    if (ti) {
        if (t.is_const) ti->modifiers |= TYPE_MOD_CONST;
        if (t.is_volatile) ti->modifiers |= TYPE_MOD_VOLATILE;
        if (t.is_restrict) ti->modifiers |= TYPE_MOD_RESTRICT;
        if (t.is_atomic) ti->modifiers |= TYPE_MOD_ATOMIC;
    }
    zc_t2ti_depth--;
    return ti;
}

/* Convert a semantic TypeInfo* back into a canonical ZeroCCType */
static inline ZeroCCType typeinfo_to_zero_cc_type(const TypeInfo *ti) {
    ZeroCCType t;
    memset(&t, 0, sizeof(t));
    if (!ti) {
        return zero_cc_type_int();
    }
    bool is_u = (ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
    switch (ti->kind) {
        case TYPE_KIND_VOID:    t = zero_cc_type_void(); break;
        case TYPE_KIND_BOOL:    t = zero_cc_type_bool(); break;
        case TYPE_KIND_CHAR:    t = is_u ? zero_cc_type_uint8()  : zero_cc_type_char(); break;
        case TYPE_KIND_SHORT:   t = is_u ? zero_cc_type_uint16() : zero_cc_type_int16(); break;
        case TYPE_KIND_INT:     t = is_u ? zero_cc_type_uint32() : zero_cc_type_int(); break;
        case TYPE_KIND_LONG:    t = is_u ? zero_cc_type_uint64() : zero_cc_type_int64(); break;
        case TYPE_KIND_INT128: t = is_u ? zero_cc_type_uint128() : zero_cc_type_int128(); break;
        case TYPE_KIND_FLOAT:   t = zero_cc_type_float(); break;
        case TYPE_KIND_DOUBLE:  t = ti->is_float128 ? zero_cc_type_float128() : (ti->is_long_double ? zero_cc_type_long_double() : zero_cc_type_double()); break;
        case TYPE_KIND_ENUM:    t = zero_cc_type_enum(ti->details.record.tag); break;
        case TYPE_KIND_FUNCTION: {
            ZeroCCType ret_t = typeinfo_to_zero_cc_type(ti->details.function.ret);
            t = ret_t;
            t.base = zero_cc_type_alloc_base(&ret_t);
            t.is_func = true;
            break;
        }
        case TYPE_KIND_POINTER: {
            if (ti->details.pointer.base && ti->details.pointer.base->kind == TYPE_KIND_FUNCTION) {
                ZeroCCType ret_t = typeinfo_to_zero_cc_type(ti->details.pointer.base->details.function.ret);
                t = zero_cc_type_ptr(&ret_t);
                t.is_func_ptr = true;
                /* Rebuild the pointee's declared prototype when the
                 * TypeInfo carries one. Function PARAMETERS reach the
                 * lowering stage through this conversion (the parser sets
                 * nf->param_types[pi] = typeinfo_to_zero_cc_type(
                 * fc->param_tis[pi])), so without the prototype an indirect
                 * call through a function-pointer parameter marshals
                 * arguments against no signature: `void t(void (*f)
                 * (float, float)); f(1.0f, 2.0f)` stored the floats as
                 * model doubles instead of 32-bit floats (the SysV
                 * float-arg form) and the callee read 0.0f. Mirrors the
                 * declarator-stack path (zero_cc_chunk_apply FUNCTION
                 * chunk) and the reverse conversion in
                 * zero_cc_type_to_typeinfo above. */
                if (t.base && ti->details.pointer.base->details.function.param_cnt > 0) {
                    const TypeInfo *pfn = ti->details.pointer.base;
                    size_t pc = pfn->details.function.param_cnt;
                    ZeroCCType *params = (ZeroCCType *)malloc(pc * sizeof(ZeroCCType));
                    if (params) {
                        for (size_t i = 0; i < pc; i++)
                            params[i] = typeinfo_to_zero_cc_type(pfn->details.function.params[i]);
                        t.base->is_func = true;
                        t.base->fn_params = (const ZeroCCType *)params;
                        t.base->fn_param_count = pc;
                        t.base->fn_has_prototype = pfn->details.function.is_prototype;
                        t.base->fn_is_variadic = pfn->details.function.is_variadic;
                    }
                }
            } else if (ti->details.pointer.base) {
                ZeroCCType base_t = typeinfo_to_zero_cc_type(ti->details.pointer.base);
                t = zero_cc_type_ptr(&base_t);
            } else {
                t = zero_cc_type_ptr(NULL);
            }
            break;
        }
        case TYPE_KIND_ARRAY: {
            ZeroCCType base_t = typeinfo_to_zero_cc_type(ti->details.array.base);
            t = zero_cc_type_array(&base_t, (ssize_t)ti->details.array.count);
            if (ti->details.array.is_vla) {
                t.is_vla = true;
                /* Element size as the static factor (runtime sizeof walks
                 * the chain), matching zero_cc_chunk_apply. */
                t.size = base_t.size > 0 ? base_t.size : 1;
                t.align = 8;
            }
            break;
        }
        case TYPE_KIND_VECTOR: {
            ZeroCCType base_t = typeinfo_to_zero_cc_type(ti->details.array.base);
            t = zero_cc_type_vector(&base_t, type_size(ti));
            break;
        }
        case TYPE_KIND_COMPLEX: {
            ZeroCCType base_t = typeinfo_to_zero_cc_type(ti->details.array.base);
            t = zero_cc_type_complex(&base_t);
            break;
        }
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION: {
            t.tag = (ti->kind == TYPE_KIND_UNION) ? TYPE_UNION : TYPE_STRUCT;
            t.size = ti->details.record.size;
            t.align = ti->details.record.align ? ti->details.record.align : 1;
            t.is_vla_struct = ti->details.record.is_vla_struct;
            if (ti->details.record.tag) {
                snprintf(t.struct_name, sizeof(t.struct_name), "%s", ti->details.record.tag);
            }
            break;
        }
        default:
            t = zero_cc_type_int();
            break;
    }
    t.is_const    = (ti->modifiers & TYPE_MOD_CONST) != 0;
    t.is_volatile = (ti->modifiers & TYPE_MOD_VOLATILE) != 0;
    t.is_restrict = (ti->modifiers & TYPE_MOD_RESTRICT) != 0;
    t.is_atomic   = (ti->modifiers & TYPE_MOD_ATOMIC) != 0;
    return t;
}

/* Canonical accessor to retrieve TypeInfo* attached to an AST node */
static inline TypeInfo *ast_node_get_typeinfo(ZeroCCCompiler *c, const ZeroCCASTNode *node) {
    if (!c || !c->sem_ctx || !c->sem_ctx->perm_arena || !node || node->aux.extra == 0) {
        return NULL;
    }
    return (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
}

/* Canonical accessor to attach TypeInfo* to an AST node */
static inline void ast_node_set_typeinfo(ZeroCCCompiler *c, ZeroCCASTNode *node, TypeInfo *ti) {
    if (!c || !c->sem_ctx || !c->sem_ctx->perm_arena || !node || !ti) {
        return;
    }
    size_t offset = arena_index_of(c->sem_ctx->perm_arena, ti);
    if (offset != (size_t)-1) {
        node->aux.extra = (uint32_t)(offset + 1);
    }
}

/* Sprint 121 (prototype unification): the function signature lives in three
 * representations - ZeroCCFunction::param_types (function table; direct calls),
 * TypeInfo.function (semantic; indirect calls), ZeroCCType.fn_params (fnptr
 * pointee). The semantic TypeInfo of a known function OFTEN carries no
 * prototype (param_cnt == 0) - reading it silently marshals a call against no
 * signature (__builtin_apply first shipped broken this way, passing
 * stale-register garbage). These helpers make the function table the single
 * source of truth for a call's argument layout: the resolved callee's
 * declared prototype wins, and the callee expression's pointee TypeInfo is
 * the fallback (indirect calls through a function pointer). */

/* The function TypeInfo a callee expression designates: the pointee for a
 * pointer-to-function, the function type itself otherwise; NULL when none. */
static inline const TypeInfo *zc_call_pointee_typeinfo(ZeroCCCompiler *c,
                                                       const ZeroCCASTNode *callee_node) {
    if (!callee_node) return NULL;
    TypeInfo *cal_ti = ast_node_get_typeinfo(c, callee_node);
    if (cal_ti && cal_ti->kind == TYPE_KIND_POINTER && cal_ti->details.pointer.base &&
        cal_ti->details.pointer.base->kind == TYPE_KIND_FUNCTION)
        return cal_ti->details.pointer.base;
    if (cal_ti && cal_ti->kind == TYPE_KIND_FUNCTION)
        return cal_ti;
    return NULL;
}

/* The parameter types a call marshals arguments against (single source of
 * truth for the call ABI's argument layout). The function table is
 * authoritative when the resolved callee declares a prototype; otherwise the
 * callee expression's pointee TypeInfo drives the layout. Pass callee_node ==
 * NULL to disable the TypeInfo fallback - a known function with no declared
 * prototype then marshals with 0 params and no fallback ( __builtin_apply's
 * semantics). Fills params_out[0..max_params-1]; returns the count (0 =
 * K&R-style: marshal without converting against declared types). */
static inline int zero_cc_effective_call_params(ZeroCCCompiler *c,
                                                ZeroCCFunction *callee_fn,
                                                const ZeroCCASTNode *callee_node,
                                                ZeroCCType *params_out, int max_params) {
    if (callee_fn && callee_fn->param_types && callee_fn->param_count > 0) {
        size_t n = callee_fn->param_count;
        if (n > (size_t)max_params) n = (size_t)max_params;
        for (size_t i = 0; i < n; i++) params_out[i] = callee_fn->param_types[i];
        return (int)n;
    }
    int n = 0;
    const TypeInfo *pf = zc_call_pointee_typeinfo(c, callee_node);
    if (pf) {
        size_t pc = pf->details.function.param_cnt;
        n = (int)(pc < (size_t)max_params ? pc : (size_t)max_params);
        for (int i = 0; i < n; i++)
            params_out[i] = typeinfo_to_zero_cc_type(pf->details.function.params[i]);
    }
    return n;
}

/* Canonical base type resolver for declarations, parameters, and return types */
static inline ZeroCCType zero_cc_resolve_base_type(ZeroCCCompiler *c, ZeroCCTokType type_tok, const char *type_name) {
    if (type_tok == TK_CHAR || type_tok == TK_BOOL_KW) return zero_cc_type_char();
    if (type_tok == TK_SHORT) return zero_cc_type_int16();
    if (type_tok == TK_INT) return zero_cc_type_int();
    if (type_tok == TK_LONG) return zero_cc_type_int64();
    if (type_tok == TK_FLOAT_KW) return zero_cc_type_float();
    if (type_tok == TK_DOUBLE_KW) return zero_cc_type_double();
    if (type_tok == TK_VOID) return zero_cc_type_void();
    if (type_tok == TK_STRUCT || type_tok == TK_UNION) {
        ZeroCCType *st = type_name ? zero_cc_find_struct(c, type_name) : NULL;
        if (!st && type_name) st = zero_cc_find_typedef(c, type_name);
        if (st) return *st;
        /* Forward reference to a not-yet-defined struct/union tag: give it
         * an incomplete (zero-size) identity instead of silently falling
         * back to long - `struct S *p;` with S undefined must be a pointer
         * TO S, not long* (c-torture pr46388: member assignment REJECTed
         * as incompatible).  A later definition re-registers the tag with
         * the complete layout (zero_cc_register_struct replaces by name
         * and the sync helpers refresh dependent types). */
        if (type_name &&
            strcmp(type_name, "struct") != 0 && strcmp(type_name, "union") != 0 &&
            strcmp(type_name, "enum") != 0) {
            ZeroCCType inc;
            memset(&inc, 0, sizeof(inc));
            inc.tag = (type_tok == TK_UNION) ? TYPE_UNION : TYPE_STRUCT;
            inc.size = 0;
            inc.align = 1;
            snprintf(inc.struct_name, sizeof(inc.struct_name), "%s", type_name);
            zero_cc_register_struct(c, type_name, inc);
            return inc;
        }
        return zero_cc_type_int64();
    }
    if (type_tok == TK_ENUM) {
        ZeroCCType *et = type_name ? zero_cc_find_enum(c, type_name) : NULL;
        if (!et && type_name) et = zero_cc_find_typedef(c, type_name);
        return et ? *et : zero_cc_type_enum(type_name);
    }
    if (type_name) {
        if (strcmp(type_name, "__builtin_va_list") == 0) return zero_cc_type_va_list();
        /* GNU __int128 / __uint128_t (Sprint 109): 16-byte scalar base. */
        if (strcmp(type_name, "__int128") == 0 || strcmp(type_name, "__int128_t") == 0)
            return zero_cc_type_int128();
        if (strcmp(type_name, "__uint128_t") == 0) return zero_cc_type_uint128();
        /* GNU __float128 (Sprint 112): 16-byte quad-precision FP base
         * (lowered through the 64-bit double value model). */
        if (strcmp(type_name, "__float128") == 0) return zero_cc_type_float128();
        ZeroCCType *tdef = zero_cc_find_typedef(c, type_name);
        if (!tdef) tdef = zero_cc_find_struct(c, type_name);
        if (!tdef) tdef = zero_cc_find_enum(c, type_name);
        if (tdef) return *tdef;
    }
    return zero_cc_type_int64();
}

/* Cross-system type compatibility: checks if a ZeroCCType and TypeInfo match */
static inline bool zero_cc_type_matches_typeinfo(const ZeroCCType *zt, const TypeInfo *ti) {
    if (!zt || !ti) return false;
    if (zt->is_complex != (ti->kind == TYPE_KIND_COMPLEX)) return false;
    if (zt->is_long_double != ti->is_long_double) return false;
    if (zt->is_float128 != ti->is_float128) return false;
    if (zero_cc_type_is_unsigned(zt) != type_is_unsigned(ti)) return false;

    if (zero_cc_type_is_integer(zt)) return type_is_integer(ti) && zt->size == type_size(ti);
    if (zero_cc_type_is_floating(zt)) return type_is_floating(ti) && zt->size == type_size(ti);
    if (zero_cc_type_is_pointer(zt)) {
        if (!type_is_pointer(ti)) return false;
        if (zt->is_func_ptr || (zt->base && zt->base->is_func)) {
            return ti->details.pointer.base && type_is_function(ti->details.pointer.base);
        }
        if (!zt->base || !ti->details.pointer.base) return true;
        return zero_cc_type_matches_typeinfo(zt->base, ti->details.pointer.base);
    }
    if (zero_cc_type_is_array(zt)) {
        if (!type_is_array(ti)) return false;
        if (zt->array_len != ti->details.array.count && zt->array_len != 0 && ti->details.array.count != 0) return false;
        if (!zt->base || !ti->details.array.base) return true;
        return zero_cc_type_matches_typeinfo(zt->base, ti->details.array.base);
    }
    if (zero_cc_type_is_struct_or_union(zt)) {
        if (!type_is_struct_or_union(ti)) return false;
        if ((zt->tag == TYPE_STRUCT) != (ti->kind == TYPE_KIND_STRUCT)) return false;
        if (zt->struct_name[0] && ti->details.record.tag) {
            return strcmp(zt->struct_name, ti->details.record.tag) == 0;
        }
        return zt->size == ti->details.record.size;
    }
    return false;
}

#endif /* ZERO_CC_JIT_TYPE_INFO_H */
