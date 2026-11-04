/*
 * emmc.c -- eMMC boot-partition boot over SDIO3 (BS 8 = 4-bit, BS 9 =
 * 8-bit).  Only reachable from the strap dispatch: next_boot_source()
 * never falls back to eMMC.
 *
 *   func_056 0x3f84 emmc_boot_end
 *   func_057 0x3fac emmc_boot_read
 *   func_061 0x48ec emmc_boot
 *
 * The card is read in eMMC "boot operation" mode: after CMD0 with the
 * GO_PRE_IDLE argument the controller holds CMD low (SDIO_CMD bit13) and
 * the card streams its boot partition from offset 0, so there are no
 * block addresses -- every read just continues the stream, and
 * emmc_boot_end() releases CMD at the very end.
 */
#include "bootrom.h"
#include "na51089-regmap.h"
#include "sram.h"

#define SDIO3(off)      REG32(SDIO3_BASE + (off))

/* TODO(headers): CMD0 command word (index 0, no response) and the two
   boot-acknowledge error bits as one mask are not in regmap-sdio.h. */
#define EMMC_CMD0_FLAGS         0x00
#define SDIO_STS_BOOT_ERR       (SDIO_STS_BOOT_ERR0 | SDIO_STS_BOOT_ERR1)

/*
 * func_056 0x3f84 -- end boot operation: SDIO3+0 = 0 (CMD line
 * released), then wait for boot-end status (+0x28 bit17).  No timeout,
 * and the status bit is not cleared.
 */
void emmc_boot_end(void)
{
    SDIO3(SDIO_CMD) = SDIO_CMD_END_BOOT;
    while (!(SDIO3(SDIO_STS) & SDIO_STS_BOOT_END))
        ;
}

/*
 * func_057 0x3fac -- read the next len bytes of the boot stream to dst.
 *
 * cont == 0 is the first read: it raises the boot request (SDIO_CMD
 * bit13, CMD held low) and waits for the boot acknowledge (bit15), or
 * returns -1 on bit16/bit18.  Later reads pass a non-zero cont (the
 * callers use 0x200) and just keep receiving.
 *
 * Transfer setup: DMA address +0x110 in XFER_DMA mode, direct-DMA
 * destination +0x70 when SRAM_DMA_DIRECT bit31 is set, +0x20, length,
 * then the transfer-mode write bracketed by an SDIO3 clock-off/on with a
 * 1 us delay.  In XFER_DMA mode +0x10c is written twice, 2 then 3 (the
 * original stores g_sd.xfer_mode itself first).
 *
 * Direct DMA (bit31): start (+0x74 bit31), data config, wait for any done
 * bit [7:4], stop -- all BEFORE the boot request, so this mode is only
 * usable on continuation reads (emmc_boot clears SRAM_DMA_DIRECT before
 * the first one).  Returns 0 without touching the FIFO.  Otherwise the
 * data config is written and sdio_read_data() collects the bytes.
 */
int emmc_boot_read(void *dst, int cont, int len)
{
    u32 sts;

    if (g_sd.xfer_mode == XFER_DMA)
        SDIO3(SDIO_DMA_ADDR) = (u32)dst;
    if (g_dma_direct & DMA_DIRECT_EN)
        SDIO3(SDIO_DMA_DST) = (u32)dst;
    SDIO3(SDIO_DATA_CTRL) = SDIO_DATA_CTRL_VAL;
    SDIO3(SDIO_LEN) = len;

    REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SDIO3;
    udelay(1);
    if (g_sd.xfer_mode == XFER_DMA)
        SDIO3(SDIO_XFER_MODE) = g_sd.xfer_mode;
    SDIO3(SDIO_XFER_MODE) = (g_sd.xfer_mode == XFER_DMA) ? 3 : 1;
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SDIO3;

    if (g_dma_direct & DMA_DIRECT_EN) {
        SDIO3(SDIO_DMA_CTRL) = SDIO_DMA_CTRL_START;
        SDIO3(SDIO_DATA_CFG) = SDIO_DATA_CFG_VAL;
        while (!(SDIO3(SDIO_STS) & SDIO_STS_DONE_MASK))
            ;
        SDIO3(SDIO_DMA_CTRL) = 0;
    }
    if (!(g_dma_direct & DMA_DIRECT_EN))
        SDIO3(SDIO_DATA_CFG) = SDIO_DATA_CFG_VAL;

    if (cont == 0) {
        SDIO3(SDIO_CMD) |= SDIO_CMD_BOOT_REQ;
        for (;;) {
            sts = SDIO3(SDIO_STS);
            if (sts & SDIO_STS_BOOT_ERR)
                return -1;
            if (sts & SDIO_STS_BOOT_ACK)
                break;
        }
        SDIO3(SDIO_STS) = SDIO_STS_BOOT_ACK;
    }

    if (g_dma_direct & DMA_DIRECT_EN)
        return 0;
    return sdio_read_data(1, dst, len);
}

