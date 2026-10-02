# Expressions.h Refactoring Status

## Overview
This document tracks the refactoring of the monolithic `expressions.h` file into modular sub-files.

## Status: ✅ Complete

The monolithic `expressions.h` (1,688 lines) has been split into 6 modular header files.

## File Structure

| File | Lines | Purpose |
|------|-------|---------|
| `expressions.h` | 13 | Wrapper that includes sub-files |
| `expressions/primary.h` | 589 | Primary expression parsing (literals, identifiers, calls) |
| `expressions/unary.h` | 186 | Unary operator parsing (++ , --, +, -, !, ~, *, &) |
| `expressions/ternary.h` | 357 | Conditional/ternary operator parsing |
| `expressions/binary.h` | 420 | Binary operators (+, -, *, /, ==, !=, <, >, etc.) and const eval |
| `expressions/visitor.h` | 240 | Type inference helpers, member lookup, type_name parsing |

**Total**: 1,805 lines (includes + headers overhead)

## Implementation Details

### Forward Declarations
Each sub-file includes forward declarations for:
- `zero_cc_parse_expr()` - Main expression parser
- `zero_cc_parse_stmt()` - Statement parser
- `zero_cc_eval_const_expr()` - Constant expression evaluation
- `zero_cc_parse_type_name()` - Type name parsing
- `zero_cc_type_of_*()` - Type-only parsers for sizeof/_Alignof
- `zero_cc_type_find_member()` - Struct/union member lookup

### Key Functions by File

#### primary.h
- `zero_cc_parse_primary()` - Parse primary expressions
- `parse_primary()` - Constant evaluation version

#### unary.h
- `zero_cc_parse_unary()` - Parse unary operators
- `parse_unary()` - Constant evaluation version

#### ternary.h
- `zero_cc_parse_expr()` - Main expression parser (includes conditional)
- Conditional/ternary operator handling

#### binary.h
- `parse_multiplicative()` - *, /, %
- `parse_additive()` - +, -
- `parse_relational()` - <, >, <=, >=
- `parse_equality()` - ==, !=
- Type-only parsers: `zero_cc_type_of_*`
- Constant evaluation versions

#### visitor.h
- `zero_cc_parse_type_name()` - Parse type names
- `zero_cc_type_find_member()` - Find struct/union member types
- `eval_sizeof()` - sizeof operator
- `eval_alignof()` - _Alignof operator
- `zero_cc_eval_const_expr()` - Constant expression evaluation

## Test Results
- **Total tests**: 40
- **Passing**: 40
- **Failing**: 0

## Files Modified
- `src/zero_cc/expressions.h` - Wrapper (13 lines)
- `src/zero_cc/expressions/primary.h` - Primary expression parsing
- `src/zero_cc/expressions/unary.h` - Unary operator parsing
- `src/zero_cc/expressions/ternary.h` - Conditional parsing
- `src/zero_cc/expressions/binary.h` - Binary operators
- `src/zero_cc/expressions/visitor.h` - Type inference helpers

## Next Steps
Continue with remaining refactoring targets:
- `semantic/` directory
- `jit/` directory

---

# Core.h Refactoring Status

## Status: ✅ Complete

The monolithic `core.h` (1,197 lines) has been split into 5 modular header files.

## File Structure

| File | Lines | Purpose |
|------|-------|---------|
| `core.h` | 16 | Wrapper that includes sub-files |
| `core/utils.h` | 88 | Utilities, strlcpy, register mapping |
| `core/buffers.h` | 111 | Buffer and pointer stack utilities |
| `core/compiler_state.h` | 56 | Constants, hash, compiler state structs |
| `core/preprocessor.h` | 943 | Preprocessor implementation |

**Total**: 1,214 lines (includes + headers overhead)

## Implementation Details

### Key Functions by File

#### utils.h
- `zero_strlcpy()` - Safe string copy
- `zero_cc_get_regmap()` - Register mapping for target architecture

#### buffers.h
- `zero_cc_buf_*` - Byte/string buffer utilities
- `zero_ptr_stack_*` - Pointer stack utilities

#### compiler_state.h
- Constants (MAX_INCLUDE_PATHS, MAX_MACROS, etc.)
- `zero_cc_hash64()` - FNV-1a hash function
- Compiler state structs (ZeroCCPreprocessor, etc.)

#### preprocessor.h
- `zero_cc_cpp_*` - Preprocessor functions
- `zero_cc_read_file()` - File reading
- `zero_cc_cpp_process_text()` - Text preprocessing
- `zero_cc_preprocess()` - Main preprocessing entry point

## Test Results
- **Total tests**: 40
- **Passing**: 40
- **Failing**: 0
