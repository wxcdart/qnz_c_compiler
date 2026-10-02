import re

def get_file(path):
    with open(path) as f:
        return f.read()

binary_content = get_file('src/zero_cc/expressions/binary.h')
visitor_content = get_file('src/zero_cc/expressions/visitor.h')

def extract_func(content, func_name, return_type='static inline int64_t'):
    # Simple regex to find the function block
    pattern = re.compile(rf"{return_type} {func_name}\([^)]*\)\s*{{.*?^}}", re.MULTILINE | re.DOTALL)
    match = pattern.search(content)
    if match:
        return match.group(0)
    
    # Try different return types if not found
    pattern = re.compile(rf"static inline ZeroCCType {func_name}\([^)]*\)\s*{{.*?^}}", re.MULTILINE | re.DOTALL)
    match = pattern.search(content)
    if match:
        return match.group(0)
        
    return f"// Function {func_name} not found"

with open('src/zero_cc/parser/const_eval.h', 'w') as f:
    f.write('''#ifndef ZERO_CC_PARSER_CONST_EVAL_H
#define ZERO_CC_PARSER_CONST_EVAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../types/registry.h"

/* Forward declarations for recursive functions */
static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_additive(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_unary(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_primary(ZeroCCCompiler *c, size_t *pos);

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);

static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);
static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos);

static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
static inline int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);

''')

    f.write(extract_func(visitor_content, 'eval_sizeof') + '\n\n')
    f.write(extract_func(visitor_content, 'eval_alignof') + '\n\n')
    
    f.write(extract_func(binary_content, 'parse_equality') + '\n\n')
    f.write(extract_func(binary_content, 'parse_relational') + '\n\n')
    f.write(extract_func(binary_content, 'parse_additive') + '\n\n')
    f.write(extract_func(binary_content, 'parse_multiplicative') + '\n\n')
    f.write(extract_func(binary_content, 'parse_unary') + '\n\n')
    f.write(extract_func(binary_content, 'parse_primary') + '\n\n')
    
    f.write(extract_func(binary_content, 'zero_cc_type_of_equality') + '\n\n')
    f.write(extract_func(binary_content, 'zero_cc_type_of_relational') + '\n\n')
    f.write(extract_func(binary_content, 'zero_cc_type_of_additive') + '\n\n')
    f.write(extract_func(binary_content, 'zero_cc_type_of_multiplicative') + '\n\n')
    f.write(extract_func(binary_content, 'zero_cc_type_of_unary') + '\n\n')
    f.write(extract_func(binary_content, 'zero_cc_type_of_primary') + '\n\n')
    
    f.write(extract_func(visitor_content, 'zero_cc_type_find_member') + '\n\n')
    f.write(extract_func(visitor_content, 'zero_cc_parse_type_name_at_pos') + '\n\n')
    f.write(extract_func(visitor_content, 'zero_cc_parse_type_name') + '\n\n')
    f.write(extract_func(visitor_content, 'zero_cc_eval_const_expr') + '\n\n')
    
    f.write('#endif /* ZERO_CC_PARSER_CONST_EVAL_H */\n')

