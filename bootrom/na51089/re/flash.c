/*
 * flash.c -- SPI NOR and SPI NAND boot (BS 0, 2, 3, 6, 0xA) through the
 * SPI flash controller at 0xF0400000.
 *
 *   func_006 0x0320 spi_settle          func_012 0x04c0 spi_bus
 *   func_007 0x0348 spi_cmd             func_013 0x04e8 spi_mode
 *   func_008 0x036c spi_wait            func_014 0x0510 spi_wide
 *   func_009 0x03b0 spi_pio_read        func_015 0x0538 nor_read
 *   func_010 0x0414 spi_xfer_setup      func_016 0x0704 nand_read_page
 *   func_011 0x0498 spi_cs              func_017 0x09a0 flash_boot
 *
 * Address registers: spi_xfer_setup(ctrl, r38, r3c, len) writes r38 to
 * +0x38 and r3c to +0x3c.  Every caller puts the first-command address
 * (NOR byte address, NAND row/page) in +0x3c and the NAND READ CACHE
 * column in +0x38: SPI_ROW (+0x3c) and SPI_COL (+0x38) in regmap-spi.h.
 *
 * Opcode words (SPI_OPCODE) pack one byte per command phase, low byte
 * first: 0x03c00f13 = PAGE READ 0x13, GET FEATURE 0x0f @ 0xc0 (status
 * poll, see SPI_POLL_CMD/MASK), READ CACHE 0x03; 0xb00f = GET FEATURE
 * @ 0xb0.
 */
#include "bootrom.h"
#include "na51089-regmap.h"
#include "sram.h"

#define SPI(off)        REG32(SPI_BASE + (off))

/* TODO(headers): the opcode word of GET FEATURE @ 0xb0 (opcode in [7:0],
   feature address in [15:8]) and the SET FEATURE address field. */
#define SPINAND_GET_FEATURE_CFG ((SPINAND_FEATURE_CFG << 8) | SPINAND_OP_GET_FEATURE)
/* TODO(headers): spi_mode() arguments; the ROM only ever passes these. */
#define SPI_MODE_8              8
#define SPI_MODE_4              4
/* TODO(headers): status masks spi_wait() is called with besides DONE. */
#define SPI_STS_NAND_RS_ECC     0x7000          /* bits 12..14 */
#define SPI_STS_NAND_SPARE_RS   0x15000         /* bits 12, 14, 16 */
#define SPI_RS_SPARE_BITS(i)    ((i) * 2)       /* SPI_ECC_STS2: 2 bits/sector */
#define SPI_RS_ODECC_BITS(i)    ((i) * 4)       /* SPI_ECC_STS: 4-bit stride, 3 used */
/* TODO(headers): the NAND scan constants. */
#define NAND_SCAN_BLOCKS        256
#define NAND_BLOCK_SHIFT        6               /* 64 pages per block */
#define NAND_LOADER_COPIES      2
#define NAND_MARKER_MIN_ONES    6               /* good block: popcount > 6 */

/*
 * func_006 0x0320 -- five dummy reads of SPI+0x204: a short settle delay
 * after flipping the SPI_SPARE_CTRL bits.
 */
void spi_settle(void)
{
    (void)SPI(SPI_SPARE1);
    (void)SPI(SPI_SPARE1);
    (void)SPI(SPI_SPARE1);
    (void)SPI(SPI_SPARE1);
    (void)SPI(SPI_SPARE1);
}

/* func_007 0x0348 -- load the opcode word and start the sequence (GO). */
void spi_cmd(u32 op)
{
    SPI(SPI_OPCODE) = op;
    SPI(SPI_CTRL) |= SPI_CTRL_GO;
}

/*
 * func_008 0x036c -- wait until a bit of mask is set in SPI_STS, return 2
 * for bit13 (ECC error), 4 for bit16 (bit16 wins over bit13), else 0,
 * and clear with SPI_STS |= 0x17000 (read-modify-write, so the data byte
 * [7:0] is written back too).  No timeout.
 */
int spi_wait(u32 mask)
{
    u32 sts;
    int rc;

    do {
        sts = SPI(SPI_STS) & mask;
    } while (sts == 0);

    rc = (sts & SPI_STS_ECCERR) ? 2 : 0;
    if (sts & SPI_STS_BIT16)
        rc = 4;
    SPI(SPI_STS) |= SPI_STS_CLEAR;
    return rc;
}

/*
 * Drain len bytes from the data FIFO, storing at most want of them (want
 * is rounded up to whole words, len is not and must be a multiple of 4).  Shared by spi_pio_read and
 * nand_read_page, which carries its own inlined copy in the original.
 */
