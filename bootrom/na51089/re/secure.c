/*
 * secure.c -- secure boot, crypto engines, efuse:
 *   func_018 aes_load_efuse_key   func_019 aes_cbc_decrypt
 *   func_020 reverse256           func_033 rsa_load_n
 *   func_034 rsa_load_e           func_035 rsa_run
 *   func_036 rsa_read             func_037 secure_boot_verify
 *   func_064 otp_boot_cfg         func_065 efuse_key_select
 *   func_116 sha256               func_117 efuse_read
 *
 * Secure boot is driven by the OTP boot-config word (OTP+0x50):
 *   bit5 secure boot on ('X'); bit7 the image body is AES-128-CBC
 *   encrypted ('S'); bit1 RSA-2048 signature ('R') else an AES-encrypted
 *   SHA-256 ('A'); bit3 (RSA only) SHA-256 of the modulus must match
 *   efuse words 0x14..0x1b ('C').
 * The AES key never passes through the CPU: aes_load_efuse_key() reads
 * efuse words 0x10..0x13 with a key-path select set first, and the
 * engine picks the key up from there (inferred: the values read are
 * discarded).  All scratch lives at SRAM_SB_* (0xF07D0000..0x0620).
 */
#include "bootrom.h"
#include "na51089-regmap.h"
#include "sram.h"

/*
 * TODO(headers): regmap-crypto.h names only the first-block HASH_CTRL
 * value.  sha256() rewrites the control word to 0x12 after the first
 * block: the same mode without bit20, i.e. bit20 = "initialise the
 * digest" (inferred).
 */
#define HASH_CTRL_SHA256_CONT   0x00000012
/* TODO(headers): efuse_key_select(a, ...) value used for the AES key path */
#define EFUSE_KEYSEL_AES        1

#define SB_WORDS(addr)          ((u32 *)(addr))

/* 0x4df4 -- OTP boot-configuration word (OTP_CFG_* bits). */
u32 otp_boot_cfg(void)
{
    return REG32(OTP_BASE + OTP_BOOT_CFG);
}

/* 0x4e0c -- efuse key-path select; only aes_load_efuse_key uses it. */
void efuse_key_select(u32 a, u32 b)
{
    REG32(OTP_BASE + OTP_KEYSEL_A) = a;
    REG32(OTP_BASE + OTP_KEYSEL_B) = b;
}

/*
 * 0x6eb4 -- read efuse word idx & 31.  Exported at 0x7FE4.
 * Enable, wait ready, set the index, trigger a read and wait for the
 * trigger bit to self-clear, wait valid, take the data, disable, wait
 * ready to drop.  (The first test of the trigger-clear loop is made on
 * the value just written, which always has the bit set, so in effect
 * the register is polled from the next read on.)
 */
u32 efuse_read(int idx)
{
    u32 v;

    REG32(OTP_BASE + OTP_CTRL) = OTP_CTRL_EN;
    while (!(REG32(OTP_BASE + OTP_STS) & OTP_STS_READY))
        ;
    REG32(OTP_BASE + OTP_IDX) = idx & (EFUSE_WORDS - 1);
    REG32(OTP_BASE + OTP_CTRL) |= OTP_CTRL_READ;
    while (REG32(OTP_BASE + OTP_CTRL) & OTP_CTRL_READ)
        ;
    while (!(REG32(OTP_BASE + OTP_STS) & OTP_STS_VALID))
        ;
    v = REG32(OTP_BASE + OTP_DATA);
    REG32(OTP_BASE + OTP_CTRL) = 0;
    while (REG32(OTP_BASE + OTP_STS) & OTP_STS_READY)
        ;
    return v;
}

/*
 * 0x1364 -- push the efuse AES key (words 0x10..0x13) into the engine:
 * for each word select key path (1, k), then read it.  The read values
 * are thrown away; the hardware latches them on the selected path.
 */
void aes_load_efuse_key(void)
{
    int k;

    for (k = 0; k < 4; k++) {
        efuse_key_select(EFUSE_KEYSEL_AES, k);
        efuse_read(EFUSE_AES_KEY + k);
    }
}

/*
 * 0x13c0 -- AES-128-CBC decrypt len bytes (whole 16-byte blocks, a
 * partial last block is processed as a full one) from src to dst with
 * the IV at SRAM_SB_IV.  The engine is reset and re-armed for every
 * block.  The next IV is read back from the engine's input registers,
 * so src == dst (in place) works.  Always returns 0.
 */
