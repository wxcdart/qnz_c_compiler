/* ==========================================================================
 *  zero_cc_fwd.h
 *  Forward Declarations for zero_cc Compiler
 *  ==========================================================================
 *
 *  This header provides forward declarations for all major zero_cc types.
 *  Include this instead of full headers when you only need opaque pointers.
 *
 *  Benefits:
 *  - Reduces compilation dependencies
 *  - Breaks circular include chains
 *  - Faster incremental compilation
 *
 *  Usage:
 *  #include "zero_cc_fwd.h"
 *  // Use pointers to types, e.g., ZeroCCCompiler* comp;
 *  ==========================================================================*/

#ifndef ZERO_CC_FWD_H
#define ZERO_CC_FWD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ==========================================================================
 *  Core Types (from core/compiler_state.h, core/buffers.h)
 * ==========================================================================*/

typedef struct ZeroCCBuffer ZeroCCBuffer;
typedef struct ZeroCCPreprocessor ZeroCCPreprocessor;
typedef struct ZeroCCIncludeDirs ZeroCCIncludeDirs;
typedef struct ZeroCCMacro ZeroCCMacro;

/* ==========================================================================
 *  Token Types (from types/token.h)
 * ==========================================================================*/

typedef enum ZeroCCTokType ZeroCCTokType;

typedef struct ZeroCCToken ZeroCCToken;
typedef ZeroCCToken ZeroCCTokenV2;

/* ==========================================================================
 *  Type System (from types/type_system.h)
 * ==========================================================================*/

typedef enum ZeroTypeTag ZeroTypeTag;

typedef struct ZeroCCType ZeroCCType;
typedef struct ZeroCCStructMember ZeroCCStructMember;
typedef struct ZeroCCSym ZeroCCSym;
typedef struct ZeroCCTypedef ZeroCCTypedef;
typedef struct ZeroCCStructDef ZeroCCStructDef;
typedef struct ZeroCCEnumConst ZeroCCEnumConst;
typedef struct ZeroCCFunction ZeroCCFunction;
typedef struct ZeroCCReloc ZeroCCReloc;
typedef struct ZeroCCStringReloc ZeroCCStringReloc;

/* ==========================================================================
 *  Compiler Context (from types/symbols.h)
 * ==========================================================================*/

typedef struct ZeroCCCompiler ZeroCCCompiler;

/* ==========================================================================
 *  Backend Types (from zero_cc_backends.h)
 * ==========================================================================*/

typedef struct ZeroCCTLSBackend ZeroCCTLSBackend;
typedef struct ZeroCCEmitBackend ZeroCCEmitBackend;
typedef struct ZeroCCLinkBackend ZeroCCLinkBackend;
typedef struct ZeroCCMemoryBackend ZeroCCMemoryBackend;
typedef struct ZeroCCABICallBackend ZeroCCABICallBackend;
typedef struct ZeroCCAtomicBackend ZeroCCAtomicBackend;
typedef struct ZeroCCGenericBackend ZeroCCGenericBackend;
typedef struct ZeroCCStdlibBackend ZeroCCStdlibBackend;
typedef struct ZeroCCDiagBackend ZeroCCDiagBackend;

/* ==========================================================================
 *  Lexer Types (from lexer/state.h)
 * ==========================================================================*/

typedef struct ZeroCCLexer ZeroCCLexer;

/* ==========================================================================
 *  AST Types (from parser.h)
 * ==========================================================================*/

typedef uint32_t ASTRef;
#define AST_NULL 0

typedef enum ZeroCCASTTag ZeroCCASTTag;

typedef struct ZeroCCASTNode ZeroCCASTNode;
typedef struct ZeroCCASTArena ZeroCCASTArena;

/* ==========================================================================
 *  Helper Types (from core/buffers.h)
 * ==========================================================================*/

typedef struct ZeroPtrStack ZeroPtrStack;

#endif /* ZERO_CC_FWD_H */
