/*
 * regmap-cg.h -- CG 0xF0020000: clock generator, clock enables, resets.
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_CG_H
#define BOOTROM_NA51089_REGMAP_CG_H

/* +0x000 rmw: bit3 cleared by hdr_script_run; bit6 set by eth_boot */
#define CG_CTRL0                0x000
#define CG_CTRL0_SCRIPT_CLR     BIT(3)
#define CG_CTRL0_ETH_PLL        BIT(6)          /* (inferred: ETH PLL enable) */

/* +0x004 p: bit6 polled by eth_boot (PLL lock, inferred) */
#define CG_STAT                 0x004
#define CG_STAT_ETH_PLL_LOCK    BIT(6)

/* +0x018 rmw: reset, bit26 cleared at reset */
#define CG_RST0                 0x018
#define CG_RST0_BIT26           BIT(26)

/* +0x020 rmw: SD: clear [5:4]; prejump = 0x00010000 */
#define CG_SD_CTRL              0x020
#define CG_SD_CTRL_CLR          (0x3 << 4)
#define CG_SD_CTRL_DEFAULT      0x00010000

/* +0x024 rmw: eMMC: clear [1:0]; prejump = 0x00000f00 */
#define CG_EMMC_CTRL            0x024
#define CG_EMMC_CTRL_CLR        0x3
#define CG_EMMC_CTRL_DEFAULT    0x00000f00

/* +0x03c rmw: SDIO1 clock divider [10:0] */
#define CG_SDIO1_DIV            0x03c
#define CG_SDIO1_DIV_MASK       0x7ff
#define CG_SDIO1_DIV_IDENT      0x1e9           /* identification (400 kHz, inferred) */
#define CG_SDIO1_DIV_XFER       7               /* transfer */
#define CG_SDIO1_DIV_DEFAULT    0x01f001f0      /* prejump value of the word */

/* +0x040 rmw: SPI divider [17:12], SDIO3 divider [10:0] */
#define CG_DIV40                0x040
#define CG_DIV40_SPI_SHIFT      12
#define CG_DIV40_SPI_MASK       (0x3f << 12)
#define CG_DIV40_SPI_DEFAULT    0x27            /* or header[29:24] */
#define CG_DIV40_SDIO3_MASK     0x7ff
#define CG_DIV40_SDIO3_DEFAULT  0xf             /* or header[31:24] */
#define CG_DIV40_DEFAULT        0x1303f1f0      /* prejump value of the word */

/* +0x070 rmw: bit16 set at reset; bit23 AES clock */
#define CG_CTRL70               0x070
#define CG_CTRL70_BIT16         BIT(16)
#define CG_CTRL70_AES           BIT(23)

/* +0x074 rmw: clock enables 0 (saved/restored by timer_start/timer_stop) */
#define CG_CLKEN0               0x074
#define CG_CLKEN0_SPI           BIT(0)          /* also toggled to reset its FIFO */
#define CG_CLKEN0_SDIO1         BIT(2)
#define CG_CLKEN0_UART0         BIT(10)
#define CG_CLKEN0_SDIO3         BIT(14)
#define CG_CLKEN0_WDT           BIT(17)
#define CG_CLKEN0_TIMER         BIT(18)
#define CG_CLKEN0_ETH           BIT(29)

/* +0x078 rmw: clock enables 1 */
#define CG_CLKEN1               0x078
#define CG_CLKEN1_ETH           BIT(24)
#define CG_CLKEN1_TRNG          (BIT(25) | BIT(28))
#define CG_CLKEN1_RSA           BIT(26)
#define CG_CLKEN1_HASH          BIT(27)

/* +0x084 rmw: resets 0 (pulsed) */
#define CG_RST84                0x084
#define CG_RST84_SPI            BIT(0)
#define CG_RST84_SDIO1          BIT(2)
#define CG_RST84_SDIO3          BIT(14)
#define CG_RST84_ETH            BIT(29)
#define CG_RST84_PREJUMP        0x20004005      /* SPI|SDIO1|SDIO3|ETH */

/* +0x088 rmw: resets 1 */
#define CG_RST88                0x088
#define CG_RST88_TRNG           BIT(25)

#endif
