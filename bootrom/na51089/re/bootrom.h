/*
 * bootrom.h -- shared types, constants and the prototype of every ledger
 * function (scratchpad/functions.txt func_000..func_124) of the NA51089
 * mask ROM reconstruction.  Names and signatures are the ledger's; the
 * comment after each prototype is its original address and owner file.
 *
 * Constants are usable from assembly; types and prototypes are C-only.
 * This header does not include the register map or sram.h.
 */
#ifndef BOOTROM_NA51089_BOOTROM_H
#define BOOTROM_NA51089_BOOTROM_H

/*
 * Console letters (UART0 THR, 115200 8N1).  Most are single raw stores
 * with no FIFO wait (UART0_PUTC_RAW); uart_boot uses uart0_putc.
 * Several letters are reused, so read them in sequence:
 *
 *   N        ROM start (uart0_init_banner)
 *   P        SPI NOR (BS 0)
 *   D        SPI NAND, on-die ECC (BS 2/6)  -- and, after the strap
 *            letter, SPI clock from pad (OTP bit4)
 *   R        SPI NAND, RS ECC (BS 3/0xA)    -- also eMMC OTP option
 *            (bit14) and RSA secure boot
 *   I        SPI internal clock (OTP bit4 clear) -- also RMII refclk in
 *            ("HI") and the per-frame "RI\r\n" in eth_boot
 *   p        NOR RDSR step (OTP bit6 clear)
 *   W        BST[12]: watchdog not armed for internal storage
 *   J        previous reset came from the watchdog: internal storage
 *            skipped, straight to next_boot_source()
 *   E        eMMC (BS 8/9), then '4' or '8' (bus width) -- also the
 *            built-in PHY ("HE")
 *   S 1      SD card (SDIO1); F = SD OTP option (bit11); e x = exFAT
 *            -- 'S' is also secure boot's encrypted area, and S1..S4 are
 *            the Ethernet DHCP/ARP/TFTP sends
 *   H        Ethernet, then E / O / I (internal PHY / RMII out / in),
 *            "AN0"/"AN1" autoneg timeouts, s100F|s100H|s10F|s10H, 'X'
 *            when the MAC is up, then the state-machine letters (a b c
 *            Of Ak C D r p t, "IP .. SRV ..", "MSK ..", block numbers)
 *            and "Dn" when the transfer is complete
 *   U        USB (BS 5/0xB)
 *   K<n>     boot_fail(n): 'K' then '0'+n, so code 10 prints "K:"
 *   X        secure boot enabled (OTP bit5); then S (decrypt), R (RSA)
 *            or A (AES digest), C (key-hash check)
 *   T        jump to the loader (run_loader; a word store in start.S)
 *   .        on UART1, every 256 payload words of a UART download
 */
#define LTR_ROM_START           'N'
#define LTR_NOR                 'P'
#define LTR_NAND_ODECC          'D'
#define LTR_NAND_RSECC          'R'
#define LTR_SPI_PADCLK          'D'
#define LTR_SPI_INTCLK          'I'
#define LTR_NOR_RDSR            'p'
#define LTR_NO_WDT              'W'
#define LTR_WDT_RESET           'J'
#define LTR_EMMC                'E'
#define LTR_EMMC_OPT            'R'
#define LTR_SD                  'S'
#define LTR_SD_PORT             '1'
#define LTR_SD_OPT              'F'
#define LTR_ETH                 'H'
#define LTR_ETH_INTERNAL        'E'
#define LTR_ETH_RMII_OUT        'O'
#define LTR_ETH_RMII_IN         'I'
#define LTR_ETH_UP              'X'
#define LTR_USB                 'U'
#define LTR_FAIL                'K'
#define LTR_SECURE              'X'
#define LTR_SECURE_DECRYPT      'S'
#define LTR_SECURE_RSA          'R'
#define LTR_SECURE_AES          'A'
#define LTR_SECURE_KEYHASH      'C'
#define LTR_JUMP                'T'
#define LTR_UART_PROGRESS       '.'

