/*
 * test_baremetal.c - Freestanding baremetal startup test.
 *
 * Verifies standard embedded startup sequence:
 * 1. Copy initialized .data from Flash (LMA, _sidata) to RAM (VMA, _sdata.._edata)
 * 2. Zero uninitialized .bss in RAM (_sbss.._ebss)
 * 3. Verify memory state and execute test logic
 */

extern unsigned char _sidata;   /* Flash LMA of .data */
extern unsigned char _sdata;    /* RAM VMA start of .data */
extern unsigned char _edata;    /* RAM VMA end of .data */
extern unsigned char _sbss;     /* RAM VMA start of .bss */
extern unsigned char _ebss;     /* RAM VMA end of .bss */
extern unsigned char _stack_top;

int initialized_val1 = 0x12345678;
int initialized_val2 = 0x55aa55aa;
int bss_val1;
int bss_val2;
volatile int test_status = 0;

void Reset_Handler(void) {
    /* 1. Copy data from Flash LMA to RAM VMA */
    unsigned char *src = &_sidata;
    unsigned char *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* 2. Zero BSS */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    /* 3. Verify data and bss */
    if (initialized_val1 != 0x12345678) {
        test_status = 1;
        return;
    }
    if (initialized_val2 != 0x55aa55aa) {
        test_status = 2;
        return;
    }
    if (bss_val1 != 0 || bss_val2 != 0) {
        test_status = 3;
        return;
    }

    initialized_val1 += 100;
    bss_val1 = 200;
    if (initialized_val1 == 0x12345678 + 100 && bss_val1 == 200) {
        test_status = 42; /* Success */
    } else {
        test_status = 4;
    }
}
