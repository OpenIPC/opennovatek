/*
 * regmap-uart.h -- UART0 0xF0290000 (console) and UART1 0xF0300000.
 * 16550-style, 4-byte register stride.  Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_UART_H
#define BOOTROM_NA51089_REGMAP_UART_H

#define UART_THR                0x000           /* w: transmit holding */
#define UART_RBR                0x000           /* r: receive buffer */
#define UART_DLL                0x000           /* DLAB=1: divisor low */
#define UART_DLM                0x004           /* DLAB=1: divisor high (= 0) */
#define UART_FCR                0x008           /* w: FIFO control */
#define UART_STAT8              0x008           /* r: IIR slot, bit4 polled */
#define UART_LCR                0x00c
#define UART_LSR                0x014
#define UART_SCR                0x01c           /* UART1: lzma debug switch */

#define UART_DIV_115200         13              /* DLL value */
#define UART_FCR_INIT           7               /* enable + reset both FIFOs */
#define UART_STAT8_TXFULL       BIT(4)          /* TX FIFO full (inferred):
                                                   uart0_putc/put32 wait clear */
#define UART_LCR_8N1            0x3             /* [1:0] = 3 */
#define UART_LCR_DLAB           BIT(7)
#define UART_LSR_DR             BIT(0)          /* data ready (uart_boot) */
#define UART_LSR_THRE           BIT(5)          /* THR empty (debug_msg) */

/* UART1 SCR as lzma_inflate debug switch */
#define UART_SCR_LZMA_ARGS      BIT(0)          /* print the six arguments */
#define UART_SCR_LZMA_HDR       BIT(1)          /* print the LZMA header fields */

#endif
