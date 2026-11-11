/* ABI interop s11: data interop - zerocc globals read by gcc. */
typedef struct { int id; double val; char name[8]; } Rec;
Rec zc_table[4];
int zc_counter = 0;
void zc_init_data(void) {
    for (int i = 0; i < 4; i++) {
        zc_table[i].id = i * 10;
        zc_table[i].val = (double)i * 1.5;
        zc_table[i].name[0] = (char)('A' + i);
        zc_table[i].name[1] = 0;
    }
    zc_counter = 42;
}
void zc_bump_counter(int k) { zc_counter += k; }
