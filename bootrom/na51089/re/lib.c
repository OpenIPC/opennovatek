/*
 * lib.c -- generic helpers of the NA51089 mask ROM; no MMIO.
 *
 *   func_021 0x14fc popcount      func_042 0x284c rd8
 *   func_025 0x16e8 memcpy32      func_043 0x2870 rd32le
 *   func_026 0x1714 memset32      func_044 0x28b8 rd16le
 *   func_045 0x28e0 wr16le        func_046 0x292c wr8
 *   func_047 0x295c memcpy8       func_050 0x2b14 wr32le
 *   func_052 0x2c84 swap16x2
 *
 * The byte helpers (rd8, wr8, wr16le, wr32le, memcpy8) never issue a
 * byte or unaligned access: every load and store is an aligned word, with
 * shifts and masks to pick the bytes.  eth_boot uses them on frame
 * fields at arbitrary offsets.
 */
#include "bootrom.h"

/*
 * ARM register-specified shifts take the amount from the bottom byte and
 * give 0 for amounts of 32 and up; C leaves those undefined.  memcpy8 and
 * the shifts below depend on the ARM result, so spell it out.
 */
static inline u32 arm_lsl(u32 x, u32 n)
{
    n &= 0xff;
    return n >= 32 ? 0 : x << n;
}

static inline u32 arm_lsr(u32 x, u32 n)
{
    n &= 0xff;
    return n >= 32 ? 0 : x >> n;
}

/* Insert byte b at byte lane (addr & 3) of word w. */
static inline u32 byte_insert(u32 w, u32 addr, u32 b)
{
    u32 sh = (addr & 3) * 8;

    return (w & ~(0xffu << sh)) | ((b & 0xff) << sh);
}

/*
 * 0x14fc  popcount: Kernighan's loop.  flash_boot counts the ones of the
 * NAND bad-block marker word with it (more than 6 = good block).
 */
int popcount(u32 x)
{
    int n = 0;

    while (x) {
        x &= x - 1;
        n++;
    }
    return n;
}

/*
 * 0x16e8  memcpy32: word copy, n rounded down to a multiple of 4.  Both
 * pointers must be word aligned.  Also exported at 0x7FE8.
 */
void memcpy32(void *dst, const void *src, u32 n)
{
    u32 *d = dst;
    const u32 *s = src;
    const u32 *end = (const u32 *)((const u8 *)src + (n & ~3u));

    while (s != end)
        *d++ = *s++;
}

/* 0x1714  memset32: word fill with c in all four bytes, n rounded down. */
void memset32(void *dst, u8 c, u32 n)
{
    u32 *d = dst;
    u32 *end = (u32 *)((u8 *)dst + (n & ~3u));
    u32 w = (u32)c << 16 | (u32)c << 8 | c | (u32)c << 24;

    while (d != end)
        *d++ = w;
}

/* 0x284c  rd8: one byte via an aligned word load. */
u8 rd8(const void *p)
{
    u32 a = (u32)p;

    return (u8)(*(const u32 *)(a & ~3u) >> ((a & 3) * 8));
}

/* 0x2870  rd32le: four rd8, little-endian (bytes read 3, 2, 1, 0). */
u32 rd32le(const void *p)
{
    const u8 *b = p;
    u32 b3 = rd8(b + 3);
    u32 b2 = rd8(b + 2);
    u32 b1 = rd8(b + 1);
    u32 b0 = rd8(b);

    return b3 << 24 | b2 << 16 | b0 | b1 << 8;
}

/* 0x28b8  rd16le: two rd8 (byte 1 first). */
u16 rd16le(const void *p)
{
    const u8 *b = p;
    u32 b1 = rd8(b + 1);
    u32 b0 = rd8(b);

    return (u16)(b0 | b1 << 8);
}

/*
 * 0x28e0  wr16le: one aligned word load, two byte inserts, one store
 * back to the same word.
 *
 * Quirk, kept: the word is never advanced, so with (p & 3) == 3 the high
 * byte lands in byte 0 of the SAME word (lane (p + 1) & 3 = 0) instead of
 * the next one.  eth_boot only calls it on even offsets.
 */
void wr16le(void *p, u16 v)
{
    u32 a = (u32)p;
    u32 *wp = (u32 *)(a & ~3u);
    u32 w = *wp;
    u32 val = v;
    int i;

    for (i = 0; i < 2; i++) {
        w = byte_insert(w, a, val);
        a++;
        val >>= 8;
    }
    *wp = w;
}

