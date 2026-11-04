/*
 * regmap-pad.h -- PAD 0xF0030000: pad drive/pull (block identity inferred).
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_PAD_H
#define BOOTROM_NA51089_REGMAP_PAD_H

#define PAD_FLASH               0x040           /* flash: = 0 */
#define PAD_FLASH_DEFAULT       0x00010000      /* prejump */

#define PAD_SD                  0x068
#define PAD_SD_IDENT            0x01000000      /* identification */
#define PAD_SD_XFER             0x01111112      /* transfer */
#define PAD_SD_DEFAULT          0x01000001      /* prejump */

#define PAD_ETH                 0x098
#define PAD_ETH_REFCLK_OUT      BIT(10)         /* RMII refclk output ('O'): |= */
#define PAD_ETH_DEFAULT         0x00100000      /* prejump */

#endif
