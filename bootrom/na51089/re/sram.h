/*
 * sram.h -- every SRAM address and ROM global of the NA51089 mask ROM
 * (scratchpad/sram.txt), and the in-memory loader header layout
 * (scratchpad/bootrom.txt).  This is the only file where 0xF07Cxxxx,
 * 0xF07Dxxxx and 0xF080xxxx appear.
 *
 * The ROM has no .data or .bss: start.S never clears or copies RAM and
 * every global is written by the boot path that owns it before it is
 * read.  Globals therefore are NOT C variables here.  Each one is an
 * address macro SRAM_<NAME> (usable from assembly) plus, in C, an lvalue
 * macro g_<name> that dereferences that fixed address:
 *
 *     g_boot_source = BS_SD;            (u32 at 0xF07D4004)
 *     g_eth.state = 1;                  (struct eth_ctx at 0xF07D4018)
 *     memcpy32((void *)SRAM_HDR_STAGE, (void *)SRAM_IMAGE, HDR_STAGE_COPY);
 *
 * Paths never run concurrently, so regions overlap on purpose (Ethernet
 * descriptors and secure-boot scratch share 0xF07D0000).
 *
 * The lvalue macros are not volatile: this is ordinary RAM, and the
 * original was compiled as ordinary globals.
 */
#ifndef BOOTROM_NA51089_SRAM_H
#define BOOTROM_NA51089_SRAM_H

#include "bootrom.h"
#include "na51089-regmap.h"         /* BIT(), struct eqos_desc */

/* ==== windows ========================================================= */
#define SRAM_LOADER_BASE        0xF07C0000      /* loader SRAM, 64 KiB */
#define SRAM_SCRATCH_BASE       0xF07D0000      /* per-path scratch + stack */
#define SRAM_GLOBALS_BASE       0xF07D4000      /* ROM globals 0x..4000-0x..4213 */
#define SRAM2_BASE              0xF0800000      /* second RAM (role inferred) */

/* ==== stack =========================================================== */
#define SRAM_STACK_TOP          0xF07D3000      /* sp at 0xb4, SVC, full-descending */
#define SRAM_STACK_LIMIT        0xF07D0620      /* lowest byte it may reach */

/* ==== loader image area =============================================== */
#define SRAM_IMAGE              0xF07C0000      /* 32 KiB loader image */
#define SRAM_IMAGE_SIZE         0x8000
#define SRAM_IMAGE_HDR_EXT      0xF07C0200      /* extra header sectors */
#define SRAM_IMAGE_FLAGS        0xF07C0088      /* secure_boot_verify reads
                                                   header+0x88 here whatever
                                                   the load address */
#define SRAM_TFTP_BLOCK(n)      (SRAM_IMAGE + ((n) - 1) * 512)  /* n = 1..64 */
#define TFTP_MAX_BLOCKS         64

/* ==== Ethernet (eth_boot, eth_xmit) =================================== */
#define SRAM_ETH_RXBUF0         0xF07C8000      /* 4 x 2 KiB RX buffers */
#define ETH_RXBUF_SIZE          0x800
#define ETH_RING_LEN            4
#define SRAM_ETH_RXBUF(i)       (SRAM_ETH_RXBUF0 + (i) * ETH_RXBUF_SIZE)
                                                /* the ROM forms it as
                                                   (i + 0x1E0F90) << 11 */
#define SRAM_ETH_TXDESC         0xF07D0000      /* 4 TX descriptors (only [0] used) */
#define SRAM_ETH_TXDESC_TAIL    0xF07D0010
#define SRAM_ETH_RXDESC         0xF07D0040      /* 4 RX descriptors */
#define SRAM_ETH_RXDESC_TAIL    0xF07D0070
#define SRAM_ETH_TXHDR          0xF07D0080      /* 0x40: Ethernet/IP/UDP or ARP header */
#define ETH_TXHDR_LEN           0x2a
#define SRAM_ETH_TXPAY          0xF07D00C0      /* 0x12c: BOOTP or TFTP RRQ/ACK */
#define ETH_TXPAY_SIZE          0x12c

