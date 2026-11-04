/*
 * eth.c -- Ethernet hardware: the EQOS MDIO accessors (func_048
 * mdio_read, func_049 mdio_write), the single-shot transmitter (func_051
 * eth_xmit) and the hardware half of func_053 eth_boot.
 *
 * eth_boot is ONE function in the original (0x2cb4..0x3e7c, frame
 * pointer, nine saved registers).  gcc inlined both halves into it; they
 * are split here only so that two people can own them (see eth.h):
 *
 *   eth_boot()      0x2cb4   eth_hw_init(); net_run();
 *   eth_hw_init()   0x2cb4..0x3380  this file, static
 *   net_run()       0x3384..0x3e7c  net.c
 *
 * Every console letter here is a raw THR store with no FIFO wait.
 */
#include "sram.h"
#include "eth.h"

/* TODO(headers): regmap-eqos.h names the RBSZ value (0x1000) but not the
   mask the ROM clears around it: bits [14:4], `bic #0x7f00; bic #0xf0`. */
#define DMA_CH0_RX_CTRL_RBSZ_MASK       0x7ff0

/* TODO(headers): regmap-cg.h has CG_CLKEN1_TRNG as BIT(25)|BIT(28); the
   ROM sets and clears the two bits with separate read-modify-writes. */
#define CG_CLKEN1_TRNG_A                BIT(25)
#define CG_CLKEN1_TRNG_B                BIT(28)

/* TODO(headers): regmap-eqos.h has MII_LPA_100FULL = BIT(8) only; the
   speed decode tests bits 9:8 together (bit 9 is 100BASE-T4). */
#define MII_LPA_100_ANY                 (BIT(9) | BIT(8))

/* EQOS TX descriptor length fields (normal descriptor, read format) */
#define EQOS_TDES2_HL_MASK              0x3fff          /* header length */
#define EQOS_TDES3_FL_MASK              0x7fff          /* frame length */

/* the TX tail eth_hw_init leaves behind: descriptor 3 */
#define SRAM_ETH_TXDESC3                (SRAM_ETH_TXDESC + 3 * sizeof(struct eqos_desc))

/* delays, microseconds */
#define DLY_EXT_PHY_LONG                200000          /* external PHY reset */
#define DLY_EXT_PHY_SHORT               20000
#define DLY_INT_PHY_1                   20              /* internal PHY bring-up */
#define DLY_INT_PHY_2                   200
#define DLY_INT_PHY_3                   250
#define DLY_INT_PHY_4                   10000

#define MDIO_PHY_ADDRS                  32

/*
 * func_048 0x2a84: clause-22 MDIO read through the EQOS MAC.
 * Wait GB clear, start a read (CR/GOC/GB = 0xd), wait GB clear, return
 * the low 16 data bits.  No timeout: a dead MDIO bus hangs here.
 */
u16 mdio_read(int phy, int reg)
{
    while (REG32(ETH_MAC_BASE + MAC_MDIO_ADDR) & MAC_MDIO_ADDR_GB)
        ;
    REG32(ETH_MAC_BASE + MAC_MDIO_ADDR) =
        (u32)phy << MAC_MDIO_ADDR_PA_SHIFT |
        (u32)reg << MAC_MDIO_ADDR_RDA_SHIFT | MAC_MDIO_ADDR_READ;
    while (REG32(ETH_MAC_BASE + MAC_MDIO_ADDR) & MAC_MDIO_ADDR_GB)
        ;
    return REG32(ETH_MAC_BASE + MAC_MDIO_DATA) & 0xffff;
}

/*
 * func_049 0x2acc: MDIO write.  Wait GB clear, data first, then the
 * command word (0x5), then wait for completion.
 */
void mdio_write(int phy, int reg, u16 v)
{
    while (REG32(ETH_MAC_BASE + MAC_MDIO_ADDR) & MAC_MDIO_ADDR_GB)
        ;
    REG32(ETH_MAC_BASE + MAC_MDIO_DATA) = v;
    REG32(ETH_MAC_BASE + MAC_MDIO_ADDR) =
        (u32)phy << MAC_MDIO_ADDR_PA_SHIFT |
        (u32)reg << MAC_MDIO_ADDR_RDA_SHIFT | MAC_MDIO_ADDR_WRITE;
    while (REG32(ETH_MAC_BASE + MAC_MDIO_ADDR) & MAC_MDIO_ADDR_GB)
        ;
}