/*
 * Boot source (BS) strap codes: TOP+0[3:0], the index of the 0x1c0
 * dispatch table.  g_boot_source holds the same codes: the strap at
 * dispatch, then the source next_boot_source() switched to (it only
 * ever stores BOOT_SRC_* below).
 */
#define BS_SPI_NOR              0x0     /* flash_boot */
#define BS_SD                   0x1     /* sd_boot */
#define BS_NAND_ODECC_2K        0x2     /* flash_boot */
#define BS_NAND_RSECC_2K        0x3     /* flash_boot */
#define BS_ETH                  0x4     /* eth_boot */
#define BS_USB_HS               0x5     /* usb_boot */
#define BS_NAND_ODECC_4K        0x6     /* flash_boot */
#define BS_RESERVED_7           0x7     /* hang_reserved_strap */
#define BS_EMMC_4BIT            0x8     /* emmc_boot */
#define BS_EMMC_8BIT            0x9     /* emmc_boot */
#define BS_NAND_RSECC_4K        0xA     /* flash_boot */
#define BS_USB_FS               0xB     /* usb_boot, DEV_CTRL FORCE_FS */
#define BS_UART                 0xC     /* uart_boot */
#define BS_RESERVED_D           0xD     /* hang_reserved_strap */
#define BS_RESERVED_E           0xE
#define BS_RESERVED_F           0xF
#define BS_COUNT                16

/* Boot-source ids next_boot_source() stores in g_boot_source. */
#define BOOT_SRC_SD             BS_SD
#define BOOT_SRC_ETH            BS_ETH
#define BOOT_SRC_USB            BS_USB_HS       /* passed as usb_boot(5) */
#define BOOT_SRC_UART           BS_UART

/*
 * next_boot_source(): m = 1 << g_boot_source (g_boot_source > 12 -> ETH)
 *   m & 0x174D (BS 0,2,3,6,8,9,10,12)  -> SD
 *   m & 0x0820 (BS 5,11)               -> UART
 *   m & 0x0010 (BS 4)                  -> USB
 *   otherwise (BS 1, 7)                -> ETH
 * i.e. flash/eMMC -> SD -> ETH -> USB -> UART -> SD -> ...
 */
#define FALLBACK_TO_SD_MASK     0x174D
#define FALLBACK_TO_UART_MASK   0x0820
#define FALLBACK_TO_USB_MASK    0x0010

/* boot_fail() codes: printed as 'K', '0' + code */
#define BOOT_FAIL_SD_INIT       2       /* SD card init */
#define BOOT_FAIL_MBR           3       /* SD MBR read */
#define BOOT_FAIL_PBR           4       /* SD PBR read */
#define BOOT_FAIL_FIRST_SECTOR  5       /* first sector / eMMC boot read */
#define BOOT_FAIL_BAD_STRAP     6       /* flash_boot with a non-flash strap */
#define BOOT_FAIL_LOADER_READ   7       /* body read / NAND: no good copy */
#define BOOT_FAIL_ETH_OR_HDR    8       /* ETH DMA/PHY, or header extension read */
#define BOOT_FAIL_CHECKSUM      9       /* checksum / 0xAA55 tag / secure boot */
#define BOOT_FAIL_TIMEOUT       10      /* prints "K:" */

/* Timer limits as the ROM writes them (TIMER_CNT ticks, 1 us).  The
   N s ones are N*1e6-1 compared with "<=", the 30 s one is exact. */
#define TMO_1S                  0x000F423F
#define TMO_4S                  0x003D08FF
#define TMO_5S                  0x004C4B40
#define TMO_10S                 0x0098967F
#define TMO_30S                 0x01C9C380      /* times (OTP t + 1) */
#define DLY_100MS               100000          /* flash reset after a WDT reset */
#define DLY_500MS               500000          /* eth_boot before RX */
#define DHCP_RETRIES_PER_T      6               /* times (OTP t + 1) */

/* Loader checksum / tag (check_and_run_loader, hdr_tag_ok) */
#define LOADER_TAG              0xAA55
#define CDB_SUM_BIAS            0x1c    /* cdb_sum: 8 halfwords + 0x1c == 0 */

#ifndef __ASSEMBLER__

