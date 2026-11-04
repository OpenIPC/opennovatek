/*
 * boot.c -- the boot-source fallback chain and the pre-jump cleanup.
 *
 *   func_027 0x1748 prejump_restore    func_030 0x18b4 boot_fail
 *   func_029 0x180c next_boot_source   func_032 0x20b4 wdt_arm_or_skip
 *
 * Fallback is by nested calls, never by return: a boot path that fails
 * calls boot_fail(code), which prints "K<code>" and calls
 * next_boot_source(), which calls the next path, and so on.  The stack is
 * never unwound (bootrom.txt "Notes on the reset path").
 */
#include "na51089-regmap.h"
#include "sram.h"

#define DLY_10MS        10000   /* wdt_arm_or_skip, OTP_CFG_DELAY10MS */

/*
 * 0x1748  prejump_restore: called by run_loader just before the jump.
 * For eMMC (BS 8/9) it does nothing, so the loader inherits the SDIO3
 * clock and pads as emmc_boot left them.  Otherwise the storage clocks,
 * dividers and pads go back to their reset defaults:
 *   CG+0x20/0x24/0x3c/0x40 = defaults,
 *   SPI, SDIO1, SDIO3 and ETH reset bits of CG+0x84 cleared and then the
 *   original word written back (one pulse; a block that was held in reset
 *   stays there),
 *   SPI, SDIO1, SDIO3 clocks off (three separate read-modify-writes),
 *   PAD+0x68/0x40/0x98 = defaults.
 * The ETH clock (CG+0x74 bit29) is left on.
 */
void prejump_restore(void)
{
    u32 rst;

    if (g_boot_source - BS_EMMC_4BIT <= 1)      /* BS 8 or 9 */
        return;

    REG32(CG_BASE + CG_SD_CTRL) = CG_SD_CTRL_DEFAULT;
    REG32(CG_BASE + CG_EMMC_CTRL) = CG_EMMC_CTRL_DEFAULT;
    REG32(CG_BASE + CG_SDIO1_DIV) = CG_SDIO1_DIV_DEFAULT;
    REG32(CG_BASE + CG_DIV40) = CG_DIV40_DEFAULT;

    rst = REG32(CG_BASE + CG_RST84);
    REG32(CG_BASE + CG_RST84) = rst & ~CG_RST84_PREJUMP;
    REG32(CG_BASE + CG_RST84) = rst;

    REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SPI;
    REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SDIO1;
    REG32(CG_BASE + CG_CLKEN0) &= ~CG_CLKEN0_SDIO3;

    REG32(PAD_BASE + PAD_SD) = PAD_SD_DEFAULT;
    REG32(PAD_BASE + PAD_FLASH) = PAD_FLASH_DEFAULT;
    REG32(PAD_BASE + PAD_ETH) = PAD_ETH_DEFAULT;
}

/*
 * 0x180c  next_boot_source: watchdog clock on and the watchdog disabled,
 * then pick the next source from the current one and call it:
 *
 *   BS 0,2,3,6,8,9,10,12 (flash, eMMC, UART) -> SD
 *   BS 1 (SD), 7, 13..15                     -> Ethernet
 *   BS 4 (Ethernet)                          -> USB, as usb_boot(5): HS
 *   BS 5, 11 (USB)                           -> UART
 *
 * i.e. flash/eMMC -> SD -> ETH -> USB -> UART -> SD -> ...  The callee
 * does not come back in practice; if it did, this returns.
 */
void next_boot_source(void)
{
    u32 bs, m;

    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_WDT;
    REG32(WDT_BASE + WDT_CTRL) = WDT_CTRL_DISABLE;

    bs = g_boot_source;
    if (bs <= BS_UART) {
        m = 1u << bs;
        if (m & FALLBACK_TO_SD_MASK) {
            g_boot_source = BOOT_SRC_SD;
            sd_boot();
            return;
        }
        if (m & FALLBACK_TO_UART_MASK) {
            g_boot_source = BOOT_SRC_UART;
            uart_boot();
            return;
        }
        if (m & FALLBACK_TO_USB_MASK) {
            g_boot_source = BOOT_SRC_USB;
            usb_boot(BOOT_SRC_USB);
            return;
        }
    }
    g_boot_source = BOOT_SRC_ETH;
    eth_boot();
}

/*
 * 0x18b4  boot_fail: "K" and '0' + code as two raw THR byte stores (no
 * FIFO check), so code 10 (timeout) prints "K:".  Then the next source.
 */
void boot_fail(int code)
{
    UART0_PUTC_RAW(LTR_FAIL);
    UART0_PUTC_RAW('0' + code);
    next_boot_source();
}

/*
 * 0x20b4  wdt_arm_or_skip: called first by flash_boot and emmc_boot.
 *
 *   TOP+0 bit12 (BST[12]) set: print 'W', leave the watchdog alone.
 *   Otherwise: watchdog clock on; if WDT+0x80[15:8] != 0 the previous
 *   reset came from the watchdog -- print 'J' and skip internal storage
 *   by calling next_boot_source() directly; else arm the watchdog
 *   (0x5A961042, then |= 0x5A960001), so a loader that hangs gets the
 *   board reset into the 'J' branch next time.
 *
 * Every path that comes back here then waits 10 ms if OTP+0x50 bit8 is
 * set -- including the 'J' path, should next_boot_source() return.
 */
void wdt_arm_or_skip(void)
{
    if (REG32(TOP_BASE + TOP_STRAP) & TOP_STRAP_NO_WDT) {
        UART0_PUTC_RAW(LTR_NO_WDT);
    } else {
        REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_WDT;
        if (REG32(WDT_BASE + WDT_STS) & WDT_STS_RSTCNT_MASK) {
            UART0_PUTC_RAW(LTR_WDT_RESET);
            next_boot_source();
        } else {
            REG32(WDT_BASE + WDT_CTRL) = WDT_CTRL_ARM_CFG;
            REG32(WDT_BASE + WDT_CTRL) |= WDT_CTRL_ARM_EN;
        }
    }

    if (otp_boot_cfg() & OTP_CFG_DELAY10MS)
        udelay(DLY_10MS);
}
