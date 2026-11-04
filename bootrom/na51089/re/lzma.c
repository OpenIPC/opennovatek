/*
 * lzma.c -- exported LZMA decoder and its strings (data_32):
 *   func_120 rc_normalize, func_121 rc_bit, func_124 lzma_inflate.
 *
 * A one-shot decoder for the 13-byte-header "LZMA alone" format (props,
 * dict_size, 64-bit unpacked size), decoding straight into the caller's
 * output buffer, which doubles as the dictionary.  The probability model
 * lives in the caller's tmp buffer.  Structure and constants are those of
 * the classic small unlzma decoders (busybox / Linux decompress_unlzma):
 * the same probability layout, a literal/match loop with a shared
 * "copy bytes" tail, rep0..rep3, and an end marker when rep0 wraps to 0.
 *
 * Debug switch: UART1's scratch register (0xF030001C), which nothing else
 * in the ROM touches, selects extra output -- bit0 prints the six
 * arguments, bit1 the decoded header fields (debug_msg_var each).
 */
#include "bootrom.h"
#include "na51089-regmap.h"

/* ---- probability model layout (u16 slots in tmp) -------------------- */
#define LZMA_NUM_POS_BITS_MAX   4
#define LZMA_LEN_NUM_LOW_BITS   3
#define LZMA_LEN_NUM_MID_BITS   3
#define LZMA_LEN_NUM_HIGH_BITS  8
#define LZMA_LEN_CHOICE         0
#define LZMA_LEN_CHOICE_2       1
#define LZMA_LEN_LOW            2
#define LZMA_LEN_MID            130     /* LOW + (16 << 3) */
#define LZMA_LEN_HIGH           258     /* MID + (16 << 3) */
#define LZMA_NUM_LEN_PROBS      514

#define LZMA_NUM_STATES         12
#define LZMA_NUM_LIT_STATES     7
#define LZMA_START_POS_MODEL_INDEX 4
#define LZMA_END_POS_MODEL_INDEX 14
#define LZMA_NUM_POS_SLOT_BITS  6
#define LZMA_NUM_LEN_TO_POS_STATES 4
#define LZMA_NUM_ALIGN_BITS     4
#define LZMA_MATCH_MIN_LEN      2

#define LZMA_IS_MATCH           0
#define LZMA_IS_REP             192     /* IS_MATCH + (12 << 4) */
#define LZMA_IS_REP_G0          204
#define LZMA_IS_REP_G1          216
#define LZMA_IS_REP_G2          228
#define LZMA_IS_REP_0_LONG      240
#define LZMA_POS_SLOT           432     /* IS_REP_0_LONG + (12 << 4) */
#define LZMA_SPEC_POS           688     /* POS_SLOT + (4 << 6) */
#define LZMA_ALIGN              802     /* SPEC_POS + 128 - 14 */
#define LZMA_LEN_CODER          818     /* ALIGN + 16 */
#define LZMA_REP_LEN_CODER      1332    /* LEN_CODER + 514 */
#define LZMA_LITERAL            1846    /* REP_LEN_CODER + 514 */
#define LZMA_BASE_SIZE          1846
#define LZMA_LIT_SIZE           768

#define LZMA_HDR_SIZE           13
#define LZMA_PROB_INIT          0x400   /* 1/2 in 11-bit probability */
#define RC_TOP                  0x01000000u
#define RC_MOVE_BITS            5

/*
 * State after a literal.  In the image these 12 bytes sit unlabelled at
 * 0x7c12, between the ARP frame template (data_28) and the SCSI REQUEST
 * SENSE reply (data_29).
 */
static const u8 lzma_lit_next_state[LZMA_NUM_STATES] = {
    0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5,
};

/*
 * 0x7044 -- shift one input byte into the range coder (the unconditional
 * half of a normalisation; callers test range < 2^24 first).  Running
 * out of input only prints "die: truncated input": it does not stop, and
 * the byte past the end is read anyway.
 */