/*
 * func_051 0x2ba4: send one frame and wait for it to leave.
 *
 * Only TX descriptor 0 is ever used.  The header always comes from the
 * fixed buffer SRAM_ETH_TXHDR (buffer 1), the payload (buffer 2) is the
 * caller's.  Checksum insertion is full (CIC = 3), which is why every
 * template carries zero IP/UDP checksums.  No timeouts anywhere.
 */
void eth_xmit(int hdr_len, const void *payload, int pay_len)
{
    volatile struct eqos_desc *d = (volatile struct eqos_desc *)SRAM_ETH_TXDESC;

    /* 0x2bb8..0x2bf8: build descriptor 0 */
    memset32((void *)SRAM_ETH_TXDESC, 0, sizeof(struct eqos_desc));
    d->des0 = SRAM_ETH_TXHDR;
    d->des1 = (u32)payload;
    d->des2 = EQOS_TDES2_IOC | (u32)pay_len << EQOS_TDES2_B2L_SHIFT |
              ((u32)hdr_len & EQOS_TDES2_HL_MASK);
    d->des3 = EQOS_TDES3_OWN | EQOS_TDES3_FD | EQOS_TDES3_LD |
              EQOS_TDES3_CIC_FULL | ((u32)(hdr_len + pay_len) & EQOS_TDES3_FL_MASK);

    /* 0x2bfc: spin until OWN reads back as set -- it was just written, so
       this is only a read-back of the store (acts as a barrier) */
    while (!(d->des3 & EQOS_TDES3_OWN))
        ;

    /* 0x2c08..0x2c28: tail = descriptor 1.  If the TX DMA is stopped
       (TPS = 0) start it; if it is still running, the ROM instead points
       the tail back at descriptor 0. */
    REG32(ETH_DMA_BASE + DMA_CH0_TXDESC_TAIL) = SRAM_ETH_TXDESC_TAIL;
    if ((REG32(ETH_DMA_BASE + DMA_DEBUG_STATUS0) & DMA_DEBUG_TPS_MASK) == 0)
        REG32(ETH_DMA_BASE + DMA_CH0_TX_CTRL) |= DMA_CH0_TX_CTRL_ST;
    else
        REG32(ETH_DMA_BASE + DMA_CH0_TXDESC_TAIL) = SRAM_ETH_TXDESC;

    /* 0x2c2c..0x2c40: wait TI, acknowledge it (W1C) */
    while (!(REG32(ETH_DMA_BASE + DMA_CH0_STATUS) & DMA_CH0_STATUS_TI))
        ;
    REG32(ETH_DMA_BASE + DMA_CH0_STATUS) = DMA_CH0_STATUS_TI;

    /* 0x2c44..0x2c50: wait for the DMA to hand the descriptor back */
    while (d->des3 & EQOS_TDES3_OWN)
        ;

    /* 0x2c54..0x2c6c: stop TX, wait idle, rewind the list to descriptor 0
       so the next eth_xmit uses it again */
    REG32(ETH_DMA_BASE + DMA_CH0_TX_CTRL) &= ~DMA_CH0_TX_CTRL_ST;
    while (REG32(ETH_DMA_BASE + DMA_DEBUG_STATUS0) & DMA_DEBUG_TPS_MASK)
        ;
    REG32(ETH_DMA_BASE + DMA_CH0_TXDESC_LIST) = SRAM_ETH_TXDESC;
}

/*
 * Hardware half of func_053 eth_boot, 0x2cb4..0x3380.
 *
 * On return: g_eth.speed/duplex/phy_addr/rnd/own_mac are set,
 * server_mac is zeroed, both descriptor rings are built, g_eth.rx_idx = 0
 * and the RX DMA is running.
 */
