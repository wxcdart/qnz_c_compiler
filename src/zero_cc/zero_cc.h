/* Auto-generated include/zero_cc.h from parts */
/* ==========================================================================
 *  zero_cc.h
 *  Single-Header C99 Freestanding Compiler, JIT Engine & ELF64 Linker.
 *  
 *  Pipeline:
 *    [Source C Code / zero.h Headers]
 *          │
 *          ▼
 *    1. Preprocessor (CPP): #include, #define (obj/func), #if/#ifdef/#ifndef, ##, #
 *          │
 *          ▼
 *    2. Type & Struct Registry: typedef, struct members, pointers (*), arrays ([])
 *          │
 *          ▼
 *    3. Parser & Multi-Function Compiler:
 *       - if/else, while, for, do/while, break, continue, return
 *       - GNU Statement Expressions ({ ... })
 *       - Struct member access (. and ->), Array indexing ([]), Pointer math
 *       - Multi-argument function calls & System V AMD64 ABI (rdi, rsi, rdx, rcx, r8, r9)
 *          │
 *          ▼
 *    4. Linker & Machine Code Emitters:
 *       - JIT Engine (mmap / dlsym host symbol resolution / execution)
 *       - ELF64 Object Emitter (.o with .text, .rodata, .data, .symtab, .rela)
 *       - ELF64 Executable Emitter (Standalone executable binary with _start)
 *  ========================================================================== */

#ifndef ZERO_CC_H
#define ZERO_CC_H

/* Direct sub-headers for zero_cc */
#include "zero_cc/core.h"
#include "zero_cc/types_lexer.h"
#include "zero_cc/expressions.h"
#include "zero_cc/statements.h"
#include "zero_cc/jit_linker.h"


#endif /* ZERO_CC_H */
