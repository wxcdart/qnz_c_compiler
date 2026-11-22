/* Test auto-vectorization of division (float/double), bitwise loops, and remainder-loop peeling */

static float g_fa[10] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f};
static float g_fb[10] = {2.0f, 4.0f, 5.0f, 8.0f, 10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 20.0f};
static float g_fc[10];

static double g_da[5] = {100.0, 200.0, 300.0, 400.0, 500.0};
static double g_db[5] = {4.0, 5.0, 6.0, 8.0, 10.0};
static double g_dc[5];

static int g_ia16[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};
static int g_ib16[16] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};
static int g_ic16[16];

static int g_ia11[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static int g_ib11[11] = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
static int g_ic11[11];

static int g_ia7[7] = {0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A, 0x69};
static int g_ib7[7] = {0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96};
static int g_ic7[7];

/* float division with trip=10 (vector width 4: 8 vector + 2 peeled remainder) */
void test_fdiv_peel(void) {
    for (int i = 0; i < 10; i++) {
        g_fc[i] = g_fa[i] / g_fb[i];
    }
}

/* double division with trip=5 (vector width 2: 4 vector + 1 peeled remainder) */
void test_ddiv_peel(void) {
    for (int i = 0; i < 5; i++) {
        g_dc[i] = g_da[i] / g_db[i];
    }
}

/* bitwise and with trip=16 (vector width 4: 16 vector, exact multiple) */
void test_and16(void) {
    for (int i = 0; i < 16; i++) {
        g_ic16[i] = g_ia16[i] & g_ib16[i];
    }
}

/* bitwise or with trip=16 (vector width 4: 16 vector, exact multiple) */
void test_or16(void) {
    for (int i = 0; i < 16; i++) {
        g_ic16[i] = g_ia16[i] | g_ib16[i];
    }
}

/* bitwise xor with trip=16 (vector width 4: 16 vector, exact multiple) */
void test_xor16(void) {
    for (int i = 0; i < 16; i++) {
        g_ic16[i] = g_ia16[i] ^ g_ib16[i];
    }
}

/* bitwise and with trip=11 (vector width 4: 8 vector + 3 peeled remainder) */
void test_and_peel11(void) {
    for (int i = 0; i < 11; i++) {
        g_ic11[i] = g_ia11[i] & g_ib11[i];
    }
}

/* bitwise or with trip=7 (vector width 4: 4 vector + 3 peeled remainder) */
void test_or_peel7(void) {
    for (int i = 0; i < 7; i++) {
        g_ic7[i] = g_ia7[i] | g_ib7[i];
    }
}

int main(void) {
    test_fdiv_peel();
    if (g_fc[0] < 4.99f || g_fc[0] > 5.01f) return 1;
    if (g_fc[1] < 4.99f || g_fc[1] > 5.01f) return 2;
    if (g_fc[7] < 4.99f || g_fc[7] > 5.01f) return 3;
    if (g_fc[8] < 4.99f || g_fc[8] > 5.01f) return 4;
    if (g_fc[9] < 4.99f || g_fc[9] > 5.01f) return 5;

    test_ddiv_peel();
    if (g_dc[0] < 24.99 || g_dc[0] > 25.01) return 6;
    if (g_dc[1] < 39.99 || g_dc[1] > 40.01) return 7;
    if (g_dc[3] < 49.99 || g_dc[3] > 50.01) return 8;
    if (g_dc[4] < 49.99 || g_dc[4] > 50.01) return 9;

    test_and16();
    for (int i = 0; i < 16; i++) {
        if (g_ic16[i] != (g_ia16[i] & g_ib16[i])) return 100 + i;
    }

    test_or16();
    for (int i = 0; i < 16; i++) {
        if (g_ic16[i] != (g_ia16[i] | g_ib16[i])) return 200 + i;
    }

    test_xor16();
    for (int i = 0; i < 16; i++) {
        if (g_ic16[i] != (g_ia16[i] ^ g_ib16[i])) return 300 + i;
    }

    test_and_peel11();
    for (int i = 0; i < 11; i++) {
        if (g_ic11[i] != (g_ia11[i] & g_ib11[i])) return 400 + i;
    }

    test_or_peel7();
    for (int i = 0; i < 7; i++) {
        if (g_ic7[i] != (g_ia7[i] | g_ib7[i])) return 500 + i;
    }

    return 0;
}
