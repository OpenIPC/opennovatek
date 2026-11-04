/*
 * sd.c -- SD card boot over SDIO1 (BS 1, and the first fallback of every
 * flash/eMMC/UART source).
 *
 *   func_022 0x1528 mbr_first_lba
 *   func_023 0x155c fat_data_start
 *   func_024 0x16d0 fat_cluster_sects
 *   func_059 0x4244 sd_read_blocks
 *   func_060 0x439c sd_boot
 *
 * There is no file-system walk: the loader must be the first file in the
 * data area of the first partition (or of a superfloppy).  sd_boot scans
 * at most ten clusters from there for a header with the 0xAA55 tag.
 */
#include "bootrom.h"
#include "na51089-regmap.h"
#include "sram.h"

#define SDIO1(off)      REG32(SDIO1_BASE + (off))

/* TODO(headers): only CMD8/12/13/18 are in regmap-sdio.h; these are the
   other command words sd_boot passes to sdio_cmd (index | 0x40 short /
   0x80 long). */
#define SD_CMD0_FLAGS           0x00    /* GO_IDLE_STATE, no response */
#define SD_CMD8_FLAGS           0x48    /* SEND_IF_COND */
#define SD_CMD55_FLAGS          0x77    /* APP_CMD */
#define SD_ACMD41_FLAGS         0x69    /* SD_SEND_OP_COND */
#define SD_CMD2_FLAGS           0xc2    /* ALL_SEND_CID (long wins) */
#define SD_CMD3_FLAGS           0x43    /* SEND_RELATIVE_ADDR */
#define SD_CMD7_FLAGS           0x47    /* SELECT_CARD */
#define SD_ACMD6_FLAGS          0x46    /* SET_BUS_WIDTH */
#define SD_CMD16_FLAGS          0x50    /* SET_BLOCKLEN */
#define SD_ACMD41_ARG_HCS       0x40ff8000u     /* HCS + 2.7-3.6 V */
#define SD_ACMD41_ARG           0x00ff8000u     /* 2.7-3.6 V */
#define SD_OCR_BUSY_DONE        BIT(31)
#define SD_OCR_CCS              BIT(30)
#define SD_ACMD41_TRIES         5       /* CMD55/ACMD41 errors tolerated */
#define SD_ACMD41_POLLS         6000    /* "still busy" answers tolerated */
#define SD_R1_STATE_MASK        0x1e00  /* CMD13 card state [12:9] */
#define SD_R1_STATE_TRAN        0x0800  /* state 4 = tran */
#define SD_ACMD6_4BIT           2
#define SD_FAT_SCAN_CLUSTERS    10

/* little-endian fields of an unaligned boot sector, read bytewise */
static inline u32 le16_at(const u8 *p, int off)
{
    return p[off] | (p[off + 1] << 8);
}

static inline u32 le32_at(const u8 *p, int off)
{
    return p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | ((u32)p[off + 3] << 24);
}

/*
 * func_022 0x1528 -- LBA of partition 1 from an MBR, or 0.
 *
 * The only test is that the boot-indicator byte of entry 0 (0x1be) has
 * bits[6:0] clear, i.e. reads 0x00 or 0x80.  No 0x55AA check: a FAT boot
 * sector without a partition table passes too whenever that byte happens
 * to be 0x00/0x80, and then whatever sits at 0x1c6 is taken as the LBA.
 * 0 means "no MBR" and the caller treats sector 0 as the boot sector.
 */
u32 mbr_first_lba(const u8 *mbr)
{
    if (mbr[0x1be] & 0x7f)
        return 0;
    return le32_at(mbr, 0x1c6);
}

