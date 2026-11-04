/*
 * loader.c -- loader header checks and the header's register script.
 *
 *   func_028 0x17f0 hdr_tag_ok      func_031 0x18e0 hdr_script_run
 *
 * Both take script = header + 0x38 (HDR_SCRIPT_PTR), the script word,
 * and reach the rest of the header at negative offsets.
 *
 * The script is how a loader brings up DRAM before the ROM loads its
 * body: flash_boot, sd_boot and emmc_boot read the header (and its extra
 * sectors), run the script, then load the image.  Entries start at
 * header + 0x40; the count is script_word[11:0].  Each entry is one or
 * two words; w0[27:24] is the opcode and w0 with those four bits cleared
 * is the address operand:
 *
 *   op  words  effect
 *    0   2     poll *addr until the masked bits match (w1: n = [23:20]
 *              bits, bit i at position (w1 >> 5i) & 31 must equal
 *              (w1 >> (24 + i)) & 1).  No timeout.
 *    1   2     *addr = w1
 *    2   1     udelay(w0[23:0])
 *    3   2     bytes of w1 to the words addr+0, +4, +8, +0xc
 *    4   1     command to the register at header+0x34, poll it for one bit
 *    5   2     lane bytes of w1 to registers under header+0x3c
 *    6   2     the same under the DDR PHY, 0xC0021000
 *    7   2     read-gate/DQS training through 0xC000130C/0xC0001310,
 *              only if script_word bit19; skipped (still two words) if not
 *    8   1     load a pair of trims from efuse word w0[4:0]
 *    9   1     apply a trim to a run of lane registers, clamped to 0..31
 *   10   1     set the "merge" flag for op 7 from w0 bit0
 *   11+  1     ignored
 *
 * The meaning of ops 4, 7 and 9 (DDR training) is inferred from the
 * arithmetic; the effect below is the ROM's, register for register.
 */
#include "na51089-regmap.h"
#include "sram.h"

/* op 4: cmd 2 also carries w0[22:4] into the command word bits 30:12 */
#define SCRIPT_CMD2_ARG_MASK    0x7ffff000

/* op 7 */
#define TRAIN_ROUNDS            16          /* the ROM counts 17 down, pre-decrement */
#define TRAIN_READ_SENTINEL     0x80000000  /* "no previous read" for the stable read */
#define TRAIN_INIT_SUM          0x8000      /* initial first/current sum */
#define TRAIN_INIT_DIFF         0xffffffff  /* initial first/current difference */
#define TRAIN_PULSE_US          3           /* udelay after each command */
#define TRAIN_LOG_A             0x20000000  /* log word: avg_a | this */
#define TRAIN_LOG_B             0x30000000  /* log word: avg_b | this */
#define TRAIN_CODE_MASK         0x3f        /* lane code: [5] valid, [4] sign, [3:0] magnitude */
#define TRAIN_CODE_EN           BIT(5)
#define TRAIN_CODE_SIGN         BIT(4)
#define TRAIN_CODE_MAG          0xf
#define TRAIN_CODE_HI           0x30        /* [5:4] */

/* op 9 */
#define TRIM_NEG                BIT(5)
#define TRIM_MAG                0x1f
#define TRIM_MAX                31

#define DDR_CTRL(off)           REG32(DDR_CTRL_BASE + (off))

/*
 * ARM register-specified shift: amounts of 32 and up give 0.  Op 0's
 * field extraction can shift by up to 70 for large n and relies on that.
 */
static inline u32 arm_lsr(u32 x, u32 n)
{
    n &= 0xff;
    return n >= 32 ? 0 : x >> n;
}

/*
 * 0x17f0  hdr_tag_ok: 0 if the u16 at header+0x30 is 0xAA55, else -1.
 */
int hdr_tag_ok(const u32 *script)
{
    const struct loader_hdr *hdr =
        (const struct loader_hdr *)((const u8 *)script - HDR_SCRIPT_PTR);

    return hdr->tag == LOADER_TAG ? 0 : -1;
}

/* Ops 5 and 6: byte 0 of v to base; bytes 1..3 to base + 4 * w0 nibble
   [15:12] / [19:16] / [23:20], each only if w0 bit 29 / 30 / 31. */
static void script_write_lanes(u32 base, u32 w0, u32 v)
{
    REG32(base) = v & 0xff;
    if (w0 & BIT(29))
        REG32(base + (((w0 >> 12) & 0xf) << 2)) = (v >> 8) & 0xff;
    if (w0 & BIT(30))
        REG32(base + (((w0 >> 16) & 0xf) << 2)) = (v >> 16) & 0xff;
    if (w0 & BIT(31))
        REG32(base + (((w0 >> 20) & 0xf) << 2)) = v >> 24;
}