static inline void spi_fifo_drain(u32 *dst, int want, int len)
{
    u32 w;

    want = (want + 3) & ~3;
    while (len != 0) {
        if (SPI(SPI_FIFO_STS) & SPI_FIFO_STS_EMPTY)
            continue;
        w = SPI(SPI_FIFO);
        if (want != 0) {
            want -= 4;
            *dst++ = w;
        }
        len -= 4;
    }
}

/*
 * func_009 0x03b0 -- PIO data phase: with g_flash.mode != 0 (XFER_DMA)
 * the controller moves the data and this only waits for DONE; otherwise
 * drain len bytes from the FIFO keeping the first want, then wait DONE.
 */
int spi_pio_read(u32 *dst, int want, int len)
{
    if (g_flash.mode == XFER_PIO)
        spi_fifo_drain(dst, want, len);
    return spi_wait(SPI_STS_DONE);
}

/*
 * func_010 0x0414 -- program the address registers, the length and the
 * control word (plain store of ctrl | SETUP).  When the length is 8, or
 * more than 16 << g_flash.page_shift (the spare size of a NAND page, 16
 * bytes for NOR), the transfer-mode register is also written, g_flash.mode
 * | 1, bracketed by the SPI clock enable off/on.
 */
void spi_xfer_setup(u32 ctrl, u32 r38, u32 r3c, u32 len)
{
    SPI(SPI_COL) = r38;             /* +0x38: NAND READ CACHE column */
    SPI(SPI_ROW) = r3c;             /* +0x3c: NOR address / NAND row */
    SPI(SPI_LEN) = len;
    SPI(SPI_CTRL) = ctrl | SPI_CTRL_SETUP;

    if (len > (16u << g_flash.page_shift) || len == 8) {
        REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SPI;
        SPI(SPI_XFER_MODE) = g_flash.mode | 1;
        REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SPI;
    }
}

/* func_011 0x0498 -- SPI_BUS bit4 (the ROM raises it around commands). */
void spi_cs(int on)
{
    if (on)
        SPI(SPI_BUS) |= SPI_BUS_CS;
    else
        SPI(SPI_BUS) &= ~SPI_BUS_CS;
}

/* func_012 0x04c0 -- SPI_BUS[1:0] = a | b. */
void spi_bus(u32 a, u32 b)
{
    SPI(SPI_BUS) = (SPI(SPI_BUS) & ~SPI_BUS_MODE_MASK) | b | a;
}

/* func_013 0x04e8 -- SPI_CFG[3:0] = a | b. */
void spi_mode(u32 a, u32 b)
{
    SPI(SPI_CFG) = (SPI(SPI_CFG) & ~SPI_CFG_MODE_MASK) | b | a;
}

/* func_014 0x0510 -- SPI_BUS[11:9] = 0xa00 (x2 data) or 0. */
void spi_wide(int on)
{
    u32 v = SPI(SPI_BUS) & ~SPI_BUS_WIDE_MASK;

    if (on)
        v |= SPI_BUS_WIDE_X2;
    SPI(SPI_BUS) = v;
}

/*
 * func_015 0x0538 -- read len bytes from NOR address addr into dst.
 *
 * CS pulse, mode 8, command phase READ (0x03), or DUAL OUTPUT READ (0x3b)
 * with x2 data lines when g_flash.wide == 1, which also needs a dummy
 * phase (ctrl 0x1e, SPI_DUMMY = 7, opcode word 3).  Data phase ctrl 0x18:
 * PIO through spi_pio_read(), or with SRAM_DMA_DIRECT bit31 controller
 * DMA straight to dst (DMA_CTRL then DMA_ADDR, TOP DMA select, the opcode
 * and a plain 0x3018 control store instead of spi_cmd, a DONE poll and a
 * plain-store status clear).  The return value is the last spi_wait()
 * result (never negative, so the `< 0` exits are dead).  Every caller
 * ignores it.
 */
