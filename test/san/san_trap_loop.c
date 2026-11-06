/* zero_san v1: overflow inside a loop must trap at every -O level.
 * Guards the pass interaction: the check must stay adjacent to its add
 * (LICM whitelist, DCE liveness, CSE) - a hoisted/dropped check would let
 * this program exit 0 instead of SIGILL. */
int main(void) {
    int s = 1500000000;
    for (int i = 0; i < 2; i++) s = s + 1500000000; /* overflows iter 1 */
    return s;
}
