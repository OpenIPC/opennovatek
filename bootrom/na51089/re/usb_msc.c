/*
 * usb_msc.c -- NA51089 mask ROM: USB boot (BS 5 high speed, BS 0xB full
 * speed, and the fallback after Ethernet), the mass-storage bulk-only
 * transport, a minimal SCSI target with no medium, and the Novatek
 * vendor commands tunnelled through vendor SCSI CDBs.  The SCSI reply
 * templates data_29..data_31 (0x7c20..0x7c5f) live here.
 *
 * The device presents itself as a removable SCSI disk ("Novatek NTIVOT")
 * with nothing in it: TEST UNIT READY and every media command fail with
 * NOT READY / MEDIUM NOT PRESENT.  The host-side download tool talks
 * through CDBs whose first byte is 0xC0, 0xE0 or 0xF0 and whose second
 * byte is the same command code as the UART download protocol
 * (uart_boot.c), with the same 16-bit checksum over the 16-byte CDB:
 *
 *   0x81 IS_NVT      4-byte data-in: the constant 0x70210000 (the UART
 *                    path sends the chip id instead)
 *   0x01 OPEN_DEVICE stops the boot timeout, unlocks the three below
 *   0x03 ADDR_WRITE  *(CB[2..5]) = CB[6..9], then dsb
 *   0x83 ADDR_READ   8-byte data-in: address, *(u32 *)address
 *   0x02 WRLOADER    the CBW's data-out stage is DMA'd to CB[2..5]; the
 *                    length is the CBW's dCBWDataTransferLength (the
 *                    size field of the CDB is not used)
 *   0x82 WAIT_DONE   once its CSW is sent, usb_boot() leaves the poll
 *                    loop and runs the loader from the WRLOADER address
 *
 * A bad checksum (or a command before OPEN_DEVICE) fails the CSW; the
 * command itself still "succeeds" (no SCSI sense is set).
 *
 * BOT phases (g_bot_state): every CBW is handled completely inside the
 * OUT-FIFO interrupt -- the data stage runs synchronously from
 * scsi_dispatch() -- so the state only ever moves CBW -> CSW -> CBW.
 * The DATA_IN / DATA_OUT / STALL / HALT branches below are unreachable;
 * they are kept as the ROM has them.
 *
 * usb.c has the controller, EP0 and descriptors.  Register accesses go
 * through USB_REG(), which reloads g_usb_regs each time like the ROM.
 */
#include "na51089-regmap.h"
#include "bootrom.h"
#include "sram.h"

/*
 * gcc 14 puts ARM-mode switch tables in .rodata, which would sit between
 * the frame templates and the SCSI replies (and after data_31, see
 * scsi_mode_sense6).  The original's tables are inline in .text.
 */
#pragma GCC optimize("no-jump-tables")

/* TODO(headers): no barrier helper in the shared headers. */
static inline void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * TODO(headers): the third argument of usb_fifo_dma() is "data comes from
 * the host" (FIFO2 / EP2 OUT, DMA to memory), not "in"; regmap-usb.h's
 * USB_DMA_FIFO0/2 and USB_DMA_CTRL_FROM_HOST comments are inverted too
 * (see usb.c).
 */
#define FROM_HOST               1
#define TO_HOST                 0

/* the bulk endpoint registers */
#define BULK_IN_EP              USB_DEV_INEP(USB_EP_BULK_IN)    /* +0x60 */
#define BULK_OUT_EP             USB_DEV_OUTEP(USB_EP_BULK_OUT)  /* +0x84 */
#define MISG1_BULK_OUT          (USB_MISG1_OUT0 | USB_MISG1_OUT1)
#define MISG1_BULK_IN           USB_MISG1_IN0

/* CBW flags / g_xfer.dir */
#define DIR_IN                  0x80
#define DIR_OUT                 0

/* CSW status */
#define CSW_GOOD                0
#define CSW_FAILED              1
#define CSW_PHASE_ERROR         2

/* SCSI opcodes the ROM recognises */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_MODE_SENSE6        0x1a
#define SCSI_PREVENT_ALLOW      0x1e
#define SCSI_READ_CAPACITY10    0x25
#define SCSI_READ10             0x28
#define SCSI_WRITE10            0x2a
#define SCSI_VERIFY10           0x2f