/* Op 7 helper: read regA | regB << 8 until two reads in a row agree. */
static u32 train_read_stable(u32 reg_a, u32 reg_b)
{
    u32 prev = TRAIN_READ_SENTINEL;
    u32 cur;

    for (;;) {
        cur = REG32(reg_a);
        cur |= REG32(reg_b) << 8;
        if (cur == prev)
            return cur;
        prev = cur;
    }
}

/*
 * Op 7: DDR training of one lane register (the original has this inline
 * in hdr_script_run).
 *
 *   w0[3:0]   busy bit to poll in DDR_TRAIN_STS after each command
 *   w0[7:4]   command bit, always set
 *   w0[11:8]  position of the A/B select bit in the command
 *   w0[12]    select value for command A (B gets the inverse)
 *   w0[19:16] "step enabled" bit of the lane register
 *   w0[22:20] position of the direction bit set with the step bit
 *   w0[23]    direction for avg_b > avg_a (avg_b < avg_a gets the inverse)
 *   w0[31:28] N: samples per measurement, and the settle udelay(N)
 *   w1[7:0]   lane register index (lane_base + 4 * idx)
 *   w1[23:16] / w1[31:24]  result registers A (low byte) and B (<< 8)
 *
 * Up to 16 rounds: issue command B and wait N us, then N times issue A
 * and B and sum a stable read of the result registers after each, giving
 * avg_a and avg_b (by a repeated-subtraction "divide" that stops at
 * remainder <= N, not < N: it is one short whenever the sum is an exact
 * multiple, and 0 for sum <= N).  Equal averages end the training.
 * Otherwise, with diff = |avg_b - avg_a| and sum = avg_a + avg_b:
 *   - step bit not yet set in the lane register: set it with the
 *     direction bit for this sign, clear the low code bits [5:0], and
 *     remember (sum, diff) as the first measurement;
 *   - set, and the register's bit4 equals this sign's direction value
 *     (as computed: (dir_value ^ reg) & 0x10 -- only meaningful when
 *     w0[22:20] == 4): step the register by +1;
 *   - set, and different (the sign flipped): decide between the last two
 *     steps by cross-multiplied ratios and stop:
 *         first_diff * cur_sum < first_sum * cur_diff -> clear [5:0];
 *         else cur_sum * diff > cur_diff * sum        -> step back
 *              (-1, or clear the step bit when [3:0] is already 0);
 *         else keep.
 * The register is written after every round.  "first" and "current"
 * start at (0x8000, 0xffffffff) and all arithmetic is unsigned 32-bit.
 *
 * With merge set (op 10), the register's previous value is saved and
 * zeroed before training and afterwards merged with the result as two
 * sign/magnitude codes ([5] valid, [4] sign, [3:0] magnitude): average
 * of the magnitudes for equal signs, half the difference with the larger
 * one's sign otherwise (0 when they cancel), half the one valid code if
 * only one is valid, the result unchanged if neither is.
 *
 * Log (header+0x0c bit30): avg_a | 0x20000000, avg_b | 0x30000000 per
 * round, then 0x70654321, which the next op 7 overwrites.
 */
