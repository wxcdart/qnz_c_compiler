#ifndef ZERO_CC_JIT_COMPILE_UNIT_H
#define ZERO_CC_JIT_COMPILE_UNIT_H

#include "zero_cc/types/types.h"

/* Compile a complete C99/C11 translation unit into executable machine code / ELF sections */
void zero_cc_compile_c99(ZeroCCCompiler *c);

#endif /* ZERO_CC_JIT_COMPILE_UNIT_H */
