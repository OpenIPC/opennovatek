/*
 * uart_boot.c -- BS 0xC UART download protocol: func_071 uart_boot.
 *
 * Wire protocol, as the ROM implements it (UART0, 115200 8N1):
 *
 *   Every received byte goes into a 4-byte assembler; every completed
 *   little-endian word is appended to a 4-word CDB ring.  There is no
 *   framing and no resync: the ring simply restarts after every fourth
 *   word, and payload words of a download go through the ring as well, so
 *   every 16 payload bytes are also decoded as a CDB.
 *
 *   A CDB is good when its eight halfwords plus 0x1c sum to 0 (mod 2^16).
 *   Recognised opcodes (w0..w3 = the four CDB words):
 *
 *     w0 == 0x000081c0  IS_NVT      echo (w3 high half OR'ed with the
 *                                   right checksum), then on a good sum
 *                                   the chip id as 4 bytes -- of which
 *                                   the first is always 0, see below --
 *                                   or one zero word on a bad sum
 *     w0 == 0x000001c0  OPEN_DEVICE timer_stop() (no more timeout), echo
 *     lo16 == 0x83c0    ADDR_READ   echo, then addr and *(u32 *)addr as
 *                                   8 bytes; addr = w0[31:16] | w1[15:0]<<16
 *     lo16 == 0x03c0    ADDR_WRITE  echo, *(u32 *)addr = w1[31:16] |
 *                                   w2[15:0]<<16
 *     lo16 == 0x02c0    WRLOADER    no reply now; addr as above, length in
 *                                   bytes w1[31:16] | w2[15:0]<<16; the
 *                                   following words are stored at addr,
 *                                   '.' on UART1 every 256 words, and the
 *                                   saved WRLOADER CDB is echoed once the
 *                                   count is reached
 *     w0 == 0x000082c0  WAIT_DONE   echo, then leave the loop
 *
 *   "echo" is the CDB itself on a good sum and four zero words on a bad
 *   one.  ADDR_READ / ADDR_WRITE execute even when the sum is bad.  The
 *   opcode tests are not an else-if chain: each one is made against the
 *   same CDB in turn.
 *
 * Timeout: (OTP t + 1) * 30 s from entry, until OPEN_DEVICE stops the
 * timer; expiry is boot_fail(BOOT_FAIL_TIMEOUT) ("K:").
 */
#include "bootrom.h"
#include "na51089-regmap.h"
#include "sram.h"

/* Echo the current CDB, or four zero words if its checksum was bad. */
static void uart_echo_cdb(void)
{
    int i;

    if (g_uart.cdb_sum != 0) {
        uart0_put32(0);
        uart0_put32(0);
        uart0_put32(0);
        uart0_put32(0);
    } else {
        for (i = 0; i < 4; i++)
            uart0_put32(g_uart.cdb[i]);
    }
}

/* Send a word LSB first through uart0_putc (one FIFO wait per byte). */
static void uart_put_bytes(u32 w)
{
    uart0_putc(w & 0xff);
    uart0_putc((w >> 8) & 0xff);
    uart0_putc((w >> 16) & 0xff);
    uart0_putc(w >> 24);
}

/* Address / value packed across halfwords: w[a][31:16] | w[a+1][15:0] << 16 */
static u32 cdb_hi_lo(u32 w_lo, u32 w_hi)
{
    return (w_lo >> 16) | (w_hi << 16);
}

/*
 * 0x4f28 -- BS 0xC entry, also reached through next_boot_source().
 * The helpers above are written out inline in the original.
 */
