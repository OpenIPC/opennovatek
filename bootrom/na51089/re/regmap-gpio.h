/*
 * regmap-gpio.h -- GPIO 0xF0070000.  Write-only direction / set / clear
 * words, one word per bank of 32 pins (bank 7 inferred from +0x3c being
 * the 8th word of each group).  Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_GPIO_H
#define BOOTROM_NA51089_REGMAP_GPIO_H

#define GPIO_DIR(bank)          (0x020 + 4 * (bank))    /* 1 = output */
#define GPIO_SET(bank)          (0x040 + 4 * (bank))    /* write 1: high */
#define GPIO_CLR(bank)          (0x060 + 4 * (bank))    /* write 1: low */

#define GPIO_DIR0               0x020
#define GPIO_SET0               0x040
#define GPIO_CLR0               0x060
#define GPIO_DIR7               0x03c
#define GPIO_SET7               0x05c
#define GPIO_CLR7               0x07c

/* bank 0 */
#define GPIO0_NOR_RESET         BIT(9)          /* BS 0 with OTP bit9 */
#define GPIO0_EMMC_RESET        BIT(10)         /* BS 8/9 */
/* bank 7 */
#define GPIO7_ETH_PHY_RESET     BIT(6)          /* external RMII PHY ('O'/'I') */

#endif