int aes_cbc_decrypt(const u8 *src, u8 *dst, int len)
{
    const u32 *in = (const u32 *)src;
    u32 *out = (u32 *)dst;
    u32 *iv = SB_WORDS(SRAM_SB_IV);
    u8 *end = dst + len;
    u32 sts;
    int i;

    while (end - (u8 *)out > 0) {
        REG32(AES_BASE + AES_CTRL) = AES_CTRL_RESET;
        while (REG32(AES_BASE + AES_CTRL) & AES_CTRL_RESET)
            ;
        REG32(AES_BASE + AES_CTRL) = AES_CTRL_DECRYPT;
        for (i = 0; i < 4; i++)
            REG32(AES_BASE + AES_DIN(i)) = in[i];
        REG32(AES_BASE + AES_START) = 1;
        while (!((sts = REG32(AES_BASE + AES_STS)) & AES_STS_DONE))
            ;
        REG32(AES_BASE + AES_STS) = sts;        /* W1C what was read */

        for (i = 0; i < 4; i++)
            out[i] = REG32(AES_BASE + AES_DOUT(i)) ^ iv[i];
        for (i = 0; i < 4; i++)
            iv[i] = REG32(AES_BASE + AES_DIN(i));

        in += 4;
        out += 4;
    }
    return 0;
}

/*
 * 0x14b8 -- dst[255 - i] = src[i] for a 256-byte block: big-endian word
 * reads from src stored from the end of dst backwards.  Converts the
 * RSA signature to the engine's order and back.
 */
void reverse256(const u8 *src, u8 *dst)
{
    u32 *d = (u32 *)(dst + 256);

    while (d != (u32 *)dst) {
        *--d = ((u32)src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
        src += 4;
    }
}

/* Little-endian word from bytes (the RSA loaders read byte by byte). */
static u32 rd_le32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

/* Write nwords from src to an RSA data port, then zero-pad to 64 words. */
static void rsa_load_port(u32 idx_reg, u32 data_reg, const u32 *src, int nwords)
{
    const u8 *s = (const u8 *)src;
    int i;

    REG32(RSA_BASE + idx_reg) = 0;
    for (i = 0; i < nwords; i++, s += 4)
        REG32(RSA_BASE + data_reg) = rd_le32(s);
    for (i = 0; (u32)i < (u32)(RSA_WORDS - nwords); i++)
        REG32(RSA_BASE + data_reg) = 0;
}

/* 0x2158 -- load the modulus N (the original is rsa_load_port inlined). */
void rsa_load_n(const u32 *src, int nwords)
{
    rsa_load_port(RSA_N_IDX, RSA_N_DATA, src, nwords);
}

/* 0x21d4 -- load the public exponent E. */
void rsa_load_e(const u32 *src, int nwords)
{
    rsa_load_port(RSA_E_IDX, RSA_E_DATA, src, nwords);
}

/*
 * 0x2250 -- load the message (signature), start, and wait: first for
 * BUSY to clear (10 s), then -- after timer_stop() -- for DONE (1 s).
 * The second wait therefore polls timer_now() on a stopped timer.
 * Either timeout is boot_fail(BOOT_FAIL_TIMEOUT), "K:"; should
 * boot_fail return, the wait simply resumes.
 */
void rsa_run(const u32 *msg, int nwords)
{
    rsa_load_port(RSA_M_IDX, RSA_M_DATA, msg, nwords);
    REG32(RSA_BASE + RSA_START) = 1;

    timer_start();
    while (REG32(RSA_BASE + RSA_STS) & RSA_STS_BUSY) {
        if (timer_now() > TMO_10S)
            boot_fail(BOOT_FAIL_TIMEOUT);
    }
    timer_stop();
    while (!(REG32(RSA_BASE + RSA_STS) & RSA_STS_DONE)) {
        if (timer_now() > TMO_1S)
            boot_fail(BOOT_FAIL_TIMEOUT);
    }
}

/*
 * 0x2338 -- read nwords of the result from the M port after resetting its
 * index.  (The original first builds an on-stack table of the nwords
 * destination pointers, 64 entries, and stores through it; that has no
 * visible effect and is written as a plain loop here.)
 */
void rsa_read(u32 *dst, int nwords)
{
    int i;

    REG32(RSA_BASE + RSA_M_IDX) = 0;
    for (i = 0; i < nwords; i++)
        dst[i] = REG32(RSA_BASE + RSA_M_DATA);
}

/*
 * 0x6d68 -- SHA-256 of len bytes at data into out[8] (words as the engine
 * returns them).  Exported at 0x7FBC.
 *
 * Only whole 64-byte blocks are hashed and the padding block is built by
 * software as if len were a multiple of 64: one extra block of 0x80, zeros
 * and the bit length (32 bits, big-endian) in the last word.  The first
 * block is always fed, even for len < 64, and the loop over the rest runs
 * until it *equals* data + 64 * (len / 64), so len < 64 would run away.
 * Every caller passes 256 or 0x8000.
 *
 * out is first cleared with memset32(out, 0, 8) -- 8 bytes, i.e. two of
 * the eight words -- and then fully overwritten anyway.
 */
void sha256(const void *data, int len, u32 out[8])
{
    const u32 *w = (const u32 *)data;
    const u32 *end;
    u32 sts, bits;
    int i;

    REG32(CG_BASE + CG_CLKEN1) |= CG_CLKEN1_HASH;

    /* first block, with the initialise bit */
    REG32(HASH_BASE + HASH_CTRL) = HASH_CTRL_SHA256;
    for (i = 0; i < 16; i++)
        REG32(HASH_BASE + HASH_DIN) = w[i];
    while (!((sts = REG32(HASH_BASE + HASH_STS)) & HASH_STS_DONE))
        ;
    REG32(HASH_BASE + HASH_STS) = sts;

    /* remaining whole blocks */
    REG32(HASH_BASE + HASH_CTRL) = HASH_CTRL_SHA256_CONT;
    end = w + (len / 64) * 16;
    for (w += 16; w != end; w += 16) {
        for (i = 0; i < 16; i++)
            REG32(HASH_BASE + HASH_DIN) = w[i];
        while (!((sts = REG32(HASH_BASE + HASH_STS)) & HASH_STS_DONE))
            ;
        REG32(HASH_BASE + HASH_STS) = sts;
    }

    /* padding block */
    bits = __builtin_bswap32((u32)len << 3);
    for (i = 0; i < 16; i++)
        REG32(HASH_BASE + HASH_DIN) = i == 0 ? 0x80 : i == 15 ? bits : 0;
    while (!((sts = REG32(HASH_BASE + HASH_STS)) & HASH_STS_DONE))
        ;
    REG32(HASH_BASE + HASH_STS) = sts;

    memset32(out, 0, 8);
    for (i = 0; i < 8; i++)
        out[i] = REG32(HASH_BASE + HASH_DIGEST(i));

    REG32(CG_BASE + CG_CLKEN1) &= ~CG_CLKEN1_HASH;
}

/* AES-decrypt in place with the efuse key, AES clock on around it. */
static void sb_aes_decrypt(const u8 *src, u8 *dst, int len)
{
    REG32(CG_BASE + CG_CTRL70) |= CG_CTRL70_AES;
    aes_load_efuse_key();
    aes_cbc_decrypt(src, dst, len);
    REG32(CG_BASE + CG_CTRL70) &= ~CG_CTRL70_AES;
}

/* 8-word compare; a mismatch is boot_fail(9), and the compare stops. */
static void sb_expect_equal(const u32 *a, const u32 *b)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (a[i] != b[i]) {
            boot_fail(BOOT_FAIL_CHECKSUM);
            return;
        }
    }
}

