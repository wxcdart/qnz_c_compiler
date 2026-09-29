# Macro Features

## Overview

ZeroCC supports C preprocessor macro features including object macros, function-like macros, token pasting, and stringification.

## Features Implemented

### ✅ Object Macros
Simple macro substitution without parameters.

```c
#define PI 3.14159
#define MAX(a) ((a) > 100 ? (a) : 100)

int x = PI * 2;
```

### ✅ Function-like Macros
Macros with parameters.

```c
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

int y = SQUARE(5);  // 25
```

### ✅ Token Pasting (##)
Concatenates two tokens into a single token.

```c
#define CONCAT(a, b) a ## b

int var1 = 10;
int x = CONCAT(var, 1);  // References var1

// With parameters
#define MAKE_FUNC(name) void name##_func() {}
MAKE_FUNC(test)  // Becomes: void test_func() {}
```

### ✅ Stringification (#)
Converts a macro argument to a string literal.

```c
#define STR(x) #x
#define INT_TO_STR(x) STR(x)

const char *s1 = STR(hello);      // "hello"
const char *s2 = INT_TO_STR(42);  // "42" (double expansion)
```

### ✅ Variadic Macros (__VA_ARGS__)
Macros with variable number of arguments.

```c
#define PRINT(fmt, ...) printf(fmt, __VA_ARGS__)
#define LOG(msg, ...) PRINT("[%s] " msg, __func__, ##__VA_ARGS__)
```

### ✅ Predefined Macros
Built-in macros that expand to compile-time values.

| Macro | Value |
|-------|-------|
| `__FILE__` | Current filename (string) |
| `__LINE__` | Current line number |
| `__func__` | Current function name (string) |
| `__STDC__` | 1 (standard C compiler) |
| `__STDC_VERSION__` | 201112L (C11) |
| `__GNUC__` | 4 (GCC compatibility) |
| `NULL` | ((void*)0) |
| `true` | 1 |
| `false` | 0 |

### ✅ Extern Declarations
Declare external symbols.

```c
extern int printf(const char *fmt, ...);
extern void exit(int code);
```

## Test Cases

### Token Pasting

```c
// test/token_paste.c
#define CONCAT(a, b) a ## b

int main() {
    int CONCAT(var,iable) = 42;  // Becomes: int variable = 42;
    return variable == 42 ? 0 : 1;
}
```

### Token Pasting with Global Variables

```c
// test/token_paste2.c
#define CONCAT(a, b) a ## b

int var1 = 10;
int var2 = 20;

int main() {
    int x = CONCAT(var, 1) + CONCAT(var, 2);  // var1 + var2 = 30
    return x == 30 ? 0 : 1;
}
```

### Stringification

```c
// test/stringification.c
#define STR(x) #x
#define INT_TO_STR(x) STR(x)

int main() {
    const char *s1 = STR(hello);
    const char *s2 = INT_TO_STR(42);
    return s1[0] == 'h' && s2[0] == '4' ? 0 : 1;
}
```

## Implementation Details

### Token Pasting Algorithm

1. When `##` is encountered in macro replacement, set `pending_paste` flag
2. Collect the next token (identifier, number, or operator)
3. Concatenate with previous token in paste buffer
4. Output the combined token

### Stringification Algorithm

1. When `#param` is encountered, find the matching argument
2. Wrap the argument value in double quotes
3. Escape special characters (quotes, backslashes)

### Double Expansion

Stringification uses double expansion to ensure arguments are fully expanded before being stringified:

```c
#define STR(x) #x
#define X 42
STR(X)        // "X" - not expanded
INT_TO_STR(X) // "42" - expanded via intermediate macro
```

## Status

| Feature | Status | Notes |
|---------|--------|-------|
| Object macros | ✅ | Basic substitution |
| Function-like macros | ✅ | Parameter substitution |
| Token pasting (##) | ✅ | Concatenates tokens |
| Stringification (#) | ✅ | Converts to string |
| Variadic macros | ✅ | `__VA_ARGS__` support |
| Predefined macros | ✅ | `__FILE__`, `__LINE__`, `__func__`, etc. |
| `#pragma once` | ❌ | Not implemented |
| `#include` | ✅ | File inclusion works |
| `#ifdef`/`#ifndef` | ✅ | Conditional compilation |
| `#if`/`#elif`/`#else` | ✅ | Expression evaluation |
| `#undef` | ✅ | Macro undefinition |

## Limitations

1. **Token pasting restrictions**: Result must be a valid preprocessing token
   - `CONCAT(1, +)` is invalid (pastes to `1+` which is not a valid token in this context)
   - `CONCAT(var, 1)` is valid (pastes to `var1`)

2. **Stringification**: Does not escape all special characters
   - Quotes and backslashes need proper handling for complex strings

3. **Variadic macros**: `##__VA_ARGS__` comma suppression is simplified
   - Full GCC compatibility not guaranteed for all edge cases
