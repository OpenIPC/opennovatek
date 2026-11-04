/*
 * lz.c -- exported Novatek LZ77 decompressor: func_118 LZ_Uncompress.
 *
 * Stream format (as decoded here):
 *   in[0]                  marker byte, chosen by the compressor
 *   b != marker            literal b
 *   marker, 0              literal marker byte
 *   marker, len, off       copy len bytes from off bytes back in the
 *                          output; len and off are variable-length
 *                          integers, 7 bits per byte, most significant
 *                          group first, bit7 set on every byte but the
 *                          last
 * This is the format of the widely used "Basic Compression Library" LZ77
 * coder; the ROM's code is a straight decoder for it with no bounds
 * checks on either buffer.
 */
#include "bootrom.h"

/* Read one variable-length integer, return the number of bytes used.
   gcc inlined both uses in the original. */
static inline u32 lz_read_varsize(u32 *x, const u8 *buf)
{
    u32 y = 0, n = 0;
    u8 b;

    do {
        b = buf[n++];
        y = (y << 7) | (b & 0x7f);
    } while (b & 0x80);
    *x = y;
    return n;
}

/*
 * 0x6f2c -- exported at 0x7FD4 as LZ_Uncompress(in, out, insize).
 * Returns the number of bytes written; insize == 0 returns 0 without
 * touching anything.  A back-reference copies byte by byte, so overlapping
 * runs (off < len) replicate as LZ77 requires.
 */
u32 LZ_Uncompress(const u8 *in, u8 *out, u32 insize)
{
    u8 marker, symbol;
    u32 inpos, outpos, length, offset, i;

    if (insize == 0)
        return 0;

    marker = in[0];
    inpos = 1;
    outpos = 0;
    do {
        symbol = in[inpos++];
        if (symbol != marker) {
            out[outpos++] = symbol;
        } else if (in[inpos] == 0) {
            out[outpos++] = marker;
            inpos++;
        } else {
            inpos += lz_read_varsize(&length, &in[inpos]);
            inpos += lz_read_varsize(&offset, &in[inpos]);
            for (i = 0; i < length; i++)
                out[outpos + i] = out[outpos + i - offset];
            outpos += length;
        }
    } while (inpos < insize);

    return outpos;
}
