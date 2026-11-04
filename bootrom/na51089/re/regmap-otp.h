/*
 * regmap-otp.h -- OTP / efuse 0xF0660000, including the +0x50 boot
 * configuration word every boot path reads (otp_boot_cfg).
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_OTP_H
#define BOOTROM_NA51089_REGMAP_OTP_H

#define OTP_IDX                 0x004           /* word index 0..31 */
#define OTP_DATA                0x008
#define OTP_CTRL                0x020
#define OTP_CTRL_EN             BIT(0)
#define OTP_CTRL_READ           BIT(4)
#define OTP_STS                 0x030
#define OTP_STS_READY           BIT(0)
#define OTP_STS_VALID           BIT(1)
#define OTP_KEYSEL_A            0x040           /* efuse_key_select(a, b) */
#define OTP_KEYSEL_B            0x044

/* +0x050 r: boot configuration */
#define OTP_BOOT_CFG            0x050
#define OTP_CFG_RSA             BIT(1)          /* RSA ('R'), else AES ('A') */
#define OTP_CFG_KEYHASH         BIT(3)          /* SHA-256(N) vs efuse 0x14.. ('C') */
#define OTP_CFG_SPI_PADCLK      BIT(4)          /* SPI clock from pad: 'D', else 'I' */
#define OTP_CFG_SECURE          BIT(5)          /* secure boot ('X') */
#define OTP_CFG_NOR_NO_RDSR     BIT(6)          /* skip the NOR 'p' step */
#define OTP_CFG_ENCRYPTED       BIT(7)          /* encrypted area ('S') */
#define OTP_CFG_DELAY10MS       BIT(8)          /* wdt_arm_or_skip: udelay(10000) */
#define OTP_CFG_NOR_RESET       BIT(9)          /* NOR reset pulse on GPIO0 pin 9 */
#define OTP_CFG_SD_OPT          BIT(11)         /* SD: 'F', SDIO_OPT_SAMPLE */
#define OTP_CFG_EMMC_OPT        BIT(14)         /* eMMC: 'R', no SDIO_OPT_SAMPLE */
#define OTP_CFG_TMO_SHIFT       18              /* [20:18] t: DHCP (t+1)*6 tries, */
#define OTP_CFG_TMO_MASK        (0x7 << 18)     /*   USB/UART (t+1)*30 s */

/* efuse word indices */
#define EFUSE_AES_KEY           0x10            /* 0x10..0x13 */
#define EFUSE_KEY_HASH          0x14            /* 0x14..0x1b SHA-256(N) */
#define EFUSE_WORDS             32

#endif