/* sense key / additional sense code pairs */
#define SK_NOT_READY            2
#define SK_ILLEGAL_REQUEST      5
#define ASC_INVALID_OPCODE      0x20
#define ASC_INVALID_FIELD       0x24
#define ASC_LUN_NOT_SUPPORTED   0x25
#define ASC_MEDIUM_NOT_PRESENT  0x3a

/* Novatek vendor CDBs: CB[1] is the high byte of sram.h's NVT_CDB_* */
#define NVT_OP(cdb)             (((cdb) >> 8) & 0xff)
#define NVT_GROUP_MASK          0xdf    /* 0xC0 and 0xE0 */
#define NVT_GROUP               0xc0
#define NVT_GROUP_F0            0xf0

/* usb_bot_data_phase: other transfer types count 2112-byte units (a 2 KiB
   NAND page + 64 B spare -- inherited from a larger Novatek stack) */
#define XFER_UNIT_OTHER         2112

/* =====================================================================
 * SCSI reply templates, data_29..data_31 (0x7c20..0x7c5f), in one object
 * so they keep the ROM's order.  Copied with memcpy32(), so word-aligned.
 * ===================================================================== */
struct scsi_rom_replies {
    u8 sense[18];               /* data_29 0x7c20 fixed format, add-len 10 */
    u8 pad0[2];
    u8 inquiry[36];             /* data_30 0x7c34 */
    u8 mode_sense[8];           /* data_31 0x7c58 */
};
_Static_assert(__builtin_offsetof(struct scsi_rom_replies, inquiry) == 0x14, "inquiry");
_Static_assert(__builtin_offsetof(struct scsi_rom_replies, mode_sense) == 0x38, "mode_sense");

static const struct scsi_rom_replies scsi_replies __attribute__((aligned(4))) = {
    .sense = {
        0x70, 0, 0 /* key */, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0 /* ASC */, 0,
        0, 0, 0, 0,
    },
    .inquiry = {
        0x00,                   /* direct-access block device */
        0x80,                   /* removable */
        0x02,                   /* SCSI-2 */
        0x02,                   /* response data format 2 */
        0x1f,                   /* additional length */
        0, 0, 0,
        'N', 'o', 'v', 'a', 't', 'e', 'k', ' ',
        'N', 'T', 'I', 'V', 'O', 'T', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ',
        '1', '.', '1', '1',
    },
    .mode_sense = { 0x20, 0, 0, 0, 0, 0, 0, 0 },
};

