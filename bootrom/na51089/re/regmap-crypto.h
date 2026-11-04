/*
 * regmap-crypto.h -- AES 0xF0620000, HASH 0xF0670000, TRNG 0xF0680000,
 * RSA 0xF06A0000 (secure boot; TRNG also feeds the Ethernet MAC/xid).
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_CRYPTO_H
#define BOOTROM_NA51089_REGMAP_CRYPTO_H

/* ---- AES ------------------------------------------------------------- */
#define AES_CTRL                0x000
#define AES_CTRL_RESET          1               /* poll bit0 clear */
#define AES_CTRL_DECRYPT        0x122           /* AES-128 decrypt (inferred) */
#define AES_START               0x004           /* = 1 */
#define AES_STS                 0x00c           /* W1C */
#define AES_STS_DONE            BIT(0)
#define AES_DIN(i)              (0x030 + 4 * (i))       /* i = 0..3, read back as IV */
#define AES_DOUT(i)             (0x040 + 4 * (i))       /* i = 0..3 */

/* ---- HASH (SHA-256) -------------------------------------------------- */
#define HASH_CTRL               0x000
#define HASH_CTRL_SHA256        0x00100012      /* (inferred) */
#define HASH_STS                0x00c           /* W1C */
#define HASH_STS_DONE           BIT(0)
#define HASH_DIGEST(i)          (0x070 + 4 * (i))       /* i = 0..7 */
#define HASH_DIN                0x090           /* 16 words per block */

/* ---- TRNG (inferred) ------------------------------------------------- */
#define TRNG_CTRL               0x000
#define TRNG_CTRL_EN            0x300           /* |= */
#define TRNG_DATA               0x00c           /* random word */

/* ---- RSA ------------------------------------------------------------- */
#define RSA_MODE                0x000
#define RSA_MODE_VAL            6
#define RSA_START               0x004           /* = 1 */
#define RSA_STS                 0x00c
#define RSA_STS_DONE            BIT(0)          /* waited second, 1 s -> K: */
#define RSA_STS_BUSY            BIT(1)          /* waited first, 10 s -> K: */
#define RSA_N_DATA              0x010           /* N data port */
#define RSA_N_IDX               0x014           /* = 0: index reset */
#define RSA_E_DATA              0x018
#define RSA_E_IDX               0x01c
#define RSA_M_DATA              0x020           /* message in / result out */
#define RSA_M_IDX               0x024
#define RSA_WORDS               64              /* every port is padded to 64 words */

#endif