void rc_normalize(rc_t *rc)
{
    if (rc->ptr >= rc->end)
        debug_msg("die: truncated input");
    rc->range <<= 8;
    rc->code = (rc->code << 8) | *rc->ptr++;
}

/*
 * 0x7094 -- decode one bit with an adaptive 11-bit probability and update
 * it (shift 5).  The bound is kept in the coder state as well.
 */
int rc_bit(rc_t *rc, u16 *prob)
{
    if (rc->range < RC_TOP)
        rc_normalize(rc);

    rc->bound = (rc->range >> 11) * *prob;
    if (rc->code < rc->bound) {
        rc->range = rc->bound;
        *prob += (0x800 - *prob) >> RC_MOVE_BITS;
        return 0;
    }
    rc->range -= rc->bound;
    rc->code -= rc->bound;
    *prob -= *prob >> RC_MOVE_BITS;
    return 1;
}

/* Bit-tree decode of num_bits bits (gcc inlined every use). */
static inline int rc_bit_tree(rc_t *rc, u16 *p, int num_bits)
{
    int i = num_bits, m = 1;

    while (i--)
        m = (m << 1) + rc_bit(rc, p + m);
    return m - (1 << num_bits);
}

/* One equiprobable bit (inlined in the original). */
static inline u32 rc_direct_bit(rc_t *rc)
{
    if (rc->range < RC_TOP)
        rc_normalize(rc);
    rc->range >>= 1;
    if (rc->code >= rc->range) {
        rc->code -= rc->range;
        return 1;
    }
    return 0;
}

/*
 * Error exits.  Note the odd 4 KiB threshold: the "failed" messages are
 * printed only once more than 0x1000 bytes have been produced; below
 * that the decoder returns -1 silently (or with only the
 * "...buffer:" line, see the copy path).
 */
static int lzma_fail_out_full(u32 pos)
{
    if (pos > 0x1000) {
        debug_msg("wrn: too small output buffer");
        debug_msg("wrn: failed");
    }
    return -1;
}

/*
 * 0x716c -- exported at 0x7FD0.
 *   lzma_inflate(in, in_size, out, out_size, tmp, tmp_size)
 * Returns the number of bytes written, or -1.  Decoding stops at the
 * header's unpacked size (low 32 bits only) or at an end marker.
 * dst_size_high is decoded and printed but never used.  There is no
 * check that a match distance fits the dictionary or the data written
 * so far; a negative source index is wrapped by adding dict_size.
 */