/*
 * func_023 0x155c -- sector (relative to the partition) where the loader
 * file is expected, and g_cluster_sects as a side effect.
 *
 * exFAT ("\xEB\x76\x90EXFAT", 8 bytes compared): prints 'e' 'x';
 *   cluster = 1 << pbr[0x6d] (shift loop), and returns
 *   ClusterHeapOffset(0x58) + (FirstClusterOfRootDirectory(0x60) - 1) *
 *   cluster, i.e. the cluster right after the root directory's first
 *   cluster (cluster heap starts at cluster 2).
 * FAT: cluster = pbr[0xd];
 *   RootEntCnt (0x11) == 0 -> FAT32: rsvd + nfats * FATSz32 + cluster
 *     (one cluster past the FAT area: skips cluster 2, the root
 *     directory; the ledger note omits this term);
 *   else FAT12/16: rsvd + nfats * FATSz16 + RootEntCnt / 16 (the first
 *     data cluster).
 * Note FAT32 is detected by the root-entry count alone, from its two
 * bytes tested separately.
 */
u32 fat_data_start(const u8 *pbr)
{
    u32 rsvd, nfats, cluster;
    u32 i;

    /* byte compares against immediates, as the original: no table */
    if (pbr[0] == 0xeb && pbr[1] == 0x76 && pbr[2] == 0x90 && pbr[3] == 'E' &&
        pbr[4] == 'X' && pbr[5] == 'F' && pbr[6] == 'A' && pbr[7] == 'T') {
        UART0_PUTC_RAW('e');
        UART0_PUTC_RAW('x');
        cluster = 1;
        for (i = 0; i < pbr[0x6d]; i++)
            cluster <<= 1;
        g_cluster_sects = cluster;
        return le32_at(pbr, 0x58) + cluster * (le32_at(pbr, 0x60) - 1);
    }

    cluster = pbr[0x0d];
    g_cluster_sects = cluster;
    rsvd = le16_at(pbr, 0x0e);
    nfats = pbr[0x10];
    if (pbr[0x11] == 0 && pbr[0x12] == 0)
        return le32_at(pbr, 0x24) * nfats + cluster + rsvd;
    return nfats * le16_at(pbr, 0x16) + rsvd + (le16_at(pbr, 0x11) >> 4);
}

/* func_024 0x16d0 -- sectors per cluster of the last fat_data_start(). */
u32 fat_cluster_sects(void)
{
    return g_cluster_sects;
}

/*
 * func_059 0x4244 -- read n 512-byte blocks from lba into dst.
 *
 * CMD13 until the card is in tran state (no limit; a failing CMD13
 * returns its error).  Byte address unless g_sd.hc.  Data path setup as
 * in emmc_boot_read (XFER_DMA address, +0x20, length, transfer mode
 * written under an SDIO1 clock-off/1 us/clock-on bracket, 2 then 3 in
 * XFER_DMA mode), data config, then CMD18 issued inline -- not through
 * sdio_cmd: the +0x1fc arm write comes first, before the data setup --
 * its two status waits, RESP0 read (and dropped), the data, and CMD12.
 * Returns the first negative status, else 0.
 */
int sd_read_blocks(u32 lba, int n, void *dst)
{
    u32 resp;
    u32 bytes;
    int rc;

    do {
        rc = sdio_cmd(0, SD_CMD13_FLAGS, g_sd.rca, &resp);
        if (rc < 0)
            return rc;
    } while ((resp & SD_R1_STATE_MASK) != SD_R1_STATE_TRAN);

    bytes = (u32)n << 9;
    SDIO1(SDIO_ARM) = 1;
    if (g_sd.hc == 0)
        lba <<= 9;
    if (g_sd.xfer_mode == XFER_DMA)
        SDIO1(SDIO_DMA_ADDR) = (u32)dst;
    SDIO1(SDIO_DATA_CTRL) = SDIO_DATA_CTRL_VAL;
    SDIO1(SDIO_LEN) = bytes;

    REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SDIO1;
    udelay(1);
    if (g_sd.xfer_mode == XFER_DMA)
        SDIO1(SDIO_XFER_MODE) = g_sd.xfer_mode;
    SDIO1(SDIO_XFER_MODE) = (g_sd.xfer_mode == XFER_DMA) ? 3 : 1;
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SDIO1;

    SDIO1(SDIO_DATA_CFG) = SDIO_DATA_CFG_VAL;
    SDIO1(SDIO_ARG) = lba;
    SDIO1(SDIO_CMD) = (SDIO1(SDIO_CMD) & ~SDIO_CMD_IDX_MASK) | SD_CMD18_FLAGS;
    rc = sdio_wait(0, SDIO_STS_CMD_DONE);
    if (rc < 0)
        return rc;
    rc = sdio_wait(0, SDIO_STS_DATA_DONE_PIO);
    if (rc < 0)
        return rc;
    resp = SDIO1(SDIO_RESP(0));             /* CMD18 R1, never looked at */

    rc = sdio_read_data(0, dst, bytes);
    if (rc < 0)
        return rc;

    rc = sdio_cmd(0, SD_CMD12_FLAGS, 0, &resp);
    return rc < 0 ? rc : 0;
}