/* field offsets inside SRAM_ETH_TXHDR */
#define TXH_SRC_MAC             0x06            /* 0xF07D0086 */
#define TXH_IP_TOTLEN           0x10            /* 0xF07D0090, TFTP ACK: 0x0020 */
#define TXH_ARP_OP              0x14            /* 0xF07D0094 */
#define TXH_ARP_SHA             0x16            /* 0xF07D0096 sender MAC */
#define TXH_IP_SRC              0x1a            /* 0xF07D009A */
#define TXH_ARP_SPA             0x1c            /* 0xF07D009C sender IP */
#define TXH_IP_DST              0x1e            /* 0xF07D009E */
#define TXH_ARP_THA             0x20            /* 0xF07D00A0 target MAC */
#define TXH_UDP_DPORT           0x24            /* 0xF07D00A4 */
#define TXH_UDP_LEN             0x26            /* 0xF07D00A6 */
#define TXH_ARP_TPA             0x26            /* 0xF07D00A6 target IP */
/* field offsets inside SRAM_ETH_TXPAY (BOOTP) */
#define TXP_BOOTP_XID           0x04            /* 0xF07D00C4 */
#define TXP_BOOTP_CHADDR        0x1c            /* 0xF07D00DC */
#define TXP_BOOTP_OPTIONS       0xf0            /* 0xF07D01B0..0xF07D01BF */

#define ETH_TFTP_PORT_LOCAL     2962            /* 0x0b92 */
#define ETH_TFTP_PORT_SERVER    69

/* ==== secure boot scratch (secure_boot_verify, aes, sha256) ========== */
#define SRAM_SB_DIGEST          0xF07D0000      /* 0x20 decrypted digest ('A') /
                                                   RSA result tail ('R') */
#define SRAM_SB_STORED_SHA      0xF07D0020      /* 0x20 copy of header+0x2e0 */
#define SRAM_SB_IMAGE_SHA       0xF07D0060      /* 0x20 SHA-256 of the 32 KiB image */
#define SRAM_SB_IV              0xF07D0080      /* 0x20 AES IV (first 16 B used) */
#define SRAM_SB_ZERO            0xF07D00A0      /* 0x10 zeroed before hashing */
#define SRAM_SB_RSA_N           0xF07D0200      /* 0x100 modulus */
#define SRAM_SB_RSA_E           0xF07D0300      /* 0x20 exponent */
#define SRAM_SB_RSA_SIG         0xF07D0400      /* 0x100 signature, byte-reversed */
#define SRAM_SB_RSA_OUT         0xF07D0500      /* 0x100 engine output */
#define SRAM_SB_RSA_OUT_DIGEST  0xF07D05E0      /* digest tail of the output */
#define SRAM_SB_N_HASH          0xF07D0600      /* 0x20 SHA-256(N) ('C') */

/* ==== ROM globals 0xF07D4000.. ======================================= */
#define SRAM_CLUSTER_SECTS      0xF07D4000      /* FAT/exFAT sectors per cluster */
#define SRAM_BOOT_SOURCE        0xF07D4004      /* BS code of the current source */
#define SRAM_TIMEOUT            0xF07D4008      /* ETH: max DHCP retries;
                                                   USB/UART: timeout in us */
#define SRAM_OTP_TMO            0xF07D400C      /* t = OTP+0x50[20:18] */
#define SRAM_SAVED_CG74         0xF07D4010      /* CG+0x74 before timer_start */
#define SRAM_SAVED_TMR30        0xF07D4014      /* TIMER+0x30 before timer_start */
#define SRAM_ETH                0xF07D4018      /* struct eth_ctx */
#define SRAM_SD                 0xF07D4054      /* struct sd_ctx */
#define SRAM_FLASH              0xF07D4064      /* struct flash_ctx */
#define SRAM_USB_STATE          0xF07D4098      /* 0 run, 1 WAIT_DONE, 2 CSW sent */
#define SRAM_USB_DL_ADDR        0xF07D409C      /* WRLOADER address */
#define SRAM_USB_REPLY          0xF07D40A0      /* 0x80 data-in buffer */
#define USB_REPLY_SIZE          0x80
#define SRAM_XFER               0xF07D4120      /* struct usb_xfer */
#define SRAM_SENSE_KEY          0xF07D4134
#define SRAM_SENSE_ASC          0xF07D4138
#define SRAM_CSW                0xF07D413C      /* struct bot_csw */
#define SRAM_CBW                0xF07D414C      /* struct bot_cbw */
#define SRAM_CBW_CB             0xF07D415B      /* CBW command block */
#define SRAM_USB_REGS           0xF07D416C      /* = USB_DEV_BASE */
#define SRAM_UART               0xF07D4170      /* struct uart_ctx */
#define UART_CTX_CLEAR_LEN      0x24            /* uart_boot zeroes cdb_idx..dl_total,
                                                   9 word stores (functions.txt
                                                   says "..+0x20": inclusive) */