int nor_read(void *dst, u32 addr, u32 len)
{
    int rc;

    spi_cs(1);
    spi_bus(0, 2);
    spi_cs(0);
    spi_mode(SPI_MODE_8, 0);
    spi_xfer_setup(SPI_SEQ_NOR_CMD, 0, addr, 0);

    if (g_flash.wide == 1) {
        spi_wide(1);
        spi_cmd(SPINOR_OP_READ_DUAL);
    } else {
        spi_wide(0);
        spi_cmd(SPINOR_OP_READ);
    }
    rc = spi_wait(SPI_STS_DONE);
    if (rc < 0)
        goto out;

    if (g_flash.wide == 1) {
        spi_xfer_setup(SPI_SEQ_1E, 0, 0, 0);
        SPI(SPI_DUMMY) = SPI_DUMMY_X2;
        spi_cmd(SPINOR_OP_READ);
        rc = spi_wait(SPI_STS_DONE);
        if (rc < 0)
            goto out;
    }

    spi_xfer_setup(SPI_SEQ_DATA, 0, 0, len);
    if (g_dma_direct & DMA_DIRECT_EN) {
        SPI(SPI_DMA_CTRL) = SPI_DMA_CTRL_EN;
        SPI(SPI_DMA_ADDR) = (u32)dst;
        REG32(TOP_BASE + TOP_DMA_SEL) |= TOP_DMA_SEL_SPI;
    }
    if (g_dma_direct & DMA_DIRECT_EN) {
        SPI(SPI_OPCODE) = SPINOR_OP_READ;
        SPI(SPI_CTRL) = SPI_CTRL_SETUP | SPI_CTRL_GO | SPI_SEQ_DATA;
    } else {
        spi_cmd(SPINOR_OP_READ);
    }
    if (!(g_dma_direct & DMA_DIRECT_EN))
        rc = spi_pio_read(dst, len, len);
    if (g_dma_direct & DMA_DIRECT_EN) {
        while (!(SPI(SPI_STS) & SPI_STS_DONE))
            ;
        SPI(SPI_STS) = SPI_STS_CLEAR;
        SPI(SPI_DMA_CTRL) = 0;
    }
out:
    spi_cs(1);
    spi_bus(0, 0);
    return rc;
}

/*
 * func_016 0x0704 -- read one NAND page (row) from column col, len bytes
 * kept at dst.  Always returns 0.
 *
 * Mode 8|1, setup ctrl 0x16 for a full page (512 << page_shift), status
 * poll 0xc00f / 0x100, opcode word 0x03c00f13 or 0x3bc00f13 (x2 cache
 * read when g_flash.wide == 1).
 *
 *  - SRAM_DMA_DIRECT bit31: DMA_ADDR then DMA_CTRL, GO, DONE poll,
 *    plain-store status clear, DMA off.
 *  - on-die ECC (g_flash.odecc == 1): spi_pio_read(dst, len, page).
 *  - RS ECC: drain the FIFO by PIO (XFER_PIO only), then wait on
 *    0x7000; if that reports an ECC error (2), correct: SPI_SPARE_CTRL
 *    bit1, settle, per-sector status at SPI_ECC_STS (4-bit stride, 3
 *    bits = error count; 0 and 5 mean nothing to fix) for len / 512
 *    sectors, each error word at SPI_RS_ERR0(i) + 4 * k, k = 1..count:
 *    [15:0] byte offset, [23:16] XOR mask.  Offsets < 512 are patched in
 *    dst at sector i (word RMW, offset & 0xfffc); larger ones are in the
 *    controller's spare buffer, patched at SPI + 16 * i + offset with
 *    SPI_SPARE_CTRL bit0 set around it.  A count of 6 or 7 walks into the
 *    next sector's words; nothing marks a page uncorrectable here.
 */