/* ---- freestanding types --------------------------------------------- */
typedef unsigned char           u8;
typedef unsigned short          u16;
typedef unsigned int            u32;
typedef unsigned long long      u64;
typedef signed int              s32;
typedef __SIZE_TYPE__           size_t;

#ifndef NULL
#define NULL                    ((void *)0)
#endif

#define __noreturn              __attribute__((noreturn))
#define ARRAY_SIZE(a)           (sizeof(a) / sizeof((a)[0]))

/* LZMA range coder state (rc_normalize / rc_bit, lzma.c) */
typedef struct rc {
    const u8 *ptr;              /* +0x00 next input byte */
    const u8 *end;              /* +0x04 end of input */
    u32 code;                   /* +0x08 */
    u32 range;                  /* +0x0c */
    u32 bound;                  /* +0x10 last bound computed by rc_bit */
} rc_t;

/* ---- start.S --------------------------------------------------------- */
/* exception stubs, reset path and dispatch are asm labels: not callable */
void exc_hang(void) __noreturn;                         /* func_000 0x003c */
void reset_handler(void) __noreturn;                    /* func_001 0x0054 */
void boot_dispatch(void) __noreturn;                    /* func_002 0x01a4 */
/* returns only when !fatal and the loader fails; never on success */
void check_and_run_loader(int fatal, int hwsum_valid, u32 hwsum); /* func_003 0x0200 */
/* asm tail: expects r4 = load address -- do not call from C */
void run_loader(void) __noreturn;                       /* func_004 0x02b0 */
void hang_reserved_strap(void) __noreturn;              /* func_005 0x02dc */

/* ---- flash.c --------------------------------------------------------- */
void spi_settle(void);                                  /* func_006 0x0320 */
void spi_cmd(u32 op);                                   /* func_007 0x0348 */
int spi_wait(u32 mask);                                 /* func_008 0x036c */
int spi_pio_read(u32 *dst, int want, int len);          /* func_009 0x03b0 */
void spi_xfer_setup(u32 ctrl, u32 r38, u32 r3c, u32 len); /* func_010 0x0414 */
void spi_cs(int on);                                    /* func_011 0x0498 */
void spi_bus(u32 a, u32 b);                             /* func_012 0x04c0 */
void spi_mode(u32 a, u32 b);                            /* func_013 0x04e8 */
void spi_wide(int on);                                  /* func_014 0x0510 */
int nor_read(void *dst, u32 addr, u32 len);             /* func_015 0x0538 */
int nand_read_page(u32 row, u32 col, void *dst, u32 len); /* func_016 0x0704 */
/* returns 0 only when a NAND scan finds nothing (re-runs the dispatch) */
int flash_boot(int strap);                              /* func_017 0x09a0 */

/* ---- secure.c -------------------------------------------------------- */
void aes_load_efuse_key(void);                          /* func_018 0x1364 */
int aes_cbc_decrypt(const u8 *src, u8 *dst, int len);   /* func_019 0x13c0 */
void reverse256(const u8 *src, u8 *dst);                /* func_020 0x14b8 */
void rsa_load_n(const u32 *src, int nwords);            /* func_033 0x2158 */
void rsa_load_e(const u32 *src, int nwords);            /* func_034 0x21d4 */
void rsa_run(const u32 *msg, int nwords);               /* func_035 0x2250 */
void rsa_read(u32 *dst, int nwords);                    /* func_036 0x2338 */
int secure_boot_verify(u8 *img);                        /* func_037 0x239c */
u32 otp_boot_cfg(void);                                 /* func_064 0x4df4 */
void efuse_key_select(u32 a, u32 b);                    /* func_065 0x4e0c */
void sha256(const void *data, int len, u32 out[8]);     /* func_116 0x6d68 */
u32 efuse_read(int idx);                                /* func_117 0x6eb4 */

