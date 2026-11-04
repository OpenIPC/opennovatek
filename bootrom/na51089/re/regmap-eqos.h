/*
 * regmap-eqos.h -- Ethernet: Synopsys DWC Ethernet QoS (EQOS) MAC at
 * 0xF02B0000, its DMA at 0xF02B1000, the Novatek glue / internal PHY at
 * 0xF02B3000, and the unidentified 0xF0FF0020 word eth_boot toggles.
 * Register names follow the Synopsys databook where the ROM's use
 * matches.  Include through na51089-regmap.h.
 */
#ifndef BOOTROM_NA51089_REGMAP_EQOS_H
#define BOOTROM_NA51089_REGMAP_EQOS_H

/* ---- MAC, offsets from ETH_MAC_BASE ---------------------------------- */
#define MAC_CONFIG              0x000
#define MAC_CONFIG_BASE         0x08040003      /* RE, TE, bit18, IPC */
#define MAC_CONFIG_DM           0x2000          /* full duplex */
#define MAC_CONFIG_100M         0xc000          /* PS|FES */
#define MAC_CONFIG_10M          0x8000          /* PS */
#define MAC_Q0_TX_FLOW_CTRL     0x070           /* = 2 */
#define MAC_Q0_TX_FLOW_CTRL_VAL 2
#define MAC_RX_FLOW_CTRL        0x090           /* = 1 */
#define MAC_RX_FLOW_CTRL_VAL    1
#define MAC_MDIO_ADDR           0x200
#define MAC_MDIO_ADDR_PA_SHIFT  21              /* PHY address */
#define MAC_MDIO_ADDR_RDA_SHIFT 16              /* register */
#define MAC_MDIO_ADDR_READ      0xd             /* GOC read | GB (+CR) */
#define MAC_MDIO_ADDR_WRITE     0x5             /* GOC write | GB (+CR) */
#define MAC_MDIO_ADDR_GB        BIT(0)          /* busy */
#define MAC_MDIO_DATA           0x204
#define MAC_ADDR0_HIGH          0x300           /* MAC bytes 4,5 */
#define MAC_ADDR0_LOW           0x304           /* MAC bytes 0..3 */
#define MTL_TXQ0_OPMODE         0xd00           /* = 2 (TSF) */
#define MTL_TXQ0_OPMODE_VAL     2
#define MTL_RXQ0_OPMODE         0xd30           /* = 0x23 */
#define MTL_RXQ0_OPMODE_VAL     0x23

/* ---- DMA, offsets from ETH_DMA_BASE ---------------------------------- */
#define DMA_MODE                0x000
#define DMA_MODE_SWR            BIT(0)          /* poll clear, 1 s -> K8 */
#define DMA_DEBUG_STATUS0       0x00c
#define DMA_DEBUG_TPS_SHIFT     12              /* [15:12] TX process state */
#define DMA_DEBUG_TPS_MASK      (0xf << 12)
#define DMA_CH0_TX_CTRL         0x104
#define DMA_CH0_TX_CTRL_VAL     0x00100010      /* TxPBL 16, OSF */
#define DMA_CH0_TX_CTRL_ST      BIT(0)
#define DMA_CH0_RX_CTRL         0x108
#define DMA_CH0_RX_CTRL_RBSZ    0x1000          /* field [14:1] = 2048 */
#define DMA_CH0_RX_CTRL_PBL_MASK (0x3f << 16)
#define DMA_CH0_RX_CTRL_PBL16   (16 << 16)
#define DMA_CH0_RX_CTRL_SR      BIT(0)
#define DMA_CH0_TXDESC_LIST     0x114           /* = SRAM_ETH_TXDESC */
#define DMA_CH0_RXDESC_LIST     0x11c           /* = SRAM_ETH_RXDESC */
#define DMA_CH0_TXDESC_TAIL     0x120
#define DMA_CH0_RXDESC_TAIL     0x128
#define DMA_CH0_TXDESC_RINGLEN  0x12c           /* = 3 (4 descriptors) */
#define DMA_CH0_RXDESC_RINGLEN  0x130           /* = 3 */
#define DMA_CH0_RINGLEN_VAL     3
#define DMA_CH0_STATUS          0x160           /* W1C */
#define DMA_CH0_STATUS_TI       BIT(0)
#define DMA_CH0_STATUS_RI       BIT(6)