static void script_train(u32 w0, u32 w1, u32 lane_base, int merge,
                         int log_en, u32 **logp)
{
    u32 busy = 1u << (w0 & 0xf);
    u32 cmd_bit = 1u << ((w0 >> 4) & 0xf);
    u32 sel_pos = (w0 >> 8) & 0xf;
    u32 sel = (w0 >> 12) & 1;
    u32 step_bit = 1u << ((w0 >> 16) & 0xf);
    u32 dir_pos = (w0 >> 20) & 0x7;
    u32 dir = (w0 >> 23) & 1;
    u32 n = w0 >> 28;
    u32 cmd_a = cmd_bit | (sel << sel_pos);
    u32 cmd_b = cmd_bit | ((sel ^ 1) << sel_pos);
    u32 dir_up = dir << dir_pos;            /* used when avg_b > avg_a */
    u32 dir_down = (dir ^ 1) << dir_pos;    /* used when avg_b < avg_a */
    u32 reg = lane_base + ((w1 & 0xff) << 2);
    u32 reg_a = lane_base + (((w1 >> 16) & 0xff) << 2);
    u32 reg_b = lane_base + ((w1 >> 24) << 2);
    u32 saved = 0;
    u32 first_sum = TRAIN_INIT_SUM, first_diff = TRAIN_INIT_DIFF;
    u32 cur_sum = TRAIN_INIT_SUM, cur_diff = TRAIN_INIT_DIFF;
    u32 *log = *logp;
    int round;

    if (merge == 1) {
        saved = REG32(reg);
        REG32(reg) = 0;
    }

    for (round = 0; round < TRAIN_ROUNDS; round++) {
        u32 sum_a = 0, sum_b = 0, avg_a = 0, avg_b = 0;
        u32 k, v, sum, diff, dir_val;

        DDR_CTRL(DDR_TRAIN_CMD) = 0;
        DDR_CTRL(DDR_TRAIN_CMD) = cmd_b;
        udelay(n);

        for (k = n; k != 0; k--) {
            DDR_CTRL(DDR_TRAIN_CMD) = cmd_a;
            udelay(TRAIN_PULSE_US);
            while (DDR_CTRL(DDR_TRAIN_STS) & busy)
                ;
            sum_a += train_read_stable(reg_a, reg_b);

            DDR_CTRL(DDR_TRAIN_CMD) = cmd_b;
            udelay(TRAIN_PULSE_US);
            while (DDR_CTRL(DDR_TRAIN_STS) & busy)
                ;
            sum_b += train_read_stable(reg_a, reg_b);
        }

        while (n < sum_a) {
            sum_a -= n;
            avg_a++;
        }
        while (n < sum_b) {
            sum_b -= n;
            avg_b++;
        }

        if (log_en) {
            log[0] = avg_a | TRAIN_LOG_A;
            log[1] = avg_b | TRAIN_LOG_B;
            log += 2;
        }

        v = REG32(reg);
        if (avg_b == avg_a)
            break;
        if (avg_b > avg_a) {
            diff = avg_b - avg_a;
            dir_val = dir_up;
        } else {
            diff = avg_a - avg_b;
            dir_val = dir_down;
        }
        sum = avg_b + avg_a;

        if (!(v & step_bit)) {
            v = step_bit | dir_val | (v & ~TRAIN_CODE_MASK);
            first_sum = sum;
            first_diff = diff;
        } else if (((dir_val ^ v) & TRAIN_CODE_SIGN) == 0) {
            v += 1;
        } else {
            /* sign flipped: pick a step and stop */
            u32 new_ratio = cur_sum * diff;
            u32 cur_ratio = cur_diff * sum;
            u32 first_x_cur = first_sum * cur_diff;
            u32 cur_x_first = first_diff * cur_sum;

            if (cur_x_first < first_x_cur)
                v &= ~TRAIN_CODE_MASK;
            else if (new_ratio > cur_ratio)
                v = (v & TRAIN_CODE_MAG) ? v - 1 : v & ~step_bit;
            REG32(reg) = v;
            break;
        }
        cur_diff = diff;
        cur_sum = sum;
        REG32(reg) = v;
    }

    DDR_CTRL(DDR_TRAIN_CMD) = 0;

    if (merge == 1) {
        u32 now = REG32(reg);

        if (saved & TRAIN_CODE_EN) {
            u32 x = saved ^ now;

            if (!(now & TRAIN_CODE_EN))
                now = ((saved >> 1) & 7) | (saved & TRAIN_CODE_HI);
            else if (!(x & TRAIN_CODE_SIGN))
                now = (now & TRAIN_CODE_HI) |
                      (((now & TRAIN_CODE_MAG) + (saved & TRAIN_CODE_MAG)) >> 1);
            else if (!(x & TRAIN_CODE_MAG))
                now = 0;
            else if ((saved & TRAIN_CODE_MAG) > (now & TRAIN_CODE_MAG))
                now = (saved & TRAIN_CODE_HI) |
                      (((saved & TRAIN_CODE_MAG) - (now & TRAIN_CODE_MAG)) >> 1);
            else
                now = (now & TRAIN_CODE_HI) |
                      (((now & TRAIN_CODE_MAG) - (saved & TRAIN_CODE_MAG)) >> 1);
        } else if (now & TRAIN_CODE_EN) {
            now = ((now >> 1) & 7) | (now & TRAIN_CODE_HI);
        }
        REG32(reg) = now;
    }

    if (log_en)
        *log = HDR_TRAIN_LOG_END;
    *logp = log;
}

/*
 * 0x18e0  hdr_script_run: run the header's register script (above).
 *
 * The 0xAA55 tag at header+0x30 is checked first; on a mismatch
 * boot_fail(9) is called -- and should it ever return, the script runs
 * anyway.  CG+0 bit3 is cleared before the first entry.  The log pointer
 * is header+0x0c with bits 29:28 cleared (bits 31:30 stay in the
 * address; bit30 is also the enable).
 */
