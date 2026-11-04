/*
 * start.S -- 0x0000..0x031f of the NA51089 mask ROM: exception vectors,
 * reset handler, strap dispatch and the loader check/jump.
 *
 * The original is hand-written assembly (no frame pointers, one shared
 * literal pool at 0x2e4), so this file is too.  It is laid out in the
 * same order as the ROM; addresses in the comments are the original's.
 * The literal pool is written out explicitly (lit_* below) instead of
 * `ldr rX, =const`, so the assembler cannot fold a constant into a movw
 * and the pool keeps the ROM's order.
 *
 * Calls out to C: uart0_init_banner, udelay, secure_boot_verify,
 * prejump_restore, boot_fail, and through the dispatch table flash_boot,
 * sd_boot, eth_boot, usb_boot, emmc_boot, uart_boot.
 *
 * Nothing here clears or copies RAM: the ROM has no .data/.bss (sram.h).
 */
#include "arm-asm.h"
#include "na51089-regmap.h"
#include "sram.h"

    .syntax unified
    .arm
    .text

/* ---------------------------------------------------------------------
 * 0x0000  vectors: `ldr pc, [pc, #..]` through the address words at 0x20.
 * The 0x14 slot (reserved / hyp trap) is a plain nop that falls into the
 * IRQ vector at 0x18.
 * ------------------------------------------------------------------- */
    .global _start
_start:
    ldr     pc, vec_reset               @ 0x00 reset
    ldr     pc, vec_undef               @ 0x04 undefined instruction
    ldr     pc, vec_swi                 @ 0x08 SVC
    ldr     pc, vec_pabt                @ 0x0c prefetch abort
    ldr     pc, vec_dabt                @ 0x10 data abort
    nop                                 @ 0x14 reserved: nop, falls into 0x18
    ldr     pc, vec_irq                 @ 0x18 IRQ
    ldr     pc, vec_fiq                 @ 0x1c FIQ

vec_reset:  .word   reset_handler       @ 0x20 = 0x054
vec_undef:  .word   exc_undef           @ 0x24 = 0x03c
vec_swi:    .word   exc_swi             @ 0x28 = 0x040
vec_pabt:   .word   exc_pabt            @ 0x2c = 0x044
vec_dabt:   .word   exc_dabt            @ 0x30 = 0x048
vec_irq:    .word   exc_irq             @ 0x34 = 0x04c
vec_fiq:    .word   exc_fiq             @ 0x38 = 0x050

/* ---------------------------------------------------------------------
 * func_000 0x003c  exc_hang: six one-instruction `b .` stubs, one per
 * exception.  There are no handlers; IRQ/FIQ are never unmasked, so in
 * practice only an undefined instruction or an abort ends up here, and
 * the core spins forever.
 * ------------------------------------------------------------------- */
FUNC_BEGIN exc_hang
exc_undef:  b       exc_undef           @ 0x3c
exc_swi:    b       exc_swi             @ 0x40
exc_pabt:   b       exc_pabt            @ 0x44
exc_dabt:   b       exc_dabt            @ 0x48
exc_irq:    b       exc_irq             @ 0x4c
exc_fiq:    b       exc_fiq             @ 0x50
FUNC_END exc_hang

/* ---------------------------------------------------------------------
 * func_001 0x0054  reset_handler
 *
 * SVC mode with IRQ/FIQ masked; caches, alignment checking and the MMU
 * off; timer clock and timer 0 enabled and its clock select set; the
 * unidentified word 0xF0060018 = 1; sp; CG+0x70 bit16 set and CG+0x18
 * bit26 cleared; console up ("N").
 *
 * Then an optional reset pulse for the boot flash, done before the
 * dispatch so the part comes out of whatever state a warm reset left it
 * in:
 *   BS 0 (SPI NOR) and OTP+0x50 bit9: GPIO0 pin 9
 *   BS 8/9 (eMMC), always:            GPIO0 pin 10
 * The pin is driven low for 1 ms -- 101 ms when WDT+0x80[15:8] says the
 * last reset came from the watchdog -- then high for 1 ms, then released
 * to input.  The GPIO direction word is *stored*, not read-modify-
 * written: every other pin of bank 0 is made an input as a side effect.
 *
 * Falls through into boot_dispatch.
 * ------------------------------------------------------------------- */
