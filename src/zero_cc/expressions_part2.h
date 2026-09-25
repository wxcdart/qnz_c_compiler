                    else if (sub_op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E);
                    else if (sub_op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F);
                    else if (sub_op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D);
                    zero_cc_emit_u8(&c->text_section, 0xC0);
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0xB6); zero_cc_emit_u8(&c->text_section, 0xC0);
                }
            }
            /* test rax, rax; setne al; movzx rax, al */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x95);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            /* jmp done */
            zero_cc_emit_u8(&c->text_section, 0xEB);
            zero_cc_emit_u8(&c->text_section, 0x07); /* skip 7 bytes of mov rax, 0 */

            /* false_branch: */
            int32_t jz_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
            memcpy(c->text_section.data + jz_pos, &jz_rel, 4);
            /* mov rax, 0 */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xC7);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u32(&c->text_section, 0);
            continue;
        }

        /* store LHS on stack: if float, store 8-byte double; else push rax (8-byte int) */
        if (lhs_is_float) {
            /* sub rsp,8; movsd [rsp], xmm0 */
            zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xEC); zero_cc_emit_u8(&c->text_section, 0x08);
            zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x11);
            /* modrm for [rsp] -> 0x04 0x24 addressing: use movsd [rsp], xmm0 -> F2 0F 11 04 24 */
            zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
        } else {
            zero_cc_emit_u8(&c->text_section, 0x50); /* push rax (LHS int) */
        }

        zero_cc_parse_unary(c);                  /* RHS in rax or xmm0 */

        bool rhs_is_float = c->last_expr_is_float;

        /* If either operand is float, perform SSE double ops */
        if (lhs_is_float || rhs_is_float) {
            /* Load LHS from stack into xmm0 as double (cvtsi2sd if int, movsd if float) */
            if (lhs_is_float) {
                /* movsd xmm0, [rsp] -> F2 0F 10 04 24 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x10);
                zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
            } else {
                /* cvtsi2sd xmm0, qword [rsp] -> F2 0F 2A 04 24 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x2A);
                zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
            }

            /* Load RHS into xmm1: if RHS is float, xmm0 currently holds RHS, so move xmm0->xmm1; else convert rax->xmm1 */
            if (rhs_is_float) {
                /* movsd xmm1, xmm0 -> F2 0F 11 C8? We'll use movsd xmm1, xmm0: F2 0F 10 C8? Use movapd variant: 66 0F 28 C8? Simpler: movlhps can't be used. We'll use movsd with reg-reg: F2 0F 10 C8+ (reg=dest<<3|src)
                 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x10);
                /* modrm: dest=1 (xmm1), src=0 (xmm0) -> 0xC8 */
                zero_cc_emit_u8(&c->text_section, 0xC8);
            } else {
                /* cvtsi2sd xmm1, rax -> F2 0F 2A C8 (modrm dest=1 src=0) */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x2A);
                zero_cc_emit_u8(&c->text_section, 0xC8);
            }

            /* perform operation on xmm0 (LHS) and xmm1 (RHS), result in xmm0 */
            if (op == TK_PLUS) {
                /* addsd xmm0, xmm1 -> F2 0F 58 C1? modrm reg=0 dest=0? For xmm0,xmm1: reg=0 dest=0 src=1 -> 0xC1? We'll use reg=0, rm=1 -> 0xC1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x58); zero_cc_emit_u8(&c->text_section, 0xC1);
            } else if (op == TK_MINUS) {
                /* subsd xmm0, xmm1 -> F2 0F 5C C1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x5C); zero_cc_emit_u8(&c->text_section, 0xC1);
            } else if (op == TK_STAR) {
                /* mulsd xmm0, xmm1 -> F2 0F 59 C1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x59); zero_cc_emit_u8(&c->text_section, 0xC1);
            } else if (op == TK_SLASH) {
                /* divsd xmm0, xmm1 -> F2 0F 5E C1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x5E); zero_cc_emit_u8(&c->text_section, 0xC1);
            }

            /* cleanup stack slot used for LHS */
            if (lhs_is_float) {
                zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xC4); zero_cc_emit_u8(&c->text_section, 0x08);
            } else {
                /* pop original LHS int placeholder -> adjust rsp by 8 (pop into rax) and then discard; we'll use add rsp,8 */
                zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xC4); zero_cc_emit_u8(&c->text_section, 0x08);
            }

            c->last_expr_is_float = 1;
            continue;
        }

        /* Non-floating binary ops (existing integer/path) */
        /* mov rcx, rax (RHS in rcx) */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0xC1);
        zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax (LHS in rax) */

        if (op == TK_PLUS) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_MINUS) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x29);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_STAR) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xAF);
            zero_cc_emit_u8(&c->text_section, 0xC1);
        } else if (op == TK_SLASH) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
        } else if (op == TK_PERCENT) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
            /* result in rdx -> mov rax, rdx */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xD0);
        } else if (op == TK_AMP) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x21);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_PIPE) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x09);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_CARET) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_SHL) {
            /* shl rax, cl */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, 0xE0);
        } else if (op == TK_SHR) {
            /* shr rax, cl */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, 0xE8);
        } else {
            /* Comparisons: rax is LHS, rcx is RHS. cmp rax, rcx computes (rax - rcx) */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x39);
            zero_cc_emit_u8(&c->text_section, 0xC8);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            if (op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);
            else if (op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95);
            else if (op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C);
            else if (op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E);
            else if (op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F);
            else if (op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0xC0);
        }
    }

    /* Ternary Conditional Operator: cond ? then_val : else_val */
    if (c->tokens[c->token_pos].type == TK_QUESTION) {
        c->token_pos++;
        /* test cond in rax: test rax, rax; jz else_branch */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x85);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x84);
        size_t jz_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        zero_cc_parse_expr(c); /* then_val in rax */

        /* jmp end_pos */
        zero_cc_emit_u8(&c->text_section, 0xE9);
        size_t jmp_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        int32_t else_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
        memcpy(c->text_section.data + jz_pos, &else_rel, 4);

        if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
        zero_cc_parse_expr(c); /* else_val in rax */

        int32_t end_rel = (int32_t)(c->text_section.length - (jmp_pos + 4));
        memcpy(c->text_section.data + jmp_pos, &end_rel, 4);
    }
}