/* ---- Novatek glue / internal PHY, offsets from ETH_GLUE_BASE -------- */
#define ETHG_CTRL               0x004
#define ETHG_CTRL_BIT31         BIT(31)         /* cleared */
#define ETHG_CTRL_RMII          BIT(4)          /* set RMII / clear internal PHY */
#define ETHG_PHYCFG             0x00c           /* internal PHY: = 0x312 */
#define ETHG_PHYCFG_VAL         0x312
#define ETHG_REFCLK             0x014
#define ETHG_REFCLK_IN          BIT(0)          /* clear 'O', set 'I' */
#define ETHG_REFCLK_OUT         BIT(4)          /* 'O' */
#define ETHG_REFCLK_EN          BIT(5)          /* set last */
#define ETHG_PHY_PWR            0x800           /* = 2, then 8 */
#define ETHG_PHY_PWR_1          2
#define ETHG_PHY_PWR_2          8
#define ETHG_PHY_89C            0x89c           /* bit0 cleared */
#define ETHG_PHY_8C8            0x8c8           /* bits 0,1 cleared */
#define ETHG_PHY_8CC            0x8cc           /* bit0 cleared */
#define ETHG_PHY_8DC            0x8dc           /* bit0 set */
#define ETHG_PHY_8F8            0x8f8           /* |= 0x80 */
#define ETHG_PHY_8F8_SET        0x80
#define ETHG_PHY_900            0x900           /* = 0x40 */
#define ETHG_PHY_900_VAL        0x40
#define ETHG_PHY_A84            0xa84           /* = 0xc9 */
#define ETHG_PHY_A84_VAL        0xc9
#define ETHG_PHY_A88            0xa88           /* = 0x53 */
#define ETHG_PHY_A88_VAL        0x53
#define ETHG_PHY_A8C            0xa8c           /* = 7 */
#define ETHG_PHY_A8C_VAL        7
#define ETHG_PHY_AE8            0xae8           /* bit0 cleared */

/* ---- 0xF0FF0020, offset from MISC_FF0_BASE (block not identified) --- */
#define ETH_FF0_CTRL            0x020
#define ETH_FF0_CTRL_BIT0       BIT(0)          /* cleared before RX, set before
                                                   the loader check */

/* ---- MII registers (IEEE 802.3 clause 22) the ROM uses -------------- */
#define MII_BMCR                0               /* reg0: = 0x8000 reset;
                                                   (v & ~0x2040) | 0x2200 */
#define MII_BMCR_RESET          0x8000
#define MII_BMCR_CLR            0x2040
#define MII_BMCR_SET            0x2200
#define MII_BMSR                1               /* reg1: != 0/0xffff = present */
#define MII_BMSR_ANEGDONE       BIT(5)          /* waited for, 4 s -> "AN0" */
#define MII_LPA                 5               /* reg5: bits 8/7/6/5 =
                                                   100F/100H/10F/10H */
#define MII_LPA_100FULL         BIT(8)
#define MII_LPA_100HALF         BIT(7)
#define MII_LPA_10FULL          BIT(6)
#define MII_LPA_10HALF          BIT(5)
#define MII_CTRL1000            9               /* reg9 &= ~0x300 */
#define MII_CTRL1000_CLR        0x300

/* ---- DMA descriptors (EQOS normal format) --------------------------- */
#define EQOS_TDES2_IOC          BIT(31)
#define EQOS_TDES2_B2L_SHIFT    16              /* payload length */
#define EQOS_TDES3_OWN          BIT(31)
#define EQOS_TDES3_FD           BIT(29)
#define EQOS_TDES3_LD           BIT(28)
#define EQOS_TDES3_CIC_FULL     (3 << 16)
#define EQOS_RDES3_OWN          BIT(31)
#define EQOS_RDES3_RECYCLE      0xC1000000      /* OWN|IOC|BUF1V */

#ifndef __ASSEMBLER__
struct eqos_desc {
    unsigned int des0;          /* TX: header buffer / RX: buffer */
    unsigned int des1;          /* TX: payload buffer */
    unsigned int des2;          /* TX: IOC | pay_len << 16 | hdr_len */
    unsigned int des3;          /* OWN, flags, lengths */
};
#endif

#endif
