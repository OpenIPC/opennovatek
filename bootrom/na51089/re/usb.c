/*
 * usb.c -- NA51089 mask ROM: USB device controller (Faraday FOTG210-style,
 * device block at USB_DEV_BASE = 0xFF600100), the chapter 9 control
 * endpoint, and the descriptors data_09..data_23 (0x78d8..0x7a4f).
 *
 * Bulk-only transport, SCSI and the Novatek vendor commands are in
 * usb_msc.c; usb_boot() there calls usb_init() and then spins in
 * usb_poll().  There are no interrupts: usb_poll() reads the group
 * status registers and dispatches.
 *
 * Every device register access goes through USB_REG()/USB_REG8(), which
 * reload the base from g_usb_regs (0xF07D416C) as the original does.
 *
 * Endpoint / FIFO plan (usb_cfg_eps_hs/fs): EP1 IN (0x81) on FIFO0+1,
 * EP2 OUT (0x02) on FIFO2+3, both bulk, double-buffered.
 *
 * The EP0 state lives in g_ep0 (sram.h, 0xF07D41E0).  Handlers return 0
 * when the request was accepted, -1 to stall it; usb_poll() turns
 * g_ep0.result (EP0_DONE / EP0_STALL) into CX FIFO DONE / STALL after the
 * group-0 events have been handled.
 *
 * Faithful quirks, kept on purpose and commented where they happen:
 *  - wIndex is read as the 32-bit word at setup+4, i.e. together with
 *    wLength, in GET_STATUS, CLEAR/SET_FEATURE and GET/SET_INTERFACE.
 *  - SET_FEATURE(ENDPOINT_HALT) picks the IN/OUT endpoint register the
 *    wrong way round and does not complete the control transfer.
 *  - GET_DESCRIPTOR for interface/endpoint types re-sends whatever the
 *    previous descriptor request left in g_ep0.in_ptr/in_len.
 */
#include "na51089-regmap.h"
#include "bootrom.h"
#include "sram.h"

/*
 * gcc 14 puts ARM-mode switch tables in .rodata (byte offsets), where
 * they would land in front of the descriptors at __const_start.  The
 * original's tables are inline in .text (`ldrls pc, [pc, rN, lsl #2]`).
 * Compare chains keep this unit's .rodata = data_09..data_23 only.
 */
#pragma GCC optimize("no-jump-tables")

/*
 * TODO(headers): regmap-usb.h names the DMA FIFO selector and DMA_CTRL
 * bit1 backwards.  FIFO0 (0x1) is mapped to EP1 IN and FIFO2 (0x4) to
 * EP2 OUT (EPMAP/FIFOMAP below, and ISG1 bits 4/5 = FIFO2 OUT, bit16 =
 * FIFO0 IN), and DMA_CTRL bit1 is FOTG210's DMA_TYPE "memory -> FIFO",
 * set when *sending* to the host.  Local names with the right meaning:
 */
#define DMA_FIFO_BULK_IN        USB_DMA_FIFO0           /* 0x1: EP1 IN */
#define DMA_FIFO_BULK_OUT       USB_DMA_FIFO2           /* 0x4: EP2 OUT */
#define DMA_CTRL_MEM_TO_FIFO    USB_DMA_CTRL_FROM_HOST  /* bit1: transmit */

/* TODO(headers): no barrier helper in the shared headers. */
static inline void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * The setup packet as the ROM reads it: usb_setup_irq() stores the two
 * CX port words, and several handlers test the whole second word
 * (wIndex | wLength << 16) where they mean wIndex.
 */
#define setup_w1                SRAM_VAR(u32, SRAM_EP0 + 4)

/* chapter 9 request / descriptor / feature codes the ROM handles */
enum {
    REQ_GET_STATUS = 0, REQ_CLEAR_FEATURE = 1, REQ_SET_FEATURE = 3,
    REQ_SET_ADDRESS = 5, REQ_GET_DESCRIPTOR = 6, REQ_GET_CONFIGURATION = 8,
    REQ_SET_CONFIGURATION = 9, REQ_GET_INTERFACE = 10,
    REQ_SET_INTERFACE = 11,
};
enum {
    DT_DEVICE = 1, DT_CONFIG = 2, DT_STRING = 3, DT_INTERFACE = 4,
    DT_ENDPOINT = 5, DT_QUALIFIER = 6, DT_OTHER_SPEED = 7,
};
#define FEAT_ENDPOINT_HALT      0
#define FEAT_REMOTE_WAKEUP      1
#define REQ_TYPE_STANDARD       0
#define REQ_TYPE_CLASS          1
#define BOT_GET_MAX_LUN         0xfe
#define BOT_RESET               0xff
#define RECIP_DEVICE            0
#define RECIP_INTERFACE         1
#define RECIP_ENDPOINT          2

/* FIFO / endpoint programming values (usb_cfg_eps_*) */
#define EPMAP_EP1_IN_FIFO0      0xf0    /* OUT none, IN FIFO0 */
#define EPMAP_EP2_OUT_FIFO2     0x2f    /* OUT FIFO2, IN none */
#define FIFOMAP_EP1_IN          0x11    /* dir IN, EP1 */
#define FIFOMAP_EP2_OUT         0x02    /* dir OUT, EP2 */
#define FIFOCFG_BULK_2BLK       0x26    /* enable, 2 blocks, bulk */
#define BULK_MAXPKT_HS          0x200
#define BULK_MAXPKT_FS          0x40