FUNC_BEGIN reset_handler
    mrs     r0, cpsr
    bic     r0, r0, #0x1f               @ mode bits
    orr     r0, r0, #0xd3               @ SVC (0x13), I+F masked (0xc0)
    msr     cpsr_fc, r0

    mrc     p15, 0, r0, c1, c0, 0       @ SCTLR
    bic     r0, r0, #(1 << 12)          @ I: I-cache off
    bic     r0, r0, #(1 << 2)           @ C: D-cache off
    bic     r0, r0, #(1 << 1)           @ A: alignment check off
    bic     r0, r0, #(1 << 0)           @ M: MMU off
    mcr     p15, 0, r0, c1, c0, 0

    ldr     r0, lit_cg                  @ 0x7c
    ldr     r1, [r0, #CG_CLKEN0]
    mov     r2, #1
    orr     r1, r1, r2, lsl #18         @ CG_CLKEN0_TIMER
    str     r1, [r0, #CG_CLKEN0]

    ldr     r0, lit_timer               @ 0x90
    ldr     r1, [r0, #TIMER_EN]
    orr     r1, r1, r2, lsl #16         @ TIMER_EN_T0
    str     r1, [r0, #TIMER_EN]
    mov     r2, #TIMER_CLKSEL_VAL
    str     r2, [r0, #TIMER_CLKSEL]     @ TIMER+0x100 = 3

    ldr     r1, lit_unk60               @ 0xa8
    mov     r0, #UNK60_REG18_VAL
    str     r0, [r1, #UNK60_REG18]      @ *(0xF0060018) = 1, purpose unknown

    ldr     sp, lit_stack               @ 0xb4: the only stack, full-descending

    ldr     r1, lit_cg                  @ 0xb8
    ldr     r0, [r1, #CG_CTRL70]
    orr     r0, r0, #CG_CTRL70_BIT16
    str     r0, [r1, #CG_CTRL70]
    ldr     r0, [r1, #CG_RST0]
    bic     r0, r0, #CG_RST0_BIT26
    str     r0, [r1, #CG_RST0]

    bl      uart0_init_banner           @ 0xd4: 115200 8N1, prints "N"

    ldr     r1, lit_top                 @ 0xd8
    ldr     r0, [r1, #TOP_STRAP]
    and     r0, r0, #TOP_STRAP_BS_MASK
    cmp     r0, #BS_SPI_NOR
    bne     .Lemmc_check

    /* BS 0: NOR reset pulse on GPIO0 pin 9, only if OTP asks for it */
    ldr     r1, lit_otp                 @ 0xec
    ldr     r2, [r1, #OTP_BOOT_CFG]
    and     r2, r2, #OTP_CFG_NOR_RESET
    cmp     r2, #OTP_CFG_NOR_RESET
    bne     boot_dispatch

    ldr     r1, lit_wdt                 @ 0x100
    ldr     r0, [r1, #WDT_STS]
    and     r0, r0, #WDT_STS_RSTCNT_MASK @ r0 != 0: last reset was the WDT
    ldr     r4, lit_gpio
    mov     r5, #GPIO0_NOR_RESET
    str     r5, [r4, #GPIO_DIR0]        @ pin 9 output (whole bank written)
    str     r5, [r4, #GPIO_CLR0]        @ low
    cmp     r0, #0
    beq     1f
    ldr     r0, lit_100ms
    bl      udelay                      @ udelay(100000) after a WDT reset
1:  mov     r0, #1000
    bl      udelay                      @ udelay(1000)
    str     r5, [r4, #GPIO_SET0]        @ high
    mov     r0, #1000
    bl      udelay
    mov     r0, #0
    str     r0, [r4, #GPIO_DIR0]        @ back to input
    /* r0 is now 0, so the eMMC test below falls through to the dispatch */

.Lemmc_check:                           @ 0x148
    cmp     r0, #BS_EMMC_4BIT
    beq     .Lemmc_reset
    cmp     r0, #BS_EMMC_8BIT
    beq     .Lemmc_reset
    bne     boot_dispatch               @ (unconditional in effect)

    /* BS 8/9: the same pulse on GPIO0 pin 10, with no OTP condition */
.Lemmc_reset:                           @ 0x15c
    ldr     r1, lit_wdt
    ldr     r0, [r1, #WDT_STS]
    and     r0, r0, #WDT_STS_RSTCNT_MASK
    ldr     r4, lit_gpio
    mov     r5, #GPIO0_EMMC_RESET
    str     r5, [r4, #GPIO_DIR0]        @ pin 10 output (whole bank written)
    str     r5, [r4, #GPIO_CLR0]        @ low
    cmp     r0, #0
    beq     1f
    ldr     r0, lit_100ms
    bl      udelay
1:  mov     r0, #1000
    bl      udelay
    str     r5, [r4, #GPIO_SET0]        @ high
    mov     r0, #1000
    bl      udelay
    mov     r0, #0
    str     r0, [r4, #GPIO_DIR0]        @ input
    /* falls through */
FUNC_END reset_handler

/* ---------------------------------------------------------------------
 * func_002 0x01a4  boot_dispatch: re-read the strap, record it in
 * g_boot_source (next_boot_source() works from it) and jump through the
 * 16-entry table.  It is a jump, not a call: the boot_* routine runs with
 * whatever lr the last `bl` left (0xd8 after uart0_init_banner, or the
 * return point of the last udelay of a reset pulse).  None of them
 * normally returns; flash_boot's "NAND scan found nothing" return 0 lands
 * back in reset_handler and re-runs the dispatch (bootrom.txt).
 * ------------------------------------------------------------------- */
FUNC_BEGIN boot_dispatch
    ldr     r1, lit_top                 @ 0x1a4
    ldr     r0, [r1, #TOP_STRAP]
    ldr     r2, lit_boot_source
    and     r0, r0, #TOP_STRAP_BS_MASK
    str     r0, [r2]                    @ g_boot_source = BS
    ldr     r1, lit_dispatch
    ldr     pc, [r1, r0, lsl #2]        @ 0x1bc: pc = dispatch_table[BS]

dispatch_table:                         @ 0x1c0
    .word   flash_boot                  @ 0x0 SPI NOR
    .word   sd_boot                     @ 0x1 SD (SDIO1)
    .word   flash_boot                  @ 0x2 SPI NAND on-die ECC, 2K
    .word   flash_boot                  @ 0x3 SPI NAND RS ECC, 2K
    .word   eth_boot                    @ 0x4 Ethernet
    .word   usb_boot                    @ 0x5 USB high speed
    .word   flash_boot                  @ 0x6 SPI NAND on-die ECC, 4K
    .word   hang_reserved_strap         @ 0x7 reserved
    .word   emmc_boot                   @ 0x8 eMMC 4-bit
    .word   emmc_boot                   @ 0x9 eMMC 8-bit
    .word   flash_boot                  @ 0xA SPI NAND RS ECC, 4K
    .word   usb_boot                    @ 0xB USB full speed
    .word   uart_boot                   @ 0xC UART
    .word   hang_reserved_strap         @ 0xD reserved
    .word   hang_reserved_strap         @ 0xE reserved
    .word   hang_reserved_strap         @ 0xF reserved
FUNC_END boot_dispatch

/* ---------------------------------------------------------------------
 * func_003 0x0200  check_and_run_loader(int fatal, int hwsum_valid,
 *                                       u32 hwsum)
 *
 * The loader image is at load = *(0xF0800204), the load address of the
 * staged header copy.  secure_boot_verify(load) == 0 means "verified":
 * run it with no checksum at all.  Otherwise (secure boot off, or not
 * conclusive):
 *   hwsum_valid != 0  the storage controller summed the halfwords and
 *                     the caller added the index term: run if
 *                     (hwsum & 0xffff) == 0.  The 0xAA55 tag is NOT
 *                     checked on this path.
 *   hwsum_valid == 0  software sum over size/2 halfwords h[i] (size from
 *                     the STAGED header, 0xF0800224) of h[i] + i; run if
 *                     the low 16 bits are 0 and *(u16 *)(load+0x30) is
 *                     0xAA55.
 * On failure: fatal == 1 -> boot_fail(9) as a tail branch ("K9", next
 * boot source); anything else returns to the caller (NAND first-copy
 * retry).
 *
 * Quirks, kept: the halfword loop is a do-while that steps two halfwords
 * at a time and tests `i == size/2` only after both, so size/2 odd (or
 * size < 2) never matches and the loop runs off through memory until the
 * counter wraps.  Only the low 16 bits of the sum survive the `lsl #16`.
 * The hwsum path enters run_loader with `bl` where a `b` would do.
 * ------------------------------------------------------------------- */
FUNC_BEGIN check_and_run_loader
    push    {r4, r5, r6, r7, r8, lr}
    mov     r7, r0                      @ r7 = fatal
    cmp     r1, #0
    beq     .Lsw_sum

    /* hardware checksum path */
    ldr     r4, lit_ffff
    and     r5, r2, r4                  @ r5 = hwsum & 0xffff
    ldr     r6, lit_hdr_stage
    ldr     r4, [r6, #HDR_LOAD]         @ r4 = load address
    mov     r0, r4
    bl      secure_boot_verify
    cmp     r0, #0
    beq     run_loader                  @ verified
    cmp     r5, #0
    bne     .Lcheck_fail
    bl      run_loader                  @ 0x238: never returns

.Lsw_sum:                               @ 0x23c
    mov     r2, #0
    mov     r1, #0
    ldr     r6, lit_hdr_stage
    ldr     r4, [r6, #HDR_LOAD]         @ r4 = load address
    mov     r0, r4
    bl      secure_boot_verify
    cmp     r0, #0
    beq     run_loader                  @ verified
    mov     r2, #0                      @ r2 = i
    mov     r1, #0                      @ r1 = sum
    ldr     r8, [r6, #HDR_SIZE]         @ r8 = size, from the staged copy
    sub     r3, r4, #4                  @ r3 = p - 1 word
    ldr     r6, lit_ffff
1:  ldr     r0, [r3, #4]!               @ w = *++p
    and     r5, r6, r0
    add     r1, r1, r5                  @ sum += lo16(w)
    add     r1, r1, r2                  @ sum += i
    add     r2, r2, #1                  @ i++
    and     r5, r6, r0, lsr #16
    add     r1, r1, r5                  @ sum += hi16(w)
    add     r1, r1, r2                  @ sum += i
    add     r2, r2, #1                  @ i++
    cmp     r2, r8, lsr #1              @ until i == size/2
    bne     1b
    ldrh    r0, [r4, #HDR_TAG]
    orr     r0, r0, r1, lsl #16         @ tag | sum << 16
    ldr     r3, lit_tag
    cmp     r0, r3                      @ == 0x0000AA55 ?
    bne     .Lcheck_fail
    /* falls into run_loader */
FUNC_END check_and_run_loader

/* ---------------------------------------------------------------------
 * func_004 0x02b0  run_loader: put clocks/pads back (prejump_restore),
 * print 'T' and jump to header word 0.  Expects r4 = load address.  The
 * 'T' is a *word* store to THR with no FIFO check.  r0..r2 reach the
 * loader as prejump_restore left them.
 * ------------------------------------------------------------------- */
FUNC_BEGIN run_loader
    bl      prejump_restore
    ldr     r1, lit_uart0
    mov     r0, #LTR_JUMP
    str     r0, [r1, #UART_THR]         @ 'T' (str, not strb)
    ldr     r3, [r4, #HDR_ENTRY]
    mov     pc, r3                      @ enter the loader
FUNC_END run_loader

    /* 0x2c8: tail of check_and_run_loader, failure */
.Lcheck_fail:
    mov     r0, #BOOT_FAIL_CHECKSUM
    cmp     r7, #1
    beq     boot_fail                   @ fatal: "K9", tail branch
    pop     {r4, r5, r6, r7, r8, pc}    @ else return to the caller

    nop                                 @ 0x2d8 padding

/* ---------------------------------------------------------------------
 * func_005 0x02dc  hang_reserved_strap: BS 7, 0xD, 0xE, 0xF.  Spins with
 * nothing printed beyond the "N" banner.
 * ------------------------------------------------------------------- */
FUNC_BEGIN hang_reserved_strap
    b       hang_reserved_strap
FUNC_END hang_reserved_strap

    nop                                 @ 0x2e0 padding

/* ---- literal pool, 0x2e4..0x31c, in the ROM's order ------------------ */
    .align  2
lit_cg:             .word   CG_BASE             @ 0x2e4
lit_timer:          .word   TIMER_BASE          @ 0x2e8
lit_unk60:          .word   UNK60_BASE          @ 0x2ec
lit_stack:          .word   SRAM_STACK_TOP      @ 0x2f0
lit_top:            .word   TOP_BASE            @ 0x2f4
lit_otp:            .word   OTP_BASE            @ 0x2f8
lit_wdt:            .word   WDT_BASE            @ 0x2fc
lit_gpio:           .word   GPIO_BASE           @ 0x300
lit_100ms:          .word   DLY_100MS           @ 0x304
lit_boot_source:    .word   SRAM_BOOT_SOURCE    @ 0x308
lit_dispatch:       .word   dispatch_table      @ 0x30c
lit_ffff:           .word   0x0000ffff          @ 0x310
lit_hdr_stage:      .word   SRAM_HDR_STAGE      @ 0x314
lit_tag:            .word   LOADER_TAG          @ 0x318
lit_uart0:          .word   UART0_BASE          @ 0x31c