int nand_read_page(u32 row, u32 col, void *dst, u32 len)
{
    u32 op, sts, e, off, mask, shift, nsect, i, k, cnt;
    volatile u32 *spare;

    spi_mode(SPI_MODE_8, 1);
    spi_xfer_setup(SPI_SEQ_NAND_READ, col, row, 512u << g_flash.page_shift);

    if (g_flash.wide == 1) {
        spi_wide(1);
        op = SPINAND_READ_SEQ_X2;
    } else {
        spi_wide(0);
        op = SPINAND_READ_SEQ;
    }
    SPI(SPI_POLL_MASK) = SPI_POLL_MASK_VAL;
    SPI(SPI_POLL_CMD) = SPI_POLL_CMD_VAL;

    if (g_dma_direct & DMA_DIRECT_EN) {
        SPI(SPI_DMA_ADDR) = (u32)dst;
        SPI(SPI_DMA_CTRL) = SPI_DMA_CTRL_EN;
        spi_cmd(op);
        while (!(SPI(SPI_STS) & SPI_STS_DONE))
            ;
        SPI(SPI_STS) = SPI_STS_CLEAR;
        SPI(SPI_DMA_CTRL) = 0;
        return 0;
    }

    spi_cmd(op);
    if (g_flash.odecc == 1) {
        spi_pio_read(dst, len, 512 << g_flash.page_shift);
        return 0;
    }

    if (g_flash.mode == XFER_PIO)
        spi_fifo_drain(dst, len, 512 << g_flash.page_shift);
    if (spi_wait(SPI_STS_NAND_RS_ECC) != 2)
        return 0;

    SPI(SPI_SPARE_CTRL) |= SPI_SPARE_CTRL_B1;
    spi_settle();
    sts = SPI(SPI_ECC_STS);
    nsect = len >> 9;
    for (i = 0; i != nsect; i++) {
        cnt = (sts >> SPI_RS_ODECC_BITS(i)) & 7;
        if (cnt == 0 || cnt == 5)
            continue;
        for (k = 1; k <= cnt; k++) {
            e = SPI(SPI_RS_ERR0(i) + 4 * k);
            off = e & 0xffff;
            mask = (e >> 16) & 0xff;
            shift = (off & 3) * 8;
            if (off < 512) {
                off = (off & 0xfffc) + (i << 9);
                *(u32 *)((u8 *)dst + off) ^= mask << shift;
            } else {
                SPI(SPI_SPARE_CTRL) |= SPI_SPARE_CTRL_B0;
                spi_settle();
                spare = (volatile u32 *)((SPI_BASE + 16 * i + off) & ~3u);
                *spare ^= mask << shift;
                SPI(SPI_SPARE_CTRL) &= ~SPI_SPARE_CTRL_B0;
            }
        }
    }
    SPI(SPI_SPARE_CTRL) &= ~SPI_SPARE_CTRL_B1;
    return 0;
}

/* Poll until (SPI + off) & bit clears; past 1 s "K:" and keep polling. */
static void spi_wait_clear_1s(u32 off, u32 bit)
{
    timer_start();
    while (SPI(off) & bit) {
        if (timer_now() > TMO_1S)
            boot_fail(BOOT_FAIL_TIMEOUT);
    }
    timer_stop();
}

/*
 * FIFO reset.  The wait compares the whole register with 0x10 and
 * SPI_FIFO_CTRL bit0 was set during init, so it reads 0x11 and the loop
 * falls through at once unless the hardware drops bit0 (faithful).
 */
static void spi_fifo_reset(void)
{
    SPI(SPI_FIFO_CTRL) |= SPI_FIFO_CTRL_RST;
    while (SPI(SPI_FIFO_CTRL) == SPI_FIFO_CTRL_RST)
        ;
}

/*
 * NOR half of flash_boot (0x0ae8..0x0d40, inline in the original).
 *
 * Reset-enable/reset (0x66, 0x99), then -- unless OTP bit6 -- 'p' and a
 * RDSR (0x05) whose result is not looked at.  SPI_CFG = 0x180001, the
 * first 512 bytes to the staging buffer, the script word's bit18 divider
 * (taken as header[31:24] -> CG+0x40[19:12] while only [17:12] are
 * cleared: the NAND path masks to 6 bits, this one does not), extra
 * header sectors to 0xF07C0200, hdr_script_run, header copied to the load
 * address, flags bit30 -> g_flash.wide, bit28 -> SRAM_DMA_DIRECT.  Body:
 *   script bit16: g_flash.mode = XFER_DMA, read to the bus address (bits
 *     31:30 stripped, inferred) from the end of the header sectors;
 *   else direct DMA clear: PIO read of the same range;
 *   else (direct DMA set): FIFO reset and a read of the WHOLE image from
 *     flash address 0 to (load + 512 * nsect - 512), which is the load
 *     address only when there are no extra header sectors.
 * Then check_and_run_loader(1, ...) with the SPI hardware checksum.
 */