int lzma_inflate(const u8 *in, u32 in_size, u8 *out, u32 out_size,
                 u16 *tmp, u32 tmp_size)
{
    u8 hdr[LZMA_HDR_SIZE];
    rc_t rc;
    u32 dict_size, dst_size, dst_size_high;
    u32 lc, lp, pb, mi, pb_mask, lp_mask, i;
    u32 pos, state, rep0, rep1, rep2, rep3, len;
    int need, idx;
    u8 prev_byte;
    u16 *prob, *p;

    /* copied before the size check: 13 bytes are read whatever in_size */
    for (i = 0; i < LZMA_HDR_SIZE; i++)
        hdr[i] = in[i];

    if (in_size <= 15) {
        debug_msg("wrn: too short lzma file");
        return -1;
    }

    if (REG32(UART1_BASE + UART_SCR) & UART_SCR_LZMA_ARGS) {
        debug_msg_var("in_ptr", (u32)in);
        debug_msg_var("in_size", in_size);
        debug_msg_var("out_ptr", (u32)out);
        debug_msg_var("out_size", out_size);
        debug_msg_var("p_tmp", (u32)tmp);
        debug_msg_var("tmp_size", tmp_size);
    }

    /* props = (pb * 5 + lp) * 9 + lc; read from in[0], not the copy */
    if (in[0] >= 9 * 5 * 5) {
        debug_msg("wrn: bad lzma header");
        return -1;
    }
    mi = 0;
    lc = in[0];
    while (lc >= 9) {
        mi++;
        lc -= 9;
    }
    pb = 0;
    lp = mi;
    while (lp >= 5) {
        pb++;
        lp -= 5;
    }

    dict_size = hdr[1] | (hdr[2] << 8) | (hdr[3] << 16) | ((u32)hdr[4] << 24);
    dst_size = 0;
    dst_size_high = 0;
    for (i = 0; i < 8; i++) {
        if (i < 4)
            dst_size += (u32)hdr[5 + i] << (8 * i);
        else
            dst_size_high += (u32)hdr[5 + i] << (8 * (i - 4));
    }

    if (REG32(UART1_BASE + UART_SCR) & UART_SCR_LZMA_HDR) {
        debug_msg_var("lc\r\n", lc);
        debug_msg_var("pb\r\n", pb);
        debug_msg_var("lp\r\n", lp);
        debug_msg_var("dst_size\r\n", dst_size);
        debug_msg_var("dst_size_high\r\n", dst_size_high);
        debug_msg_var("dict_size\r\n", dict_size);
    }

    if (dict_size == 0)
        dict_size = 1;

    need = (LZMA_BASE_SIZE + (LZMA_LIT_SIZE << (lc + lp))) * 2;
    if (need > (int)tmp_size) {         /* signed compare in the original */
        debug_msg_err("die: tmp_size need: ", need);
        return -1;
    }
    p = tmp;
    do
        *p++ = LZMA_PROB_INIT;
    while (p != tmp + need / 2);

    /* coder init: five bytes shifted in with range 0, then full range */
    rc.ptr = in + LZMA_HDR_SIZE;
    rc.end = in + in_size;
    rc.range = 0;
    rc.code = 0;
    rc.bound = 0;
    for (i = 0; i < 5; i++)
        rc_normalize(&rc);
    rc.range = 0xffffffff;

    pb_mask = (1u << pb) - 1;
    lp_mask = (1u << lp) - 1;
    pos = 0;
    state = 0;
    prev_byte = 0;
    rep0 = rep1 = rep2 = rep3 = 1;

    while (pos < dst_size) {
        u32 pos_state = pos & pb_mask;

        prob = tmp + LZMA_IS_MATCH + (state << LZMA_NUM_POS_BITS_MAX) + pos_state;
        if (rc_bit(&rc, prob) == 0) {
            /* ---- literal ------------------------------------------- */
            prob = tmp + LZMA_LITERAL + LZMA_LIT_SIZE *
                   (((pos & lp_mask) << lc) + (prev_byte >> (8 - lc)));
            mi = 1;
            if (state >= LZMA_NUM_LIT_STATES) {
                int match_byte, bit, b;

                idx = (int)(pos - rep0);
                if (idx < 0)
                    idx += dict_size;
                match_byte = out[idx];
                do {
                    match_byte <<= 1;
                    bit = match_byte & 0x100;
                    b = rc_bit(&rc, prob + 0x100 + bit + mi);
                    mi = (mi << 1) + b;
                    if (bit != (b << 8))
                        break;
                } while (mi < 0x100);
            }
            while (mi < 0x100)
                mi = (mi << 1) + rc_bit(&rc, prob + mi);
            prev_byte = (u8)mi;
            state = lzma_lit_next_state[state];
            len = 1;
            goto store;
        }

        /* ---- match / rep match --------------------------------------- */
        prob = tmp + LZMA_IS_REP + state;
        if (rc_bit(&rc, prob) == 0) {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            state = state < LZMA_NUM_LIT_STATES ? 0 : 3;
            prob = tmp + LZMA_LEN_CODER;
        } else {
            if (rc_bit(&rc, tmp + LZMA_IS_REP_G0 + state) == 0) {
                prob = tmp + LZMA_IS_REP_0_LONG +
                       (state << LZMA_NUM_POS_BITS_MAX) + pos_state;
                if (rc_bit(&rc, prob) == 0) {
                    /* short rep: one byte from rep0 */
                    state = state < LZMA_NUM_LIT_STATES ? 9 : 11;
                    len = 1;
                    goto copy;
                }
            } else {
                u32 distance;

                if (rc_bit(&rc, tmp + LZMA_IS_REP_G1 + state) == 0) {
                    distance = rep1;
                } else {
                    if (rc_bit(&rc, tmp + LZMA_IS_REP_G2 + state) == 0) {
                        distance = rep2;
                    } else {
                        distance = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = distance;
            }
            state = state < LZMA_NUM_LIT_STATES ? 8 : 11;
            prob = tmp + LZMA_REP_LEN_CODER;
        }

        /* length */
        if (rc_bit(&rc, prob + LZMA_LEN_CHOICE) == 0) {
            len = rc_bit_tree(&rc, prob + LZMA_LEN_LOW +
                              (pos_state << LZMA_LEN_NUM_LOW_BITS),
                              LZMA_LEN_NUM_LOW_BITS);
        } else if (rc_bit(&rc, prob + LZMA_LEN_CHOICE_2) == 0) {
            len = 8 + rc_bit_tree(&rc, prob + LZMA_LEN_MID +
                                  (pos_state << LZMA_LEN_NUM_MID_BITS),
                                  LZMA_LEN_NUM_MID_BITS);
        } else {
            len = 16 + rc_bit_tree(&rc, prob + LZMA_LEN_HIGH,
                                   LZMA_LEN_NUM_HIGH_BITS);
        }

        /* distance, for a plain match only */
        if (state < 4) {
            u32 pos_slot;
            int num_bits;

            state += LZMA_NUM_LIT_STATES;
            prob = tmp + LZMA_POS_SLOT +
                   ((len < LZMA_NUM_LEN_TO_POS_STATES ? len :
                     LZMA_NUM_LEN_TO_POS_STATES - 1) << LZMA_NUM_POS_SLOT_BITS);
            pos_slot = rc_bit_tree(&rc, prob, LZMA_NUM_POS_SLOT_BITS);
            if (pos_slot >= LZMA_START_POS_MODEL_INDEX) {
                u32 bitval, m;

                num_bits = (pos_slot >> 1) - 1;
                rep0 = 2 | (pos_slot & 1);
                if (pos_slot < LZMA_END_POS_MODEL_INDEX) {
                    rep0 <<= num_bits;
                    prob = tmp + LZMA_SPEC_POS + rep0 - pos_slot - 1;
                } else {
                    num_bits -= LZMA_NUM_ALIGN_BITS;
                    while (num_bits--)
                        rep0 = (rep0 << 1) | rc_direct_bit(&rc);
                    prob = tmp + LZMA_ALIGN;
                    rep0 <<= LZMA_NUM_ALIGN_BITS;
                    num_bits = LZMA_NUM_ALIGN_BITS;
                }
                bitval = 1;
                m = 1;
                while (num_bits--) {
                    int b = rc_bit(&rc, prob + m);

                    m = (m << 1) + b;
                    if (b)
                        rep0 |= bitval;
                    bitval <<= 1;
                }
            } else {
                rep0 = pos_slot;
            }
            /* end marker: distance 0xffffffff */
            if (++rep0 == 0)
                return pos;
        }
        len += LZMA_MATCH_MIN_LEN;

        /* ---- copy len bytes from rep0 back (literal enters at store) - */
        do {
copy:
            idx = (int)(pos - rep0);
            if (idx < 0)
                idx += dict_size;
            if ((u32)idx >= out_size) {
                debug_msg_var("wrn: too small output buffer:", idx);
                if (pos > 0x1000)
                    debug_msg("wrn: failed");
                return -1;
            }
            prev_byte = out[idx];
store:
            if (pos >= out_size)
                return lzma_fail_out_full(pos);
            out[pos++] = prev_byte;
            len--;
        } while (len != 0 && pos < dst_size);
    }

    return pos;
}
