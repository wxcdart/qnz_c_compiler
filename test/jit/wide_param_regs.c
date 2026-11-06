/* Sprint 117 regression: mixed-width function parameters must not be
 * misassigned across ABI argument slots. The linear-scan allocator did not
 * know that an argument value is live in its ABI slot register from function
 * entry (its interval started at the OP_PARAM_LOAD bookkeeping instruction),
 * so when the natural slot was declined for the first parameter (e.g. by a
 * fixed-interval conflict with the return register's window) the second
 * parameter could be allocated into the first's slot; the PARAM_LOAD
 * materialization move then clobbered the live value still sitting in that
 * slot, and `int * long` computed i*i instead of i*l (seen on RISC-V; the
 * latent swap exists on every ISA).
 *
 * With parameter pre-coloring (interval starts at entry, pinned to the ABI
 * slot), the allocator can never hand a live parameter's slot to another
 * interval, and the materialization move is only ever emitted into a
 * register the allocator reserved for the parameter.
 */
long mul_ll(long a, long b) { return a * b; }
long mul_ii(int a, int b) { return (long)((long)a * (long)b); }
long mul_il(int a, long b) { return (long)a * b; }
long sub_li(long a, int b) { return a - (long)b; }
long add_il3(int a, int b, long c) { return (long)a * b + c; }
static long helper(long v) { return v * 2; }
long via_call(int a, long b) { return helper((long)a * b); }
long use_after_call(int a, long b, long c) {
    long x = (long)a * b;
    if (c > 1000) c += 1;
    return x + c;
}
int main(void) {
    int ok = 0;
    if (mul_ll(2000000000L, 2L) == 4000000000L) ok++;
    if (mul_ii(2000000000, 2) == 4000000000L) ok++;
    if (mul_il(-123456, 100000L) == -12345600000L) ok++;
    if (sub_li(1000000L, -200000) == 1200000L) ok++;
    if (add_il3(-50000, 300, 777L) == -14999223L) ok++;
    if (via_call(123, 456L) == 112176L) ok++;
    if (use_after_call(-1000, 2000L, 50L) == -1999950L) ok++;
    return (ok == 7) ? 0 : (ok + 1);
}
