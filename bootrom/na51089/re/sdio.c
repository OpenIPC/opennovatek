/*
 * sdio.c -- the SDIO controller layer shared by SD (SDIO1 0xF0420000) and
 * eMMC (SDIO3 0xF0510000), which have the same register layout.  Every
 * function takes ctrl: 0 = SDIO1, anything else = SDIO3.
 *
 *   func_054 0x3e80 sdio_wait
 *   func_055 0x3ecc sdio_read_data
 *   func_058 0x40fc sdio_cmd
 *
 * The original picks the block with a conditional move on each register
 * address (two literals per register); SDIO_REG() is that selection.
 */
#include "bootrom.h"
#include "na51089-regmap.h"
#include "sram.h"

#define SDIO_REG(ctrl, off) \
    REG32(((ctrl) != 0 ? SDIO3_BASE : SDIO1_BASE) + (off))

/*
 * func_054 0x3e80 -- poll the status register until a bit of `mask`
 * comes up.  An error bit ([3:0]) wins over a done bit ([7:4]); either
 * way the whole `mask` is written back (W1C), not just the bits seen.
 * Returns -1 on error, 0 on done.  No timeout.
 */
int sdio_wait(int ctrl, u32 mask)
{
    u32 sts;

    for (;;) {
        sts = SDIO_REG(ctrl, SDIO_STS) & mask;
        if (sts & SDIO_STS_ERR_MASK) {
            SDIO_REG(ctrl, SDIO_STS) = mask;
            return -1;
        }
        if (sts & SDIO_STS_DONE_MASK) {
            SDIO_REG(ctrl, SDIO_STS) = mask;
            return 0;
        }
    }
}

/*
 * func_055 0x3ecc -- receive the data phase of a read.
 *
 * In transfer mode XFER_DMA (g_sd.xfer_mode != 0) the controller moves
 * the data itself and this only waits for the data-end status (0xaa).
 * Otherwise it drains the FIFO by PIO: while 64 bytes or more remain it
 * waits for the "16 words ready" flag (+0x108 bit9) and copies 16 words;
 * for the tail (< 64 bytes) it waits until the FIFO level [5:0] equals
 * exactly the number of words left, then copies them.  len is rounded up
 * to whole words.  Then the same data-end wait as the DMA case.
 */
int sdio_read_data(int ctrl, void *dst, int len)
{
    u32 *out = dst;
    u32 left;
    u32 lvl;
    int i;

    if (g_sd.xfer_mode != 0)
        return sdio_wait(ctrl, SDIO_STS_DATA_DONE);

    left = ((u32)len + 3) & ~3u;
    while (left != 0) {
        lvl = SDIO_REG(ctrl, SDIO_FIFO_STS);
        if (left < 0x40) {
            if ((lvl & SDIO_FIFO_STS_LVL_MASK) != left >> 2)
                continue;
            do {
                *out++ = SDIO_REG(ctrl, SDIO_FIFO);
                left -= 4;
            } while ((int)left > 0);
        } else {
            if (!(lvl & SDIO_FIFO_STS_16W))
                continue;
            for (i = 0; i < 16; i++)
                *out++ = SDIO_REG(ctrl, SDIO_FIFO);
            left -= 0x40;
        }
    }
    return sdio_wait(ctrl, SDIO_STS_DATA_DONE);
}

/*
 * func_058 0x40fc -- send one command.  cmdflags is the command index
 * ORed with SDIO_CMD_RESP_SHORT (0x40), SDIO_CMD_RESP_LONG (0x80) and
 * SDIO_CMD_BUSY (0x800), as the callers spell them.
 *
 * +0x1fc = 1, argument, then the command word: [7:0] replaced, the rest
 * of the register kept, START (0x200) added.  Wait for command done
 * (0x40); with a response, also wait on 0x15 (bits 0/2 error, bit4 done)
 * and copy RESP0..3 (long) or RESP0 (short) into resp.  Returns <0 on
 * an error status, else 0.  A long response is checked first, so 0xc2
 * (CMD2, both flags) is read as long.
 */
int sdio_cmd(int ctrl, u32 cmdflags, u32 arg, u32 *resp)
{
    int rc;

    SDIO_REG(ctrl, SDIO_ARM) = 1;
    SDIO_REG(ctrl, SDIO_ARG) = arg;
    SDIO_REG(ctrl, SDIO_CMD) = (SDIO_REG(ctrl, SDIO_CMD) & ~SDIO_CMD_IDX_MASK) |
                               cmdflags | SDIO_CMD_START;
    rc = sdio_wait(ctrl, SDIO_STS_CMD_DONE);
    if (rc < 0)
        return rc;

    if (cmdflags & SDIO_CMD_RESP_LONG) {
        rc = sdio_wait(ctrl, SDIO_STS_DATA_DONE_PIO);
        if (rc < 0)
            return rc;
        resp[0] = SDIO_REG(ctrl, SDIO_RESP(0));
        resp[1] = SDIO_REG(ctrl, SDIO_RESP(1));
        resp[2] = SDIO_REG(ctrl, SDIO_RESP(2));
        resp[3] = SDIO_REG(ctrl, SDIO_RESP(3));
        return 0;
    }
    if (!(cmdflags & SDIO_CMD_RESP_SHORT))
        return 0;

    rc = sdio_wait(ctrl, SDIO_STS_DATA_DONE_PIO);
    if (rc < 0)
        return rc;
    resp[0] = SDIO_REG(ctrl, SDIO_RESP(0));
    return 0;
}