#define SRAM_BOT_STATE          0xF07D41D8      /* u8, BOT_* */
#define SRAM_USB_OPENED         0xF07D41DC      /* OPEN_DEVICE seen */
#define SRAM_EP0                0xF07D41E0      /* struct usb_ep0_ctx */
#define SRAM_GLOBALS_END        0xF07D4214      /* first unused byte */

/* ==== second RAM 0xF0800000 ========================================== */
#define SRAM_HDR_STAGE          0xF0800200      /* 0x200 header staging */
#define SRAM_HDR_STAGE_LOAD     0xF0800204      /* +0x04 load address */
#define SRAM_HDR_STAGE_SIZE     0xF0800224      /* +0x24 size */
#define SRAM_HDR_STAGE_SCRIPT   0xF0800238      /* +0x38 script word */
#define HDR_STAGE_COPY          0x28            /* ETH/USB/UART copy this much */
#define SRAM_DMA_DIRECT         0xF0800400      /* bit31: SPI/eMMC DMA straight
                                                   to the destination */
#define DMA_DIRECT_EN           0x80000000
#define SRAM_USB_DESC_BUF       0xF0800420      /* ~0x24 descriptor bounce */

/* ==== constants of the structured globals ============================ */
/* g_eth.speed / .duplex */
#define ETH_SPEED_100M          0x2000
#define ETH_SPEED_10M           0
#define ETH_SPEED_1G_TESTED     0x40            /* tested, never stored */
#define ETH_DUPLEX_FULL         0x100
#define ETH_DUPLEX_HALF         0
/* g_eth.state (see functions.txt "Ethernet state machine") */
#define ETH_ST_INIT             0
#define ETH_ST_DISCOVER         1               /* "S1" */
#define ETH_ST_WAIT_OFFER       2
#define ETH_ST_OFFERED          3               /* "S2" REQUEST */
#define ETH_ST_WAIT_ACK         4
#define ETH_ST_ARP              5               /* "S3" */
#define ETH_ST_ARP_SENT         6
#define ETH_ST_RRQ              7               /* "S4" */
#define ETH_ST_WAIT_DATA        8
#define ETH_ST_WAIT_DATA2       9
#define ETH_ST_DONE             10              /* "Dn" */
#define ETH_OFFER_TMO           TMO_5S

/* g_sd.xfer_mode, g_flash.mode */
#define XFER_PIO                0
#define XFER_DMA                2               /* header+0x38 bit16 */

/* g_bot_state */
#define BOT_CBW                 1
#define BOT_DATA_IN             2
#define BOT_DATA_OUT            3
#define BOT_CSW                 4
#define BOT_STALL               5
#define BOT_HALT                6

/* g_xfer.type */
#define XFER_TYPE_DOWNLOAD      3               /* data-out to the loader address */
#define XFER_TYPE_REPLY         7               /* data-in from g_usb_reply */

/* g_ep0.result */
#define EP0_NONE                0
#define EP0_DONE                1
#define EP0_STALL               2

/* g_usb_state */
#define USB_ST_RUNNING          0
#define USB_ST_WAIT_DONE        1
#define USB_ST_LEAVE            2

/* BOT signatures */
#define CBW_SIGNATURE           0x43425355      /* "USBC" */
#define CSW_SIGNATURE           0x53425355      /* "USBS" */
#define CBW_LEN                 31
#define CSW_LEN                 13