static void nor_boot(void)
{
    struct loader_hdr *hdr;
    u32 sw, extra, nsect, size, off, n, hwsum;
    u8 *load, *dst;

    /* RESET ENABLE, RESET */
    spi_cs(1);
    spi_bus(0, 2);
    spi_cs(0);
    spi_xfer_setup(SPI_SEQ_07, 0, 0, 0);
    spi_cmd(SPINOR_OP_RSTEN);
    spi_wait(SPI_STS_DONE);
    spi_cs(1);
    spi_cs(0);
    spi_xfer_setup(SPI_SEQ_07, 0, 0, 0);
    spi_cmd(SPINOR_OP_RST);
    spi_wait(SPI_STS_DONE);
    spi_cs(1);
    spi_bus(0, 0);

    /* the 'p' step: READ STATUS, result not used */
    if (!(otp_boot_cfg() & OTP_CFG_NOR_NO_RDSR)) {
        UART0_PUTC_RAW(LTR_NOR_RDSR);
        spi_cs(1);
        spi_bus(0, 2);
        spi_cs(0);
        spi_xfer_setup(SPI_SEQ_1D, 0, 0, 0);
        /* SPI_POLL_MASK written back with its own value (a no-op RMW in
           the original: ldr/str with nothing in between) */
        SPI(SPI_POLL_MASK) = SPI(SPI_POLL_MASK);
        spi_cmd(SPINOR_OP_RDSR);
        spi_wait(SPI_STS_DONE);
        spi_cs(1);
        spi_bus(0, 0);
    }

    SPI(SPI_CFG) = SPI_CFG_NOR;
    spi_bus(0, 2);
    g_dma_direct = 0;
    spi_fifo_reset();
    nor_read((void *)SRAM_HDR_STAGE, 0, 0x200);

    sw = g_hdr_stage.script_word;
    if (sw & HDR_SW_NEW_DIV)
        REG32(CG_BASE + CG_DIV40) = (REG32(CG_BASE + CG_DIV40) & ~CG_DIV40_SPI_MASK) |
                                    ((sw >> 12) & 0xff000);
    extra = (sw & HDR_SW_EXTRA_MASK) >> HDR_SW_EXTRA_SHIFT;
    if (extra == 0) {
        hdr = &g_hdr_stage;
    } else {
        hdr = (struct loader_hdr *)SRAM_IMAGE;
        memcpy32((void *)SRAM_IMAGE, (void *)SRAM_HDR_STAGE, 0x200);
        nor_read((void *)SRAM_IMAGE_HDR_EXT, 0x200, extra << 9);
    }
    nsect = extra + 1;
    hdr_script_run(&hdr->script_word);

    load = (u8 *)hdr->load;
    size = hdr->size;
    if ((u8 *)hdr != load)
        memcpy32(load, hdr, nsect << 9);

    g_flash.wide = (hdr->flags & HDR_FLAG_DUAL) ? 1 : 0;
    g_dma_direct = (hdr->flags & HDR_FLAG_DMA_DIRECT) ? DMA_DIRECT_EN : 0;
    off = nsect << 9;
    dst = load + off;
    if (sw & HDR_SW_XFER_DMA) {
        g_flash.mode = XFER_DMA;
        nor_read((void *)((u32)dst & ~0xc0000000u), off, size - off);
    } else {
        g_flash.mode = XFER_PIO;
        if (!(g_dma_direct & DMA_DIRECT_EN)) {
            nor_read(dst, off, size - off);
        } else {
            spi_fifo_reset();
            nor_read(dst - 0x200, 0, size);
        }
    }

    n = hdr->size >> 1;                     /* halfwords */
    hwsum = (u16)(SPI(SPI_HWSUM) + ((n * (n - 1)) >> 1));
    check_and_run_loader(1, hdr->flags >> 31, hwsum);
}

/*
 * Spare area of page 0 of a block: read 16 << page_shift bytes from
 * column 512 << page_shift into the controller's spare buffer, waiting on
 * DONE (on-die ECC) or on 0x15000 (RS).  For RS, status 4 (bit16) means
 * the controller found errors in the spare: 2-bit codes per sector in
 * SPI_ECC_STS2; code 1 with a location >= 512 is fixed in the spare
 * buffer (bit flip at SPI + 16 * i + location, bit [15:12]); codes 2/3,
 * or code 1 pointing into the data area, make the page "bad", and then
 * page 0's first 512 bytes are read to the staging buffer through
 * nand_read_page() (which reloads the spare buffer with its own ECC).
 * (Inline in flash_boot in the original.)
 */
