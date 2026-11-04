/*
 * regmap-usb.h -- USB 0xFF600000, a Faraday FOTG210-style OTG controller.
 * Device registers are offsets from USB_DEV_BASE (0xFF600100), which the
 * ROM keeps in the g_usb_regs global and reloads for every access.
 * Names follow FOTG210 where the use matches.
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_USB_H
#define BOOTROM_NA51089_REGMAP_USB_H

/* offset from USB_BASE */
#define USB_GLUE_CTRL           0x400           /* 0xFF600400 (OTG/PHY, inferred) */
#define USB_GLUE_CTRL_START     BIT(21)         /* |= at usb_boot start */
#define USB_GLUE_CTRL_INIT      0x300000        /* |= after usb_init */

/* ---- device block, offsets from USB_DEV_BASE ------------------------- */
#define USB_DEV_CTRL            0x000
#define USB_DEV_CTRL_RWAKEUP    BIT(0)          /* remote wakeup */
#define USB_DEV_CTRL_GLINT_EN   BIT(2)
#define USB_DEV_CTRL_SFRST      BIT(4)          /* poll clear */
#define USB_DEV_CTRL_CHIP_EN    BIT(5)
#define USB_DEV_CTRL_HS         BIT(6)          /* high-speed status */
#define USB_DEV_CTRL_FORCE_FS   BIT(9)          /* BS 0xB */
#define USB_DEV_ADDR            0x004
#define USB_DEV_ADDR_MASK       0x7f
#define USB_DEV_ADDR_CONF       BIT(7)          /* configured */
#define USB_DEV_TST             0x008           /* bit0 set at init */
#define USB_DEV_TST_BIT0        BIT(0)
#define USB_DEV_SMT             0x010           /* SOF mask timer (inferred) */
#define USB_DEV_SMT_HS          0x44c
#define USB_DEV_SMT_FS          0x2710
#define USB_DEV_PHY             0x014           /* bit0 cleared at init, set by
                                                   usb_bot_halt (FOTG210 PHY test
                                                   mode: UNPLUG, inferred) */
#define USB_DEV_PHY_UNPLUG      BIT(0)
#define USB_DEV_CXFIFO          0x020           /* CX (EP0) FIFO control */
#define USB_CXFIFO_DONE         BIT(0)
#define USB_CXFIFO_STALL        BIT(2)
#define USB_CXFIFO_CLR          BIT(3)
#define USB_CXFIFO_EMPTY_MASK   (0x3 << 8)      /* polled == 3 */
#define USB_DEV_MIGR            0x030           /* group interrupt mask */
#define USB_DEV_MISG0           0x034           /* group 0 mask */
#define USB_DEV_MISG1           0x038           /* group 1 mask */
#define USB_MISG1_OUT0          BIT(4)          /* bulk FIFO OUT */
#define USB_MISG1_OUT1          BIT(5)          /* bulk FIFO OUT short */
#define USB_MISG1_IN0           BIT(16)         /* bulk FIFO IN */
#define USB_DEV_MISG2           0x03c           /* group 2 mask, = 0x600 */
#define USB_MISG2_VAL           0x600
#define USB_DEV_IGR             0x040           /* group interrupt status */
#define USB_IGR_G0              BIT(0)
#define USB_IGR_G1              BIT(1)
#define USB_IGR_G2              BIT(2)
#define USB_DEV_ISG0            0x044           /* group 0 status */
#define USB_ISG0_SETUP          BIT(0)
#define USB_ISG0_EP0_IN         BIT(1)
#define USB_ISG0_EP0_OUT        BIT(2)
#define USB_ISG0_STATUS         BIT(3)
#define USB_ISG0_STALL          BIT(4)
#define USB_ISG0_ABORT          BIT(5)
#define USB_DEV_ISG1            0x048           /* group 1 status: FIFO OUT/IN */
#define USB_DEV_ISG2            0x04c           /* group 2 status, W1C */
#define USB_ISG2_RESET          BIT(0)
#define USB_ISG2_SUSPEND        BIT(1)          /* also a DMA error bit in
                                                   usb_fifo_dma's mask */
#define USB_ISG2_RESUME         BIT(2)
#define USB_ISG2_DMA_DONE       BIT(7)
#define USB_ISG2_DMA_ERR        BIT(8)
#define USB_ISG2_DMA_ERR_MASK   (BIT(1) | BIT(2) | BIT(8))
#define USB_ISG2_DMA_ACK        0x180
#define USB_DEV_RXZLP           0x050           /* read and written back (W1C) */
#define USB_DEV_TXZLP           0x054
#define USB_DEV_INEP(n)         (0x060 + 4 * ((n) - 1)) /* IN EP n = 1..8 */
#define USB_DEV_OUTEP(n)        (0x080 + 4 * ((n) - 1)) /* OUT EP n = 1..8 */
#define USB_EP_MAXPKT_MASK      0x7ff
#define USB_EP_STALL            BIT(11)
#define USB_EP_RSTG             BIT(12)         /* reset toggle */
#define USB_DEV_EPMAP           0x0a0           /* byte per EP (0xa0..0xa7) */
#define USB_DEV_FIFOMAP         0x0a8           /* byte per FIFO (0xa8..0xab) */
#define USB_DEV_FIFOCFG         0x0ac           /* byte per FIFO (0xac..0xaf) */
#define USB_FIFOCFG_EN          BIT(5)
#define USB_DEV_FIBC            0x0b8           /* bulk OUT FIFO byte count */
#define USB_DEV_DMA_FIFO        0x0c0           /* DMA target FIFO */
#define USB_DMA_FIFO0           0x1             /* bulk OUT */
#define USB_DMA_FIFO2           0x4             /* bulk IN */
#define USB_DMA_CX              0x10000         /* EP0 */
#define USB_DEV_DMA_CTRL        0x0c8           /* 0xFF6001C8 */
#define USB_DMA_CTRL_START      BIT(0)
#define USB_DMA_CTRL_FROM_HOST  BIT(1)          /* OUT: read from host */
#define USB_DMA_CTRL_BIT5       BIT(5)          /* pulsed at usb_boot start */
#define USB_DMA_CTRL_LEN_SHIFT  8
#define USB_DEV_DMA_ADDR        0x0cc
#define USB_DEV_CXPORT          0x0d0           /* setup packet, read twice */

/* endpoint numbers the ROM configures */
#define USB_EP_BULK_IN          1               /* 0x81 */
#define USB_EP_BULK_OUT         2               /* 0x02 */

#endif