/* Novatek vendor CDBs: UART uses the whole/low word, USB CB[0..1] */
#define NVT_CDB_IS_NVT          0x81c0
#define NVT_CDB_OPEN_DEVICE     0x01c0
#define NVT_CDB_WRLOADER        0x02c0
#define NVT_CDB_WAIT_DONE       0x82c0
#define NVT_CDB_ADDR_WRITE      0x03c0
#define NVT_CDB_ADDR_READ       0x83c0
#define NVT_USB_IS_NVT_REPLY    0x70210000      /* constant over USB */

/* ==== loader header (offsets from the load address) ================== */
#define HDR_ENTRY               0x000           /* jumped to by run_loader */
#define HDR_LOAD                0x004
#define HDR_LOG                 0x00c           /* DRAM-script log word */
#define HDR_SIZE                0x024           /* bytes: checksum and load length */
#define HDR_TAG                 0x030           /* u16 LOADER_TAG */
#define HDR_POLL_REG            0x034           /* script op 4 */
#define HDR_SCRIPT_PTR          0x038           /* script word; hdr_script_run arg */
#define HDR_LANE_BASE           0x03c           /* script ops 5 and 9 */
#define HDR_SCRIPT              0x040           /* entries */
#define HDR_FLAGS               0x088
#define HDR_RSA_N               0x0c0           /* 256 B */
#define HDR_RSA_E               0x1c0           /* 32 B */
#define HDR_RSA_SIG             0x1e0           /* 256 B; also 'S' IV in RSA mode */
#define HDR_AES_DIGEST          0x2c0           /* 32 B; also 'S' IV in AES mode */
#define HDR_SHA256              0x2e0           /* 32 B (flag bit29 check) */
#define HDR_ENC_AREA            0x300           /* ..0x7fff, 'S' AES-128-CBC */
#define HDR_ENC_END             0x8000

/* HDR_LOG */
#define HDR_LOG_EN              BIT(30)
#define HDR_LOG_ADDR_CLR        0x30000000      /* hdr_script_run: addr = log & ~this
                                                   (bootrom.txt says [27:0]) */
/* HDR_SCRIPT_PTR word */
#define HDR_SW_COUNT_MASK       0xfff           /* [11:0] entry count */
#define HDR_SW_EXTRA_SHIFT      12              /* [15:12] extra 512 B header sectors */
#define HDR_SW_EXTRA_MASK       (0xf << 12)
#define HDR_SW_XFER_DMA         BIT(16)         /* g_flash.mode / g_sd.xfer_mode = 2 */
#define HDR_SW_NEW_DIV          BIT(18)         /* clock divider from [31:24] */
#define HDR_SW_TRAIN            BIT(19)         /* allow script op 7 */
#define HDR_SW_DIV_SHIFT        24
/* HDR_FLAGS */
#define HDR_FLAG_HWSUM          BIT(31)         /* trust controller checksum */
#define HDR_FLAG_DUAL           BIT(30)         /* x2 read for the body (flash) */
#define HDR_FLAG_SHA            BIT(29)         /* check HDR_SHA256 (secure boot) */
#define HDR_FLAG_DMA_DIRECT     BIT(28)         /* -> SRAM_DMA_DIRECT bit31 */

/* script entries: op = w0[27:24], addr = w0 & ~HDR_OP_MASK */
#define HDR_OP_SHIFT            24
#define HDR_OP_MASK             0x0F000000
#define HDR_OP_POLL             0
#define HDR_OP_WRITE            1
#define HDR_OP_DELAY            2               /* udelay(w0[23:0]) */
#define HDR_OP_WRITE_BYTES      3
#define HDR_OP_CMD_POLL         4
#define HDR_OP_LANE             5
#define HDR_OP_DDR_PHY          6
#define HDR_OP_TRAIN            7
#define HDR_OP_TRIM_LOAD        8
#define HDR_OP_TRIM_APPLY       9
#define HDR_OP_FLAG             10
#define HDR_OP_COUNT            11
#define HDR_TRAIN_LOG_END       0x70654321

#ifndef __ASSEMBLER__

