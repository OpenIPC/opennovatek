/*
 * regmap-top.h -- TOP 0xF0010000: boot straps, pinmux, chip id.
 * Plus TOP2 0xF0011000 (one word).  Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_TOP_H
#define BOOTROM_NA51089_REGMAP_TOP_H

/* +0x000 r: strap / boot configuration */
#define TOP_STRAP               0x000
#define TOP_STRAP_BS_MASK       0xF             /* [3:0] boot source, BS_* */
#define TOP_STRAP_ETHMODE_SHIFT 8               /* [9:8] PHY mode: */
#define TOP_STRAP_ETHMODE_MASK  (0x3 << 8)
#define TOP_ETHMODE_INTERNAL    0               /*   'E' built-in PHY */
#define TOP_ETHMODE_RMII_OUT    1               /*   'O' RMII refclk output */
#define TOP_ETHMODE_RMII_IN     2               /*   'I' RMII refclk input */
#define TOP_STRAP_NO_WDT        BIT(12)         /* BST[12]: no watchdog for
                                                   internal storage ('W') */

/* +0x004 rmw: pinmux */
#define TOP_PINMUX              0x004
#define TOP_PINMUX_FLASH_KEEP   0x110000        /* flash: v = (v & KEEP) | FLASH */
#define TOP_PINMUX_FLASH        0x2000
#define TOP_PINMUX_SD           0x4000          /* SD: |= */
#define TOP_PINMUX_EMMC4        0x20000         /* eMMC 4-bit: |= */
#define TOP_PINMUX_EMMC8        0x80020000      /* eMMC 8-bit: |= */

/* +0x014 / +0x018 / +0x0e8: Ethernet RMII pinmux */
#define TOP_ETH_PINMUX0         0x014           /* RMII: |= 0x40000000, then |= 4 */
#define TOP_ETH_PINMUX0_RMII_A  BIT(30)
#define TOP_ETH_PINMUX0_RMII_B  BIT(2)
#define TOP_ETH_PINMUX1         0x018           /* RMII: |= 0x10000000 */
#define TOP_ETH_PINMUX1_RMII    BIT(28)
#define TOP_ETH_PINMUX1_PHY_SHIFT 13            /* internal PHY after AN: [14:13] = 2 */
#define TOP_ETH_PINMUX1_PHY_MASK  (0x3 << 13)
#define TOP_ETH_PINMUX1_PHY_VAL   2
#define TOP_ETH_PINMUX2         0x0e8           /* RMII: = 0x40 */
#define TOP_ETH_PINMUX2_RMII    0x40

/* +0x020 rmw: |= bit16 when an SPI read uses DMA (DMA/bus select, inferred) */
#define TOP_DMA_SEL             0x020
#define TOP_DMA_SEL_SPI         BIT(16)

/* +0x0a0 w: pin function vs GPIO */
#define TOP_PINFUNC             0x0a0
#define TOP_PINFUNC_FLASH       0xfffffaf0
#define TOP_PINFUNC_SD          0xfffe07ff
#define TOP_PINFUNC_EMMC4       0xfffffcf0
#define TOP_PINFUNC_EMMC8       0xfffffc00

/* +0x0d0 rmw: internal PHY: clear bits 6:5 */
#define TOP_PHY_CTRL            0x0d0
#define TOP_PHY_CTRL_CLR        (0x3 << 5)

/* +0x0f0 r: chip id (UART IS_NVT reply; dummy read in eth_boot) */
#define TOP_CHIP_ID             0x0f0

/* TOP2 0xF0011000 +0x000 rmw: bit25 cleared around the RSA run, set after
   (crypto bus/protection control, inferred) */
#define TOP2_CTRL               0x000
#define TOP2_CTRL_RSA           BIT(25)

#endif