/*
 * 0x239c -- called by check_and_run_loader with the loaded 32 KiB image.
 * Returns 1 when secure boot is off (OTP bit5 clear), 0 when the image
 * verified; any failed check ends in boot_fail(BOOT_FAIL_CHECKSUM) ("K9")
 * -- and if that ever returned, verification would go on and return 0.
 *
 *   'X'  secure boot on
 *   'S'  (bit7) decrypt img+0x300..0x7fff in place, IV = the 32 bytes at
 *        img+0x1e0 (RSA mode) or img+0x2c0 (AES mode)
 *   'R'  (bit1) RSA: N, E and the byte-reversed signature are copied to
 *        scratch and zeroed in the image, which is then hashed;
 *        'C' (bit3) SHA-256(N) must equal efuse words 0x14..0x1b; the
 *        engine computes sig^E mod N and its last 32 bytes must equal
 *        the image hash
 *   'A'  (bit1 clear) the 32 bytes at img+0x2c0 are copied out and
 *        zeroed, the image is hashed, and the copy AES-decrypted (IV 0)
 *        must equal the hash
 *   then, if header flag bit29 is set (read at SRAM_IMAGE_FLAGS whatever
 *   img is), img+0x2e0 is zeroed, the image rehashed, and the original
 *   value must equal the new hash.
 *
 * Restoration afterwards is partial: the signature (RSA) or the digest
 * field (AES) and the +0x2e0 hash are written back, but in RSA mode N and
 * E stay zeroed in the image, and in AES mode the digest field gets the
 * *decrypted* digest, not the bytes that were there.
 */