/* Poll until (reg & bit) clears; past 1 s print "K:" and keep polling. */
static void sd_wait_clear_1s(u32 reg, u32 bit)
{
    timer_start();
    while (REG32(reg) & bit) {
        if (timer_now() > TMO_1S)
            boot_fail(BOOT_FAIL_TIMEOUT);
    }
    timer_stop();
}

/*
 * Card identification: CMD0, CMD8, ACMD41, CMD2, CMD3, fast clock, CMD7,
 * ACMD6 (4-bit, optional), CMD16.  Returns 0, or -1 for boot_fail(2).
 *
 * g_sd.v2 does not record the CMD8 answer: it is set to 1 before CMD8
 * whatever happens and cleared only when CMD55/ACMD41 errors exhaust
 * their 5 tries, so in effect it means "the card answered ACMD41".  CMD8
 * only decides whether ACMD41 asks for HCS.  The 5 error tries and the
 * 6000 busy polls are two separate budgets, neither reset, with no delay
 * between polls; running out of busy polls fails directly.  The ACMD6
 * and CMD16 results are not checked.  (In the original this is inline in
 * sd_boot; it is a separate function here only for the goto-free return.)
 */
static int sd_card_init(void)
{
    u32 *const r = (u32 *)SRAM_HDR_STAGE;   /* response scratch */
    u32 ocr_arg, ocr;
    int tries, polls;

    if (sdio_cmd(0, SD_CMD0_FLAGS, 0, NULL) < 0)
        return -1;

    g_sd.v2 = 1;
    if (sdio_cmd(0, SD_CMD8_FLAGS, SD_CMD8_ARG, r) < 0)
        ocr_arg = SD_ACMD41_ARG;
    else
        ocr_arg = SD_ACMD41_ARG_HCS;

    tries = SD_ACMD41_TRIES;
    polls = SD_ACMD41_POLLS;
    for (;;) {
        if (sdio_cmd(0, SD_CMD55_FLAGS, 0, r) >= 0 &&
            sdio_cmd(0, SD_ACMD41_FLAGS, ocr_arg, r) >= 0) {
            ocr = r[0];
            if (ocr & SD_OCR_BUSY_DONE) {
                g_sd.hc = ((ocr & SD_OCR_CCS) && (ocr_arg & SD_OCR_CCS)) ? 1 : 0;
                break;
            }
            if (polls == 0)
                return -1;
            polls--;
            continue;
        }
        if (tries == 0) {
            g_sd.v2 = 0;
            break;
        }
        tries--;
    }
    if (g_sd.v2 == 0)
        return -1;

    if (sdio_cmd(0, SD_CMD2_FLAGS, 0, r) < 0)
        return -1;
    g_sd.rca = 0;
    if (sdio_cmd(0, SD_CMD3_FLAGS, 0, &g_sd.rca) < 0)
        return -1;

    /* transfer clock: pads, then divider 7 with the SDIO1 clock gated */
    SDIO1(SDIO_GATE) |= SDIO_GATE_ON;
    REG32(PAD_BASE + PAD_SD) = PAD_SD_XFER;
    SDIO1(SDIO_GATE) = SDIO_GATE_ON;
    REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SDIO1;
    REG32(CG_BASE + CG_SDIO1_DIV) = (REG32(CG_BASE + CG_SDIO1_DIV) & ~CG_SDIO1_DIV_MASK) |
                                    CG_SDIO1_DIV_XFER;
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SDIO1;
    SDIO1(SDIO_GATE) = 0;

    if (sdio_cmd(0, SD_CMD7_FLAGS, g_sd.rca, r) < 0)
        return -1;
    if (sdio_cmd(0, SD_CMD55_FLAGS, g_sd.rca, r) >= 0 &&
        sdio_cmd(0, SD_ACMD6_FLAGS, SD_ACMD6_4BIT, r) >= 0)
        SDIO1(SDIO_BUSWIDTH) = SDIO_BUSWIDTH_4;
    sdio_cmd(0, SD_CMD16_FLAGS, 0x200, r);
    return 0;
}

