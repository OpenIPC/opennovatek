/*
 * regmap-wdt.h -- WDT 0xF0050000, plus the unidentified word at
 * 0xF0060018.  Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_WDT_H
#define BOOTROM_NA51089_REGMAP_WDT_H

/* +0x000 w: control, key 0x5A96 in [31:16]; timeout presumably [15:8]
   (inferred) */
#define WDT_CTRL                0x000
#define WDT_CTRL_DISABLE        0x5A960F00      /* next_boot_source */
#define WDT_CTRL_ARM_CFG        0x5A961042      /* wdt_arm_or_skip: store, */
#define WDT_CTRL_ARM_EN         0x5A960001      /* then |= this */

/* +0x080 r: [15:8] watchdog reset count/flag ('J', 100 ms flash reset) */
#define WDT_STS                 0x080
#define WDT_STS_RSTCNT_MASK     0xff00

/* UNK60 0xF0060000 +0x018 w: = 1 once at reset (block not identified) */
#define UNK60_REG18             0x018
#define UNK60_REG18_VAL         1

#endif