/* ---- lib.c ----------------------------------------------------------- */
int popcount(u32 x);                                    /* func_021 0x14fc */
void memcpy32(void *dst, const void *src, u32 n);       /* func_025 0x16e8 */
void memset32(void *dst, u8 c, u32 n);                  /* func_026 0x1714 */
u8 rd8(const void *p);                                  /* func_042 0x284c */
u32 rd32le(const void *p);                              /* func_043 0x2870 */
u16 rd16le(const void *p);                              /* func_044 0x28b8 */
void wr16le(void *p, u16 v);                            /* func_045 0x28e0 */
void wr8(void *p, u8 v);                                /* func_046 0x292c */
void memcpy8(void *dst, const void *src, u32 n);        /* func_047 0x295c */
void wr32le(void *p, u32 v);                            /* func_050 0x2b14 */
u32 swap16x2(u32 x);                                    /* func_052 0x2c84 */

/* ---- sd.c ------------------------------------------------------------ */
u32 mbr_first_lba(const u8 *mbr);                       /* func_022 0x1528 */
u32 fat_data_start(const u8 *pbr);                      /* func_023 0x155c */
u32 fat_cluster_sects(void);                            /* func_024 0x16d0 */
int sd_read_blocks(u32 lba, int n, void *dst);          /* func_059 0x4244 */
void sd_boot(void);                                     /* func_060 0x439c */

/* ---- boot.c ---------------------------------------------------------- */
void prejump_restore(void);                             /* func_027 0x1748 */
/* switches g_boot_source and calls the next boot_* (does not come back
   in practice; the original returns through its frame if one does) */
void next_boot_source(void);                            /* func_029 0x180c */
void boot_fail(int code);                               /* func_030 0x18b4 */
void wdt_arm_or_skip(void);                             /* func_032 0x20b4 */

/* ---- loader.c -------------------------------------------------------- */
/* script = header + 0x38 (HDR_SCRIPT_PTR) */
int hdr_tag_ok(const u32 *script);                      /* func_028 0x17f0 */
void hdr_script_run(const u32 *script);                 /* func_031 0x18e0 */

/* ---- timer.c --------------------------------------------------------- */
void timer_start(void);                                 /* func_038 0x277c */
u32 timer_now(void);                                    /* func_039 0x27c4 */
void timer_stop(void);                                  /* func_040 0x27dc */
void udelay(u32 us);                                    /* func_041 0x2824 */

/* ---- eth.c (net.c holds the other half of eth_boot, see eth.h) ------- */
u16 mdio_read(int phy, int reg);                        /* func_048 0x2a84 */
void mdio_write(int phy, int reg, u16 v);               /* func_049 0x2acc */
void eth_xmit(int hdr_len, const void *payload, int pay_len); /* func_051 0x2ba4 */
void eth_boot(void);                                    /* func_053 0x2cb4 */

/* ---- sdio.c (ctrl 0 = SDIO1, 1 = SDIO3) ------------------------------ */
int sdio_wait(int ctrl, u32 mask);                      /* func_054 0x3e80 */
int sdio_read_data(int ctrl, void *dst, int len);       /* func_055 0x3ecc */
int sdio_cmd(int ctrl, u32 cmdflags, u32 arg, u32 *resp); /* func_058 0x40fc */

/* ---- emmc.c ---------------------------------------------------------- */
void emmc_boot_end(void);                               /* func_056 0x3f84 */
int emmc_boot_read(void *dst, int cont, int len);       /* func_057 0x3fac */
void emmc_boot(void);                                   /* func_061 0x48ec */

/* ---- console.c ------------------------------------------------------- */
void uart0_init_banner(void);                           /* func_062 0x4c68 */
void uart0_put32(u32 w);                                /* func_066 0x4e28 */
void uart0_putc(u8 c);                                  /* func_067 0x4e68 */
u8 uart0_getc(void);                                    /* func_068 0x4e8c */
int uart0_rx_ready(void);                               /* func_069 0x4ebc */
void uart0_reinit(void);                                /* func_070 0x4ed8 */
void put_hex32(u32 v);                                  /* func_115 0x6d20 */
void debug_msg(const char *s);                          /* func_119 0x700c */
void debug_msg_err(const char *s, u32 v);               /* func_122 0x7114 */
void debug_msg_var(const char *s, u32 v);               /* func_123 0x7144 */

/* ---- uart_boot.c ----------------------------------------------------- */
void uart_boot(void);                                   /* func_071 0x4f28 */