int secure_boot_verify(u8 *img)
{
    u32 cfg = otp_boot_cfg();
    u32 rsa;
    u32 efuse_hash[8];
    int i;

    if (!(cfg & OTP_CFG_SECURE))
        return 1;

    rsa = cfg & OTP_CFG_RSA;
    UART0_PUTC_RAW(LTR_SECURE);

    if (cfg & OTP_CFG_ENCRYPTED) {
        UART0_PUTC_RAW(LTR_SECURE_DECRYPT);
        memcpy32((void *)SRAM_SB_IV, rsa ? img + HDR_RSA_SIG : img + HDR_AES_DIGEST, 32);
        sb_aes_decrypt(img + HDR_ENC_AREA, img + HDR_ENC_AREA, HDR_ENC_END - HDR_ENC_AREA);
    }

    if (rsa) {
        UART0_PUTC_RAW(LTR_SECURE_RSA);
        reverse256(img + HDR_RSA_SIG, (u8 *)SRAM_SB_RSA_SIG);
        memcpy32((void *)SRAM_SB_RSA_N, img + HDR_RSA_N, 256);
        memcpy32((void *)SRAM_SB_RSA_E, img + HDR_RSA_E, 32);
        memset32(img + HDR_RSA_SIG, 0, 256);
        memset32(img + HDR_RSA_N, 0, 256);
        memset32(img + HDR_RSA_E, 0, 32);
    } else {
        UART0_PUTC_RAW(LTR_SECURE_AES);
        memcpy32((void *)SRAM_SB_DIGEST, img + HDR_AES_DIGEST, 32);
        memset32(img + HDR_AES_DIGEST, 0, 32);
    }

    memset32((void *)SRAM_SB_ZERO, 0, 16);
    memcpy32((void *)SRAM_SB_STORED_SHA, img + HDR_SHA256, 32);
    sha256(img, SRAM_IMAGE_SIZE, SB_WORDS(SRAM_SB_IMAGE_SHA));

    if (rsa) {
        if (cfg & OTP_CFG_KEYHASH) {
            UART0_PUTC_RAW(LTR_SECURE_KEYHASH);
            memset32((void *)SRAM_SB_N_HASH, 0, 32);
            sha256((void *)SRAM_SB_RSA_N, 256, SB_WORDS(SRAM_SB_N_HASH));
            for (i = 0; i < 8; i++)
                efuse_hash[i] = efuse_read(EFUSE_KEY_HASH + i);
            sb_expect_equal(efuse_hash, SB_WORDS(SRAM_SB_N_HASH));
        }

        REG32(TOP2_BASE + TOP2_CTRL) &= ~TOP2_CTRL_RSA;
        REG32(CG_BASE + CG_CLKEN1) |= CG_CLKEN1_RSA;
        REG32(RSA_BASE + RSA_MODE) = RSA_MODE_VAL;
        rsa_load_n(SB_WORDS(SRAM_SB_RSA_N), RSA_WORDS);
        rsa_load_e(SB_WORDS(SRAM_SB_RSA_E), 8);
        rsa_run(SB_WORDS(SRAM_SB_RSA_SIG), RSA_WORDS);
        rsa_read(SB_WORDS(SRAM_SB_RSA_OUT), RSA_WORDS);
        REG32(CG_BASE + CG_CLKEN1) &= ~CG_CLKEN1_RSA;
        REG32(TOP2_BASE + TOP2_CTRL) |= TOP2_CTRL_RSA;

        memcpy32((void *)SRAM_SB_DIGEST, (void *)SRAM_SB_RSA_OUT_DIGEST, 32);
    } else {
        /* IV 0: only the first 16 bytes of SRAM_SB_IV are cleared */
        REG32(CG_BASE + CG_CTRL70) |= CG_CTRL70_AES;
        aes_load_efuse_key();
        memset32((void *)SRAM_SB_IV, 0, 16);
        aes_cbc_decrypt((u8 *)SRAM_SB_DIGEST, (u8 *)SRAM_SB_DIGEST, 32);
        REG32(CG_BASE + CG_CTRL70) &= ~CG_CTRL70_AES;
    }

    sb_expect_equal(SB_WORDS(SRAM_SB_DIGEST), SB_WORDS(SRAM_SB_IMAGE_SHA));

    if (REG32(SRAM_IMAGE_FLAGS) & HDR_FLAG_SHA) {
        memset32(img + HDR_SHA256, 0, 32);
        sha256(img, SRAM_IMAGE_SIZE, SB_WORDS(SRAM_SB_IMAGE_SHA));
        sb_expect_equal(SB_WORDS(SRAM_SB_STORED_SHA), SB_WORDS(SRAM_SB_IMAGE_SHA));
    }

    if (rsa)
        reverse256((u8 *)SRAM_SB_RSA_SIG, img + HDR_RSA_SIG);
    else
        memcpy32(img + HDR_AES_DIGEST, (void *)SRAM_SB_DIGEST, 32);
    memcpy32(img + HDR_SHA256, (void *)SRAM_SB_STORED_SHA, 32);

    return 0;
}