/* ---- Ethernet context, 0xF07D4018 ---------------------------------- */
struct eth_ctx {
    u32 speed;          /* +0x00 0x4018  ETH_SPEED_* */
    u32 duplex;         /* +0x04 0x401C  ETH_DUPLEX_* */
    u32 phy_addr;       /* +0x08 0x4020  first MDIO addr with reg1 != 0/0xffff */
    u32 rnd;            /* +0x0c 0x4024  TRNG word: MAC bytes 3..5, first xid */
    u8 server_mac[8];   /* +0x10 0x4028  from the DHCPOFFER source MAC */
    u8 own_mac[8];      /* +0x18 0x4030  00:00:00:rnd[23:16]:[15:8]:[7:0] */
    u32 rx_idx;         /* +0x20 0x4038  RX ring index 0..3 */
    u32 running;        /* +0x24 0x403C  1 until "Dn" */
    u32 state;          /* +0x28 0x4040  ETH_ST_* */
    u32 xid;            /* +0x2c 0x4044  DHCP xid, ++ per DISCOVER/REQUEST */
    u32 own_ip;         /* +0x30 0x4048  yiaddr */
    u32 retries;        /* +0x34 0x404C  DISCOVER retries */
    u32 server_ip;      /* +0x38 0x4050  siaddr */
};

/* ---- SD / eMMC context, 0xF07D4054 ---------------------------------- */
struct sd_ctx {
    u32 xfer_mode;      /* +0x00 0x4054  XFER_PIO / XFER_DMA */
    u32 v2;             /* +0x04 0x4058  CMD8 answered (SD 2.0) */
    u32 hc;             /* +0x08 0x405C  OCR bit30: block addressing */
    u32 rca;            /* +0x0c 0x4060  CMD3 response (RCA << 16) */
};

/* ---- SPI flash context, 0xF07D4064 ---------------------------------- */
struct flash_ctx {
    u32 mode;           /* +0x00 0x4064  XFER_PIO / XFER_DMA, | 1 -> SPI+0x10c */
    u32 rsv04;          /* +0x04 0x4068  not used */
    u32 sectors;        /* +0x08 0x406C  512 B ECC sectors per page (4/8) */
    u32 page_shift;     /* +0x0c 0x4070  page = 512 << n (2, 3; 0 for NOR) */
    u32 rsv10;          /* +0x10 0x4074  not used */
    u32 rsv14;          /* +0x14 0x4078  not used */
    u32 block_shift;    /* +0x18 0x407C  pages per block = 1 << 6 */
    u32 rsv1c;          /* +0x1c 0x4080  not used */
    u32 rsv20;          /* +0x20 0x4084  not used */
    u32 wide;           /* +0x24 0x4088  x2 read (header+0x88 bit30) */
    u32 odecc;          /* +0x28 0x408C  1 = on-die ECC NAND (BS 2/6) */
    u32 strap;          /* +0x2c 0x4090  BS passed to flash_boot */
    u32 cfg;            /* +0x30 0x4094  SPI_CFG_PAGE_2K / _4K */
};

/* ---- USB transfer descriptor, 0xF07D4120 ---------------------------- */
struct usb_xfer {
    u32 type;           /* +0x00 XFER_TYPE_* */
    u32 dir;            /* +0x04 0x80 in / 0 out */
    u32 addr;           /* +0x08 */
    u32 len;            /* +0x0c */
    u32 step;           /* +0x10 */
};

/* ---- bulk-only transport wrappers ----------------------------------- */
struct bot_csw {                /* 0xF07D413C, 13 bytes sent */
    u32 signature;      /* +0x00 CSW_SIGNATURE */
    u32 tag;            /* +0x04 */
    u32 residue;        /* +0x08 */
    u8 status;          /* +0x0c */
    u8 pad[3];
};

struct bot_cbw {                /* 0xF07D414C, 31 bytes received */
    u32 signature;      /* +0x00 CBW_SIGNATURE */
    u32 tag;            /* +0x04 */
    u32 data_len;       /* +0x08 */
    u8 flags;           /* +0x0c bit7 = data-in */
    u8 lun;             /* +0x0d */
    u8 cb_len;          /* +0x0e */
    u8 cb[16];          /* +0x0f 0xF07D415B */
    u8 pad;             /* +0x1f */
} __attribute__((packed, aligned(4)));