/* Recursive descent parser for constant expression */
static inline int64_t parse_primary(ZeroCCCompiler *c, size_t *pos);
static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
static inline int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_unary(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_additive(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos);

/* Type-only expression parser forward declarations */
static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);
static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos);

static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_relational(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            int64_t right = parse_relational(c, pos);
            if (op == TK_EQ)
                left = (left == right);
            else
                left = (left != right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            int64_t right = parse_additive(c, pos);
            int64_t res = 0;
            if (op == TK_LT) res = (left < right);
            else if (op == TK_GT) res = (left > right);
            else if (op == TK_LE) res = (left <= right);
            else if (op == TK_GE) res = (left >= right);
            left = res;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_additive(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_multiplicative(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_PLUS || op == TK_MINUS) {
            (*pos)++;
            int64_t right = parse_multiplicative(c, pos);
            if (op == TK_PLUS)
                left = left + right;
            else
                left = left - right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_multiplicative(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_unary(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT) {
            (*pos)++;
            int64_t right = parse_unary(c, pos);
            if (op == TK_STAR)
                left = left * right;
            else if (op == TK_SLASH)
                left = left / right;
            else
                left = left % right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_unary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_PLUS) {
        (*pos)++;
        return parse_unary(c, pos);
    }
    if (t == TK_MINUS) {
        (*pos)++;
        return -parse_unary(c, pos);
    }
    if (t == TK_TILDE) {
        (*pos)++;
        return ~parse_unary(c, pos);
    }
    if (t == TK_BANG) {
        (*pos)++;
        return !parse_unary(c, pos);
    }
    if (t == TK_SIZEOF) {
        return eval_sizeof(c, pos);
    }
    if (t == TK_ALIGNOF) {
        return eval_alignof(c, pos);
    }
    return parse_primary(c, pos);
}

static inline int64_t parse_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        int64_t val = c->tokens[*pos].num_val;
        (*pos)++;
        return val;
    }
    if (t == TK_TRUE) {
        (*pos)++;
        return 1;
    }
    if (t == TK_FALSE) {
        (*pos)++;
        return 0;
    }
    if (t == TK_NULLPTR) {
        (*pos)++;
        return 0;
    }
    if (t == TK_LPAREN) {
        (*pos)++;
        int64_t val = parse_equality(c, pos);
        if (c->tokens[*pos].type == TK_RPAREN) {
            (*pos)++;
        }
        return val;
    }
    /* Should not reach here for valid constant expression */
    return 0;
}

/* Evaluate sizeof: returns size in bytes */


/* Type-only expression parser for sizeof/_Alignof operand */

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_relational(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            ZeroCCType right = zero_cc_type_of_relational(c, pos);
            left = zero_cc_type_int(); /* comparison yields int */
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            ZeroCCType right = zero_cc_type_of_additive(c, pos);
            left = zero_cc_type_int();
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_multiplicative(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_PLUS || op == TK_MINUS) {
            (*pos)++;
            ZeroCCType right = zero_cc_type_of_multiplicative(c, pos);
            /* Pointer arithmetic: if left is ptr and right is int, result is ptr */
            if (left.tag == TYPE_PTR && (right.tag == TYPE_INT32 || right.tag == TYPE_INT64 ||
                right.tag == TYPE_UINT32 || right.tag == TYPE_UINT64)) {
                /* result is same pointer type */
            } else {
                left = zero_cc_type_int();
            }
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_unary(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT) {
            (*pos)++;
            ZeroCCType right = zero_cc_type_of_unary(c, pos);
            left = zero_cc_type_int();
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_PLUS) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_MINUS) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_TILDE) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_BANG) {
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_SIZEOF) {
        /* sizeof expression is size_t (uint64_t) */
        size_t save_pos = *pos;
        eval_sizeof(c, pos);
        return zero_cc_type_uint64();
    }
    if (t == TK_ALIGNOF) {
        size_t save_pos = *pos;
        eval_alignof(c, pos);
        return zero_cc_type_uint64();
    }
    if (t == TK_STAR) { /* dereference */
        (*pos)++;
        ZeroCCType operand = zero_cc_type_of_unary(c, pos);
        if (operand.tag == TYPE_PTR && operand.base) {
            return *operand.base;
        }
        return operand;
    }
    if (t == TK_AMP) { /* address-of */
        (*pos)++;
        ZeroCCType operand = zero_cc_type_of_unary(c, pos);
        return zero_cc_type_ptr(&operand);
    }
    return zero_cc_type_of_primary(c, pos);
}