static void eth_hw_init(void)
{
    u32 strap, pinmux1, mode, cfg, lpa, bmcr, i;
    int phy, an_timeout;

    /* 0x2cc0..0x2ce0: 'H', ETH PLL on.  The strap word and TOP+0x18 are
       both read before anything else; TOP+0x18 is read even in 'E'
       mode, where the value is dropped. */
    UART0_PUTC_RAW(LTR_ETH);
    REG32(CG_BASE + CG_CTRL0) |= CG_CTRL0_ETH_PLL;
    strap = REG32(TOP_BASE + TOP_STRAP);
    pinmux1 = REG32(TOP_BASE + TOP_ETH_PINMUX1);

    /* 0x2ce4..0x2d34, 0x2e54..0x2e94: PHY mode from TOP+0[9:8].
       0 = built-in PHY ('E'), 2 = RMII with the reference clock coming in
       ('I'), anything else -- 1, and also the undefined 3 -- RMII with the
       clock driven out ('O', which also sets the pad to output). */
    switch (strap & TOP_STRAP_ETHMODE_MASK) {
    case TOP_ETHMODE_INTERNAL << TOP_STRAP_ETHMODE_SHIFT:
        UART0_PUTC_RAW(LTR_ETH_INTERNAL);
        mode = TOP_ETHMODE_INTERNAL;
        break;
    case TOP_ETHMODE_RMII_IN << TOP_STRAP_ETHMODE_SHIFT:
        UART0_PUTC_RAW(LTR_ETH_RMII_IN);
        REG32(TOP_BASE + TOP_ETH_PINMUX0) |= TOP_ETH_PINMUX0_RMII_A;
        mode = TOP_ETHMODE_RMII_IN;
        REG32(TOP_BASE + TOP_ETH_PINMUX1) = pinmux1 | TOP_ETH_PINMUX1_RMII;
        REG32(TOP_BASE + TOP_ETH_PINMUX2) = TOP_ETH_PINMUX2_RMII;
        REG32(TOP_BASE + TOP_ETH_PINMUX0) |= TOP_ETH_PINMUX0_RMII_B;
        break;
    default:
        UART0_PUTC_RAW(LTR_ETH_RMII_OUT);
        REG32(PAD_BASE + PAD_ETH) |= PAD_ETH_REFCLK_OUT;
        REG32(TOP_BASE + TOP_ETH_PINMUX0) |= TOP_ETH_PINMUX0_RMII_A;
        mode = TOP_ETHMODE_RMII_OUT;
        REG32(TOP_BASE + TOP_ETH_PINMUX1) = pinmux1 | TOP_ETH_PINMUX1_RMII;
        REG32(TOP_BASE + TOP_ETH_PINMUX2) = TOP_ETH_PINMUX2_RMII;
        REG32(TOP_BASE + TOP_ETH_PINMUX0) |= TOP_ETH_PINMUX0_RMII_B;
        break;
    }

    /* 0x2d38..0x2d70: wait for the ETH PLL (no timeout), clock the MAC,
       pulse its reset */
    while (!(REG32(CG_BASE + CG_STAT) & CG_STAT_ETH_PLL_LOCK))
        ;
    REG32(CG_BASE + CG_CLKEN1) |= CG_CLKEN1_ETH;
    REG32(CG_BASE + CG_CLKEN0) |= CG_CLKEN0_ETH;
    REG32(CG_BASE + CG_RST84) &= ~CG_RST84_ETH;
    REG32(CG_BASE + CG_RST84) |= CG_RST84_ETH;

    if (mode == TOP_ETHMODE_RMII_OUT || mode == TOP_ETHMODE_RMII_IN) {
        /* 0x2d7c..0x2e04: external RMII PHY.  Glue to RMII, reference
           clock direction, then reset the PHY on GPIO bank 7 pin 6:
           low 200 ms, high 20 ms, low 20 ms, high, wait 200 ms. */
        REG32(ETH_GLUE_BASE + ETHG_CTRL) &= ~ETHG_CTRL_BIT31;
        REG32(ETH_GLUE_BASE + ETHG_CTRL) |= ETHG_CTRL_RMII;
        if (mode == TOP_ETHMODE_RMII_OUT) {
            REG32(ETH_GLUE_BASE + ETHG_REFCLK) &= ~ETHG_REFCLK_IN;
            REG32(ETH_GLUE_BASE + ETHG_REFCLK) |= ETHG_REFCLK_OUT;
        } else {
            REG32(ETH_GLUE_BASE + ETHG_REFCLK) |= ETHG_REFCLK_IN;
        }
        REG32(GPIO_BASE + GPIO_DIR7) = GPIO7_ETH_PHY_RESET;
        REG32(GPIO_BASE + GPIO_CLR7) = GPIO7_ETH_PHY_RESET;
        udelay(DLY_EXT_PHY_LONG);
        REG32(GPIO_BASE + GPIO_SET7) = GPIO7_ETH_PHY_RESET;
        udelay(DLY_EXT_PHY_SHORT);
        REG32(GPIO_BASE + GPIO_CLR7) = GPIO7_ETH_PHY_RESET;
        udelay(DLY_EXT_PHY_SHORT);
        REG32(GPIO_BASE + GPIO_SET7) = GPIO7_ETH_PHY_RESET;
        udelay(DLY_EXT_PHY_LONG);
        REG32(ETH_GLUE_BASE + ETHG_REFCLK) |= ETHG_REFCLK_EN;
    } else {
        /* 0x2e98..0x2f4c: built-in PHY.  Glue away from RMII (bit31 is
           left alone here), then the analog bring-up sequence. */
        REG32(ETH_GLUE_BASE + ETHG_CTRL) &= ~ETHG_CTRL_RMII;
        REG32(ETH_GLUE_BASE + ETHG_PHY_8F8) |= ETHG_PHY_8F8_SET;
        udelay(DLY_INT_PHY_1);
        REG32(ETH_GLUE_BASE + ETHG_PHY_8C8) &= ~BIT(0);
        udelay(DLY_INT_PHY_2);
        REG32(ETH_GLUE_BASE + ETHG_PHY_8C8) &= ~BIT(1);
        udelay(DLY_INT_PHY_3);
        REG32(ETH_GLUE_BASE + ETHG_PHY_AE8) &= ~BIT(0);
        REG32(ETH_GLUE_BASE + ETHG_PHY_8CC) &= ~BIT(0);
        REG32(ETH_GLUE_BASE + ETHG_PHY_8DC) |= BIT(0);
        REG32(ETH_GLUE_BASE + ETHG_PHY_89C) &= ~BIT(0);
        REG32(ETH_GLUE_BASE + ETHG_PHY_PWR) = ETHG_PHY_PWR_1;
        REG32(ETH_GLUE_BASE + ETHG_PHY_A88) = ETHG_PHY_A88_VAL;
        REG32(ETH_GLUE_BASE + ETHG_PHY_A8C) = ETHG_PHY_A8C_VAL;
        REG32(ETH_GLUE_BASE + ETHG_PHY_A84) = ETHG_PHY_A84_VAL;
        REG32(ETH_GLUE_BASE + ETHG_PHY_PWR) = ETHG_PHY_PWR_2;
        udelay(DLY_INT_PHY_4);
        REG32(ETH_GLUE_BASE + ETHG_PHY_900) = ETHG_PHY_900_VAL;
        REG32(ETH_GLUE_BASE + ETHG_PHYCFG) = ETHG_PHYCFG_VAL;
    }

    /* 0x2e08..0x2e50: link parameters default to 10M half.  The ROM then
       reads the glue control word and drops the value.  DMA software
       reset: 1 s, then boot_fail(8) -- and should boot_fail come back,
       the poll simply continues. */
    g_eth.speed = ETH_SPEED_10M;
    g_eth.duplex = ETH_DUPLEX_HALF;
    (void)REG32(ETH_GLUE_BASE + ETHG_CTRL);
    REG32(ETH_DMA_BASE + DMA_MODE) = DMA_MODE_SWR;
    timer_start();
    while (REG32(ETH_DMA_BASE + DMA_MODE) & DMA_MODE_SWR) {
        if (timer_now() > TMO_1S)
            boot_fail(BOOT_FAIL_ETH_OR_HDR);
    }

    /* 0x2f50..0x2f7c: first MDIO address whose BMSR is neither 0 nor
       0xffff; none found -> address 0, silently */
    timer_stop();
    for (phy = 0; phy < MDIO_PHY_ADDRS; phy++) {
        u16 bmsr = mdio_read(phy, MII_BMSR);

        if (bmsr != 0xffff && bmsr != 0)
            break;
    }
    if (phy == MDIO_PHY_ADDRS)
        phy = 0;
    g_eth.phy_addr = phy;

    /* 0x2f80..0x2fe8: PHY soft reset, then wait (4 s) for BMSR bit5.
       Between the two, BMCR is read once inside its own timer_start/
       timer_stop with the result unused -- what is left of a reset-done
       wait.  Quirk: timer_stop() is called only on the "AN0" timeout
       path; on success the timer is left running. */
    mdio_write(phy, MII_BMCR, MII_BMCR_RESET);
    timer_start();
    (void)mdio_read(g_eth.phy_addr, MII_BMCR);
    timer_stop();
    timer_start();
    while (!(mdio_read(g_eth.phy_addr, MII_BMSR) & MII_BMSR_ANEGDONE)) {
        if (timer_now() > TMO_4S) {
            UART0_PUTC_RAW('A');
            UART0_PUTC_RAW('N');
            UART0_PUTC_RAW('0');
            timer_stop();
            break;
        }
    }

    /* 0x2fec..0x3044: no 1000BASE-T advertisement; print BMCR before
       and after, then write BMCR back with both speed-select bits (13, 6)
       cleared and 0x2200 set: SPEED100 again plus ANRESTART.  ANENABLE
       (bit12) is not touched; it is on after the reset above. */
    phy = g_eth.phy_addr;
    mdio_write(phy, MII_CTRL1000,
               mdio_read(phy, MII_CTRL1000) & ~MII_CTRL1000_CLR);
    bmcr = mdio_read(phy, MII_BMCR);
    put_hex32(bmcr);
    bmcr = (bmcr & ~MII_BMCR_CLR) | MII_BMCR_SET;
    put_hex32(bmcr);
    mdio_write(g_eth.phy_addr, MII_BMCR, bmcr);

    /* 0x3048..0x308c: wait (4 s) for autonegotiation again, "AN1" on
       timeout; timer_stop() on both paths this time */
    an_timeout = 0;
    timer_start();
    while (!(mdio_read(g_eth.phy_addr, MII_BMSR) & MII_BMSR_ANEGDONE)) {
        if (timer_now() > TMO_4S) {
            UART0_PUTC_RAW('A');
            UART0_PUTC_RAW('N');
            UART0_PUTC_RAW('1');
            an_timeout = 1;
            break;
        }
    }
    timer_stop();

    /* 0x3090..0x30d4: link partner ability.  After an AN1 timeout the
       ROM does not read LPA and assumes 100F (0x100).  Otherwise, for
       the built-in PHY only (strap re-read), TOP+0x18[14:13] = 2 and
       TOP+0xd0[6:5] cleared before LPA is read. */
    if (an_timeout) {
        lpa = MII_LPA_100FULL;
    } else {
        if ((REG32(TOP_BASE + TOP_STRAP) & TOP_STRAP_ETHMODE_MASK) == 0) {
            REG32(TOP_BASE + TOP_ETH_PINMUX1) =
                (REG32(TOP_BASE + TOP_ETH_PINMUX1) & ~TOP_ETH_PINMUX1_PHY_MASK) |
                TOP_ETH_PINMUX1_PHY_VAL << TOP_ETH_PINMUX1_PHY_SHIFT;
            REG32(TOP_BASE + TOP_PHY_CTRL) &= ~TOP_PHY_CTRL_CLR;
        }
        lpa = mdio_read(g_eth.phy_addr, MII_LPA);
    }

    /* 0x30d8..0x358c: print LPA, 's', then the best common mode.  Bit 9
       (100BASE-T4) counts as 100F.  No mode bit at all: LPA again,
       boot_fail(8), and if that returns, carry on at 10M half. */
    put_hex32(lpa);
    UART0_PUTC_RAW('s');
    if (lpa & MII_LPA_100_ANY) {
        UART0_PUTC_RAW('1');
        UART0_PUTC_RAW('0');
        UART0_PUTC_RAW('0');
        UART0_PUTC_RAW('F');
        g_eth.speed = ETH_SPEED_100M;
        g_eth.duplex = ETH_DUPLEX_FULL;
    } else if (lpa & MII_LPA_100HALF) {
        UART0_PUTC_RAW('1');
        UART0_PUTC_RAW('0');
        UART0_PUTC_RAW('0');
        UART0_PUTC_RAW('H');
        g_eth.speed = ETH_SPEED_100M;
        g_eth.duplex = ETH_DUPLEX_HALF;
    } else if (lpa & MII_LPA_10FULL) {
        UART0_PUTC_RAW('1');
        UART0_PUTC_RAW('0');
        UART0_PUTC_RAW('F');
        g_eth.speed = ETH_SPEED_10M;
        g_eth.duplex = ETH_DUPLEX_FULL;
    } else if (lpa & MII_LPA_10HALF) {
        UART0_PUTC_RAW('1');
        UART0_PUTC_RAW('0');
        UART0_PUTC_RAW('H');
        g_eth.speed = ETH_SPEED_10M;
        g_eth.duplex = ETH_DUPLEX_HALF;
    } else {
        put_hex32(lpa);
        boot_fail(BOOT_FAIL_ETH_OR_HDR);
    }

    /* 0x315c..0x31c4: one TRNG word, with the TRNG reset pulsed and its
       two clock bits on only around the read */
    REG32(CG_BASE + CG_RST88) &= ~CG_RST88_TRNG;
    REG32(CG_BASE + CG_RST88) |= CG_RST88_TRNG;
    REG32(CG_BASE + CG_CLKEN1) |= CG_CLKEN1_TRNG_A;
    REG32(CG_BASE + CG_CLKEN1) |= CG_CLKEN1_TRNG_B;
    REG32(TRNG_BASE + TRNG_CTRL) |= TRNG_CTRL_EN;
    g_eth.rnd = REG32(TRNG_BASE + TRNG_DATA);
    REG32(CG_BASE + CG_CLKEN1) &= ~CG_CLKEN1_TRNG_A;
    REG32(CG_BASE + CG_CLKEN1) &= ~CG_CLKEN1_TRNG_B;

    /* 0x31c8..0x3238: MAC address 00:00:00:rnd[23:16]:rnd[15:8]:rnd[7:0]
       (an all-zero OUI, and not marked locally administered), into the
       MAC's address filter 0 */
    memset32(g_eth.server_mac, 0, sizeof(g_eth.server_mac));
    memset32(g_eth.own_mac, 0, sizeof(g_eth.own_mac));
    wr8(&g_eth.own_mac[3], (g_eth.rnd >> 16) & 0xff);
    wr8(&g_eth.own_mac[4], (g_eth.rnd >> 8) & 0xff);
    wr8(&g_eth.own_mac[5], g_eth.rnd & 0xff);
    REG32(ETH_MAC_BASE + MAC_ADDR0_HIGH) =
        (u32)g_eth.own_mac[5] << 8 | g_eth.own_mac[4];
    REG32(ETH_MAC_BASE + MAC_ADDR0_LOW) =
        (u32)g_eth.own_mac[3] << 24 | (u32)g_eth.own_mac[2] << 16 |
        (u32)g_eth.own_mac[1] << 8 | g_eth.own_mac[0];

    /* 0x323c..0x3284: flow control, then MAC_CONFIG from the decoded
       link.  speed == 0x40 (a 1G setting) is tested but never stored, so
       PS/FES is always one of 100M / 10M. */
    REG32(ETH_MAC_BASE + MAC_RX_FLOW_CTRL) = MAC_RX_FLOW_CTRL_VAL;
    REG32(ETH_MAC_BASE + MAC_Q0_TX_FLOW_CTRL) = MAC_Q0_TX_FLOW_CTRL_VAL;
    cfg = MAC_CONFIG_BASE;
    if (g_eth.duplex == ETH_DUPLEX_FULL)
        cfg |= MAC_CONFIG_DM;
    if (g_eth.speed != ETH_SPEED_1G_TESTED)
        cfg |= g_eth.speed == ETH_SPEED_100M ? MAC_CONFIG_100M : MAC_CONFIG_10M;
    REG32(ETH_MAC_BASE + MAC_CONFIG) = cfg;

    /* 0x3288..0x32dc: RX buffer size 2048; 4 TX descriptors with both
       buffers at the TX header buffer and des2/des3 = 0; TX ring length
       and a tail at descriptor 3 (TX DMA not started yet) */
    REG32(ETH_DMA_BASE + DMA_CH0_RX_CTRL) =
        (REG32(ETH_DMA_BASE + DMA_CH0_RX_CTRL) & ~DMA_CH0_RX_CTRL_RBSZ_MASK) |
        DMA_CH0_RX_CTRL_RBSZ;
    for (i = 0; i < ETH_RING_LEN; i++) {
        eth_txdesc[i].des0 = SRAM_ETH_TXHDR;
        eth_txdesc[i].des1 = SRAM_ETH_TXHDR;
        eth_txdesc[i].des2 = 0;
        eth_txdesc[i].des3 = 0;
    }
    REG32(ETH_DMA_BASE + DMA_CH0_TXDESC_RINGLEN) = DMA_CH0_RINGLEN_VAL;
    REG32(ETH_DMA_BASE + DMA_CH0_TXDESC_TAIL) = SRAM_ETH_TXDESC3;

    /* 0x32e0..0x3300: 4 RX descriptors owned by the DMA, buffer i at
       SRAM_ETH_RXBUF(i).  (The ROM stores des1..des3 before des0; the
       DMA is not running yet, so the order is immaterial.) */
    for (i = 0; i < ETH_RING_LEN; i++) {
        eth_rxdesc[i].des0 = SRAM_ETH_RXBUF(i);
        eth_rxdesc[i].des1 = 0;
        eth_rxdesc[i].des2 = 0;
        eth_rxdesc[i].des3 = EQOS_RDES3_RECYCLE;
    }
    g_eth.rx_idx = 0;

    /* 0x3304..0x3354: RX ring, list bases, DMA burst lengths, MTL queue
       modes (RX 0x23, TX store-and-forward) */
    REG32(ETH_DMA_BASE + DMA_CH0_RXDESC_RINGLEN) = DMA_CH0_RINGLEN_VAL;
    REG32(ETH_DMA_BASE + DMA_CH0_RXDESC_TAIL) = SRAM_ETH_RXDESC_TAIL;
    REG32(ETH_DMA_BASE + DMA_CH0_TXDESC_LIST) = SRAM_ETH_TXDESC;
    REG32(ETH_DMA_BASE + DMA_CH0_RXDESC_LIST) = SRAM_ETH_RXDESC;
    REG32(ETH_DMA_BASE + DMA_CH0_TX_CTRL) = DMA_CH0_TX_CTRL_VAL;
    REG32(ETH_DMA_BASE + DMA_CH0_RX_CTRL) =
        (REG32(ETH_DMA_BASE + DMA_CH0_RX_CTRL) & ~DMA_CH0_RX_CTRL_PBL_MASK) |
        DMA_CH0_RX_CTRL_PBL16;
    REG32(ETH_MAC_BASE + MTL_RXQ0_OPMODE) = MTL_RXQ0_OPMODE_VAL;
    REG32(ETH_MAC_BASE + MTL_TXQ0_OPMODE) = MTL_TXQ0_OPMODE_VAL;

    /* 0x3358..0x3380: 'X', settle 0.5 s, clear 0xF0FF0020 bit0 (net_run
       sets it again before the loader check), start RX DMA */
    UART0_PUTC_RAW(LTR_ETH_UP);
    udelay(DLY_500MS);
    REG32(MISC_FF0_BASE + ETH_FF0_CTRL) &= ~ETH_FF0_CTRL_BIT0;
    REG32(ETH_DMA_BASE + DMA_CH0_RX_CTRL) |= DMA_CH0_RX_CTRL_SR;
}

/*
 * func_053 0x2cb4: Ethernet boot (BS 4, or the fallback after SD).
 * net_run() ends in check_and_run_loader(1, ...) or boot_fail(): it does
 * not come back in practice, although the original has an epilogue.
 */
void eth_boot(void)
{
    eth_hw_init();
    net_run();
}