static void nand_read_spare(u32 row)
{
    u32 nsect, sts, e, loc, bit, a, i;
    int bad;

    spi_mode(SPI_MODE_8, 1);
    spi_xfer_setup(SPI_SEQ_NAND_READ, 512u << g_flash.page_shift, row,
                   16u << g_flash.page_shift);
    SPI(SPI_POLL_MASK) = SPI_POLL_MASK_VAL;
    SPI(SPI_POLL_CMD) = SPI_POLL_CMD_VAL;
    spi_cmd(SPINAND_READ_SEQ);
    if (spi_wait(g_flash.odecc != 0 ? SPI_STS_DONE : SPI_STS_NAND_SPARE_RS) != 4)
        return;

    nsect = g_flash.sectors;
    SPI(SPI_SPARE_CTRL) |= SPI_SPARE_CTRL_B1;
    spi_settle();
    sts = SPI(SPI_ECC_STS2);
    bad = 0;
    for (i = 0; i != nsect; i++) {
        switch ((sts >> SPI_RS_SPARE_BITS(i)) & 3) {
        case 0:
            break;
        case 2:
        case 3:
            bad = -1;
            break;
        default:                        /* 1: one correctable error */
            e = SPI(SPI_RS_ERR2(i));
            loc = e & SPI_RS_ERR_BYTE_MASK;
            if (loc < 0x200) {
                bad = -1;
                break;
            }
            SPI(SPI_SPARE_CTRL) |= SPI_SPARE_CTRL_B0;
            spi_settle();
            bit = (e >> SPI_RS_ERR_BIT_SHIFT) & 0xf;
            a = SPI_BASE + 16 * i + loc;
            REG32(a & ~3u) ^= (1u << bit) << ((a & 3) * 8);
            SPI(SPI_SPARE_CTRL) &= ~SPI_SPARE_CTRL_B0;
            break;
        }
    }
    SPI(SPI_SPARE_CTRL) &= ~SPI_SPARE_CTRL_B1;
    if (bad == -1)
        nand_read_page(row, 0, (void *)SRAM_HDR_STAGE, 0x200);
}

/*
 * NAND half of flash_boot (0x0dd4..0x1320, inline in the original).
 *
 * Geometry: BS 6/0xA are 4K pages (cfg 0x70, 8 sectors, page_shift 3),
 * BS 2/3 2K (0x30, 4, 2); 64 pages per block; BS 2/6 on-die ECC
 * (SPI_CFG 0x80305 | cfg), BS 3/0xA the controller's RS ECC (0x80a05 |
 * cfg).  SPI_BUS NAND bit, RESET (0xff), udelay(0x400).  For RS the
 * chip's own ECC is switched off: GET FEATURE 0xb0, and if bit4 (ECC_EN)
 * is set, SET FEATURE 0xb0 with it cleared (mode 4|1).
 *
 * Scan of blocks 0..255, page 0 of each:
 *   spare marker -- byte 0 of SPI+0x200, or bits[15:8] of SPI+0x204 when
 *     g_flash.sectors == 1, which never holds (4 or 8): that RS-looking
 *     branch is dead and every strap uses SPI+0x200 -- must be non-zero
 *     with more than 6 bits set, else next block;
 *   a good marker uses up one of the two copies BEFORE the tag is checked;
 *   page 0's first 512 bytes to staging, hdr_tag_ok, bit18 divider
 *     (6-bit, header[29:24]);
 *   first copy only: extra header sectors -- always just 512 bytes from
 *     column 512, whatever the count says -- and hdr_script_run.  The
 *     second copy reuses the staging header and does NOT rerun the
 *     script;
 *   flags/mode as on NOR (script bit16 -> XFER_DMA, bit30 wide, bit28
 *     direct DMA + TOP DMA select), FIFO reset, size >> (9 + page_shift)
 *     whole pages read from the block's first page to the load address
 *     (the header pages are read again: nothing is copied first);
 *   first copy: check_and_run_loader(0, ...); if it returns, SPI clock
 *     divider back to 0x27 (clock gated around it), wide off; second
 *     copy: check_and_run_loader(1, ...), i.e. K9 and the next source.
 * After any block, once both copies are used up the next iteration
 * starts with boot_fail(7) -- unless 256 blocks are done, which is
 * checked first: returning 0 then lands back in the strap dispatch
 * (boot_dispatch jumps here without setting lr), an endless rescan.
 */
