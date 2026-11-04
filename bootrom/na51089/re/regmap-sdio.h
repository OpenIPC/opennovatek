/*
 * regmap-sdio.h -- SDIO1 0xF0420000 (SD) and SDIO3 0xF0510000 (eMMC):
 * the same controller.  sdio_* take ctrl 0 = SDIO1, 1 = SDIO3.
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_SDIO_H
#define BOOTROM_NA51089_REGMAP_SDIO_H

/* +0x000 rmw: command */
#define SDIO_CMD                0x000
#define SDIO_CMD_IDX_MASK       0xff            /* [7:0] index | flags below */
#define SDIO_CMD_RESP_SHORT     0x40
#define SDIO_CMD_RESP_LONG      0x80
#define SDIO_CMD_START          0x200
#define SDIO_CMD_BUSY           0x800
#define SDIO_CMD_POWER          BIT(10)         /* reset/power, poll clear (1 s -> K:) */
#define SDIO_CMD_BOOT_REQ       BIT(13)         /* eMMC: CMD line held low */
#define SDIO_CMD_BOOT_CFG       0x14000         /* eMMC boot config */
#define SDIO_CMD_END_BOOT       0               /* emmc_boot_end */

#define SDIO_ARG                0x004
#define SDIO_RESP(i)            (0x008 + 4 * (i))       /* i = 0..3 */
#define SDIO_DATA_CFG           0x01c
#define SDIO_DATA_CFG_VAL       0x02000240      /* data timeout / block cfg (inferred) */
#define SDIO_DATA_CTRL          0x020
#define SDIO_DATA_CTRL_VAL      0x04000000

/* +0x028 p/W1C: status */
#define SDIO_STS                0x028
#define SDIO_STS_ERR_MASK       0xf             /* [3:0] errors */
#define SDIO_STS_DONE_MASK      0xf0            /* [7:4] done */
#define SDIO_STS_CMD_DONE       0x40
#define SDIO_STS_DATA_DONE      0xaa            /* sdio_read_data DMA wait mask */
#define SDIO_STS_DATA_DONE_PIO  0x15
#define SDIO_STS_BOOT_ACK       BIT(15)
#define SDIO_STS_BOOT_ERR0      BIT(16)
#define SDIO_STS_BOOT_END       BIT(17)
#define SDIO_STS_BOOT_ERR1      BIT(18)
#define SDIO_STS_CLEAR_ALL      0xffffffff

#define SDIO_GATE               0x038           /* = 0x400 then 0 */
#define SDIO_GATE_ON            0x400
#define SDIO_BUSWIDTH           0x03c
#define SDIO_BUSWIDTH_4         1
#define SDIO_BUSWIDTH_8         2
#define SDIO_BOOT_MODE          0x048           /* eMMC: = 0x00100000 */
#define SDIO_BOOT_MODE_VAL      0x00100000
#define SDIO_RESET              0x04c           /* bit0 = 1, poll clear (1 s -> K:) */
#define SDIO_RESET_BIT          BIT(0)
#define SDIO_OPT                0x050           /* bit12 unless OTP says otherwise */
#define SDIO_OPT_SAMPLE         BIT(12)         /* sampling option (inferred) */
#define SDIO_FIFO_CTRL          0x068
#define SDIO_FIFO_CTRL_EN       BIT(0)
#define SDIO_FIFO_CTRL_RST      BIT(4)          /* polled */
#define SDIO_HWSUM              0x06c           /* r: 16-bit checksum of data read */
#define SDIO_DMA_DST            0x070           /* used when 0xF0800400 bit31 */
#define SDIO_DMA_CTRL           0x074
#define SDIO_DMA_CTRL_START     BIT(31)         /* 0 = stop */
#define SDIO_FIFO               0x100
#define SDIO_LEN                0x104
#define SDIO_FIFO_STS           0x108
#define SDIO_FIFO_STS_LVL_MASK  0x3f            /* [5:0] level */
#define SDIO_FIFO_STS_16W       BIT(9)          /* 16 words ready */
#define SDIO_XFER_MODE          0x10c           /* 1, or 3 when g_sd.xfer_mode == 2 */
#define SDIO_DMA_ADDR           0x110           /* xfer_mode 2 */
#define SDIO_ARM                0x1fc           /* = 1 before every command (inferred) */

/* SD/MMC command words as passed to sdio_cmd (index | flags) */
#define SD_CMD13_FLAGS          0x4d            /* SEND_STATUS, short */
#define SD_CMD18_FLAGS          0x252           /* READ_MULTIPLE, short, data */
#define SD_CMD12_FLAGS          0x84c           /* STOP, short, busy */
#define SD_CMD8_ARG             0x1aa
#define EMMC_GO_PRE_IDLE_ARG    0xF0F0F0F0      /* CMD0 arg */

#endif