/* ---- UART download context, 0xF07D4170 ------------------------------ */
struct uart_ctx {
    u32 cdb_idx;        /* +0x00 0x4170  words in the current CDB 0..4 */
    u32 done;           /* +0x04 0x4174  set by WAIT_DONE */
    u32 byte_idx;       /* +0x08 0x4178  bytes in the current word 0..3 */
    u32 cdb_sum;        /* +0x0c 0x417C  16-bit check of the last CDB, 0 = good */
    u32 reply_sum;      /* +0x10 0x4180  IS_NVT reply checksum */
    u32 dl_addr;        /* +0x14 0x4184  WRLOADER address, != 0: payload phase */
    u32 dl_words;       /* +0x18 0x4188  payload words received */
    u32 chip_id;        /* +0x1c 0x418C  TOP+0xF0, sent by IS_NVT */
    u32 dl_total;       /* +0x20 0x4190  payload words expected */
    u32 rx_bytes[4];    /* +0x24 0x4194  one byte per word */
    u32 cdb[4];         /* +0x34 0x41A4  current 16-byte CDB */
    u32 dl_ptr;         /* +0x44 0x41B4  store pointer (= dl_addr) */
    u32 wrloader_cdb[4];/* +0x48 0x41B8  echoed when the payload is complete */
};

/* ---- USB EP0 / chapter 9 context, 0xF07D41E0 ------------------------ */
struct usb_setup {
    u8 bmRequestType;
    u8 bRequest;
    u16 wValue;
    u16 wIndex;         /* 0xF07D41E4 */
    u16 wLength;        /* 0xF07D41E6 */
};

struct usb_ep0_ctx {
    struct usb_setup setup;     /* +0x00 0x41E0 */
    u32 seen_setup;     /* +0x08 0x41E8  first SETUP after reset handled */
    u32 result;         /* +0x0c 0x41EC  EP0_* */
    u32 in_len;         /* +0x10 0x41F0  control-IN bytes left */
    u32 in_ptr;         /* +0x14 0x41F4 */
    u32 config;         /* +0x18 0x41F8  SET_CONFIGURATION value */
    u32 iface;          /* +0x1c 0x41FC */
    u32 alt;            /* +0x20 0x4200 */
    u32 ep0_halt;       /* +0x24 0x4204  SET_FEATURE(HALT) on EP0 */
    u32 high_speed;     /* +0x28 0x4208  DEV_CTRL bit6 at the first SETUP */
    u32 in_pending;     /* +0x2c 0x420C */
    u32 init_done;      /* +0x30 0x4210 */
};

/* ---- loader header as the ROM reads it ------------------------------ */
struct loader_hdr {
    u32 entry;          /* +0x000 */
    u32 load;           /* +0x004 */
    u32 rsv008;         /* +0x008 */
    u32 log;            /* +0x00c HDR_LOG_* */
    u32 rsv010[5];      /* +0x010 */
    u32 size;           /* +0x024 */
    u32 rsv028[2];      /* +0x028 */
    u16 tag;            /* +0x030 LOADER_TAG */
    u16 rsv032;         /* +0x032 */
    u32 poll_reg;       /* +0x034 */
    u32 script_word;    /* +0x038 HDR_SW_* */
    u32 lane_base;      /* +0x03c */
    u32 script[18];     /* +0x040 entries (read sequentially, count from
                           script_word: nothing bounds them to 0x88) */
    u32 flags;          /* +0x088 HDR_FLAG_* */
    u32 rsv08c[13];     /* +0x08c */
    u8 rsa_n[256];      /* +0x0c0 */
    u8 rsa_e[32];       /* +0x1c0 */
    union {
        u8 rsa_sig[256];        /* +0x1e0 RSA mode */
        struct {
            u8 rsv1e0[0xe0];
            u8 aes_digest[32];  /* +0x2c0 AES mode */
        };
    };
    u8 sha256[32];      /* +0x2e0 */
    /* +0x300 .. +0x7fff encrypted area */
};

/* ---- lvalue accessors for the fixed-address globals ----------------- */
#define SRAM_VAR(type, addr)    (*(type *)(addr))