void hdr_script_run(const u32 *script)
{
    const struct loader_hdr *hdr =
        (const struct loader_hdr *)((const u8 *)script - HDR_SCRIPT_PTR);
    u32 log_word = hdr->log;
    int log_en = (log_word >> 30) & 1;
    u32 *log = (u32 *)(log_word & ~HDR_LOG_ADDR_CLR);
    volatile u32 *poll_reg;
    u32 sw, lane_base, count, i;
    u32 trim_a = 0, trim_b = 0;     /* op 8 -> op 9 */
    int merge = 0;                  /* op 10 -> op 7 */
    const u32 *p;

    if (hdr->tag != LOADER_TAG)
        boot_fail(BOOT_FAIL_CHECKSUM);

    poll_reg = (volatile u32 *)hdr->poll_reg;
    sw = script[0];
    lane_base = script[1];          /* header+0x3c */

    REG32(CG_BASE + CG_CTRL0) &= ~CG_CTRL0_SCRIPT_CLR;

    count = sw & HDR_SW_COUNT_MASK;
    p = script + 2;                 /* header+0x40 */

    for (i = 0; i != count; i++) {
        u32 w0 = p[0];
        u32 op = (w0 >> HDR_OP_SHIFT) & 0xf;
        u32 addr = w0 & ~HDR_OP_MASK;
        const u32 *next = p + 1;

        switch (op) {
        case HDR_OP_POLL: {
            u32 w1 = p[1];
            u32 nbits = (w1 >> 20) & 0xf;
            u32 mask = 0, want = 0, b;

            next = p + 2;
            for (b = 0; b != nbits; b++) {
                u32 pos = arm_lsr(w1, 5 * b) & 31;

                mask |= 1u << pos;
                want |= (arm_lsr(w1, 24 + b) & 1) << pos;
            }
            while ((REG32(addr) & mask) != want)
                ;
            break;
        }

        case HDR_OP_WRITE:
            next = p + 2;
            REG32(addr) = p[1];
            break;

        case HDR_OP_DELAY:
            udelay(w0 & 0x00ffffff);
            break;

        case HDR_OP_WRITE_BYTES: {
            u32 v = p[1];

            next = p + 2;
            REG32(addr + 0x0) = v & 0xff;
            REG32(addr + 0x4) = (v >> 8) & 0xff;
            REG32(addr + 0x8) = (v >> 16) & 0xff;
            REG32(addr + 0xc) = v >> 24;
            break;
        }

        case HDR_OP_CMD_POLL: {
            /* command: bit0 | bit w0[3:0] (| w0[22:4] << 12 for cmd 2);
               then wait for bit w0[31:28] to equal w0 bit23 */
            u32 cmd = w0 & 0xf;
            u32 bit = w0 >> 28;
            u32 v = (1u << cmd) | 1;
            u32 want;

            if (cmd == 2)
                v |= (w0 << 8) & SCRIPT_CMD2_ARG_MASK;
            *poll_reg = v;
            want = (w0 & BIT(23)) >> (23 - bit);
            while ((*poll_reg & (1u << bit)) != want)
                ;
            break;
        }

        case HDR_OP_LANE:
            next = p + 2;
            script_write_lanes(lane_base + ((w0 & 0xfff) << 2), w0, p[1]);
            break;

        case HDR_OP_DDR_PHY:
            next = p + 2;
            script_write_lanes(DDR_PHY_BASE + DDR_PHY_REG(w0 & DDR_PHY_IDX_MASK),
                               w0, p[1]);
            break;

        case HDR_OP_TRAIN:
            next = p + 2;
            if (sw & HDR_SW_TRAIN)
                script_train(w0, p[1], lane_base, merge, log_en, &log);
            break;

        case HDR_OP_TRIM_LOAD: {
            /* efuse word: bit15 valid, [5:0] trim A, [11:6] trim B;
               bit14 forces both to 0 */
            u32 e = efuse_read(w0 & 31);

            if (e & BIT(15)) {
                trim_a = e & 0x3f;
                trim_b = (e >> 6) & 0x3f;
            }
            if (e & BIT(14))
                trim_a = trim_b = 0;
            break;
        }

        case HDR_OP_TRIM_APPLY: {
            /* lanes lane_base + 4 * w0[7:0], w0[15:8] of them; trim B if
               w0 bit29 else A; bit30: odd lanes only, bit31: even lanes
               only (both: none).  Trim bit5 = subtract. */
            u32 lanes = lane_base + ((w0 & 0xff) << 2);
            u32 nlanes = (w0 >> 8) & 0xff;
            u32 trim = (w0 & BIT(29)) ? trim_b : trim_a;
            u32 j;

            for (j = 0; j != nlanes; j++) {
                s32 v;

                if ((w0 & BIT(30)) && !(j & 1))
                    continue;
                if ((w0 & BIT(31)) && (j & 1))
                    continue;
                v = (s32)REG32(lanes + 4 * j);
                if (trim & TRIM_NEG) {
                    v -= trim & TRIM_MAG;
                    if (v < 0)
                        v = 0;
                } else {
                    v += trim & TRIM_MAG;
                    if (v >= TRIM_MAX)
                        v = TRIM_MAX;
                }
                REG32(lanes + 4 * j) = (u32)v;
            }
            break;
        }

        case HDR_OP_FLAG:
            merge = w0 & 1;
            break;

        default:
            break;
        }
        p = next;
    }
}
