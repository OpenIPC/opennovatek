/*
 * na51089-regmap.h -- umbrella register map of the NA51089 (NT98560/561/
 * 562/566) mask ROM reconstruction.
 *
 * Block base addresses live here; per-block register offsets and bit
 * names live in regmap-<block>.h, which this file includes.  Every name
 * maps to an access the ROM actually performs (scratchpad/regmap.txt);
 * nothing else is described.  "(inferred)" means the meaning comes only
 * from how the ROM uses the register.
 *
 * Usable from C and from assembly (.S through cpp): constants are plain
 * integers, C-only parts are guarded by __ASSEMBLER__.  Bit macros use
 * BIT(n), which is unsigned in C and a plain shift in assembly.
 *
 * Convention: <BLOCK>_<REG> is a byte offset from <BLOCK>_BASE,
 * <BLOCK>_<REG>_<FIELD> a bit or mask in that register.  Access:
 *     REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_UART0;
 */
#ifndef BOOTROM_NA51089_REGMAP_H
#define BOOTROM_NA51089_REGMAP_H

#ifdef __ASSEMBLER__
#define U32C(x)         x
#else
#define U32C(x)         x##u
#endif
#define BIT(n)          (U32C(1) << (n))

/* ---- the ROM itself ------------------------------------------------- */
#define ROM_BASE        0x00000000      /* run (link) address */
#define ROM_HOME        0x80000000      /* same 32 KiB, never referenced */
#define ROM_SIZE        0x00008000
#define ROM_EXPORT_BASE 0x00007FB8      /* service table, see export.S */

/* ---- MMIO blocks, in address order --------------------------------- */
#define DDR_CTRL_BASE   0xC0001000      /* DRAM controller (script op 7) */
#define DDR_PHY_BASE    0xC0021000      /* DRAM PHY lanes (script op 6) */
#define TOP_BASE        0xF0010000      /* straps, pinmux, chip id */
#define TOP2_BASE       0xF0011000      /* bit25 around RSA (inferred) */
#define CG_BASE         0xF0020000      /* clock generator / resets */
#define PAD_BASE        0xF0030000      /* pad drive/pull (inferred) */
#define TIMER_BASE      0xF0040000
#define WDT_BASE        0xF0050000
#define UNK60_BASE      0xF0060000      /* not identified, +0x18 = 1 */
#define GPIO_BASE       0xF0070000
#define UART0_BASE      0xF0290000      /* console, 16550-style */
#define ETH_MAC_BASE    0xF02B0000      /* Synopsys DWC EQOS MAC + MTL */
#define ETH_DMA_BASE    0xF02B1000      /* EQOS DMA */
#define ETH_GLUE_BASE   0xF02B3000      /* Novatek glue + internal PHY */
#define UART1_BASE      0xF0300000
#define SPI_BASE        0xF0400000      /* SPI NOR/NAND controller */
#define SDIO1_BASE      0xF0420000      /* SD card */
#define SDIO3_BASE      0xF0510000      /* eMMC, same layout as SDIO1 */
#define AES_BASE        0xF0620000
#define OTP_BASE        0xF0660000      /* efuse */
#define HASH_BASE       0xF0670000      /* SHA-256 */
#define TRNG_BASE       0xF0680000      /* (inferred) */
#define RSA_BASE        0xF06A0000
#define MISC_FF0_BASE   0xF0FF0000      /* not identified, see ETH_FF0_CTRL */
#define USB_BASE        0xFF600000      /* FOTG210-style OTG */
#define USB_DEV_BASE    0xFF600100      /* device registers (g_usb_regs) */

#include "regmap-top.h"
#include "regmap-cg.h"
#include "regmap-pad.h"
#include "regmap-timer.h"
#include "regmap-wdt.h"
#include "regmap-gpio.h"
#include "regmap-uart.h"
#include "regmap-spi.h"
#include "regmap-sdio.h"
#include "regmap-eqos.h"
#include "regmap-crypto.h"
#include "regmap-otp.h"
#include "regmap-ddr.h"
#include "regmap-usb.h"

#ifndef __ASSEMBLER__

/* Volatile MMIO accessors: REG32(addr) is an lvalue. */
#define REG32(a)        (*(volatile unsigned int *)(a))
#define REG16(a)        (*(volatile unsigned short *)(a))
#define REG8(a)         (*(volatile unsigned char *)(a))

/*
 * Raw console stores.  Most letters the ROM prints are a single `strb`
 * to UART0 THR with no FIFO/LSR check (boot_fail, the boot paths, the
 * secure-boot letters, put_hex32).  Use these for those call sites;
 * uart0_putc() (FIFO wait) only where the original calls it (uart_boot).
 * The flavour is visible on the wire: keep each call site's choice.
 * start.S's 'T' is a word `str`, not `strb` (bootrom.txt 0x2bc).
 */
#define UART0_PUTC_RAW(c)  (REG8(UART0_BASE + UART_THR) = (unsigned char)(c))
#define UART1_PUTC_RAW(c)  (REG8(UART1_BASE + UART_THR) = (unsigned char)(c))

#endif /* !__ASSEMBLER__ */

#endif /* BOOTROM_NA51089_REGMAP_H */
