/* zero_rt process termination: exit, atexit, abort, and the entry wrapper.
 *
 * `zeroc_main_wrapper` is the function the T2 linker's generated `_start`
 * stub calls INSTEAD of main when zero_rt is linked (the linker force-
 * extracts the archive member defining it and retargets the stub's call).
 * Routing main through exit() is what runs the atexit list on a plain
 * `return` from main.
 */
#include "rt_os.h"

typedef void (*RtAtexitFn)(void);

#define RT_MAX_ATEXIT 32

static RtAtexitFn g_atexit_fns[RT_MAX_ATEXIT];
static int g_atexit_count = 0;

int atexit(RtAtexitFn fn) {
    if (g_atexit_count >= RT_MAX_ATEXIT) return 1;
    g_atexit_fns[g_atexit_count++] = fn;
    return 0;
}

void exit(int code) {
    while (g_atexit_count > 0) {
        g_atexit_fns[--g_atexit_count]();
    }
    zeroc_rt_exit_group(code);
    for (;;) { } /* unreachable */
}

void abort(void) {
    exit(134); /* SIGABRT status */
}

void zeroc_main_wrapper(void) {
    extern int main(void);
    exit(main());
}
