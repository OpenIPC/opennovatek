/*
 * timer.c -- the ROM's one time base: TIMER 0 at 0xF0040000, a 1 us
 * up-counter (inferred from the 1/4/5/30 s constants).
 *
 *   func_038 0x277c timer_start    func_040 0x27dc timer_stop
 *   func_039 0x27c4 timer_now      func_041 0x2824 udelay
 *
 * There is only one counter and every timeout in the ROM is "elapsed
 * since timer_start", so timed sections never nest: udelay() inside a
 * timed loop restarts the caller's count (the boot paths are written
 * around that).  timer_start saves the two enable words it touches in
 * SRAM and timer_stop puts them back, which also drops the timer clock
 * again if it was off before.
 */
#include "na51089-regmap.h"
#include "sram.h"

/*
 * 0x277c  timer_start: save CG+0x74 and turn the timer clock on, save
 * TIMER+0x30 and enable timer 0, then (re)start the count from 0.
 */
void timer_start(void)
{
    u32 v;

    v = REG32(CG_BASE + CG_CLKEN0);
    g_saved_cg74 = v;
    REG32(CG_BASE + CG_CLKEN0) = v | CG_CLKEN0_TIMER;

    v = REG32(TIMER_BASE + TIMER_EN);
    g_saved_tmr30 = v;
    REG32(TIMER_BASE + TIMER_EN) = v | TIMER_EN_T0;

    REG32(TIMER_BASE + TIMER_CTRL) = TIMER_CTRL_START;
}

/* 0x27c4  timer_now: microseconds since timer_start. */
u32 timer_now(void)
{
    return REG32(TIMER_BASE + TIMER_CNT);
}

/*
 * 0x27dc  timer_stop: stop, wait for the control word to read back 0,
 * then restore TIMER+0x30 and CG+0x74 from the copies timer_start made.
 */
void timer_stop(void)
{
    REG32(TIMER_BASE + TIMER_CTRL) = TIMER_CTRL_STOP;
    while (REG32(TIMER_BASE + TIMER_CTRL) != 0)
        ;
    REG32(TIMER_BASE + TIMER_EN) = g_saved_tmr30;
    REG32(CG_BASE + CG_CLKEN0) = g_saved_cg74;
}

/*
 * 0x2824  udelay: busy-wait until the counter reaches us.  The original
 * reads TIMER+0x118 inline (timer_now inlined).
 */
void udelay(u32 us)
{
    timer_start();
    while (timer_now() < us)
        ;
    timer_stop();
}
