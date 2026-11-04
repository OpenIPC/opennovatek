/*
 * regmap-spi.h -- SPI NOR/NAND flash controller 0xF0400000.
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_SPI_H
#define BOOTROM_NA51089_REGMAP_SPI_H

/* +0x000 rmw: configuration; [3:0] mode (spi_mode) */
#define SPI_CFG                 0x000
#define SPI_CFG_MODE_MASK       0xf
#define SPI_CFG_NOR             0x00180001
#define SPI_CFG_NAND_ODECC      0x00080305      /* | g_flash.cfg, on-die ECC */
#define SPI_CFG_NAND_RSECC      0x00080a05      /* | g_flash.cfg, RS ECC */
#define SPI_CFG_PAGE_2K         0x30            /* g_flash.cfg */
#define SPI_CFG_PAGE_4K         0x70

/* +0x008 rmw */
#define SPI_CTRL8               0x008
#define SPI_CTRL8_RESET         BIT(0)          /* set, poll clear (1 s -> K:) */
#define SPI_CTRL8_INTCLK        BIT(3)          /* cleared when OTP bit4 ('D') */

/* +0x014 rmw: bus */
#define SPI_BUS                 0x014
#define SPI_BUS_MODE_MASK       0x3             /* [1:0] spi_bus */
#define SPI_BUS_CS              BIT(4)          /* chip select (spi_cs) */
#define SPI_BUS_WIDE_MASK       (0x7 << 9)      /* [11:9] */
#define SPI_BUS_WIDE_X2         0xa00           /* x2 data (spi_wide) */
#define SPI_BUS_NAND            BIT(11)

/* +0x020 rmw: control */
#define SPI_CTRL                0x020
#define SPI_CTRL_SEQ_MASK       0xff            /* sequence type: */
#define SPI_SEQ_07              0x07
#define SPI_SEQ_0B              0x0b
#define SPI_SEQ_NOR_CMD         0x0c            /* nor_read command phase */
#define SPI_SEQ_NAND_READ       0x16            /* nand_read_page */
#define SPI_SEQ_DATA            0x18            /* nor_read data phase */
#define SPI_SEQ_1D              0x1d
#define SPI_SEQ_1E              0x1e
#define SPI_CTRL_GO             BIT(12)
#define SPI_CTRL_SETUP          BIT(13)         /* always set by spi_xfer_setup */
#define SPI_CTRL_SWRST          BIT(15)         /* soft reset, poll clear (1 s -> K:) */

#define SPI_TIMING              0x024
#define SPI_TIMING_VAL          0x551

#define SPI_SPARE_CTRL          0x028           /* bit0/bit1 around spare/ECC reads */
#define SPI_SPARE_CTRL_B0       BIT(0)
#define SPI_SPARE_CTRL_B1       BIT(1)

#define SPI_OPCODE              0x034           /* byte-packed opcode word */
#define SPI_COL                 0x038           /* NAND READ CACHE column */
#define SPI_ROW                 0x03c           /* NOR byte address / NAND row (page) */

/* +0x048 p/W1C: status */
#define SPI_STS                 0x048
#define SPI_STS_DATA_MASK       0xff            /* [7:0] byte returned by GET FEATURE
                                                   (flash_boot tests bit4 = feature
                                                   0xB0 ECC_EN; regmap.txt's "bit4
                                                   feature byte valid" is this) */
#define SPI_STS_DONE            BIT(12)
#define SPI_STS_ECCERR          BIT(13)         /* spi_wait -> 2 */
#define SPI_STS_BIT16           BIT(16)         /* spi_wait -> 4 */
#define SPI_STS_CLEAR           0x17000         /* spi_wait: |= (rmw), not = */

/* +0x054 rmw */
#define SPI_FIFO_CTRL           0x054
#define SPI_FIFO_CTRL_EN        BIT(0)
#define SPI_FIFO_CTRL_RST       BIT(4)          /* polled */

#define SPI_HWSUM               0x058           /* r: 16-bit checksum of data read */
#define SPI_DMA_ADDR            0x060           /* DMA destination */
#define SPI_DMA_CTRL            0x064
#define SPI_DMA_CTRL_EN         BIT(31)         /* 0 to stop */
#define SPI_ECC_STS             0x070           /* per-sector ECC status (nand_read_page) */
#define SPI_ECC_STS2            0x080           /* per-sector ECC, 2 bits each (flash_boot) */
#define SPI_POLL_CMD            0x0b4           /* = 0xc00f */
#define SPI_POLL_CMD_VAL        0xc00f
#define SPI_POLL_MASK           0x0bc           /* = 0x100; NOR 'p' step re-writes it */
#define SPI_POLL_MASK_VAL       0x100
#define SPI_DUMMY               0x0c0           /* = 7 for the x2 read */
#define SPI_DUMMY_X2            7
#define SPI_FIFO                0x100           /* r: data */
#define SPI_LEN                 0x104           /* transfer length */
#define SPI_FIFO_STS            0x108
#define SPI_FIFO_STS_EMPTY      BIT(8)
#define SPI_XFER_MODE           0x10c           /* = g_flash.mode | 1 */
#define SPI_SPARE0              0x200           /* spare byte 0 (on-die ECC marker) */
#define SPI_SPARE1              0x204           /* spare byte 1 (RS); spi_settle */

/* RS ECC error locations, sector i: +0x3fc+16i, +0x400+16i, +0x404+16i */
#define SPI_RS_ERR0(i)          (0x3fc + 16 * (i))
#define SPI_RS_ERR1(i)          (0x400 + 16 * (i))
#define SPI_RS_ERR2(i)          (0x404 + 16 * (i))
#define SPI_RS_ERR_BYTE_MASK    0xfff           /* [11:0] byte */
#define SPI_RS_ERR_BIT_SHIFT    12              /* [15:12] bit / byte mask */

/* SPI flash opcodes the ROM sends (byte-packed into SPI_OPCODE) */
#define SPINOR_OP_READ          0x03
#define SPINOR_OP_READ_DUAL     0x3b
#define SPINOR_OP_RDSR          0x05            /* the 'p' step */
#define SPINOR_OP_RSTEN         0x66
#define SPINOR_OP_RST           0x99
#define SPINAND_OP_RESET        0xff
#define SPINAND_OP_GET_FEATURE  0x0f
#define SPINAND_OP_SET_FEATURE  0x1f
#define SPINAND_FEATURE_CFG     0xb0            /* bit4 on-die ECC enable */
#define SPINAND_FEATURE_CFG_ECC BIT(4)
#define SPINAND_FEATURE_STATUS  0xc0
#define SPINAND_READ_SEQ        0x03c00f13      /* PAGE READ, GET FEATURE @c0, READ CACHE */
#define SPINAND_READ_SEQ_X2     0x3bc00f13      /* same, x2 READ CACHE */

#endif
