# Global Variable Declarations

## Overview

Global variable declarations allow C programs to define variables at file scope with optional initialization.

## Syntax

```c
// Declaration with initialization
int x = 42;
char *str = "hello";

// Declaration without initialization (zero-initialized by default)
int y;

// Multiple declarations
int a = 1, b = 2, c;

// With static keyword (internal linkage)
static int hidden = 10;
```

## Implementation

### Data Structures

```c
// In ZeroCCCompiler struct
ZeroCCBuffer data_section;  // Initialized globals (.data)

// Global variable tracking
typedef struct {
    char name[128];
    ZeroCCType type;
    size_t data_offset;       /* Offset in data_section */
    size_t size;              /* Size in bytes */
    bool is_static;           /* Internal linkage */
    bool has_initializer;     /* Has = expression */
} ZeroCCGlobalVar;

static ZeroCCGlobalVar global_vars[ZERO_CC_MAX_GLOBALS];
static size_t global_var_count = 0;
```

### Parsing Flow

1. **Type detection**: Check for type specifier at file scope
2. **Identifier parsing**: Parse variable name
3. **Initializer parsing** (optional): Parse `= expression`
4. **Emission**: Write to `data_section` with proper alignment

### Emission Strategy

```c
// Simple integer: emit immediate value
int x = 42;  // → .data: dq 42

// String literal: emit to rodata, store pointer in data
char *s = "hello";  // → .rodata: db "hello", 0
                    // → .data: dq rodata_offset

// Zero initialization: emit zeros
int y;  // → .data: dq 0
```

### Alignment

- All globals are aligned to 8 bytes
- Padding bytes are emitted before each global variable

## Test Cases

### Basic Global Variable

```c
// test/global_vars.c
int global_x = 42;

int main() {
    int result = global_x;
    return result == 42 ? 0 : 1;
}
```

### Uninitialized Global

```c
// test/global_vars2.c
int global_x;       /* Uninitialized (zero) */
int global_y = 100; /* Initialized */

int main() {
    int result = global_x + global_y;
    return result == 100 ? 0 : 1;
}
```

### String Pointer Global

```c
// test/global_vars3.c
char *global_str = "hello";

int main() {
    return global_str[0] == 'h' && global_str[1] == 'e' ? 0 : 1;
}
```

## Status

### Core Features ✅
- [x] `data_section` buffer initialized
- [x] Parsing support for type + identifier
- [x] Emission to data_section with alignment
- [x] Symbol registration in global_vars array
- [x] Basic integer initialization
- [x] Uninitialized globals (zero)
- [x] String pointer initialization
- [x] Function/variable distinction (infinite loop fix)

### Future Enhancements
- [ ] Static keyword support (internal linkage)
- [ ] Multiple declarations (int a = 1, b = 2)
- [ ] Complex initializers (structs, arrays)
- [ ] BSS section for zero-initialized globals
- [ ] ELF .data section emission

## Bug Fixes

### Infinite Loop Fix (2024)
**Issue**: Global variable parsing caused infinite loop when encountering function definitions.

**Root Cause**: The parser would detect a function definition (e.g., `int main(void)`), back up to the start position, but then unconditionally `continue` the loop, causing it to repeat forever.

**Fix**: Only `continue` for actual variable declarations. Function definitions now fall through to the function parser.

**Commit**: `00abb41` - "fix: Resolve infinite loop in global variable parsing"