/*
 * func_060 0x439c -- SD card boot.
 *
 * 'S', SDIO1 clock on and gated, pinmux (TOP+4 |= SD, keeping only the
 * 0x110000 bits from a second read: the result is just old | 0x4000),
 * identification pads and clock (divider 0x1e9), '1', controller
 * power/reset waits (1 s -> "K:", polling goes on), status cleared, OTP
 * bit11: SDIO_OPT |= bit12 and 'F'; 1 ms; card init (any failure: K2).
 *
 * Then: sector 0 (K3); an MBR gives the partition LBA and its first
 * sector is read as the boot sector (K4), otherwise sector 0 is it;
 * fat_data_start(); up to 10 clusters are tried for a header tag (a
 * read error is K5).  If none of the ten has the tag the scan just
 * stops: the 10th candidate's sector address has already been advanced
 * by one more cluster, nothing is reported, and the boot goes on with
 * whatever the staging buffer holds -- the failure only shows up as K9
 * from the checksum.  Extra header sectors (K8), hdr_script_run (here
 * WITHOUT the bit18 clock-divider option the flash and eMMC paths have),
 * header copied to the load address, body read (K7), and
 * check_and_run_loader(1, ...) with the SDIO1 hardware checksum.
 *
 * SD selects XFER_DMA with script-word BIT 17, not bit16 like the flash
 * and eMMC paths (0x47a0 tests 0x20000), and never sets SRAM_DMA_DIRECT.
 * boot_fail never returns in practice; if it does, the boot carries on.
 */
