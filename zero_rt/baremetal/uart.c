/* zero_rt/baremetal/uart.c - Baremetal MMIO UART driver implementation.
 *
 * Implements serial character I/O targeting:
 * - 16550-compatible UART (default base 0x10000000 on RISC-V)
 * - ARM PL011 UART (default base 0x09000000 on ARM/AArch32)
 * - In-memory ring buffer (always updated for simulation / debugging)
 */
#include "baremetal/uart.h"

char g_baremetal_uart_buf[UART_RING_BUF_SIZE];
volatile uint32_t g_baremetal_uart_pos = 0;

#if defined(__riscv)
uintptr_t g_baremetal_uart_base = 0x10000000UL; /* 16550 UART on QEMU / SiFive */
#elif defined(__arm__) || defined(__aarch64__)
uintptr_t g_baremetal_uart_base = 0x09000000UL; /* PL011 UART on ARM virt */
#else
uintptr_t g_baremetal_uart_base = 0;
#endif

void uart_init(uintptr_t base_addr) {
    g_baremetal_uart_base = base_addr;
}

void uart_putc(char c) {
    /* 1. In-memory trace buffer */
    uint32_t pos = g_baremetal_uart_pos;
    g_baremetal_uart_buf[pos % UART_RING_BUF_SIZE] = c;
    g_baremetal_uart_pos = pos + 1;

    /* 2. MMIO UART output */
    if (g_baremetal_uart_base != 0) {
        volatile uint8_t *uart = (volatile uint8_t *)g_baremetal_uart_base;
        *uart = (uint8_t)c;
    }
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        uart_putc(*s++);
    }
}

int uart_getc(void) {
    if (g_baremetal_uart_base != 0) {
        volatile uint8_t *uart = (volatile uint8_t *)g_baremetal_uart_base;
        return (int)(*uart);
    }
    return -1;
}