/* =====================================================================
 * Descriptors and strings, data_09..data_23 (0x78d8..0x7a4d).
 *
 * One object, so the layout is the ROM's whatever order gcc would emit
 * separate constants in.  link.ld places usb.o's .rodata first, at
 * __const_start (0x78d8, exported at 0x7FC4).  Every descriptor starts
 * on a word, because usb_get_descriptor() copies with memcpy32(); the
 * pad fields are the ROM's zero bytes.  The object itself is 0x176
 * bytes; the two bytes to 0x7a50 are link fill (0xFF in the ROM).
 *
 * String indices are the ROM's own 0x10-step codes.  The full- and
 * high-speed device descriptors (and qualifiers) are identical copies.
 * ===================================================================== */
#define LE16(x)                 ((x) & 0xff), (((x) >> 8) & 0xff)

#define USB_VID                 0x1233
#define USB_PID                 0x5677
#define STR_LANGID              0x00
#define STR_MANUFACTURER        0x10
#define STR_PRODUCT             0x20
#define STR_CONFIG              0x30
#define STR_INTERFACE           0x40
#define STR_OTHER_SPEED         0x50
#define STR_SERIAL              0x60

/* configuration + interface + 2 bulk endpoints, 0x20 bytes */
#define CONFIG_DESC(type, maxpkt)                                         \
    9, (type), LE16(0x20), 1, 1, STR_CONFIG, 0x80, 0x32,                  \
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, STR_INTERFACE,  /* MSC/SCSI/BOT */   \
    7, 5, 0x81, 2, LE16(maxpkt), 0,                  /* bulk IN */        \
    7, 5, 0x02, 2, LE16(maxpkt), 0                   /* bulk OUT */

#define DEVICE_DESC                                                       \
    18, DT_DEVICE, LE16(0x0200), 0, 0, 0, 64, LE16(USB_VID),              \
    LE16(USB_PID), LE16(0x0001), STR_MANUFACTURER, STR_PRODUCT,           \
    STR_SERIAL, 1

#define QUALIFIER_DESC          10, DT_QUALIFIER, LE16(0x0200), 0, 0, 0, 64, 1, 0

/* string descriptor header as the first UTF-16 unit */
#define STRING_HDR(nchars)      ((DT_STRING << 8) | (2 + 2 * (nchars)))

struct usb_rom_desc {
    u16 str_serial[11];         /* data_09 0x78d8 */
    u16 pad0;
    u16 str_other_speed[22];    /* data_10 0x78f0 */
    u16 str_interface[17];      /* data_11 0x791c */
    u16 pad1;
    u16 str_config[14];         /* data_12 0x7940 */
    u16 str_product[7];         /* data_13 0x795c */
    u16 pad2;
    u16 str_manufacturer[16];   /* data_14 0x796c */
    u16 str_langid[2];          /* data_15 0x798c */
    u8 other_speed_fs[32];      /* data_16 0x7990 sent at FS: 512 B EPs */
    u8 qualifier_fs[10];        /* data_17 0x79b0 */
    u8 pad3[2];
    u8 config_fs[32];           /* data_18 0x79bc 64 B bulk EPs */
    u8 device_fs[18];           /* data_19 0x79dc */
    u8 pad4[2];
    u8 other_speed_hs[32];      /* data_20 0x79f0 sent at HS: 64 B EPs */
    u8 qualifier_hs[10];        /* data_21 0x7a10 */
    u8 pad5[2];
    u8 config_hs[32];           /* data_22 0x7a1c 512 B bulk EPs */
    u8 device_hs[18];           /* data_23 0x7a3c */
};