static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_FLOAT) {
        (*pos)++;
        return zero_cc_type_double();
    }
    if (t == TK_CHAR_LIT) {
        (*pos)++;
        return zero_cc_type_char();
    }
    if (t == TK_TRUE || t == TK_FALSE) {
        (*pos)++;
        return zero_cc_type_int(); /* _Bool */
    }
    if (t == TK_NULLPTR) {
        (*pos)++;
        return zero_cc_type_nullptr();
    }
    if (t == TK_STRING) {
        (*pos)++;
        return zero_cc_type_ptr(NULL);
    }
    /* Type keywords as primary (for sizeof(type)/_Alignof(type)) */
    if (t == TK_INT) { (*pos)++; return zero_cc_type_int(); }
    if (t == TK_CHAR) { (*pos)++; return zero_cc_type_char(); }
    if (t == TK_VOID) { (*pos)++; return zero_cc_type_void(); }
    if (t == TK_FLOAT_KW) { (*pos)++; return zero_cc_type_float(); }
    if (t == TK_DOUBLE_KW) { (*pos)++; return zero_cc_type_double(); }
    if (t == TK_BOOL_KW) { (*pos)++; return zero_cc_type_bool(); }
    if (t == TK_SHORT) { (*pos)++; return zero_cc_type_int16(); }
    if (t == TK_LONG) { (*pos)++; return zero_cc_type_int64(); }
    if (t == TK_SIGNED) { (*pos)++; return zero_cc_type_int(); }
    if (t == TK_UNSIGNED) { (*pos)++; return zero_cc_type_uint32(); }
    if (t == TK_ATOMIC) { (*pos)++; if (c->tokens[*pos].type == TK_LPAREN) { (*pos)++; } ZeroCCType ty = zero_cc_type_of_primary(c, pos); if (c->tokens[*pos].type == TK_RPAREN) (*pos)++; ty.is_atomic = true; return ty; }
    if (t == TK_LPAREN) {
        (*pos)++;
        /* Could be cast or parenthesized expression */
        /* Look ahead: if next token is a type keyword, treat as cast */
        ZeroCCTokType next = c->tokens[*pos].type;
        if (next == TK_IDENT || next == TK_INT || next == TK_CHAR || next == TK_VOID ||
            next == TK_FLOAT_KW || next == TK_DOUBLE_KW || next == TK_BOOL_KW ||
            next == TK_SIGNED || next == TK_UNSIGNED || next == TK_SHORT || next == TK_LONG || next == TK_ATOMIC) {
            /* Cast: (type) expr */
            ZeroCCType cast_ty = zero_cc_parse_type_name_at_pos(c, pos);
            if (c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            ZeroCCType expr_ty = zero_cc_type_of_unary(c, pos);
            (void)expr_ty; /* ignore - cast result is cast_ty */
            return cast_ty;
        }
        /* Parenthesized expression */
        ZeroCCType ty = zero_cc_type_of_equality(c, pos);
        if (c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        return ty;
    }
    if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        (*pos)++;
        /* Check for local variable */
        ZeroCCSym *sym = zero_cc_find_local(c, name);
        if (sym) {
            ZeroCCType ty = sym->type;
            /* Handle member access: sym->member or sym.member */
            while (true) {
                ZeroCCTokType mt = c->tokens[*pos].type;
                if (mt == TK_DOT || mt == TK_ARROW) {
                    bool is_arrow = (mt == TK_ARROW);
                    (*pos)++;
                    if (c->tokens[*pos].type == TK_IDENT) {
                        const char *mem_name = c->tokens[*pos].str_val;
                        (*pos)++;
                        ty = zero_cc_type_find_member(c, ty, mem_name);
                    }
                    continue;
                }
                if (mt == TK_LBRACKET) { /* array indexing */
                    (*pos)++;
                    zero_cc_type_of_equality(c, pos); /* consume index */
                    if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    if (ty.tag == TYPE_ARRAY && ty.base) {
                        ty = *ty.base;
                    } else if (ty.tag == TYPE_PTR && ty.base) {
                        ty = *ty.base;
                    }
                    continue;
                }
                break;
            }
            return ty;
        }
        /* Check for typedef/struct */
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (tdef) {
            ZeroCCType ty = *tdef;
            /* Handle member access on type name (e.g., S.x) */
            while (c->tokens[*pos].type == TK_DOT) {
                (*pos)++;
                if (c->tokens[*pos].type == TK_IDENT) {
                    const char *mem_name = c->tokens[*pos].str_val;
                    (*pos)++;
                    ty = zero_cc_type_find_member(c, ty, mem_name);
                }
            }
            return ty;
        }
        /* Unknown identifier */
        return zero_cc_type_int();
    }
    /* Should not reach here for valid constant expression */
    return zero_cc_type_int();
}

