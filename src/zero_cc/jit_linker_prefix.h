/* Parse and Compile a Top-Level Function */
#ifndef ZERO_CC_JIT_LINKER_H
#define ZERO_CC_JIT_LINKER_H

static inline void zero_cc_compile_function(ZeroCCCompiler *c) {
    /* Skip static / inline / const */
    while (c->tokens[c->token_pos].type == TK_STATIC || c->tokens[c->token_pos].type == TK_INLINE ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0)) {
        c->token_pos++;
    }

    /* Return type */
    ZeroCCType fn_ret_type = zero_cc_type_int();
    if (c->tokens[c->token_pos].type == TK_STRUCT) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCType *st = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
            if (!st) st = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
            if (st) fn_ret_type = *st;
            c->token_pos++;
        }
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
        if (tdef) fn_ret_type = *tdef;
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_CHAR ||
               c->tokens[c->token_pos].type == TK_VOID || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
               c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
               c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
               c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
        c->token_pos++;
    }
    while (c->tokens[c->token_pos].type == TK_STAR) {
        fn_ret_type = zero_cc_type_ptr(NULL);
        c->token_pos++;
    }

    if (c->tokens[c->token_pos].type != TK_IDENT) return;
    char fn_name[128];
    snprintf(fn_name, sizeof(fn_name), "%s", c->tokens[c->token_pos].str_val);
    c->token_pos++;

    if (c->tokens[c->token_pos].type != TK_LPAREN) return;
    c->token_pos++;

    /* Parse Parameters in signature */
    ZeroCCType param_types[8];
    char param_names[8][128];
    size_t param_count = 0;
    bool is_variadic_fn = false;

    while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
        size_t ppos = c->token_pos;
        if (c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            is_variadic_fn = true;
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_IDENT || c->tokens[c->token_pos].type == TK_INT ||
            c->tokens[c->token_pos].type == TK_CHAR || c->tokens[c->token_pos].type == TK_VOID ||
            c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
            c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
            c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
            c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
            const char *ptname = c->tokens[c->token_pos].str_val;
            ZeroCCType *pfound = zero_cc_find_typedef(c, ptname);
            if (!pfound) pfound = zero_cc_find_struct(c, ptname);
            c->token_pos++;
            bool is_ptr = false;
            while (c->tokens[c->token_pos].type == TK_STAR) { is_ptr = true; c->token_pos++; }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                if (param_count < 8) {
                    snprintf(param_names[param_count], sizeof(param_names[param_count]), "%s", c->tokens[c->token_pos].str_val);
                    param_types[param_count] = is_ptr ? zero_cc_type_ptr(pfound) : (pfound ? *pfound : zero_cc_type_int64());
                    if (pfound) snprintf(param_types[param_count].struct_name, sizeof(param_types[param_count].struct_name), "%s", ptname);
                    param_count++;
                }
                c->token_pos++;
            }
        }
        if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        if (c->token_pos == ppos) c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    if (c->tokens[c->token_pos].type == TK_SEMI) {
        /* Function prototype declaration: do not emit prologue */
        c->token_pos++;
        return;
    }

    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;

        if (c->function_count >= ZERO_CC_MAX_FUNCTIONS) return;
        ZeroCCFunction *fn = &c->functions[c->function_count++];
        snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
        fn->text_offset = c->text_section.length;
        fn->param_count = param_count;
        fn->return_type = fn_ret_type;
        fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT);
        fn->ret_struct_sz = fn_ret_type.size;
        fn->is_variadic = is_variadic_fn;

        c->local_count = 0;
        c->current_stack_frame = 16;

        /* Function Prologue */
        zero_cc_emit_u8(&c->text_section, 0x55);          /* push rbp */
        zero_cc_emit_u8(&c->text_section, 0x48);          /* mov rbp, rsp */
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0xE5);
        zero_cc_emit_u8(&c->text_section, 0x48);          /* sub rsp, 256 */
        zero_cc_emit_u8(&c->text_section, 0x81);
        zero_cc_emit_u8(&c->text_section, 0xEC);
        zero_cc_emit_u32(&c->text_section, 256);

        /* Save parameter registers (RDI, RSI, RDX, RCX, R8, R9) to stack */
        static const uint8_t save_param_regs[6][3] = {
            {0x48, 0x89, 0xBD}, /* mov [rbp - offset], rdi */
            {0x48, 0x89, 0xB5}, /* mov [rbp - offset], rsi */
            {0x48, 0x89, 0x95}, /* mov [rbp - offset], rdx */
            {0x48, 0x89, 0x8D}, /* mov [rbp - offset], rcx */
            {0x4C, 0x89, 0x85}, /* mov [rbp - offset], r8  */
            {0x4C, 0x89, 0x8D}  /* mov [rbp - offset], r9  */
        };

        for (size_t p = 0; p < param_count; p++) {
            ZeroCCSym *sym = zero_cc_add_local(c, param_names[p], param_types[p]);
            sym->is_param = true;
            sym->param_reg_idx = (int)p;
            if (p < 6) {
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][0]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][1]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][2]);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            }
        }

        /* Variadic spill area: spill remaining registers for va_start */
        if (is_variadic_fn) {
            for (size_t p = param_count; p < 6; p++) {
                c->current_stack_frame += 8;
                int32_t spill_offset = c->current_stack_frame;
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][0]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][1]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][2]);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-spill_offset));
            }
        }

        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            size_t spos = c->token_pos;
            zero_cc_parse_stmt(c);
            if (c->token_pos == spos) c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;

        /* Function Epilogue */
        zero_cc_emit_u8(&c->text_section, 0xC9);
        zero_cc_emit_u8(&c->text_section, 0xC3);
    }
}

/* Master Translation Unit Compiler */
static inline void zero_cc_compile_c99(ZeroCCCompiler *c) {
    zero_cc_buf_init(&c->text_section, 4096);
    zero_cc_buf_init(&c->rodata_section, 1024);
    zero_cc_buf_init(&c->data_section, 1024);
    c->function_count = 0;
    c->reloc_count = 0;
    zero_cc_init_standard_types(c);