#define DESC_OFF(f, addr)                                                 \
    _Static_assert(__builtin_offsetof(struct usb_rom_desc, f) ==          \
                   (addr) - 0x78d8, #f)
DESC_OFF(str_other_speed, 0x78f0);
DESC_OFF(str_interface, 0x791c);
DESC_OFF(str_config, 0x7940);
DESC_OFF(str_product, 0x795c);
DESC_OFF(str_manufacturer, 0x796c);
DESC_OFF(str_langid, 0x798c);
DESC_OFF(other_speed_fs, 0x7990);
DESC_OFF(qualifier_fs, 0x79b0);
DESC_OFF(config_fs, 0x79bc);
DESC_OFF(device_fs, 0x79dc);
DESC_OFF(other_speed_hs, 0x79f0);
DESC_OFF(qualifier_hs, 0x7a10);
DESC_OFF(config_hs, 0x7a1c);
DESC_OFF(device_hs, 0x7a3c);
_Static_assert(sizeof(struct usb_rom_desc) == 0x7a4e - 0x78d8, "usb_rom_desc size");

static const struct usb_rom_desc usb_desc __attribute__((aligned(4))) = {
    .str_serial = {
        STRING_HDR(10), '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    },
    .str_other_speed = {
        STRING_HDR(21), 'O', 'T', 'H', 'E', 'R', '_', 'S', ' ', 'C', 'O',
        'N', 'F', 'I', 'G', ' ', 'S', 'T', 'R', 'I', 'N', 'G',
    },
    .str_interface = {
        STRING_HDR(16), 'I', 'N', 'T', 'E', 'R', 'F', 'A', 'C', 'E', ' ',
        'S', 'T', 'R', 'I', 'N', 'G',
    },
    .str_config = {
        STRING_HDR(13), 'C', 'O', 'N', 'F', 'I', 'G', ' ', 'S', 'T', 'R',
        'I', 'N', 'G',
    },
    .str_product = {
        STRING_HDR(6), 'N', 'T', 'I', 'V', 'O', 'T',
    },
    .str_manufacturer = {
        STRING_HDR(15), 'N', 'o', 'v', 'a', 'T', 'e', 'k', ' ', ' ', ' ',
        ' ', 'I', 'N', 'C', '.',
    },
    .str_langid = { STRING_HDR(1), 0x0409 },
    .other_speed_fs = { CONFIG_DESC(DT_OTHER_SPEED, BULK_MAXPKT_HS) },
    .qualifier_fs = { QUALIFIER_DESC },
    .config_fs = { CONFIG_DESC(DT_CONFIG, BULK_MAXPKT_FS) },
    .device_fs = { DEVICE_DESC },
    .other_speed_hs = { CONFIG_DESC(DT_OTHER_SPEED, BULK_MAXPKT_FS) },
    .qualifier_hs = { QUALIFIER_DESC },
    .config_hs = { CONFIG_DESC(DT_CONFIG, BULK_MAXPKT_HS) },
    .device_hs = { DEVICE_DESC },
};

/* =====================================================================
 * DMA between memory and a FIFO.  usb_fifo_dma() and usb_ep0_dma() both
 * end in the same start-and-wait sequence; the original has it inlined
 * into each.
 * ===================================================================== */
static inline int usb_dma_start_wait(u32 ctrl)
{
    u32 st;
    int ret;

    USB_REG(USB_DEV_DMA_CTRL) = ctrl;
    USB_REG(USB_DEV_DMA_CTRL) |= USB_DMA_CTRL_START;

    for (;;) {
        st = USB_REG(USB_DEV_ISG2) & ~USB_REG(USB_DEV_MISG2);
        if (st & USB_ISG2_DMA_DONE) {
            ret = 0;
            break;
        }
        /* bits 1/2 (suspend/resume) and 8 (DMA error) abort the transfer */
        if (st & USB_ISG2_DMA_ERR_MASK) {
            /* clears the CX (EP0) FIFO even for a bulk transfer */
            USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_CLR;
            ret = -1;
            break;
        }
    }
    USB_REG(USB_DEV_ISG2) |= USB_ISG2_DMA_ACK;
    return ret;
}

/*
 * func_072 0x53c0 -- DMA len bytes between buf and a bulk FIFO.
 * from_host != 0: EP2 OUT FIFO2 -> buf; 0: buf -> EP1 IN FIFO0.  (The
 * bootrom.h prototype calls the flag "in"; it is the receive flag.)
 * Returns 0, or -1 on a DMA error / suspend / resume.
 */
int usb_fifo_dma(void *buf, u32 len, int from_host)
{
    u32 ctrl = len << USB_DMA_CTRL_LEN_SHIFT;

    USB_REG(USB_DEV_DMA_ADDR) = (u32)buf;
    dsb();
    if (from_host) {
        USB_REG(USB_DEV_DMA_FIFO) = DMA_FIFO_BULK_OUT;
    } else {
        ctrl |= DMA_CTRL_MEM_TO_FIFO;
        USB_REG(USB_DEV_DMA_FIFO) = DMA_FIFO_BULK_IN;
    }
    return usb_dma_start_wait(ctrl);
}

/*
 * func_080 0x5964 -- reset and enable the device controller: soft reset
 * (wait for SFRST to self-clear), acknowledge reset/suspend/resume, mask
 * every group-1 (FIFO) interrupt and the group-2 bits 9/10, set TST
 * bit0, enable chip + global interrupt, clear the PHY UNPLUG bit.
 */
void usb_dev_hw_init(void)
{
    USB_REG(USB_DEV_CTRL) |= USB_DEV_CTRL_SFRST;
    while (USB_REG(USB_DEV_CTRL) & USB_DEV_CTRL_SFRST)
        ;
    USB_REG(USB_DEV_ISG2) |= USB_ISG2_RESET | USB_ISG2_SUSPEND | USB_ISG2_RESUME;
    USB_REG(USB_DEV_MISG2) = USB_MISG2_VAL;
    USB_REG(USB_DEV_MISG1) = 0xffffffff;
    USB_REG(USB_DEV_TST) |= USB_DEV_TST_BIT0;
    USB_REG(USB_DEV_CTRL) |= USB_DEV_CTRL_CHIP_EN | USB_DEV_CTRL_GLINT_EN;
    USB_REG(USB_DEV_PHY) &= ~USB_DEV_PHY_UNPLUG;
}

/*
 * func_081 0x59dc -- set g_usb_regs, clear the EP0 context (not setup,
 * in_ptr or high_speed), bring the controller up, mark init done.
 */
void usb_init(void)
{
    g_usb_regs = USB_DEV_BASE;
    g_ep0.result = EP0_NONE;
    g_ep0.seen_setup = 0;
    g_ep0.in_len = 0;
    g_ep0.in_pending = 0;
    g_ep0.config = 0;
    g_ep0.iface = 0;
    g_ep0.alt = 0;
    g_ep0.ep0_halt = 0;
    usb_dev_hw_init();
    g_ep0.init_done = 1;
}

/*
 * func_082 0x5a34 -- program the endpoint/FIFO maps for configuration 1,
 * interface 0, alt 0 at high speed (512-byte bulk packets).  Any other
 * configuration state: leave the controller alone.
 */
void usb_cfg_eps_hs(void)
{
    if (g_ep0.config != 1 || g_ep0.iface != 0 || g_ep0.alt != 0)
        return;

    USB_REG8(USB_DEV_EPMAP + 0) = EPMAP_EP1_IN_FIFO0;
    USB_REG8(USB_DEV_FIFOMAP + 0) = FIFOMAP_EP1_IN;
    USB_REG8(USB_DEV_FIFOCFG + 0) = FIFOCFG_BULK_2BLK;
    USB_REG8(USB_DEV_FIFOMAP + 1) = FIFOMAP_EP1_IN;
    USB_REG8(USB_DEV_FIFOCFG + 1) = FIFOCFG_BULK_2BLK;
    USB_REG(USB_DEV_INEP(USB_EP_BULK_IN)) = BULK_MAXPKT_HS;
    USB_REG8(USB_DEV_EPMAP + 1) = EPMAP_EP2_OUT_FIFO2;
    USB_REG8(USB_DEV_FIFOMAP + 2) = FIFOMAP_EP2_OUT;
    USB_REG8(USB_DEV_FIFOCFG + 2) = FIFOCFG_BULK_2BLK;
    USB_REG8(USB_DEV_FIFOMAP + 3) = FIFOMAP_EP2_OUT;
    USB_REG8(USB_DEV_FIFOCFG + 3) = FIFOCFG_BULK_2BLK;
    USB_REG(USB_DEV_OUTEP(USB_EP_BULK_OUT)) = BULK_MAXPKT_HS;
}

/*
 * func_083 0x5ac0 -- as usb_cfg_eps_hs with 64-byte packets.  The
 * original writes FIFOMAP[2], FIFOCFG[2] and FIFOCFG[3] a second time
 * (the HS variant does not); kept.
 */
void usb_cfg_eps_fs(void)
{
    if (g_ep0.config != 1 || g_ep0.iface != 0 || g_ep0.alt != 0)
        return;

    USB_REG8(USB_DEV_EPMAP + 0) = EPMAP_EP1_IN_FIFO0;
    USB_REG8(USB_DEV_FIFOMAP + 0) = FIFOMAP_EP1_IN;
    USB_REG8(USB_DEV_FIFOCFG + 0) = FIFOCFG_BULK_2BLK;
    USB_REG8(USB_DEV_FIFOMAP + 1) = FIFOMAP_EP1_IN;
    USB_REG8(USB_DEV_FIFOCFG + 1) = FIFOCFG_BULK_2BLK;
    USB_REG(USB_DEV_INEP(USB_EP_BULK_IN)) = BULK_MAXPKT_FS;
    USB_REG8(USB_DEV_EPMAP + 1) = EPMAP_EP2_OUT_FIFO2;
    USB_REG8(USB_DEV_FIFOMAP + 2) = FIFOMAP_EP2_OUT;
    USB_REG8(USB_DEV_FIFOCFG + 2) = FIFOCFG_BULK_2BLK;
    USB_REG8(USB_DEV_FIFOMAP + 3) = FIFOMAP_EP2_OUT;
    USB_REG8(USB_DEV_FIFOCFG + 3) = FIFOCFG_BULK_2BLK;
    USB_REG8(USB_DEV_FIFOMAP + 2) = FIFOMAP_EP2_OUT;      /* again */
    USB_REG8(USB_DEV_FIFOCFG + 2) = FIFOCFG_BULK_2BLK;    /* again */
    USB_REG8(USB_DEV_FIFOCFG + 3) = FIFOCFG_BULK_2BLK;    /* again */
    USB_REG(USB_DEV_OUTEP(USB_EP_BULK_OUT)) = BULK_MAXPKT_FS;
}

/* func_084 0x5b58 -- status stage seen: drop any pending IN, CX DONE. */
void usb_ep0_status(void)
{
    g_ep0.in_pending = 0;
    USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_DONE;
}

/*
 * func_085 0x5b8c -- as usb_fifo_dma, through the CX (EP0) FIFO.
 * from_host == 0 transmits; every caller passes 0.
 */
int usb_ep0_dma(void *buf, u32 len, int from_host)
{
    u32 ctrl = len << USB_DMA_CTRL_LEN_SHIFT;

    USB_REG(USB_DEV_DMA_ADDR) = (u32)buf;
    dsb();
    if (!from_host)
        ctrl |= DMA_CTRL_MEM_TO_FIFO;
    USB_REG(USB_DEV_DMA_FIFO) = USB_DMA_CX;
    return usb_dma_start_wait(ctrl);
}

/*
 * func_086 0x5c1c -- send the pending control-IN data.  The whole rest
 * goes in one DMA (no max-packet split), so in_len always reaches 0 and
 * the request completes: result = DONE, nothing pending.
 */
void usb_ep0_tx_next(void)
{
    u32 len = g_ep0.in_len;

    if (usb_ep0_dma((void *)g_ep0.in_ptr, len, 0) < 0)
        return;
    g_ep0.in_len -= len;
    g_ep0.in_ptr += len;
    if (g_ep0.in_len == 0) {
        g_ep0.result = EP0_DONE;
        g_ep0.in_pending = 0;
    }
}

/*
 * func_087 0x5c74 -- pulse the reset-toggle bit of all eight IN and then
 * all eight OUT endpoint registers (one read, write value|RSTG, write
 * the value back).
 */
void usb_reset_toggles(void)
{
    u32 ep, v;

    for (ep = 1; ep <= 8; ep++) {
        v = USB_REG(USB_DEV_INEP(ep));
        USB_REG(USB_DEV_INEP(ep)) = v | USB_EP_RSTG;
        USB_REG(USB_DEV_INEP(ep)) = v;
    }
    for (ep = 1; ep <= 8; ep++) {
        v = USB_REG(USB_DEV_OUTEP(ep));
        USB_REG(USB_DEV_OUTEP(ep)) = v | USB_EP_RSTG;
        USB_REG(USB_DEV_OUTEP(ep)) = v;
    }
}

/*
 * func_088 0x5cd8 -- GET_STATUS.  Device: bit1 = remote wakeup (DEV_CTRL
 * bit0).  Interface: 0.  Endpoint: the stall bit of the endpoint's
 * register, after checking that it maps to an enabled FIFO.
 *
 * Quirk: "endpoint 0" is recognised by the whole word wIndex | wLength
 * << 16 being 0, which a GET_STATUS (wLength 2) never has; EP0 then goes
 * down the generic path with ep = 0, i.e. EPMAP[-1] (+0x9f) and the
 * register just below IN/OUT EP1.  ep_index is not checked for 0.
 */
int usb_get_status(void)
{
    u32 status;
    u32 idx, ep, dir_in, map, fifo;

    switch (g_ep0.setup.bmRequestType & 0xf) {
    case RECIP_DEVICE:
        status = (USB_REG(USB_DEV_CTRL) << 1) & 2;
        break;
    case RECIP_INTERFACE:
        status = 0;
        break;
    case RECIP_ENDPOINT:
        idx = setup_w1;
        if (idx == 0) {
            status = g_ep0.ep0_halt;
            break;
        }
        ep = idx & 0x7f;
        dir_in = idx & 0x80;
        if (ep > 8)
            return -1;
        map = USB_REG8(USB_DEV_EPMAP + ep - 1);
        fifo = dir_in ? (map & 0xf) : (map >> 4);
        if (fifo > 3)
            return -1;
        if (!(USB_REG8(USB_DEV_FIFOCFG + fifo) & USB_FIFOCFG_EN))
            return -1;
        status = (USB_REG(dir_in ? USB_DEV_INEP(ep) : USB_DEV_OUTEP(ep)) >> 11) & 1;
        break;
    default:
        return -1;
    }

    usb_ep0_dma(&status, 2, 0);         /* result not checked */
    g_ep0.result = EP0_DONE;
    return 0;
}

/*
 * func_089 0x5dd0 -- CLEAR_FEATURE.  The recipient is not checked:
 * selector 0 is ENDPOINT_HALT, 1 DEVICE_REMOTE_WAKEUP.
 * ENDPOINT_HALT on "endpoint 0" (whole wIndex|wLength word 0) clears
 * ep0_halt; otherwise pulse the toggle reset, clear the stall bit and
 * re-arm the bulk interrupts for the current BOT phase.
 */
int usb_clear_feature(void)
{
    u32 idx, ep, dir_in, map, fifo, reg;

    switch (g_ep0.setup.wValue) {
    case FEAT_ENDPOINT_HALT:
        idx = setup_w1;
        if (idx == 0) {
            g_ep0.ep0_halt = 0;
            break;
        }
        ep = idx & 0x7f;
        if (ep > 8)
            return -1;
        dir_in = idx & 0x80;
        map = USB_REG8(USB_DEV_EPMAP + ep - 1);
        fifo = dir_in ? (map & 0xf) : (map >> 4);
        if (fifo > 3)
            return -1;
        if (!(USB_REG8(USB_DEV_FIFOCFG + fifo) & USB_FIFOCFG_EN))
            return -1;
        reg = dir_in ? USB_DEV_INEP(ep) : USB_DEV_OUTEP(ep);
        USB_REG(reg) |= USB_EP_RSTG;
        USB_REG(reg) &= ~USB_EP_RSTG;
        USB_REG(reg) &= ~USB_EP_STALL;
        usb_bot_arm();
        break;
    case FEAT_REMOTE_WAKEUP:
        USB_REG(USB_DEV_CTRL) &= ~USB_DEV_CTRL_RWAKEUP;
        break;
    default:
        return -1;
    }

    g_ep0.result = EP0_DONE;
    return 0;
}

/*
 * func_090 0x5eb8 -- SET_FEATURE.  Recipient not checked.
 *
 * ROM bugs kept: for ENDPOINT_HALT neither the EP0 nor the generic path
 * sets result = DONE, so the request is never completed on the bus;
 * the FIFO bound is 4 (CLEAR_FEATURE/GET_STATUS use 3); the direction is
 * (u8)(wIndex >> 7), which also takes wIndex bits 8..14, and it selects
 * the register the wrong way round: an IN endpoint (dir != 0) gets its
 * stall bit set in the OUT register at +0x20, an OUT endpoint in the IN
 * register.
 */
int usb_set_feature(void)
{
    u32 idx, ep, dir, map, fifo;

    switch (g_ep0.setup.wValue) {
    case FEAT_ENDPOINT_HALT:
        idx = setup_w1;
        if (idx == 0) {
            g_ep0.ep0_halt = 1;
            return 0;                           /* no EP0_DONE */
        }
        ep = idx & 0x7f;
        if (ep > 8)
            return -1;
        dir = (u8)(idx >> 7);
        map = USB_REG8(USB_DEV_EPMAP + ep - 1);
        fifo = dir ? (map & 0xf) : (map >> 4);
        if (fifo > 4)
            return -1;
        if (!(USB_REG8(USB_DEV_FIFOCFG + fifo) & USB_FIFOCFG_EN))
            return -1;
        /* INEP(ep) + dir * 0x20: OUT register for IN endpoints */
        USB_REG(USB_DEV_INEP(ep) + dir * 0x20) |= USB_EP_STALL;
        return 0;                               /* no EP0_DONE */
    case FEAT_REMOTE_WAKEUP:
        USB_REG(USB_DEV_CTRL) |= USB_DEV_CTRL_RWAKEUP;
        g_ep0.result = EP0_DONE;
        return 0;
    default:
        return -1;
    }
}

/*
 * func_091 0x5f84 -- SET_ADDRESS: a plain store of wValue to DEV_ADDR
 * (which also clears the CONF bit), if it fits in 8 bits.
 */
int usb_set_address(void)
{
    u32 addr = g_ep0.setup.wValue;

    if (addr > 0xff)
        return -1;
    USB_REG(USB_DEV_ADDR) = addr;
    g_ep0.result = EP0_DONE;
    return 0;
}

/*
 * func_092 0x5fc0 -- GET_DESCRIPTOR.  Picks the full- or high-speed copy
 * by g_ep0.high_speed (latched at the first SETUP), clips to wLength,
 * copies it word-wise (length + 3, so whole words) to the bounce buffer
 * at 0xF0800420 and starts the IN data stage.
 *
 * Types 4 (interface) and 5 (endpoint) do not set in_ptr/in_len at all:
 * they re-send whatever the previous GET_DESCRIPTOR left there (after
 * that one's bounce, so in_ptr is already the bounce buffer).
 */
int usb_get_descriptor(void)
{
    u32 hs;

    switch (g_ep0.setup.wValue >> 8) {
    case DT_DEVICE:
        hs = g_ep0.high_speed;
        g_ep0.in_ptr = (u32)(hs ? usb_desc.device_hs : usb_desc.device_fs);
        g_ep0.in_len = sizeof(usb_desc.device_fs);
        break;
    case DT_CONFIG:
        if ((g_ep0.setup.wValue & 0xff) != 0)
            return -1;
        hs = g_ep0.high_speed;
        g_ep0.in_ptr = (u32)(hs ? usb_desc.config_hs : usb_desc.config_fs);
        g_ep0.in_len = sizeof(usb_desc.config_fs);
        break;
    case DT_STRING:
        switch (g_ep0.setup.wValue & 0xff) {
        case STR_LANGID:
            g_ep0.in_ptr = (u32)usb_desc.str_langid;
            g_ep0.in_len = sizeof(usb_desc.str_langid);
            break;
        case STR_MANUFACTURER:
            g_ep0.in_ptr = (u32)usb_desc.str_manufacturer;
            g_ep0.in_len = sizeof(usb_desc.str_manufacturer);
            break;
        case STR_PRODUCT:
            g_ep0.in_ptr = (u32)usb_desc.str_product;
            g_ep0.in_len = sizeof(usb_desc.str_product);
            break;
        case STR_CONFIG:
            g_ep0.in_ptr = (u32)usb_desc.str_config;
            g_ep0.in_len = sizeof(usb_desc.str_config);
            break;
        case STR_INTERFACE:
            g_ep0.in_ptr = (u32)usb_desc.str_interface;
            g_ep0.in_len = sizeof(usb_desc.str_interface);
            break;
        case STR_OTHER_SPEED:
            g_ep0.in_ptr = (u32)usb_desc.str_other_speed;
            g_ep0.in_len = sizeof(usb_desc.str_other_speed);
            break;
        case STR_SERIAL:
            g_ep0.in_ptr = (u32)usb_desc.str_serial;
            g_ep0.in_len = sizeof(usb_desc.str_serial);
            break;
        default:
            return -1;
        }
        break;
    case DT_INTERFACE:
    case DT_ENDPOINT:
        break;                          /* stale in_ptr/in_len, see above */
    case DT_QUALIFIER:
        hs = g_ep0.high_speed;
        g_ep0.in_ptr = (u32)(hs ? usb_desc.qualifier_hs : usb_desc.qualifier_fs);
        g_ep0.in_len = sizeof(usb_desc.qualifier_fs);
        break;
    case DT_OTHER_SPEED:
        hs = g_ep0.high_speed;
        g_ep0.in_ptr = (u32)(hs ? usb_desc.other_speed_hs : usb_desc.other_speed_fs);
        g_ep0.in_len = sizeof(usb_desc.other_speed_fs);
        break;
    default:
        return -1;
    }

    if (g_ep0.in_len > g_ep0.setup.wLength)
        g_ep0.in_len = g_ep0.setup.wLength;
    memcpy32((void *)SRAM_USB_DESC_BUF, (const void *)g_ep0.in_ptr,
             g_ep0.in_len + 3);
    g_ep0.in_ptr = SRAM_USB_DESC_BUF;
    g_ep0.in_pending = 1;
    usb_ep0_tx_next();
    return 0;
}

/*
 * func_093 0x61b0 -- GET_CONFIGURATION: one byte of g_ep0.config.
 * Returns the DMA result; usb_std_request() ignores it.
 */
int usb_get_configuration(void)
{
    int ret = usb_ep0_dma(&g_ep0.config, 1, 0);

    g_ep0.result = EP0_DONE;
    return ret;
}

/*
 * func_094 0x61dc -- SET_CONFIGURATION.  wValue is stored in
 * g_ep0.config before it is range-checked.  0: clear CONF.  1: program
 * the endpoints for the speed latched at the first SETUP, set the SOF
 * mask timer, set CONF, reset all data toggles.
 */
int usb_set_configuration(void)
{
    u32 value = g_ep0.setup.wValue;

    g_ep0.config = value;
    if (value > 1)
        return -1;

    if (value == 0) {
        USB_REG(USB_DEV_ADDR) &= ~USB_DEV_ADDR_CONF;
    } else {
        if (g_ep0.high_speed) {
            usb_cfg_eps_hs();
            USB_REG(USB_DEV_SMT) = USB_DEV_SMT_HS;
        } else {
            usb_cfg_eps_fs();
            USB_REG(USB_DEV_SMT) = USB_DEV_SMT_FS;
        }
        USB_REG(USB_DEV_ADDR) |= USB_DEV_ADDR_CONF;
        usb_reset_toggles();
    }
    g_ep0.result = EP0_DONE;
    return 0;
}

/*
 * func_095 0x627c -- GET_INTERFACE: one byte of g_ep0.alt, when
 * configured and wIndex <= 1.  The test is on the whole wIndex|wLength
 * word, and GET_INTERFACE always has wLength 1, so a conforming request
 * is always stalled.
 */
int usb_get_interface(void)
{
    if (!(USB_REG(USB_DEV_ADDR) & USB_DEV_ADDR_CONF))
        return -1;
    if (setup_w1 > 1)
        return -1;
    usb_ep0_dma(&g_ep0.alt, 1, 0);
    g_ep0.result = EP0_DONE;
    return 0;
}

/*
 * func_096 0x62e0 -- SET_INTERFACE: configured, interface 0 (whole
 * wIndex|wLength word 0, which wLength 0 allows) and alternate 0 only.
 */
int usb_set_interface(void)
{
    if (!(USB_REG(USB_DEV_ADDR) & USB_DEV_ADDR_CONF))
        return -1;
    if (setup_w1 != 0 || g_ep0.setup.wValue != 0)
        return -1;
    g_ep0.iface = 0;
    g_ep0.alt = 0;
    usb_reset_toggles();
    g_ep0.result = EP0_DONE;
    return 0;
}

/* func_097 0x634c -- CX IN: continue a pending control read, else stall. */
void usb_ep0_in_irq(void)
{
    if (g_ep0.in_pending == 1)
        usb_ep0_tx_next();
    else
        USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_STALL;
}

/* func_098 0x638c -- CX OUT: the ROM takes no control-OUT data: stall. */
void usb_ep0_out_irq(void)
{
    USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_STALL;
}

/*
 * func_100 0x63d4 -- standard requests by bRequest.  While EP0 is halted
 * (SET_FEATURE(HALT) on endpoint 0) only GET_STATUS and CLEAR/SET_FEATURE
 * are served; GET_CONFIGURATION is then skipped but still returns 0.
 */
int usb_std_request(void)
{
    switch (g_ep0.setup.bRequest) {
    case REQ_GET_STATUS:
        return usb_get_status();
    case REQ_CLEAR_FEATURE:
        return usb_clear_feature();
    case REQ_SET_FEATURE:
        return usb_set_feature();
    case REQ_SET_ADDRESS:
        if (g_ep0.ep0_halt)
            return -1;
        return usb_set_address();
    case REQ_GET_DESCRIPTOR:
        if (g_ep0.ep0_halt)
            return -1;
        return usb_get_descriptor();
    case REQ_GET_CONFIGURATION:
        if (!g_ep0.ep0_halt)
            usb_get_configuration();
        return 0;
    case REQ_SET_CONFIGURATION:
        if (g_ep0.ep0_halt)
            return -1;
        return usb_set_configuration();
    case REQ_GET_INTERFACE:
        if (g_ep0.ep0_halt)
            return -1;
        return usb_get_interface();
    case REQ_SET_INTERFACE:
        if (g_ep0.ep0_halt)
            return -1;
        return usb_set_interface();
    default:
        return -1;
    }
}

/*
 * func_101 0x64c0 -- mass-storage class requests: GET_MAX_LUN answers
 * one zero byte, Bulk-Only Mass Storage Reset resets the BOT state.
 * wIndex/wValue/wLength are not checked.
 */
int usb_class_request(void)
{
    u32 max_lun;

    switch (g_ep0.setup.bRequest) {
    case BOT_GET_MAX_LUN:
        max_lun = 0;
        usb_ep0_dma(&max_lun, 1, 0);
        break;
    case BOT_RESET:
        usb_bot_reset();
        break;
    default:
        return -1;
    }
    g_ep0.result = EP0_DONE;
    return 0;
}

/*
 * func_102 0x6524 -- SETUP received.  The first SETUP after a bus reset
 * latches the connection speed (DEV_CTRL bit6) and resets the BOT state.
 * The 8-byte packet is two reads of the CX port.  Standard and class
 * requests go to their handlers; vendor/reserved types and handler
 * failures stall.
 */
void usb_setup_irq(void)
{
    u32 w0, w1;

    if (!g_ep0.seen_setup) {
        g_ep0.seen_setup = 1;
        g_ep0.high_speed = (USB_REG(USB_DEV_CTRL) & USB_DEV_CTRL_HS) ? 1 : 0;
        usb_bot_reset();
    }

    w0 = USB_REG(USB_DEV_CXPORT);
    w1 = USB_REG(USB_DEV_CXPORT);
    SRAM_VAR(u32, SRAM_EP0 + 0) = w0;
    setup_w1 = w1;

    switch ((w0 >> 5) & 3) {            /* bmRequestType type field */
    case REQ_TYPE_STANDARD:
        if (usb_std_request() >= 0)
            return;
        break;
    case REQ_TYPE_CLASS:
        if (usb_class_request() >= 0)
            return;
        break;
    default:
        break;
    }
    g_ep0.result = EP0_STALL;
}

/*
 * func_103 0x65b8 -- one pass over the pending interrupt groups, in the
 * order group 2 (bus events), group 0 (control endpoint), group 1 (bulk
 * FIFOs).
 *
 * Group 2: acknowledge reset / suspend / resume and the two zero-length
 * packet bits (whose detail registers are read and written back); a bus
 * reset re-arms the first-SETUP handling.
 * Group 0: SETUP and status-stage-end are exclusive (status is ignored
 * when a SETUP is pending); then CX IN, CX OUT, command failure (stall)
 * and abort (acknowledged).  Then the EP0 verdict of the handlers is
 * applied and cleared -- even when no group-0 bit was set.
 * Group 1: bulk OUT FIFO2 (packet or short packet), bulk IN FIFO0.
 */
void usb_poll(void)
{
    u32 grp, st;

    grp = USB_REG(USB_DEV_IGR) & ~USB_REG(USB_DEV_MIGR);
    if (grp == 0)
        return;

    if (grp & USB_IGR_G2) {
        st = USB_REG(USB_DEV_ISG2) & ~USB_REG(USB_DEV_MISG2);
        if (st != 0) {
            if (st & USB_ISG2_RESET) {
                USB_REG(USB_DEV_ISG2) |= USB_ISG2_RESET;
                g_ep0.seen_setup = 0;
            }
            if (st & USB_ISG2_SUSPEND)
                USB_REG(USB_DEV_ISG2) |= USB_ISG2_SUSPEND;
            if (st & USB_ISG2_RESUME)
                USB_REG(USB_DEV_ISG2) |= USB_ISG2_RESUME;
            if (st & BIT(5)) {          /* TX zero-length packet */
                USB_REG(USB_DEV_TXZLP) = USB_REG(USB_DEV_TXZLP);
                USB_REG(USB_DEV_ISG2) |= BIT(5);
            }
            if (st & BIT(6)) {          /* RX zero-length packet */
                USB_REG(USB_DEV_RXZLP) = USB_REG(USB_DEV_RXZLP);
                USB_REG(USB_DEV_ISG2) |= BIT(6);
            }
        }
    }

    if (grp & USB_IGR_G0) {
        st = USB_REG(USB_DEV_ISG0) & ~USB_REG(USB_DEV_MISG0);
        if (st != 0) {
            if (st & USB_ISG0_SETUP)
                usb_setup_irq();
            else if (st & USB_ISG0_STATUS)
                usb_ep0_status();
            if (st & USB_ISG0_EP0_IN)
                usb_ep0_in_irq();
            if (st & USB_ISG0_EP0_OUT)
                usb_ep0_out_irq();
            if (st & USB_ISG0_STALL)    /* command failure */
                USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_STALL;
            if (st & USB_ISG0_ABORT)
                USB_REG(USB_DEV_ISG0) |= USB_ISG0_ABORT;
        }
        if (g_ep0.result == EP0_STALL)
            USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_STALL;
        else if (g_ep0.result == EP0_DONE)
            USB_REG(USB_DEV_CXFIFO) |= USB_CXFIFO_DONE;
        g_ep0.result = EP0_NONE;
    }

    if (grp & USB_IGR_G1) {
        st = USB_REG(USB_DEV_ISG1) & ~USB_REG(USB_DEV_MISG1);
        if (st & (USB_MISG1_OUT0 | USB_MISG1_OUT1))
            usb_bulk_out_irq();         /* reads FIBC itself, see usb_msc.c */
        if (st & USB_MISG1_IN0)
            usb_bulk_in_irq();
    }
}
