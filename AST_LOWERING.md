# AST Lowering Status

## Overview
This document tracks the implementation status of AST lowering in the ZeroCC compiler.

## Implemented Features

### Expression Lowering

| Node Type | Status | Notes |
|-----------|--------|-------|
| NODE_INT | ✅ Complete | Integer literals |
| NODE_IDENT | ✅ Complete | Variable references |
| NODE_INDEX | ✅ Complete | Array subscript (char/byte elements) |
| NODE_UNOP | ✅ Complete | Prefix ++/--, postfix ++/-- |
| NODE_BINOP | ✅ Complete | +, -, *, /, %, ==, !=, <, >, <=, >= (including pointer comparisons) |
| NODE_ASSIGN | ✅ Complete | Assignment |
| NODE_CALL | ✅ Complete | Function calls (external) |
| NODE_CAST | ✅ Complete | Casts (no-op for integers) |
| NODE_COND | ✅ Complete | Ternary operator |

### Statement Lowering

| Node Type | Status | Notes |
|-----------|--------|-------|
| NODE_BLOCK | ✅ Complete | Compound statements |
| NODE_RETURN | ✅ Complete | Return statements |
| NODE_IF | ✅ Complete | If-else statements |
| NODE_WHILE | ✅ Complete | While loops |
| NODE_DO_WHILE | ✅ Complete | Do-while loops |
| NODE_FOR | ✅ Complete | For loops |
| NODE_BREAK | ✅ Complete | Break statements |
| NODE_CONTINUE | ✅ Complete | Continue statements |

## Recent Fixes

### NODE_INDEX (Array Subscript)
- **Fixed**: Array subscript lowering for char/byte arrays
- **Implementation**: 
  1. Evaluate base pointer -> rax
  2. Save base to rdx
  3. Evaluate index -> rax
  4. Calculate address: rax = rdx + rax
  5. Load byte: movzx rax, byte [rax]
- **Test**: `test/string_len_jit` now passes

### Postfix Increment/Decrement
- **Fixed**: Postfix ++/-- lowering
- **Implementation**:
  1. Load current value
  2. Save original value to rcx
  3. Increment/decrement rax
  4. Store back to variable
  5. Restore original value to rax (postfix semantics)
- **Test**: `test/string_len_jit` now passes

### Pointer Comparison in Semantic Analysis
- **Fixed**: Comparison operators (`==`, `!=`, `<`, `>`, `<=`, `>=`) now correctly return `int` type
- **Issue**: Previous implementation returned pointer type for pointer comparisons, causing "incompatible return type" errors
- **Test**: `test/string_literal_jit` now passes

## Test Results
- **Total tests**: 40
- **Passing**: 40
- **Failing**: 0

## Files Modified
- `src/zero_cc/parser.c` - NODE_INDEX and postfix ++/-- lowering
- `src/zero_cc/parser/ast_lowering.h` - Synced with parser.c
- `src/zero_cc/semantic/visitor.c` - Fixed comparison operator type inference
- `src/zero_cc/semantic/visitor.h` - Added token.h include