/* Poll until (reg & bit) clears; past 1 s print "K:" and keep polling. */
static void emmc_wait_clear_1s(u32 reg, u32 bit)
{
    timer_start();
    while (REG32(reg) & bit) {
        if (timer_now() > TMO_1S)
            boot_fail(BOOT_FAIL_TIMEOUT);
    }
    timer_stop();
}

/*
 * func_061 0x48ec -- eMMC boot (BS 8/9, dispatch only).
 *
 * 'E', wdt_arm_or_skip(), then the strap is read again from TOP+0 (not
 * taken from g_boot_source): BS 9 prints '8' and sets 8-bit bus width,
 * anything else '4' and 4-bit.  Clock on, pinmux, SDIO3 clock divider
 * 0xf; OTP bit14 prints 'R' and leaves SDIO_OPT alone, otherwise
 * SDIO_OPT |= bit12.  Controller power/reset waits (1 s -> "K:", polling
 * goes on), boot-mode setup, CMD0(GO_PRE_IDLE) -- failure is K5 -- and
 * the first 512 bytes of the boot partition into the header staging
 * buffer (failure: end boot, K5).  Header bit18 takes a new SDIO3 clock
 * divider from header[31:24]; [15:12] extra header sectors follow into
 * 0xF07C0200 (failure: end boot, K8); hdr_script_run; header copied to
 * the load address; the rest of the image read (result ignored),
 * emmc_boot_end(), and check_and_run_loader(1, ...) with the SDIO3
 * hardware checksum.
 *
 * Unlike SD, eMMC honours header flag bit28 (direct DMA) and selects
 * XFER_DMA with script-word bit16.  The load address is taken from the
 * staging copy (0xF0800204), the size from the header in use.  boot_fail
 * never returns in practice; if it does, the boot carries on.
 */
