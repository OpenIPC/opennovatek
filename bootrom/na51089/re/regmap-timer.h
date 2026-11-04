/*
 * regmap-timer.h -- TIMER 0xF0040000.  One free-running up-counter, 1 us
 * per tick (inferred from the 1/4/5/30 s constants).
 * Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_TIMER_H
#define BOOTROM_NA51089_REGMAP_TIMER_H

#define TIMER_EN                0x030           /* rmw */
#define TIMER_EN_T0             BIT(16)         /* enable (reset, timer_start) */
#define TIMER_CLKSEL            0x100           /* w */
#define TIMER_CLKSEL_VAL        3               /* clock/prescale select (inferred) */
#define TIMER_CTRL              0x110           /* w/p */
#define TIMER_CTRL_START        1               /* (re)start from 0 */
#define TIMER_CTRL_STOP         0               /* stop; poll until it reads 0 */
#define TIMER_CNT               0x118           /* r: counter, us */

#endif
