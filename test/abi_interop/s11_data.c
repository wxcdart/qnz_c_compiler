/* ABI interop: s11 global data both directions (shared main, compiled by BOTH compilers)
 * expect: 20 3.0 C
 * expect: 300 7.5 d 50
 * expect: 1600
 */
#include <stdio.h>
typedef struct { int id; double val; char name[8]; } Rec;
extern Rec zc_table[4]; extern int zc_counter;
extern Rec gc_table[4]; extern long long gc_total;
void zc_init_data(void); void zc_bump_counter(int);
void gc_init_data(void); long long gc_total_plus(long long);
int main(void) {
    zc_init_data();
    gc_init_data();
    zc_bump_counter(8);
    printf("%d %.1f %s\n", zc_table[2].id, zc_table[2].val, zc_table[2].name);
    printf("%d %.1f %s %d\n", gc_table[3].id, gc_table[3].val, gc_table[3].name, zc_counter);
    printf("%lld\n", gc_total_plus(500) + zc_table[1].id * 10);
    return 0;
}