static int nand_boot(int strap)
{
    struct loader_hdr *hdr;
    u32 block, row, marker, feat, sw, npages, page, end, n, hwsum, div, en;
    u8 *load;
    int copies_left, first_done;

    g_flash.block_shift = NAND_BLOCK_SHIFT;
    if (((strap - BS_NAND_ODECC_4K) & ~4) == 0) {           /* 6, 0xA */
        g_flash.cfg = SPI_CFG_PAGE_4K;
        g_flash.sectors = 8;
        g_flash.page_shift = 3;
    } else {                                                /* 2, 3 */
        g_flash.cfg = SPI_CFG_PAGE_2K;
        g_flash.sectors = 4;
        g_flash.page_shift = 2;
    }
    if ((strap & ~4) == BS_NAND_ODECC_2K) {                 /* 2, 6 */
        g_flash.odecc = 1;
        SPI(SPI_CFG) = g_flash.cfg | SPI_CFG_NAND_ODECC;
    } else {                                                /* 3, 0xA */
        g_flash.odecc = 0;
        SPI(SPI_CFG) = g_flash.cfg | SPI_CFG_NAND_RSECC;
    }

    SPI(SPI_BUS) |= SPI_BUS_NAND;
    spi_bus(0, 0);
    spi_xfer_setup(SPI_SEQ_07, 0, 0, 0);
    spi_cmd(SPINAND_OP_RESET);
    spi_wait(SPI_STS_DONE);
    udelay(0x400);

    if (g_flash.odecc == 0) {
        spi_xfer_setup(SPI_SEQ_0B, 0, 0, 0);
        spi_cmd(SPINAND_GET_FEATURE_CFG);
        spi_wait(SPI_STS_DONE);
        feat = SPI(SPI_STS);                /* [7:0] = feature byte */
        if (feat & SPINAND_FEATURE_CFG_ECC) {
            spi_xfer_setup(SPI_SEQ_NOR_CMD, 0,
                           (SPINAND_FEATURE_CFG << 8) |
                           (feat & SPI_STS_DATA_MASK & ~SPINAND_FEATURE_CFG_ECC),
                           0);
            spi_mode(SPI_MODE_4, 1);
            spi_cmd(SPINAND_OP_SET_FEATURE);
            spi_wait(SPI_STS_DONE);
        }
    }

    g_dma_direct = 0;
    first_done = 0;
    copies_left = NAND_LOADER_COPIES;
    for (block = 0;;) {
        nand_read_spare(block << g_flash.block_shift);

        SPI(SPI_SPARE_CTRL) |= SPI_SPARE_CTRL_B0;
        spi_settle();
        if (g_flash.sectors == 1)          /* never: see above */
            marker = SPI(SPI_SPARE1) & 0xff00;
        else
            marker = SPI(SPI_SPARE0) & 0xff;
        SPI(SPI_SPARE_CTRL) &= ~SPI_SPARE_CTRL_B0;
        if (marker == 0 || popcount(marker) <= NAND_MARKER_MIN_ONES)
            goto next;

        copies_left--;
        nand_read_page(block << g_flash.block_shift, 0, (void *)SRAM_HDR_STAGE, 0x200);
        if (hdr_tag_ok(&g_hdr_stage.script_word) < 0)
            goto next;

        sw = g_hdr_stage.script_word;
        if (sw & HDR_SW_NEW_DIV)
            REG32(CG_BASE + CG_DIV40) = (REG32(CG_BASE + CG_DIV40) & ~CG_DIV40_SPI_MASK) |
                                        ((sw >> 12) & CG_DIV40_SPI_MASK);
        if (first_done) {
            hdr = &g_hdr_stage;
        } else {
            if ((sw & HDR_SW_EXTRA_MASK) == 0) {
                hdr = &g_hdr_stage;
            } else {
                memcpy32((void *)SRAM_IMAGE, (void *)SRAM_HDR_STAGE, 0x200);
                hdr = (struct loader_hdr *)SRAM_IMAGE;
                nand_read_page(block << g_flash.block_shift, 0x200,
                               (void *)SRAM_IMAGE_HDR_EXT, 0x200);
            }
            hdr_script_run(&hdr->script_word);
        }

        load = (u8 *)hdr->load;
        g_flash.mode = (sw & HDR_SW_XFER_DMA) ? XFER_DMA : XFER_PIO;
        npages = hdr->size >> (g_flash.page_shift + 9);
        g_flash.wide = (hdr->flags & HDR_FLAG_DUAL) ? 1 : 0;
        g_dma_direct = (hdr->flags & HDR_FLAG_DMA_DIRECT) ? DMA_DIRECT_EN : 0;
        spi_fifo_reset();

        row = block << g_flash.block_shift;
        if (g_dma_direct & DMA_DIRECT_EN)
            REG32(TOP_BASE + TOP_DMA_SEL) |= TOP_DMA_SEL_SPI;
        end = npages + row;
        for (page = row; page != end; page++) {
            u32 plen = 512u << g_flash.page_shift;

            nand_read_page(page, 0, load, plen);
            load += plen;
        }
        if (g_dma_direct & DMA_DIRECT_EN)
            SPI(SPI_DMA_CTRL) = 0;

        n = hdr->size >> 1;                 /* halfwords */
        hwsum = (u16)(SPI(SPI_HWSUM) + ((n * (n - 1)) >> 1));
        if (copies_left != 0) {
            check_and_run_loader(0, hdr->flags >> 31, hwsum);
            /* first copy failed: SPI clock back to the default divider */
            div = REG32(CG_BASE + CG_DIV40);
            en = REG32(CG_BASE + CG_CLKEN0);
            REG32(CG_BASE + CG_CLKEN0) = en & ~CG_CLKEN0_SPI;
            REG32(CG_BASE + CG_DIV40) = (div & ~CG_DIV40_SPI_MASK) |
                                        (CG_DIV40_SPI_DEFAULT << CG_DIV40_SPI_SHIFT);
            REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SPI;
            g_flash.wide = 0;
            spi_wide(0);
        } else {
            check_and_run_loader(1, hdr->flags >> 31, hwsum);
        }
        first_done = 1;
next:
        if (++block == NAND_SCAN_BLOCKS)
            return 0;
        if (copies_left == 0)
            boot_fail(BOOT_FAIL_LOADER_READ);
    }
}