/* Helper: find member type in struct/union */
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name)
{
    if (base.tag == TYPE_PTR && base.base) {
        base = *base.base;
    }
    if (base.tag != TYPE_STRUCT) {
        return zero_cc_type_int();
    }
    for (size_t i = 0; i < base.member_count; i++) {
        if (strcmp(base.members[i].name, member_name) == 0) {
            ZeroCCType member_ty;
            memset(&member_ty, 0, sizeof(member_ty));
            member_ty.tag = base.members[i].tag;
            member_ty.size = base.members[i].size;
            member_ty.align = base.members[i].align;
            member_ty.base = base.members[i].base;
            snprintf(member_ty.struct_name, sizeof(member_ty.struct_name), "%s", base.members[i].name);
            return member_ty;
        }
    }
    /* Member not found - return int */
    return zero_cc_type_int();
}

/* Parse type name at current position (helper for casts in type-only parser) */
static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType ty;
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (tdef) {
            ty = *tdef;
            (*pos)++;
        } else {
            ty = zero_cc_type_int();
            (*pos)++;
        }
    } else if (t == TK_INT) {
        ty = zero_cc_type_int(); (*pos)++;
    } else if (t == TK_CHAR) {
        ty = zero_cc_type_char(); (*pos)++;
    } else if (t == TK_VOID) {
        ty = zero_cc_type_void(); (*pos)++;
    } else if (t == TK_FLOAT_KW) {
        ty = zero_cc_type_float(); (*pos)++;
    } else if (t == TK_DOUBLE_KW) {
        ty = zero_cc_type_double(); (*pos)++;
    } else if (t == TK_BOOL_KW) {
        ty = zero_cc_type_bool(); (*pos)++;
    } else if (t == TK_SHORT) {
        ty = zero_cc_type_int16(); (*pos)++;
    } else if (t == TK_LONG) {
        ty = zero_cc_type_int64(); (*pos)++;
    } else if (t == TK_ATOMIC) {
        ty = zero_cc_type_int(); (*pos)++;
    } else {
        ty = zero_cc_type_int();
    }
    while (c->tokens[*pos].type == TK_STAR) {
        ty = zero_cc_type_ptr(&ty);
        (*pos)++;
    }
    return ty;
}

/* Updated eval_sizeof using type-only parser */
static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_SIZEOF */
    if (c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty = zero_cc_type_of_equality(c, pos);
    if (c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.size;
}

/* Updated eval_alignof using type-only parser */
static inline int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_ALIGNOF */
    if (c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty = zero_cc_type_of_equality(c, pos);
    if (c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.align ? ty.align : 1;
}

static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c)
{
    size_t pos = c->token_pos;
    int64_t result = parse_equality(c, &pos);
    c->token_pos = pos;
    return result;
}