#define g_cluster_sects         SRAM_VAR(u32, SRAM_CLUSTER_SECTS)
#define g_boot_source           SRAM_VAR(u32, SRAM_BOOT_SOURCE)
#define g_timeout               SRAM_VAR(u32, SRAM_TIMEOUT)
#define g_otp_tmo               SRAM_VAR(u32, SRAM_OTP_TMO)
#define g_saved_cg74            SRAM_VAR(u32, SRAM_SAVED_CG74)
#define g_saved_tmr30           SRAM_VAR(u32, SRAM_SAVED_TMR30)
#define g_eth                   SRAM_VAR(struct eth_ctx, SRAM_ETH)
#define g_sd                    SRAM_VAR(struct sd_ctx, SRAM_SD)
#define g_flash                 SRAM_VAR(struct flash_ctx, SRAM_FLASH)
#define g_usb_state             SRAM_VAR(u32, SRAM_USB_STATE)
#define g_usb_dl_addr           SRAM_VAR(u32, SRAM_USB_DL_ADDR)
#define g_usb_reply             ((u8 *)SRAM_USB_REPLY)
#define g_xfer                  SRAM_VAR(struct usb_xfer, SRAM_XFER)
#define g_sense_key             SRAM_VAR(u32, SRAM_SENSE_KEY)
#define g_sense_asc             SRAM_VAR(u32, SRAM_SENSE_ASC)
#define g_csw                   SRAM_VAR(struct bot_csw, SRAM_CSW)
#define g_cbw                   SRAM_VAR(struct bot_cbw, SRAM_CBW)
#define g_usb_regs              SRAM_VAR(u32, SRAM_USB_REGS)
#define g_uart                  SRAM_VAR(struct uart_ctx, SRAM_UART)
#define g_bot_state             SRAM_VAR(u8, SRAM_BOT_STATE)
#define g_usb_opened            SRAM_VAR(u32, SRAM_USB_OPENED)
#define g_ep0                   SRAM_VAR(struct usb_ep0_ctx, SRAM_EP0)

#define g_hdr_stage             SRAM_VAR(struct loader_hdr, SRAM_HDR_STAGE)
#define g_dma_direct            SRAM_VAR(u32, SRAM_DMA_DIRECT)

#define eth_txdesc              ((struct eqos_desc *)SRAM_ETH_TXDESC)
#define eth_rxdesc              ((struct eqos_desc *)SRAM_ETH_RXDESC)

/* USB device register at g_usb_regs + off (the ROM reloads the base from
   SRAM for every access; keep that, it is how the original reads). */
#define USB_REG(off)            (*(volatile u32 *)(g_usb_regs + (off)))
#define USB_REG8(off)           (*(volatile u8 *)(g_usb_regs + (off)))

/* layout checks */
#define SRAM_CHECK(c)           _Static_assert(c, #c)
SRAM_CHECK(sizeof(struct eth_ctx) == SRAM_SD - SRAM_ETH);
SRAM_CHECK(sizeof(struct sd_ctx) == SRAM_FLASH - SRAM_SD);
SRAM_CHECK(sizeof(struct flash_ctx) == SRAM_USB_STATE - SRAM_FLASH);
SRAM_CHECK(sizeof(struct usb_xfer) == SRAM_SENSE_KEY - SRAM_XFER);
SRAM_CHECK(sizeof(struct bot_csw) == SRAM_CBW - SRAM_CSW);
SRAM_CHECK(sizeof(struct bot_cbw) == SRAM_USB_REGS - SRAM_CBW);
SRAM_CHECK(__builtin_offsetof(struct bot_cbw, cb) == SRAM_CBW_CB - SRAM_CBW);
SRAM_CHECK(sizeof(struct uart_ctx) == 0x58);
SRAM_CHECK(sizeof(struct usb_ep0_ctx) == SRAM_GLOBALS_END - SRAM_EP0);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, size) == HDR_SIZE);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, tag) == HDR_TAG);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, script_word) == HDR_SCRIPT_PTR);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, script) == HDR_SCRIPT);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, flags) == HDR_FLAGS);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, rsa_n) == HDR_RSA_N);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, rsa_e) == HDR_RSA_E);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, rsa_sig) == HDR_RSA_SIG);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, aes_digest) == HDR_AES_DIGEST);
SRAM_CHECK(__builtin_offsetof(struct loader_hdr, sha256) == HDR_SHA256);
SRAM_CHECK(sizeof(struct loader_hdr) == HDR_ENC_AREA);

#endif /* !__ASSEMBLER__ */

#endif /* BOOTROM_NA51089_SRAM_H */