/* ---- usb_msc.c ------------------------------------------------------- */
void usb_boot(int strap);                               /* func_063 0x4cd4 */
int usb_bot_rx_cbw(void);                               /* func_073 0x5458 */
void usb_bot_arm(void);                                 /* func_074 0x54bc */
void usb_bulk_out_irq(void);                            /* func_075 0x558c */
void usb_bulk_in_irq(void);                             /* func_076 0x55f0 */
void usb_bot_reset(void);                               /* func_077 0x565c */
u16 cdb_sum(u32 w0, u32 w1, u32 w2, u32 w3);            /* func_078 0x56b4 */
int nvt_vendor_cdb(const u8 *cbw);                      /* func_079 0x56e8 */
void usb_bot_halt(void);                                /* func_099 0x63b0 */
void scsi_request_sense(void);                          /* func_104 0x674c */
void scsi_clear_sense(void);                            /* func_105 0x67c8 */
void scsi_clear_sense_wrap(void);                       /* func_106 0x67e8 */
void scsi_inquiry(void);                                /* func_107 0x67f8 */
void scsi_mode_sense6(void);                            /* func_108 0x6834 */
void scsi_invalid_opcode(void);                         /* func_109 0x68c4 */
void scsi_no_medium(void);                              /* func_110 0x68f4 */
int usb_bot_data_in(void);                              /* func_111 0x6958 */
int usb_bot_data_out(void);                             /* func_112 0x69e8 */
int usb_bot_data_phase(void);                           /* func_113 0x6ab0 */
int scsi_dispatch(void);                                /* func_114 0x6c14 */

/* ---- usb.c ----------------------------------------------------------- */
int usb_fifo_dma(void *buf, u32 len, int in);           /* func_072 0x53c0 */
void usb_dev_hw_init(void);                             /* func_080 0x5964 */
void usb_init(void);                                    /* func_081 0x59dc */
void usb_cfg_eps_hs(void);                              /* func_082 0x5a34 */
void usb_cfg_eps_fs(void);                              /* func_083 0x5ac0 */
void usb_ep0_status(void);                              /* func_084 0x5b58 */
int usb_ep0_dma(void *buf, u32 len, int in);            /* func_085 0x5b8c */
void usb_ep0_tx_next(void);                             /* func_086 0x5c1c */
void usb_reset_toggles(void);                           /* func_087 0x5c74 */
int usb_get_status(void);                               /* func_088 0x5cd8 */
int usb_clear_feature(void);                            /* func_089 0x5dd0 */
int usb_set_feature(void);                              /* func_090 0x5eb8 */
int usb_set_address(void);                              /* func_091 0x5f84 */
int usb_get_descriptor(void);                           /* func_092 0x5fc0 */
int usb_get_configuration(void);                        /* func_093 0x61b0 */
int usb_set_configuration(void);                        /* func_094 0x61dc */
int usb_get_interface(void);                            /* func_095 0x627c */
int usb_set_interface(void);                            /* func_096 0x62e0 */
void usb_ep0_in_irq(void);                              /* func_097 0x634c */
void usb_ep0_out_irq(void);                             /* func_098 0x638c */
int usb_std_request(void);                              /* func_100 0x63d4 */
int usb_class_request(void);                            /* func_101 0x64c0 */
void usb_setup_irq(void);                               /* func_102 0x6524 */
void usb_poll(void);                                    /* func_103 0x65b8 */

/* ---- lz.c / lzma.c (exported ROM services) --------------------------- */
u32 LZ_Uncompress(const u8 *in, u8 *out, u32 insize);   /* func_118 0x6f2c */
void rc_normalize(rc_t *rc);                            /* func_120 0x7044 */
int rc_bit(rc_t *rc, u16 *prob);                        /* func_121 0x7094 */
int lzma_inflate(const u8 *in, u32 in_size, u8 *out, u32 out_size,
                 u16 *tmp, u32 tmp_size);               /* func_124 0x716c */

#endif /* !__ASSEMBLER__ */

#endif /* BOOTROM_NA51089_BOOTROM_H */
