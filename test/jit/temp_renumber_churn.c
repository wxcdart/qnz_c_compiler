/*
 * temp_renumber_churn.c - churn-stress test for the liveness-based
 * temp-ID renumbering pass (opt_temp_renumber.c, Sprint 151b,
 * development plan item 9).
 *
 * D(a) doubles its argument, so D^16(x) expands (266 bytes of source)
 * into a tree of 65,536 `x` leaves and 65,535 `+` nodes. Lowering the
 * tree churns ~131k temp IDs (each leaf is a local load, each node an
 * add) - DOUBLE the 65,534 cap of the pre-151b 16-bit temp-id space,
 * which would have refused to compile this function with "temp id
 * space exhausted". Peak live usage is ~34 (each add's result feeds
 * the next), so the renumber pass must compact the churned 32-bit id
 * space down to the low peak-live range and renumber every reference
 * exactly 1:1 (a single wrong remap changes the computed product).
 *
 * Verify the churn actually exceeds the old budget:
 *   ZC_RENUMBER_DEBUG=1 zerocc --run temp_renumber_churn.c
 *     -> "[renumber] main: 196659 instrs, churn 131093 -> peak 34"
 *
 * Expected output: "churn ok: 196608" (3 * 65536), exit 0.
 * Swept by make test (host gcc) and make test-opt (zerocc, -O0..-O3,
 * JIT + AOT).
 */
#include <stdio.h>

#define D(a) ((a) + (a))

int main(void) {
    int x = 3;
    int y = D(D(D(D(D(D(D(D(D(D(D(D(D(D(D(D(x))))))))))))))));
    if (y != x * 65536) {
        printf("FAIL: y=%d expected %d\n", y, x * 65536);
        return 1;
    }
    printf("churn ok: %d\n", y);
    return 0;
}
