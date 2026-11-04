/*
 * console.c -- UART0 console and the exported print helpers.
 *
 *   func_062 uart0_init_banner   func_066 uart0_put32
 *   func_067 uart0_putc          func_068 uart0_getc
 *   func_069 uart0_rx_ready      func_070 uart0_reinit
 *   func_115 put_hex32           func_119 debug_msg
 *   func_122 debug_msg_err       func_123 debug_msg_var
 *
 * UART0 is 16550-style with a 4-byte register stride.  Three different
 * "can I transmit" conventions coexist in the ROM, and each is visible on
 * the wire, so they are kept per function:
 *
 *   - no wait at all: uart0_init_banner's 'N', put_hex32 (and every raw
 *     letter elsewhere, UART0_PUTC_RAW);
 *   - wait for +0x08 bit4 clear (TX FIFO full, inferred): uart0_putc and
 *     uart0_put32, used only by uart_boot;
 *   - wait for LSR THRE: debug_msg and the two helpers built on it, the
 *     loader-facing services in the export table.
 *
 * Access widths follow the original too: the init/reinit sequences use
 * byte accesses on LCR/DLM/FCR, the uart_boot helpers use word stores to
 * THR, debug_msg and put_hex32 use byte stores.
 */
#include "bootrom.h"
#include "na51089-regmap.h"

#define UART0(off)      REG32(UART0_BASE + (off))
#define UART0_B(off)    REG8(UART0_BASE + (off))

/*
 * 0x4c68 -- first thing reset_handler calls.  Enables the UART0 clock,
 * programs 115200 8N1 (divisor 13), enables and resets both FIFOs and
 * prints the ROM's first letter 'N' with a bare THR store.
 */
void uart0_init_banner(void)
{
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_UART0;

    UART0_B(UART_LCR) |= UART_LCR_DLAB;
    UART0_B(UART_DLL) = UART_DIV_115200;
    UART0_B(UART_DLM) = 0;
    UART0_B(UART_LCR) &= (u8)~UART_LCR_DLAB;
    UART0_B(UART_LCR) |= UART_LCR_8N1;
    UART0_B(UART_FCR) = UART_FCR_INIT;

    UART0_PUTC_RAW(LTR_ROM_START);
}

/*
 * 0x4e28 -- send one word, LSB first (uart_boot replies).  The FIFO-full
 * flag is checked once for the whole word, then four word stores to THR.
 */
void uart0_put32(u32 w)
{
    while (UART0(UART_STAT8) & UART_STAT8_TXFULL)
        ;
    UART0(UART_THR) = w & 0xff;
    UART0(UART_THR) = (w >> 8) & 0xff;
    UART0(UART_THR) = (w >> 16) & 0xff;
    UART0(UART_THR) = w >> 24;
}

/* 0x4e68 -- wait for room in the TX FIFO, then one word store to THR. */
void uart0_putc(u8 c)
{
    while (UART0(UART_STAT8) & UART_STAT8_TXFULL)
        ;
    UART0(UART_THR) = c;
}

/* 0x4e8c -- block until LSR data-ready, return RBR. */
u8 uart0_getc(void)
{
    while (!(UART0(UART_LSR) & UART_LSR_DR))
        ;
    return (u8)UART0(UART_RBR);
}

/* 0x4ebc -- LSR data-ready bit. */
int uart0_rx_ready(void)
{
    return UART0(UART_LSR) & UART_LSR_DR;
}

/*
 * 0x4ed8 -- uart_boot's re-init.  Same sequence as uart0_init_banner but
 * read-modify-write throughout, and with two oddities kept from the
 * original: the divisor is OR'ed in with a *word* access (DLL |= 13, so a
 * different divisor left by someone else would be corrupted rather than
 * replaced), and FCR |= 7 reads the +0x08 slot -- which on a 16550 is IIR
 * when read -- and writes that back with the FIFO bits set.  It does not
 * touch the clock enable and prints nothing.
 */
void uart0_reinit(void)
{
    UART0_B(UART_LCR) |= UART_LCR_DLAB;
    UART0(UART_DLL) |= UART_DIV_115200;
    UART0_B(UART_LCR) &= (u8)~UART_LCR_DLAB;
    UART0_B(UART_LCR) |= UART_LCR_8N1;
    UART0_B(UART_FCR) |= UART_FCR_INIT;
}

/*
 * 0x6d20 -- eight upper-case hex digits, most significant first, as bare
 * byte stores to THR (no FIFO or LSR check).  Exported twice, at 0x7FC0
 * and 0x7FEC; the loaders call the second slot "Dec2HexStr", but it
 * prints hex and returns nothing.
 */
void put_hex32(u32 v)
{
    int shift;

    for (shift = 28; shift >= 0; shift -= 4) {
        u32 nib = (v >> shift) & 0xf;

        UART0_PUTC_RAW(nib > 9 ? nib + 'A' - 10 : nib + '0');
    }
}

/*
 * 0x700c -- puts without a newline: wait LSR THRE before every byte.
 * Exported at 0x7FD8.
 */
void debug_msg(const char *s)
{
    char c;

    while ((c = *s++) != '\0') {
        while (!(UART0_B(UART_LSR) & UART_LSR_THRE))
            ;
        UART0_B(UART_THR) = c;
    }
}

/*
 * 0x7114 -- "E:" + message + hex value, no line ending.  Exported at
 * 0x7FE0.
 */
void debug_msg_err(const char *s, u32 v)
{
    debug_msg("E:");
    debug_msg(s);
    put_hex32(v);
}

/*
 * 0x7144 -- message + hex value + CRLF.  Exported at 0x7FDC.  (In the
 * image the "\r\n" literal is the tail of lzma.c's "pb\r\n".)
 */
void debug_msg_var(const char *s, u32 v)
{
    debug_msg(s);
    put_hex32(v);
    debug_msg("\r\n");
}