/* little-endian word from the unaligned CDB bytes */
static inline u32 cdb_word(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

/*
 * func_063 0x4cd4 -- USB boot entry (strap BS 5 / 0xB, or g_boot_source
 * 5 from the fallback chain).  Prints 'U'; sets the glue enable bit;
 * pulses DMA_CTRL bit5 (one read, then value|bit5, 256 us, value&~bit5,
 * 64 us); enables the chip, forcing full speed for BS 0xB; usb_init();
 * the second glue bits.  Then polls the controller until the CSW of
 * WAIT_DONE has gone out, with a timeout of (OTP[20:18] + 1) * 30 s from
 * here (OPEN_DEVICE stops the timer, so the timeout only covers waiting
 * for the host).  Timeout -> boot_fail(10), "K:".
 *
 * The 0x28-byte header staging copy comes from the WRLOADER address
 * (g_usb_dl_addr), and the loader is run with no hardware checksum.
 * Before g_usb_regs is set the registers are addressed directly.
 */
void usb_boot(int strap)
{
    u32 dma_ctrl;

    UART0_PUTC_RAW(LTR_USB);
    REG32(USB_BASE + USB_GLUE_CTRL) |= USB_GLUE_CTRL_START;

    dma_ctrl = REG32(USB_DEV_BASE + USB_DEV_DMA_CTRL);
    REG32(USB_DEV_BASE + USB_DEV_DMA_CTRL) = dma_ctrl | USB_DMA_CTRL_BIT5;
    udelay(256);
    REG32(USB_DEV_BASE + USB_DEV_DMA_CTRL) = dma_ctrl & ~USB_DMA_CTRL_BIT5;
    udelay(64);

    REG32(USB_DEV_BASE + USB_DEV_CTRL) |= USB_DEV_CTRL_CHIP_EN;
    if (strap == BS_USB_FS)
        REG32(USB_DEV_BASE + USB_DEV_CTRL) |= USB_DEV_CTRL_FORCE_FS;

    usb_init();
    REG32(USB_BASE + USB_GLUE_CTRL) |= USB_GLUE_CTRL_INIT;

    g_usb_state = USB_ST_RUNNING;
    g_otp_tmo = (otp_boot_cfg() & OTP_CFG_TMO_MASK) >> OTP_CFG_TMO_SHIFT;
    g_timeout = (g_otp_tmo + 1) * TMO_30S;
    timer_start();

    for (;;) {
        usb_poll();
        if ((s32)g_usb_state > USB_ST_WAIT_DONE)
            break;
        if (timer_now() >= g_timeout) {
            timer_stop();
            boot_fail(BOOT_FAIL_TIMEOUT);       /* loops on if it returns */
        }
    }

    g_usb_state = USB_ST_RUNNING;
    memcpy32((void *)SRAM_HDR_STAGE, (const void *)g_usb_dl_addr, HDR_STAGE_COPY);
    check_and_run_loader(1, 0, 0);
}

/*
 * func_073 0x5458 -- read the 31-byte CBW from the OUT FIFO (DMA result
 * not checked).  A valid signature starts the CSW (tag, residue = host
 * length, status good) and runs the command; returns the next BOT state.
 * A bad signature is ignored (state stays CBW).
 */
int usb_bot_rx_cbw(void)
{
    usb_fifo_dma(&g_cbw, CBW_LEN, FROM_HOST);
    if (g_cbw.signature != CBW_SIGNATURE)
        return BOT_CBW;

    g_csw.residue = g_cbw.data_len;
    g_csw.tag = g_cbw.tag;
    g_csw.status = CSW_GOOD;            /* the ROM stores the whole word */
    return scsi_dispatch();
}

/*
 * func_074 0x54bc -- unmask the bulk FIFO interrupts the BOT phase in
 * g_bot_state waits for (MISG1: set = masked).  The IN interrupt is only
 * unmasked while EP1 IN is not stalled.  BOT_STALL and any unknown state
 * "unplug" the device (usb_bot_halt).
 */
void usb_bot_arm(void)
{
    switch (g_bot_state) {
    case BOT_CBW:
        USB_REG(USB_DEV_MISG1) &= ~MISG1_BULK_OUT;
        USB_REG(USB_DEV_MISG1) |= MISG1_BULK_IN;
        break;
    case BOT_DATA_IN:
        if (!(USB_REG(BULK_IN_EP) & USB_EP_STALL))
            USB_REG(USB_DEV_MISG1) &= ~MISG1_BULK_IN;
        break;
    case BOT_DATA_OUT:
        USB_REG(USB_DEV_MISG1) &= ~MISG1_BULK_OUT;
        break;
    case BOT_CSW:
        USB_REG(USB_DEV_MISG1) |= MISG1_BULK_OUT;
        if (!(USB_REG(BULK_IN_EP) & USB_EP_STALL))
            USB_REG(USB_DEV_MISG1) &= ~MISG1_BULK_IN;
        break;
    case BOT_HALT:
        USB_REG(USB_DEV_MISG1) |= MISG1_BULK_OUT;
        USB_REG(USB_DEV_MISG1) |= MISG1_BULK_IN;
        break;
    case BOT_STALL:
    default:
        usb_bot_halt();
        break;
    }
}

/*
 * func_075 0x558c -- bulk OUT FIFO2 has data.  Waiting for a CBW: exactly
 * 31 bytes is a CBW, anything else stalls bulk OUT.  (DATA_OUT is
 * unreachable, see the top of the file.)
 *
 * The original receives the FIFO byte count as an argument: usb_poll()
 * reads FIBC right before the call.  bootrom.h declares this function
 * (void), so the read is done here instead, first thing -- the register
 * access sequence is the same.
 */
void usb_bulk_out_irq(void)
{
    u32 fifo_bytes = USB_REG(USB_DEV_FIBC);

    switch (g_bot_state) {
    case BOT_CBW:
        if (fifo_bytes == CBW_LEN)
            g_bot_state = usb_bot_rx_cbw();
        else
            USB_REG(BULK_OUT_EP) |= USB_EP_STALL;
        break;
    case BOT_DATA_OUT:
        g_bot_state = usb_bot_data_out();
        break;
    }
    usb_bot_arm();
}

/*
 * func_076 0x55f0 -- bulk IN FIFO0 has room.  CSW phase: send the 13-byte
 * CSW and go back to waiting for a CBW; if WAIT_DONE was the command,
 * this is the moment usb_boot() may leave.  The unreachable DATA_IN
 * branch calls usb_bot_data_out(), not usb_bot_data_in() (sic).
 */
void usb_bulk_in_irq(void)
{
    switch (g_bot_state) {
    case BOT_DATA_IN:
        g_bot_state = usb_bot_data_out();
        break;
    case BOT_CSW:
        usb_fifo_dma(&g_csw, CSW_LEN, TO_HOST);
        g_bot_state = BOT_CBW;
        if (g_usb_state == USB_ST_WAIT_DONE)
            g_usb_state = USB_ST_LEAVE;
        break;
    }
    usb_bot_arm();
}

/*
 * func_077 0x565c -- BOT reset (first SETUP after a bus reset, or the
 * class request): wait for a CBW, CSW signature, OUT unmasked / IN
 * masked, sense cleared.
 */
void usb_bot_reset(void)
{
    g_bot_state = BOT_CBW;
    g_csw.signature = CSW_SIGNATURE;
    USB_REG(USB_DEV_MISG1) &= ~MISG1_BULK_OUT;
    USB_REG(USB_DEV_MISG1) |= MISG1_BULK_IN;
    scsi_clear_sense_wrap();
}

/*
 * func_078 0x56b4 -- CDB checksum, as in uart_boot: the eight halfwords
 * of the 16-byte CDB plus 0x1c, truncated to 16 bits; 0 means good.
 */
u16 cdb_sum(u32 w0, u32 w1, u32 w2, u32 w3)
{
    return (u16)((w3 & 0xffff) + (w3 >> 16) + CDB_SUM_BIAS +
                 (w2 & 0xffff) + (w2 >> 16) +
                 (w1 & 0xffff) + (w1 >> 16) +
                 (w0 & 0xffff) + (w0 >> 16));
}

/*
 * func_079 0x56e8 -- Novatek vendor command in a CBW (argument: the whole
 * CBW, &g_cbw).  Returns -1 if CB[0] is not 0xC0/0xE0/0xF0 or CB[1] is
 * unknown (scsi_dispatch then fails it as an invalid opcode), else 0;
 * failures are reported in the CSW status only.
 *
 * Addresses and values are 32-bit little-endian at CB[2..5] / CB[6..9],
 * i.e. the halves w0 >> 16 | w1 << 16 and w1 >> 16 | w2 << 16.
 * OPEN_DEVICE stops the timer before checking the sum, and sets up a
 * zero-length data-in (scsi_dispatch already zeroed g_xfer.len).
 */
int nvt_vendor_cdb(const u8 *cbw)
{
    const u8 *cb = cbw + __builtin_offsetof(struct bot_cbw, cb);
    u32 w0 = cdb_word(cb + 0);
    u32 w1 = cdb_word(cb + 4);
    u32 w2 = cdb_word(cb + 8);
    u32 w3 = cdb_word(cb + 12);
    u32 addr;

    if ((w0 & 0xff) != NVT_GROUP_F0 && (w0 & NVT_GROUP_MASK) != NVT_GROUP)
        return -1;

    g_csw.status = CSW_GOOD;

    switch (NVT_OP(w0)) {
    case NVT_OP(NVT_CDB_IS_NVT):
        g_xfer.type = XFER_TYPE_REPLY;
        g_xfer.dir = DIR_IN;
        g_xfer.len = 4;
        if (cdb_sum(w0, w1, w2, w3) != 0) {
            g_csw.status = CSW_FAILED;
            *(u32 *)g_usb_reply = 0;
        } else {
            *(u32 *)g_usb_reply = NVT_USB_IS_NVT_REPLY;
        }
        return 0;

    case NVT_OP(NVT_CDB_OPEN_DEVICE):
        timer_stop();
        g_xfer.type = XFER_TYPE_REPLY;
        g_xfer.dir = DIR_IN;
        g_xfer.addr = 0;
        g_xfer.step = 0;
        if (cdb_sum(w0, w1, w2, w3) != 0)
            g_csw.status = CSW_FAILED;
        else
            g_usb_opened = 1;
        return 0;

    case NVT_OP(NVT_CDB_WAIT_DONE):
        if (cdb_sum(w0, w1, w2, w3) != 0)
            g_csw.status = CSW_FAILED;
        else
            g_usb_state = USB_ST_WAIT_DONE;
        return 0;

    case NVT_OP(NVT_CDB_ADDR_WRITE):
        if (cdb_sum(w0, w1, w2, w3) != 0 || !g_usb_opened) {
            g_csw.status = CSW_FAILED;
            return 0;
        }
        *(volatile u32 *)((w1 << 16) + (w0 >> 16)) = (w2 << 16) + (w1 >> 16);
        dsb();
        return 0;

    case NVT_OP(NVT_CDB_ADDR_READ):
        if (cdb_sum(w0, w1, w2, w3) != 0 || !g_usb_opened) {
            g_csw.status = CSW_FAILED;
            return 0;
        }
        g_xfer.type = XFER_TYPE_REPLY;
        g_xfer.dir = DIR_IN;
        g_xfer.len = 8;
        addr = (w0 >> 16) + (w1 << 16);
        ((u32 *)g_usb_reply)[0] = addr;
        ((u32 *)g_usb_reply)[1] = *(volatile u32 *)addr;
        return 0;

    case NVT_OP(NVT_CDB_WRLOADER):
        if (cdb_sum(w0, w1, w2, w3) != 0 || !g_usb_opened) {
            g_csw.status = CSW_FAILED;
            return 0;
        }
        addr = (w1 << 16) + (w0 >> 16);
        g_xfer.type = XFER_TYPE_DOWNLOAD;
        g_xfer.dir = DIR_OUT;
        g_xfer.addr = addr;
        g_xfer.len = ((const struct bot_cbw *)cbw)->data_len;
        g_usb_dl_addr = addr;
        return 0;

    default:
        return -1;
    }
}

/*
 * func_099 0x63b0 -- set DEV_PHY bit0 (FOTG210 PHY test-mode UNPLUG,
 * inferred): a soft disconnect.  Only reached through usb_bot_arm() in
 * states that never occur.
 */
void usb_bot_halt(void)
{
    USB_REG(USB_DEV_PHY) |= USB_DEV_PHY_UNPLUG;
}

/*
 * func_104 0x674c -- REQUEST SENSE: 18-byte fixed-format reply with the
 * stored key and ASC, then the sense is cleared.  memcpy32() copies whole
 * words only, so just 16 of the 18 template bytes are copied: bytes
 * 16..17 (sense-key specific) are whatever g_usb_reply held before.
 */
void scsi_request_sense(void)
{
    u32 *reply = (u32 *)g_usb_reply;

    g_xfer.type = XFER_TYPE_REPLY;
    g_xfer.dir = DIR_IN;
    g_xfer.len = sizeof(scsi_replies.sense);
    memcpy32(g_usb_reply, scsi_replies.sense, sizeof(scsi_replies.sense));
    reply[0] += (g_sense_key << 16) & 0xff0000;     /* byte 2: sense key */
    reply[3] += (u8)g_sense_asc;                    /* byte 12: ASC */
    g_sense_key = 0;
    g_sense_asc = 0;
}

/* func_105 0x67c8 -- no sense. */
void scsi_clear_sense(void)
{
    g_sense_key = 0;
    g_sense_asc = 0;
}

/* func_106 0x67e8 -- a separate symbol in the ROM that only calls
   scsi_clear_sense (used by usb_bot_reset). */
void scsi_clear_sense_wrap(void)
{
    scsi_clear_sense();
}

/* func_107 0x67f8 -- INQUIRY: the 36-byte standard reply. */
void scsi_inquiry(void)
{
    g_xfer.type = XFER_TYPE_REPLY;
    g_xfer.dir = DIR_IN;
    g_xfer.len = sizeof(scsi_replies.inquiry);
    memcpy32(g_usb_reply, scsi_replies.inquiry, sizeof(scsi_replies.inquiry));
}

/*
 * func_108 0x6834 -- MODE SENSE(6) by CB[2] (page code with the PC bits):
 * 0x3f (all pages) answers the 8-byte template, page 0 answers 12 bytes,
 * anything else is ILLEGAL REQUEST / INVALID FIELD IN CDB.
 *
 * The template is only 8 bytes: page 0 copies the four bytes that follow
 * data_31 in the ROM too, the start of lzma.o's first string ("die:").
 * Kept as an over-read; what those bytes are depends on link.ld placing
 * lzma.o's .rodata right after this unit's.
 */
void scsi_mode_sense6(void)
{
    u8 page = g_cbw.cb[2];
    u32 len;

    if (page == 0x00) {
        len = 12;
    } else if (page == 0x3f) {
        len = 8;
    } else {
        g_sense_key = SK_ILLEGAL_REQUEST;
        g_sense_asc = ASC_INVALID_FIELD;
        g_csw.status = CSW_FAILED;
        return;
    }
    g_xfer.len = len;
    g_xfer.type = XFER_TYPE_REPLY;
    g_xfer.dir = DIR_IN;
    memcpy32(g_usb_reply, scsi_replies.mode_sense, g_xfer.len);
}

/* func_109 0x68c4 -- ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE. */
void scsi_invalid_opcode(void)
{
    g_sense_key = SK_ILLEGAL_REQUEST;
    g_sense_asc = ASC_INVALID_OPCODE;
    g_csw.status = CSW_FAILED;
}

/*
 * func_110 0x68f4 -- fail the command as NOT READY / MEDIUM NOT PRESENT
 * and stall the bulk endpoint of the host's data direction (flags byte
 * compared with 0 as a whole).
 */
void scsi_no_medium(void)
{
    g_csw.status = CSW_FAILED;
    if (g_cbw.flags == 0)
        USB_REG(BULK_OUT_EP) |= USB_EP_STALL;
    else
        USB_REG(BULK_IN_EP) |= USB_EP_STALL;
    g_sense_key = SK_NOT_READY;
    g_sense_asc = ASC_MEDIUM_NOT_PRESENT;
}

/*
 * func_111 0x6958 -- data-in stage: send the reply buffer.  If the host
 * asked for more, wait for both EP1 IN FIFO blocks to drain (CXFIFO
 * bits 9:8, FOTG210 FIFO0/1 empty) and stall bulk IN.  Anything but a
 * reply transfer, or a DMA error, is "no medium".  Always -> CSW.
 */
int usb_bot_data_in(void)
{
    if (g_xfer.type == XFER_TYPE_REPLY &&
        usb_fifo_dma(g_usb_reply, g_xfer.len, TO_HOST) >= 0) {
        g_csw.residue -= g_xfer.len;
        if (g_csw.residue != 0) {
            while ((USB_REG(USB_DEV_CXFIFO) & USB_CXFIFO_EMPTY_MASK) !=
                   USB_CXFIFO_EMPTY_MASK)
                ;
            USB_REG(BULK_IN_EP) |= USB_EP_STALL;
        }
    } else {
        scsi_no_medium();
    }
    return BOT_CSW;
}

/*
 * func_112 0x69e8 -- data-out stage.  WRLOADER: one DMA of the whole
 * length straight to the load address (a DMA error stalls bulk OUT and
 * is "no medium").  A reply-type transfer here *receives* into the reply
 * buffer, stalling bulk OUT if the host sent more -- only reachable from
 * the unreachable branches noted at the top.  Always -> CSW.
 */
int usb_bot_data_out(void)
{
    if (g_xfer.type == XFER_TYPE_DOWNLOAD) {
        if (usb_fifo_dma((void *)g_xfer.addr, g_xfer.len, FROM_HOST) >= 0) {
            g_csw.residue -= g_xfer.len;
            return BOT_CSW;
        }
        USB_REG(BULK_OUT_EP) |= USB_EP_STALL;
    } else if (g_xfer.type == XFER_TYPE_REPLY) {
        if (usb_fifo_dma(g_usb_reply, g_xfer.len, FROM_HOST) >= 0) {
            g_csw.residue -= g_xfer.len;
            if (g_csw.residue != 0)
                USB_REG(BULK_OUT_EP) |= USB_EP_STALL;
            return BOT_CSW;
        }
    }
    scsi_no_medium();
    return BOT_CSW;
}

/*
 * func_113 0x6ab0 -- the BOT "thirteen cases": compare what the host
 * announced (CBW length and flags) with what the command prepared
 * (g_xfer), run the data stage when they agree, otherwise stall and/or
 * report a phase error.  Returns the next BOT state (always CSW).
 *
 * The device length is g_xfer.len, except for transfer types other than
 * reply/download, where it is len * 2112.  A reply transfer first
 * advances g_xfer.addr by g_xfer.step (unused afterwards).  When the
 * lengths match but the directions do not, nothing is stalled and the
 * CSW is good.
 */
int usb_bot_data_phase(void)
{
    u32 dev_len, host_len;

    if (g_xfer.type == XFER_TYPE_DOWNLOAD) {
        dev_len = g_xfer.len;
    } else if (g_xfer.type == XFER_TYPE_REPLY) {
        g_xfer.addr += g_xfer.step;
        dev_len = g_xfer.len;
    } else {
        dev_len = g_xfer.len * XFER_UNIT_OTHER;
    }
    host_len = g_cbw.data_len;

    if (dev_len == host_len) {
        if (dev_len == 0)                       /* Hn = Dn */
            return BOT_CSW;
        if (g_cbw.flags != g_xfer.dir)          /* Hi <> Do, Ho <> Di */
            return BOT_CSW;
        if (g_cbw.flags == DIR_IN)              /* Hi = Di */
            return usb_bot_data_in();
        return usb_bot_data_out();              /* Ho = Do */
    }

    if (host_len == 0) {                        /* Hn < Di, Hn < Do */
        USB_REG(BULK_IN_EP) |= USB_EP_STALL;
        g_csw.status = CSW_PHASE_ERROR;
        return BOT_CSW;
    }

    if (g_cbw.flags == DIR_IN) {
        if (dev_len == 0) {                     /* Hi > Dn */
            USB_REG(BULK_IN_EP) |= USB_EP_STALL;
            return BOT_CSW;
        }
        if (g_xfer.dir != DIR_IN)               /* Hi <> Do */
            return BOT_CSW;
        if (dev_len < host_len)                 /* Hi > Di */
            return usb_bot_data_in();
        USB_REG(BULK_IN_EP) |= USB_EP_STALL;    /* Hi < Di */
        g_csw.status = CSW_PHASE_ERROR;
        return BOT_CSW;
    }

    if (dev_len == 0) {                         /* Ho > Dn */
        USB_REG(BULK_OUT_EP) |= USB_EP_STALL;
        return BOT_CSW;
    }
    if (g_xfer.dir != DIR_OUT)                  /* Ho <> Di */
        return BOT_CSW;
    if (dev_len < host_len)                     /* Ho > Do */
        return usb_bot_data_out();
    USB_REG(BULK_OUT_EP) |= USB_EP_STALL;       /* Ho < Do */
    g_csw.status = CSW_PHASE_ERROR;
    USB_REG(BULK_IN_EP) |= USB_EP_STALL;
    return BOT_CSW;
}

/*
 * func_114 0x6c14 -- run the command in the CBW, then the data stage.
 * Only LUN 0.  Media commands (and TEST UNIT READY) report no medium;
 * PREVENT ALLOW MEDIUM REMOVAL just clears the sense; unknown opcodes
 * are offered to the Novatek vendor handler.
 */
int scsi_dispatch(void)
{
    g_xfer.len = 0;

    if (g_cbw.lun != 0) {
        g_csw.status = CSW_FAILED;
        g_sense_key = SK_NOT_READY;
        g_sense_asc = ASC_LUN_NOT_SUPPORTED;
        return usb_bot_data_phase();
    }

    switch (g_cbw.cb[0]) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_READ_CAPACITY10:
    case SCSI_READ10:
    case SCSI_WRITE10:
    case SCSI_VERIFY10:
        g_sense_key = SK_NOT_READY;
        g_sense_asc = ASC_MEDIUM_NOT_PRESENT;
        g_csw.status = CSW_FAILED;
        break;
    case SCSI_REQUEST_SENSE:
        scsi_request_sense();
        break;
    case SCSI_INQUIRY:
        scsi_inquiry();
        break;
    case SCSI_MODE_SENSE6:
        scsi_mode_sense6();
        break;
    case SCSI_PREVENT_ALLOW:
        scsi_clear_sense();
        break;
    default:
        if (nvt_vendor_cdb((const u8 *)&g_cbw) < 0)
            scsi_invalid_opcode();
        break;
    }
    return usb_bot_data_phase();
}
