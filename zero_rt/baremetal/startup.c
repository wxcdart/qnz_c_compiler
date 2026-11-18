/* zero_rt/baremetal/startup.c - Freestanding baremetal startup crt0.
 *
 * Performs hardware reset initialization:
 * 1. Copies initialized .data from Flash LMA (_sidata) to RAM VMA (_sdata.._edata).
 * 2. Zeroes uninitialized .bss in RAM (_sbss.._ebss).
 * 3. Calls main(0, 0).
 * 4. Calls exit() with main's return code.
 * 5. Enters an infinite halt loop if exit() returns.
 */
#include <stdint.h>

extern unsigned char _sidata;   /* Flash LMA of .data */
extern unsigned char _sdata;    /* RAM VMA start of .data */
extern unsigned char _edata;    /* RAM VMA end of .data */
extern unsigned char _sbss;     /* RAM VMA start of .bss */
extern unsigned char _ebss;     /* RAM VMA end of .bss */
extern unsigned char _stack_top;

int main(int argc, char **argv);
void exit(int status);

__attribute__((weak)) void Reset_Handler(void) {
    /* 1. Copy initialized .data from Flash LMA to RAM VMA */
    unsigned char *src = &_sidata;
    unsigned char *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* 2. Zero uninitialized .bss in RAM */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    /* 3. Call main */
    int ret = main(0, (char **)0);

    /* 4. Exit with status */
    exit(ret);

    /* 5. Infinite loop if exit returns */
    for (;;) { }
}

__attribute__((weak)) void _start(void) {
    Reset_Handler();
}
