/* ABI interop s11: gcc-side data (read by zerocc main). */
typedef struct { int id; double val; char name[8]; } Rec;
Rec gc_table[4];
long long gc_total = 0;
void gc_init_data(void) {
    for (int i = 0; i < 4; i++) {
        gc_table[i].id = i * 100;
        gc_table[i].val = (double)i * 2.5;
        gc_table[i].name[0] = (char)('a' + i);
        gc_table[i].name[1] = 0;
    }
    gc_total = 1000;
}
long long gc_total_plus(long long x) { return gc_total + x; }
