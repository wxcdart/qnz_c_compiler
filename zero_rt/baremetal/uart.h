/* zero_rt/baremetal/uart.h - Baremetal UART driver interface.
 *
 * Supports 16550-compatible and ARM PL011 MMIO UARTs plus an in-memory
 * circular trace buffer for debug and simulation output.
 */
#ifndef ZERO_RT_BAREMETAL_UART_H
#define ZERO_RT_BAREMETAL_UART_H

#include <stdint.h>
#include <stddef.h>

#define UART_RING_BUF_SIZE 2048

#ifdef __cplusplus
extern "C" {
#endif

extern char g_baremetal_uart_buf[UART_RING_BUF_SIZE];
extern volatile uint32_t g_baremetal_uart_pos;
extern uintptr_t g_baremetal_uart_base;

void uart_init(uintptr_t base_addr);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_getc(void);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_RT_BAREMETAL_UART_H */
