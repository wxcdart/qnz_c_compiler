/* #include resolution: quoted (main-file-dir first), guard macros,
 * #pragma once, and nested includes. Both compilers read the same local
 * files, so the inlined content must match. (System headers are excluded:
 * gcc uses its own, zerocc its virtual copy - different content by design.) */
#include "inc_local.h"
#include "inc_local.h"
#include "inc_nested.h"
int after = g_local + inc_nested_val;