void uart_boot(void)
{
    u32 t, w, sum7, i;

    uart0_reinit();

    /* cdb_idx .. dl_total: nine words, in order */
    g_uart.cdb_idx = 0;
    g_uart.done = 0;
    g_uart.byte_idx = 0;
    g_uart.cdb_sum = 0;
    g_uart.reply_sum = 0;
    g_uart.dl_addr = 0;
    g_uart.dl_words = 0;
    g_uart.chip_id = 0;
    g_uart.dl_total = 0;

    t = (otp_boot_cfg() & OTP_CFG_TMO_MASK) >> OTP_CFG_TMO_SHIFT;
    g_otp_tmo = t;
    g_timeout = (t + 1) * TMO_30S;
    timer_start();

    while ((s32)g_uart.done <= 0) {
        if (!uart0_rx_ready())
            goto check_timeout;

        /* ---- byte -> word -> CDB ring (and payload store) ---------- */
        g_uart.rx_bytes[g_uart.byte_idx++] = uart0_getc();
        if ((g_uart.byte_idx & 3) == 0) {
            g_uart.byte_idx = 0;
            w = (g_uart.rx_bytes[0] & 0xff)
              | ((g_uart.rx_bytes[1] << 8) & 0xff00)
              | ((g_uart.rx_bytes[2] << 16) & 0xff0000)
              | (g_uart.rx_bytes[3] << 24);
            g_uart.cdb[g_uart.cdb_idx++] = w;

            if (g_uart.dl_addr != 0) {
                ((u32 *)g_uart.dl_ptr)[g_uart.dl_words] = w;
                g_uart.dl_words++;
                /* progress dot: a *word* store to UART1 THR, no wait */
                if ((g_uart.dl_words & 0xff) == 0)
                    REG32(UART1_BASE + UART_THR) = LTR_UART_PROGRESS;
            }
        }
        if (g_uart.cdb_idx != 4)
            goto check_timeout;

        /* ---- a complete CDB ---------------------------------------- */
        g_uart.cdb_idx = 0;
        sum7 = (g_uart.cdb[0] & 0xffff) + (g_uart.cdb[0] >> 16)
             + (g_uart.cdb[1] & 0xffff) + (g_uart.cdb[1] >> 16)
             + (g_uart.cdb[2] & 0xffff) + (g_uart.cdb[2] >> 16)
             + (g_uart.cdb[3] & 0xffff);
        g_uart.cdb_sum = (u16)(sum7 + (g_uart.cdb[3] >> 16) + CDB_SUM_BIAS);

        if (g_uart.cdb[0] == NVT_CDB_IS_NVT) {
            /*
             * Force the checksum halfword to the value that makes the
             * echoed CDB good.  It is OR'ed into w3, not stored, so a
             * host that sent garbage there gets the bits merged.
             */
            g_uart.reply_sum = (u16)(0xffffffe4u - sum7);
            g_uart.cdb[3] |= g_uart.reply_sum << 16;
            for (i = 0; i < 4; i++)
                uart0_put32(g_uart.cdb[i]);

            if (g_uart.cdb_sum == 0) {
                g_uart.chip_id = REG32(TOP_BASE + TOP_CHIP_ID);
                /*
                 * The first byte sent is not chip_id[7:0]: the compiled
                 * code passes the register still holding cdb_sum, which
                 * is 0 on this path.  Bytes 1..3 are the id's.
                 */
                uart0_putc(0);
                uart0_putc((g_uart.chip_id >> 8) & 0xff);
                uart0_putc((g_uart.chip_id >> 16) & 0xff);
                uart0_putc(g_uart.chip_id >> 24);
            } else {
                uart0_put32(0);         /* a single zero word */
            }
        }

        if (g_uart.cdb[0] == NVT_CDB_OPEN_DEVICE) {
            /* the host is here: the 30 s timeout no longer applies */
            timer_stop();
            uart_echo_cdb();
        }

        if ((g_uart.cdb[0] & 0xffff) == NVT_CDB_ADDR_READ) {
            u32 addr, val;

            uart_echo_cdb();
            /* executed whatever the checksum said */
            addr = cdb_hi_lo(g_uart.cdb[0], g_uart.cdb[1]);
            val = *(volatile u32 *)addr;
            uart_put_bytes(addr);
            uart_put_bytes(val);
        }

        if ((g_uart.cdb[0] & 0xffff) == NVT_CDB_ADDR_WRITE) {
            uart_echo_cdb();
            /* executed whatever the checksum said */
            *(volatile u32 *)cdb_hi_lo(g_uart.cdb[0], g_uart.cdb[1]) =
                cdb_hi_lo(g_uart.cdb[1], g_uart.cdb[2]);
        }

        if ((g_uart.cdb[0] & 0xffff) == NVT_CDB_WRLOADER) {
            /*
             * No checksum test and no reply here.  dl_words is not reset
             * (only at entry), so a second WRLOADER continues counting.
             */
            for (i = 0; i < 4; i++)
                g_uart.wrloader_cdb[i] = g_uart.cdb[i];
            g_uart.dl_addr = cdb_hi_lo(g_uart.cdb[0], g_uart.cdb[1]);
            g_uart.dl_total = cdb_hi_lo(g_uart.cdb[1], g_uart.cdb[2]) >> 2;
            g_uart.dl_ptr = g_uart.dl_addr;
        }

        /*
         * Download complete -> echo the saved WRLOADER CDB.  Only tested
         * here, at a CDB boundary: a length that is not a multiple of
         * 16 bytes never matches and is never acknowledged.  dl_addr is
         * not cleared either, so the words that follow (WAIT_DONE) are
         * stored past the end of the image as well.
         */
        if (g_uart.dl_addr != 0 && g_uart.dl_words == g_uart.dl_total) {
            for (i = 0; i < 4; i++)
                uart0_put32(g_uart.wrloader_cdb[i]);
        }

        if (g_uart.cdb[0] == NVT_CDB_WAIT_DONE) {
            uart_echo_cdb();
            g_uart.done = 1;
        }

check_timeout:
        /* after OPEN_DEVICE's timer_stop() this no longer fires */
        if (timer_now() >= g_timeout) {
            timer_stop();
            boot_fail(BOOT_FAIL_TIMEOUT);
        }
    }

    g_uart.done = 0;
    memcpy32((void *)SRAM_HDR_STAGE, (const void *)g_uart.dl_addr, HDR_STAGE_COPY);
    check_and_run_loader(1, 0, 0);
}