void emmc_boot(void)
{
    struct loader_hdr *hdr;
    u32 strap, pinmux, sw, extra, nsect, size, hwsum, n;
    u8 *load, *dst;

    g_sd.xfer_mode = XFER_PIO;
    g_dma_direct = 0;
    UART0_PUTC_RAW(LTR_EMMC);
    wdt_arm_or_skip();

    strap = REG32(TOP_BASE + TOP_STRAP) & TOP_STRAP_BS_MASK;
    if (strap == BS_EMMC_8BIT) {
        UART0_PUTC_RAW('8');
        SDIO3(SDIO_BUSWIDTH) = SDIO_BUSWIDTH_8;
        pinmux = TOP_PINMUX_EMMC8;
    } else {
        UART0_PUTC_RAW('4');
        SDIO3(SDIO_BUSWIDTH) = SDIO_BUSWIDTH_4;
        pinmux = TOP_PINMUX_EMMC4;
    }
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SDIO3;
    SDIO3(SDIO_GATE) = SDIO_GATE_ON;
    udelay(6);

    REG32(TOP_BASE + TOP_PINMUX) =
        (REG32(TOP_BASE + TOP_PINMUX) & TOP_PINMUX_FLASH_KEEP) | pinmux;
    REG32(TOP_BASE + TOP_PINFUNC) =
        (strap == BS_EMMC_8BIT) ? TOP_PINFUNC_EMMC8 : TOP_PINFUNC_EMMC4;
    REG32(CG_BASE + CG_EMMC_CTRL) &= ~CG_EMMC_CTRL_CLR;
    REG32(CG_BASE + CG_DIV40) = (REG32(CG_BASE + CG_DIV40) & ~CG_DIV40_SDIO3_MASK) |
                                CG_DIV40_SDIO3_DEFAULT;
    SDIO3(SDIO_GATE) = 0;

    if (otp_boot_cfg() & OTP_CFG_EMMC_OPT)
        UART0_PUTC_RAW(LTR_EMMC_OPT);
    else
        SDIO3(SDIO_OPT) |= SDIO_OPT_SAMPLE;

    SDIO3(SDIO_CMD) = SDIO_CMD_POWER;
    emmc_wait_clear_1s(SDIO3_BASE + SDIO_CMD, SDIO_CMD_POWER);
    SDIO3(SDIO_RESET) = SDIO_RESET_BIT;
    emmc_wait_clear_1s(SDIO3_BASE + SDIO_RESET, SDIO_RESET_BIT);

    SDIO3(SDIO_STS) = SDIO_STS_CLEAR_ALL;
    SDIO3(SDIO_CMD) = SDIO_CMD_BOOT_CFG;
    SDIO3(SDIO_BOOT_MODE) = SDIO_BOOT_MODE_VAL;
    udelay(0x400);

    SDIO3(SDIO_FIFO_CTRL) |= SDIO_FIFO_CTRL_EN;
    if (sdio_cmd(1, EMMC_CMD0_FLAGS, EMMC_GO_PRE_IDLE_ARG, NULL) < 0)
        boot_fail(BOOT_FAIL_FIRST_SECTOR);

    /* FIFO reset.  The wait compares the whole register with 0x10, and
       FIFO_CTRL bit0 was set just above, so it reads 0x11 and the loop
       falls through at once unless the hardware drops bit0 (faithful). */
    SDIO3(SDIO_FIFO_CTRL) |= SDIO_FIFO_CTRL_RST;
    while (SDIO3(SDIO_FIFO_CTRL) == SDIO_FIFO_CTRL_RST)
        ;

    if (emmc_boot_read((void *)SRAM_HDR_STAGE, 0, 0x200) < 0) {
        emmc_boot_end();
        boot_fail(BOOT_FAIL_FIRST_SECTOR);
    }

    sw = g_hdr_stage.script_word;
    if (sw & HDR_SW_NEW_DIV) {
        REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SDIO3;
        REG32(CG_BASE + CG_DIV40) = (REG32(CG_BASE + CG_DIV40) & ~CG_DIV40_SDIO3_MASK) |
                                    (sw >> HDR_SW_DIV_SHIFT);
        REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SDIO3;
    }

    extra = (sw & HDR_SW_EXTRA_MASK) >> HDR_SW_EXTRA_SHIFT;
    if (extra == 0) {
        hdr = &g_hdr_stage;
    } else {
        memcpy32((void *)SRAM_IMAGE, (void *)SRAM_HDR_STAGE, 0x200);
        if (emmc_boot_read((void *)SRAM_IMAGE_HDR_EXT, 0x200, extra << 9) < 0) {
            emmc_boot_end();
            boot_fail(BOOT_FAIL_ETH_OR_HDR);
        }
        hdr = (struct loader_hdr *)SRAM_IMAGE;
    }
    hdr_script_run(&hdr->script_word);

    nsect = extra + 1;
    load = (u8 *)g_hdr_stage.load;          /* staging copy, not hdr */
    size = hdr->size;
    if ((u8 *)hdr != load)
        memcpy32(load, hdr, nsect << 9);

    g_dma_direct = (hdr->flags & HDR_FLAG_DMA_DIRECT) ? DMA_DIRECT_EN : 0;
    dst = load + (nsect << 9);
    if (sw & HDR_SW_XFER_DMA) {
        g_sd.xfer_mode = XFER_DMA;
        dst = (u8 *)((u32)dst & ~0xc0000000u);  /* bus address (inferred) */
    }
    emmc_boot_read(dst, 0x200, size - (nsect << 9));
    emmc_boot_end();

    n = hdr->size >> 1;                     /* halfwords */
    hwsum = (u16)(SDIO3(SDIO_HWSUM) + ((n * (n - 1)) >> 1));
    check_and_run_loader(1, hdr->flags >> 31, hwsum);
}
