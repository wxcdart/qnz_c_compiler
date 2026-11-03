/*
 * x86-64 setjmp/longjmp runtime for ZeroCC.
 *
 * The compiler lowers `__builtin_setjmp(buf)` to a call to `zc_setjmp(buf)`
 * followed by a capture of RAX, and `__builtin_longjmp(buf, val)` to a call to
 * `zc_longjmp(buf, val)` followed by an infinite loop (longjmp never returns).
 * `zc_longjmp` restores the saved registers and jumps to the saved return
 * address with RAX = val (or 1 if val == 0), so the capture of RAX in the
 * setjmp call site re-executes with the resume value.
 *
 * Buffer layout (five 8-byte words):
 *   buf[0] = caller RSP  (= rbp + 16 in this frame)
 *   buf[1] = caller RBP  (= [rbp], the value this function's prologue saved)
 *   buf[2] = return address (= [rbp+8], pushed by the `call`)
 *   buf[3] = RBX
 *   buf[4] = R12
 *
 * This function is compiled at -O0 (keeps a frame pointer), so the frame
 * layout is fixed: [rbp] = saved caller RBP, [rbp+8] = return address,
 * [rbp+16] = the caller's RSP (the address the `call` pushed the return
 * address to). This runtime is compiled by the HOST compiler and linked into
 * zerocc, resolved by the JIT via dlsym, and called through the JIT's
 * absolute-call lowering (see x86_emit_call), which supports targets >2GB from
 * the mmap'd code image.
 */

int zc_setjmp(void *buf) {
    unsigned long *b = (unsigned long *)buf;
    __asm__ volatile (
        /* buf[0] = caller RSP = rbp + 16 */
        "leaq 16(%%rbp), %%r10\n\t"
        "movq %%r10, %0\n\t"
        /* buf[1] = caller RBP = [rbp] */
        "movq (%%rbp), %%r10\n\t"
        "movq %%r10, %1\n\t"
        /* buf[2] = return address = [rbp+8] */
        "movq 8(%%rbp), %%r10\n\t"
        "movq %%r10, %2\n\t"
        /* buf[3] = RBX, buf[4] = R12 */
        "movq %%rbx, %3\n\t"
        "movq %%r12, %4\n\t"
        : "=m"(b[0]), "=m"(b[1]), "=m"(b[2]), "=m"(b[3]), "=m"(b[4])
        :
        : "memory", "r10"
    );
    return 0; /* RAX = 0: first-pass return value */
}

void zc_longjmp(void *buf, unsigned int val) {
    unsigned long b0 = ((unsigned long *)buf)[0];
    unsigned long b1 = ((unsigned long *)buf)[1];
    unsigned long b2 = ((unsigned long *)buf)[2];
    unsigned long b3 = ((unsigned long *)buf)[3];
    unsigned long b4 = ((unsigned long *)buf)[4];
    if (val == 0) val = 1;
    /* All saved values are loaded into registers FIRST (in C, on the normal
     * stack), then the asm restores RSP/RBP/callee-saved registers, sets
     * RAX = val, and jumps to the saved return address. Never returns. */
    __asm__ volatile (
        "movl %5, %%eax\n\t"
        "movq %1, %%rsp\n\t"
        "movq %2, %%rbp\n\t"
        "movq %3, %%rbx\n\t"
        "movq %4, %%r12\n\t"
        "jmp *%0\n\t"
        :
        : "r"(b2), "r"(b0), "r"(b1), "r"(b3), "r"(b4), "r"(val)
        : "memory", "eax", "rax"
    );
    __builtin_unreachable();
}
