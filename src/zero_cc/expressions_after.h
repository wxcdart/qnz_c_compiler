    return result;
}

/* Recursive descent parser for constant expression */
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
static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_SIZEOF */
    if (c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    int64_t sz = 8; /* default */
    ZeroCCTokType st = c->tokens[*pos].type;
    if (st == TK_IDENT) {
        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[*pos].str_val);
        if (!tdef) tdef = zero_cc_find_struct(c, c->tokens[*pos].str_val);
        if (tdef) sz = tdef->size;
        else sz = 0; /* unknown type -> treat as zero to trigger error? */
    } else if (st == TK_CHAR || st == TK_BOOL_KW) {
        sz = 1;
    } else if (st == TK_SHORT) {
        sz = 2;
    } else if (st == TK_INT || st == TK_FLOAT_KW) {
        sz = 4;
    } else if (st == TK_LONG || st == TK_DOUBLE_KW || st == TK_VOID) {
        sz = 8;
    } else {
        sz = 0;
    }
    (*pos)++;
    while (c->tokens[*pos].type == TK_STAR) {
        sz = 8; /* pointer size */
        (*pos)++;
    }
    if (c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return sz;
}

#endif /* ZERO_CC_EXPRESSIONS_H */