void sd_boot(void)
{
    struct loader_hdr *hdr;
    u32 part, lba, sw, extra, nsect, total, n, hwsum, pinmux;
    u8 *load, *dst;
    int tries;

    g_sd.xfer_mode = XFER_PIO;
    UART0_PUTC_RAW(LTR_SD);
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_SDIO1;
    SDIO1(SDIO_GATE) = SDIO_GATE_ON;
    udelay(6);

    pinmux = REG32(TOP_BASE + TOP_PINMUX) | TOP_PINMUX_SD;
    REG32(TOP_BASE + TOP_PINMUX) =
        (REG32(TOP_BASE + TOP_PINMUX) & TOP_PINMUX_FLASH_KEEP) | pinmux;
    REG32(TOP_BASE + TOP_PINFUNC) = TOP_PINFUNC_SD;
    REG32(PAD_BASE + PAD_SD) = PAD_SD_IDENT;
    REG32(CG_BASE + CG_SD_CTRL) &= ~CG_SD_CTRL_CLR;
    REG32(CG_BASE + CG_SDIO1_DIV) = (REG32(CG_BASE + CG_SDIO1_DIV) & ~CG_SDIO1_DIV_MASK) |
                                    CG_SDIO1_DIV_IDENT;
    SDIO1(SDIO_GATE) = 0;
    UART0_PUTC_RAW(LTR_SD_PORT);

    SDIO1(SDIO_CMD) = SDIO_CMD_POWER;
    sd_wait_clear_1s(SDIO1_BASE + SDIO_CMD, SDIO_CMD_POWER);
    SDIO1(SDIO_RESET) = SDIO_RESET_BIT;
    sd_wait_clear_1s(SDIO1_BASE + SDIO_RESET, SDIO_RESET_BIT);
    SDIO1(SDIO_STS) = SDIO_STS_CLEAR_ALL;

    if (otp_boot_cfg() & OTP_CFG_SD_OPT) {
        SDIO1(SDIO_OPT) |= SDIO_OPT_SAMPLE;
        UART0_PUTC_RAW(LTR_SD_OPT);
    }
    udelay(0x400);

    if (sd_card_init() < 0)
        boot_fail(BOOT_FAIL_SD_INIT);

    /* MBR, then the boot sector of partition 1 (or sector 0 itself) */
    if (sd_read_blocks(0, 1, (void *)SRAM_HDR_STAGE) < 0)
        boot_fail(BOOT_FAIL_MBR);
    part = mbr_first_lba((const u8 *)SRAM_HDR_STAGE);
    if (part != 0) {
        if (sd_read_blocks(part, 1, (void *)SRAM_HDR_STAGE) < 0)
            boot_fail(BOOT_FAIL_PBR);
    }
    lba = part + fat_data_start((const u8 *)SRAM_HDR_STAGE);

    /* first sector of up to ten clusters, looking for the header tag */
    SDIO1(SDIO_FIFO_CTRL) |= SDIO_FIFO_CTRL_EN;
    tries = SD_FAT_SCAN_CLUSTERS;
    for (;;) {
        /* FIFO reset; the wait compares the whole register with 0x10
           while bit0 is set, so it does not wait (faithful) */
        SDIO1(SDIO_FIFO_CTRL) |= SDIO_FIFO_CTRL_RST;
        while (SDIO1(SDIO_FIFO_CTRL) == SDIO_FIFO_CTRL_RST)
            ;
        if (sd_read_blocks(lba, 1, (void *)SRAM_HDR_STAGE) < 0)
            boot_fail(BOOT_FAIL_FIRST_SECTOR);
        if (hdr_tag_ok(&g_hdr_stage.script_word) == 0)
            break;
        lba += fat_cluster_sects();
        if (--tries == 0)
            break;
    }

    sw = g_hdr_stage.script_word;
    extra = (sw & HDR_SW_EXTRA_MASK) >> HDR_SW_EXTRA_SHIFT;
    if (extra == 0) {
        hdr = &g_hdr_stage;
    } else {
        memcpy32((void *)SRAM_IMAGE, (void *)SRAM_HDR_STAGE, 0x200);
        if (sd_read_blocks(lba + 1, extra, (void *)SRAM_IMAGE_HDR_EXT) < 0)
            boot_fail(BOOT_FAIL_ETH_OR_HDR);
        hdr = (struct loader_hdr *)SRAM_IMAGE;
    }
    hdr_script_run(&hdr->script_word);

    nsect = extra + 1;
    load = (u8 *)hdr->load;
    total = hdr->size >> 9;                 /* whole sectors only */
    if ((u8 *)hdr != load)
        memcpy32(load, hdr, nsect << 9);

    dst = load + (nsect << 9);
    if (sw & BIT(17)) {             /* TODO(headers): no name for SD's bit17 */
        g_sd.xfer_mode = XFER_DMA;
        dst = (u8 *)((u32)dst & ~0xc0000000u);  /* bus address (inferred) */
    } else {
        g_sd.xfer_mode = XFER_PIO;
    }
    if (sd_read_blocks(lba + nsect, total - nsect, dst) < 0)
        boot_fail(BOOT_FAIL_LOADER_READ);

    n = hdr->size >> 1;                     /* halfwords */
    hwsum = (u16)(SDIO1(SDIO_HWSUM) + ((n * (n - 1)) >> 1));
    check_and_run_loader(1, hdr->flags >> 31, hwsum);
}