/* 0x292c  wr8: aligned word read-modify-write of one byte. */
void wr8(void *p, u8 v)
{
    u32 a = (u32)p;
    u32 *wp = (u32 *)(a & ~3u);

    *wp = byte_insert(*wp, a, v);
}

/*
 * 0x295c  memcpy8: byte-granular copy built only from aligned word loads
 * and stores.  The source is streamed through a 64-bit shift register
 * {hi:lo} holding `have` valid bytes; the destination is written a byte
 * at a time (read-modify-write of its word) until it is aligned, then a
 * word at a time, then the tail by read-modify-write again.
 *
 * Quirks of the original, kept (the arm_lsl/arm_lsr shifts reproduce the
 * register-shift results it relies on):
 *  - The alignment head does not look at n: a copy shorter than
 *    4 - (dst & 3) bytes still writes up to the word boundary, and n
 *    wraps, after which the word loop runs away.  Callers never do that.
 *  - When the head leaves 5 or more bytes in the shift register, i.e.
 *    (dst & 3) > (src & 3), the word loop's shift amounts reach 32+ and
 *    each new source word is dropped: the first whole word stored after
 *    the head is right, later words (and a tail needing more than the
 *    1..3 leftover bytes) are not.  The short field copies eth_boot does
 *    (4/6 bytes) never get past that first word.
 *  - The tail always loads one more source word, needed or not.
 */
void memcpy8(void *dst, const void *src, u32 n)
{
    u32 d = (u32)dst;
    u32 soff = (u32)src & 3;
    const u32 *sp = (const u32 *)((u32)src & ~3u);
    u32 lo = *sp >> (soff * 8);         /* bytes of the first source word */
    u32 hi = 0;
    u32 have = 4 - soff;                /* valid bytes in {hi:lo} */

    if ((d & 3) == 0) {
        sp++;
    } else {
        /* load a second source word, then fill the head byte by byte */
        u32 next = sp[1];
        u32 *dp = (u32 *)(d & ~3u);
        u32 w = *dp;

        sp += 2;
        lo |= arm_lsl(next, have * 8);
        hi = arm_lsr(next, 32 - have * 8);
        have = 8 - soff;
        do {
            w = byte_insert(w, d, lo);
            d++;
            n--;
            have--;
            lo = hi << 24 | lo >> 8;
            hi >>= 8;
        } while (d & 3);
        *dp = w;
    }

    /* dst is aligned: whole words */
    {
        u32 bits = have * 8;
        u32 *dp = (u32 *)d;
        const u32 *s = sp;
        u32 left = n;

        while (left > 3) {
            u32 x = *s++;

            *dp++ = lo | arm_lsl(x, bits);
            lo = hi | arm_lsr(x, 32 - bits);
            hi = 0;
            left -= 4;
        }

        /* tail: 1..3 bytes, read-modify-write of the last word */
        if (n & 3) {
            u32 x = sp[(n & ~3u) / 4];
            u32 a = d + (n & ~3u);
            u32 end = a + (n & 3);
            u32 *wp = (u32 *)(a & ~3u);
            u32 w;

            hi |= arm_lsr(x, 32 - bits);
            lo |= arm_lsl(x, bits);
            w = *wp;
            do {
                w = byte_insert(w, a, lo);
                a++;
                lo = hi << 24 | lo >> 8;
                hi >>= 8;
            } while (a != end);
            *wp = w;
        }
    }
}

/*
 * 0x2b14  wr32le: an aligned p gets one plain store; otherwise the bytes
 * up to the word boundary are inserted into the first word, it is stored,
 * and the rest go into the next word the same way.
 */
void wr32le(void *p, u32 v)
{
    u32 a = (u32)p;
    u32 *wp;
    u32 w, end;
    u32 n = 4;

    if ((a & 3) == 0) {
        *(u32 *)p = v;
        return;
    }

    wp = (u32 *)(a & ~3u);
    w = wp[0];
    while (a & 3) {
        w = byte_insert(w, a, v);
        a++;
        n--;
        v >>= 8;
    }
    wp[0] = w;

    end = a + n;
    w = wp[1];
    while (a != end) {
        w = byte_insert(w, a, v);
        a++;
        v >>= 8;
    }
    wp[1] = w;
}

/* 0x2c84  swap16x2: swap the bytes inside each halfword (htons of a
   value read with rd16le). */
u32 swap16x2(u32 x)
{
    return ((x << 8) & 0xff00ff00u) | ((x >> 8) & 0x00ff00ffu);
}