/*
 * func_017 0x09a0 -- SPI flash boot, entered from the strap dispatch with
 * the strap in r0 (the only boot entry that takes it).
 *
 * g_flash.mode/wide/strap/page_shift initialised (the rest of g_flash is
 * not cleared).  Letter: 'P' NOR, 'D' NAND on-die ECC, 'R' NAND RS ECC;
 * any other strap is boot_fail(6) with no letter -- and should that
 * return, flash_boot carries on.  wdt_arm_or_skip, pinmux, PAD+0x40 = 0,
 * SPI clock divider 0x27 and enable, soft reset (+0x20 bit15, 1 s -> "K:"
 * and keep polling), OTP bit4: SPI_CTRL8 bit3 cleared and 'D' (clock
 * from the pad), else 'I'; controller reset (+8 bit0, same timeout),
 * timing 0x551, FIFO enable.  Then nor_boot()/nand_boot() above, which
 * are one function with this in the original.
 *
 * Returns 0 only when the NAND scan finds nothing (see nand_boot); the
 * NOR path ends in check_and_run_loader(1, ...) and never returns.
 */
int flash_boot(int strap)
{
    g_flash.mode = XFER_PIO;
    g_flash.wide = 0;
    g_flash.strap = strap;
    g_flash.page_shift = 0;

    switch (strap) {
    case BS_SPI_NOR:
        UART0_PUTC_RAW(LTR_NOR);
        break;
    case BS_NAND_ODECC_2K:
    case BS_NAND_ODECC_4K:
        UART0_PUTC_RAW(LTR_NAND_ODECC);
        break;
    case BS_NAND_RSECC_2K:
    case BS_NAND_RSECC_4K:
        UART0_PUTC_RAW(LTR_NAND_RSECC);
        break;
    default:
        boot_fail(BOOT_FAIL_BAD_STRAP);
        break;
    }
    wdt_arm_or_skip();

    REG32(TOP_BASE + TOP_PINMUX) =
        (REG32(TOP_BASE + TOP_PINMUX) & TOP_PINMUX_FLASH_KEEP) | TOP_PINMUX_FLASH;
    REG32(TOP_BASE + TOP_PINFUNC) = TOP_PINFUNC_FLASH;
    REG32(PAD_BASE + PAD_FLASH) = 0;
    REG32(CG_BASE + CG_DIV40) = (REG32(CG_BASE + CG_DIV40) & ~CG_DIV40_SPI_MASK) |
                                (CG_DIV40_SPI_DEFAULT << CG_DIV40_SPI_SHIFT);
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SPI;

    SPI(SPI_CTRL) |= SPI_CTRL_SWRST;
    spi_wait_clear_1s(SPI_CTRL, SPI_CTRL_SWRST);

    if (otp_boot_cfg() & OTP_CFG_SPI_PADCLK) {
        SPI(SPI_CTRL8) &= ~SPI_CTRL8_INTCLK;
        UART0_PUTC_RAW(LTR_SPI_PADCLK);
    } else {
        UART0_PUTC_RAW(LTR_SPI_INTCLK);
    }

    SPI(SPI_CTRL8) |= SPI_CTRL8_RESET;
    spi_wait_clear_1s(SPI_CTRL8, SPI_CTRL8_RESET);
    SPI(SPI_TIMING) = SPI_TIMING_VAL;
    SPI(SPI_FIFO_CTRL) |= SPI_FIFO_CTRL_EN;

    if (g_flash.strap == BS_SPI_NOR) {
        nor_boot();
        return 0;           /* not reached: nor_boot ends in a fatal check */
    }
    return nand_boot(g_flash.strap);
}
