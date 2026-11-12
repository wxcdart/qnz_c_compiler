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

/* Exit-code override (Sprint 169): an atexit handler (the -fsanitize=leak
 * report) may request a different status; applied only when the program
 * itself would exit 0, so a real failure status is never masked. */
static int g_exit_override = -1;

void zeroc_rt_set_exit_override(int code) {
    g_exit_override = code;
}

int atexit(RtAtexitFn fn) {
    if (g_atexit_count >= RT_MAX_ATEXIT) return 1;
    g_atexit_fns[g_atexit_count++] = fn;
    return 0;
}

void exit(int code) {
    while (g_atexit_count > 0) {
        g_atexit_fns[--g_atexit_count]();
    }
    if (g_exit_override >= 0 && code == 0) code = g_exit_override;
    zeroc_rt_exit_group(code);
    for (;;) { } /* unreachable */
}

void abort(void) {
    exit(134); /* SIGABRT status */
}

/* Stack smash protection symbols */
uintptr_t __stack_chk_guard = (uintptr_t)0x00000aff0a0d0a00ULL;

void __stack_chk_fail(void) {
    const char msg[] = "*** stack smashing detected ***: terminated\n";
    zeroc_rt_write(2, msg, sizeof(msg) - 1);
    zeroc_rt_exit_group(127);
    for (;;) { }
}

#if !RT_TARGET_AARCH64 && !RT_TARGET_RISCV
static uintptr_t g_x86_tls_block[8];
#endif

/* Shared entry prologue: the x86-64 stack-protector TLS setup. */
static void rt_entry_setup(void) {
#if !RT_TARGET_AARCH64 && !RT_TARGET_RISCV
    /* On x86-64, stack protector checks load canary from %fs:40.
     * Set up FS segment base to point to g_x86_tls_block so %fs:40 (index 5)
     * contains __stack_chk_guard. */
    g_x86_tls_block[5] = __stack_chk_guard;
    zeroc_rt_arch_prctl(RT_ARCH_SET_FS, (unsigned long)g_x86_tls_block);
#endif
}

void zeroc_main_wrapper(void) {
    extern int main(void);
    rt_entry_setup();
    exit(main());
}

/* --zerocc-link stub target when -fsanitize=leak (Sprint 169): enable the
 * freestanding atexit leak report, then take the normal entry path. The
 * own linker selects it over zeroc_main_wrapper when the driver passes it
 * as the requested entry wrapper. */
void zeroc_leak_main_wrapper(void) {
    extern int main(void);
    zeroc_rt_leak_report_set(1);
    rt_entry_setup();
    exit(main());
}
