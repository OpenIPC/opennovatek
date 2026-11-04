/*
 * regmap-ddr.h -- DRAM controller 0xC0001000 / PHY 0xC0021000, touched
 * only by hdr_script_run ops 6 and 7.  Ops 0/1/3/4/5 write whatever
 * addresses the loader header supplies.  Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_DDR_H
#define BOOTROM_NA51089_REGMAP_DDR_H

/* offsets from DDR_CTRL_BASE */
#define DDR_TRAIN_CMD           0x30c           /* 0xC000130C, script op 7 */
#define DDR_TRAIN_STS           0x310           /* 0xC0001310, polled */

/* offsets from DDR_PHY_BASE: op 6 writes DDR_PHY_BASE + (w0[11:0] << 2) */
#define DDR_PHY_REG(n)          ((n) << 2)
#define DDR_PHY_IDX_MASK        0xfff

#endif
