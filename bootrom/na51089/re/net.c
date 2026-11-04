/*
 * net.c -- second half of func_053 eth_boot (0x3384..0x3e7c): the
 * DHCP / ARP / TFTP state machine that fetches "loader.bin", and the
 * frame templates data_24..data_28 (0x7a50..0x7c11).
 *
 * The original is one function with eth_hw_init (eth.c) and the static
 * helpers below all inlined; see eth.h for the split.  Every console
 * letter is a raw THR store with no FIFO wait.
 *
 * Shape of the loop (0x33cc..0x3ac0):
 *
 *   do {
 *       status = DMA_CH0_STATUS; DMA_CH0_STATUS = status;   (W1C all)
 *       if (RI) { "RI\r\n"; handle every frame the DMA has filled }
 *       one step of the timer-side state switch (table at 0x39a4)
 *   } while (g_eth.running);
 *
 * Received frames are handled by state (2 offer, 3 nothing, 4 ack,
 * anything else ARP / TFTP DATA); the send side runs from the switch.
 * The protocol is minimal and trusting:
 *   - frames are not length- or error-checked, nor are the IP/UDP
 *     headers of DHCP replies: an offer or ack is recognised by its xid
 *     alone, broadcast or unicast, any message type;
 *   - the REQUEST names siaddr as the server identifier (option 54),
 *     not the offer's own option 54;
 *   - the ARP reply for the server is never awaited; the RRQ and every
 *     ACK go to the MAC the OFFER came from;
 *   - after the RRQ there is no timeout and no retransmission: a lost
 *     RRQ, DATA or ACK hangs the ROM until the watchdog (if any);
 *   - blocks are not checked for sequence; block n lands at
 *     0xF07C0000 + (n-1)*512 if n <= 64, the first short block ends it.
 */
#include "sram.h"
#include "eth.h"

/* ---- received frame offsets (Ethernet II, IPv4 without options) ---- */
#define RXF_SRC_MAC             0x06
#define RXF_ETHERTYPE           0x0c
#define RXF_ARP_OP              0x14
#define RXF_ARP_SHA             0x16
#define RXF_ARP_SPA             0x1c
#define RXF_ARP_TPA             0x26
#define RXF_IP_SRC              0x1a
#define RXF_IP_DST              0x1e
#define RXF_UDP_SPORT           0x22
#define RXF_UDP_DPORT           0x24
#define RXF_UDP_LEN             0x26
#define RXF_UDP_DATA            0x2a    /* BOOTP / TFTP start */
#define RXF_BOOTP_XID           (RXF_UDP_DATA + 0x04)
#define RXF_BOOTP_YIADDR        (RXF_UDP_DATA + 0x10)
#define RXF_BOOTP_SIADDR        (RXF_UDP_DATA + 0x14)
#define RXF_TFTP_OP             (RXF_UDP_DATA + 0x00)
#define RXF_TFTP_BLOCK          (RXF_UDP_DATA + 0x02)
#define RXF_TFTP_DATA           (RXF_UDP_DATA + 0x04)
#define RXF_ARP_REPLY_COPY      0x22    /* Ethernet + ARP up to THA */

#define ETHERTYPE_IPV4          0x0800
#define ETHERTYPE_ARP           0x0806
#define ARP_OP_REQUEST          1
#define ARP_OP_REPLY_LE         0x0200  /* 0x0002 as stored by wr16le */
#define UDP_HDR_LEN             8
#define TFTP_HDR_LEN            4
#define TFTP_OP_DATA            3
#define TFTP_BLOCK_SIZE         512

/* BOOTP: options start after the 236-byte fixed part and the cookie */
#define BOOTP_OPTIONS_OFF       0xf0
#define DHO_SUBNET_MASK         1
#define DHO_REQUESTED_IP        50
#define DHO_MSG_TYPE            53
#define DHO_SERVER_ID           54
#define DHO_END                 255
#define DHCPREQUEST             3
#define NET_DEFAULT_MASK        0xffffff00

/* ---- transmit side ------------------------------------------------- */
#define TXHDR                   ((u8 *)SRAM_ETH_TXHDR)
#define TXPAY                   ((u8 *)SRAM_ETH_TXPAY)
#define TXH_DST_MAC             0x00
#define TXP_TFTP_OP             0x00
#define TXP_TFTP_BLOCK          0x02
#define TX_ARP_REPLY_LEN        0x36    /* 42 B of ARP + 12 B of whatever
                                           the header buffer held */
#define TX_CLEAR_HDR            0x60    /* header + first 0x20 B of payload */
#define TX_CLEAR_DHCP           0x156   /* memset32 rounds it to 0x154 */
#define TFTP_RRQ_LEN            0x13
#define TFTP_ACK_LEN            4
#define TFTP_ACK_IP_TOTLEN_LE   0x2000  /* 32 = 20 + 8 + 4, network order */
#define TFTP_ACK_UDP_LEN_LE     0x0c00  /* 12 = 8 + 4 */
#define TFTP_ACK_OP_LE          0x0400  /* bytes 00 04 00 00 */

/* ---- frame templates, data_24..data_28 ------------------------------
 * One struct so that the five tables keep the original order and gaps
 * (gcc emits separate objects in first-use order); link.ld places
 * net.o's const data first among the frame tables, at 0x7a50.  MAC
 * 00:55:7b:b5:7d:f7 and xid 0x3d1d are placeholders, always patched
 * before sending.  IP and UDP checksums are zero: the MAC inserts them
 * (TDES3 CIC = 3).  The DHCP tables are read with memcpy32: word-aligned.
 */
#define NET_TMPL_ALIGN          __attribute__((aligned(4)))

static const struct net_templates {
    u8 tftp_frame[ETH_TXHDR_LEN] NET_TMPL_ALIGN;        /* data_24 0x7a50 */
    u8 tftp_rrq[TFTP_RRQ_LEN] NET_TMPL_ALIGN;           /* data_25 0x7a7c */
    u8 dhcp_frame[ETH_TXHDR_LEN] NET_TMPL_ALIGN;        /* data_26 0x7a90 */
    u8 bootp_discover[ETH_TXPAY_SIZE] NET_TMPL_ALIGN;   /* data_27 0x7abc */
    u8 arp_request[ETH_TXHDR_LEN] NET_TMPL_ALIGN;       /* data_28 0x7be8 */
} net_tmpl = {
    /* data_24: Ethernet + IPv4 + UDP for TFTP */
    .tftp_frame = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,     /* dst (patched) */
        0x00, 0x55, 0x7b, 0xb5, 0x7d, 0xf7,     /* src (patched) */
        0x08, 0x00,                             /* IPv4 */
        0x45, 0x00, 0x00, 0x2f,                 /* total length 47 (RRQ) */
        0x00, 0x00, 0x00, 0x00,                 /* id, flags */
        0xff, 0x11, 0x00, 0x00,                 /* TTL 255, UDP, csum */
        0x00, 0x00, 0x00, 0x00,                 /* src 0.0.0.0 (patched) */
        0xff, 0xff, 0xff, 0xff,                 /* dst (patched) */
        0x0b, 0x92, 0x00, 0x45,                 /* sport 2962, dport 69 */
        0x00, 0x1b, 0x00, 0x00,                 /* UDP length 27, csum */
    },
    /* data_25: TFTP RRQ "loader.bin", mode "octet" */
    .tftp_rrq = {
        0x00, 0x01,
        'l', 'o', 'a', 'd', 'e', 'r', '.', 'b', 'i', 'n', 0x00,
        'o', 'c', 't', 'e', 't', 0x00,
    },
    /* data_26: Ethernet + IPv4 + UDP for DHCP */
    .dhcp_frame = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,     /* broadcast */
        0x00, 0x55, 0x7b, 0xb5, 0x7d, 0xf7,     /* src (patched) */
        0x08, 0x00,
        0x45, 0x00, 0x01, 0x48,                 /* total length 328 */
        0x00, 0x00, 0x00, 0x00,
        0xff, 0x11, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,                 /* 0.0.0.0 */
        0xff, 0xff, 0xff, 0xff,                 /* 255.255.255.255 */
        0x00, 0x44, 0x00, 0x43,                 /* 68 -> 67 */
        0x01, 0x34, 0x00, 0x00,                 /* UDP length 308 */
    },
    /* data_27: BOOTP DHCPDISCOVER.  flags = 0: no broadcast bit, so
       servers may unicast the reply (the ROM accepts either). */
    .bootp_discover = {
        [0x00] = 0x01, 0x01, 0x06, 0x00,        /* BOOTREQUEST, Ethernet */
        [0x04] = 0x00, 0x00, 0x3d, 0x1d,        /* xid (patched) */
        [0x1c] = 0x00, 0x55, 0x7b, 0xb5, 0x7d, 0xf7, /* chaddr (patched) */
        [0xec] = 0x63, 0x82, 0x53, 0x63,        /* magic cookie */
        [0xf0] = DHO_MSG_TYPE, 1, 1,            /* DHCPDISCOVER */
                 DHO_REQUESTED_IP, 4, 0, 0, 0, 0, /* 0.0.0.0 */
                 DHO_END,
    },
    /* data_28: ARP request */
    .arp_request = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x55, 0x7b, 0xb5, 0x7d, 0xf7,     /* src (patched) */
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04,     /* Ethernet / IPv4 */
        0x00, 0x01,                             /* request */
        0x00, 0x55, 0x7b, 0xb5, 0x7d, 0xf7,     /* SHA (patched) */
        0x00, 0x00, 0x00, 0x00,                 /* SPA (patched) */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     /* THA */
        0x00, 0x00, 0x00, 0x00,                 /* TPA (patched) */
    },
};

#define NET_TMPL_AT(m, addr) \
    _Static_assert(__builtin_offsetof(struct net_templates, m) == (addr) - 0x7a50, #m)
NET_TMPL_AT(tftp_rrq, 0x7a7c);
NET_TMPL_AT(dhcp_frame, 0x7a90);
NET_TMPL_AT(bootp_discover, 0x7abc);
NET_TMPL_AT(arp_request, 0x7be8);

/* Network-order loads from a frame: the ROM always pairs rd32le with
   `rev`, and rd16le with swap16x2. */
static inline u32 get_be32(const u8 *p)
{
    return __builtin_bswap32(rd32le(p));
}

static inline u32 get_be16(const u8 *p)
{
    return swap16x2(rd16le(p));
}

/* "\r\n" */
static inline void put_crlf(void)
{
    UART0_PUTC_RAW('\r');
    UART0_PUTC_RAW('\n');
}

/*
 * State 2, 0x3438..0x3624: a DHCPOFFER?  'a', then only the xid is
 * checked.  On a match: "Of", yiaddr / siaddr taken, printed as
 * "IP %08X SRV %08X\r\n", option 1 printed as "MSK %08X\r\n" if it is
 * found, the source MAC kept as the server's (8 bytes copied: the MAC
 * plus the ethertype), and state 3 if yiaddr and siaddr share the
 * subnet.  If they do not (a relayed offer, a TFTP server elsewhere) the
 * ROM stays in state 2 and retries DISCOVER after 5 s -- with own_ip
 * now set.  'b' after every frame seen in this state.
 */
static void rx_offer(const u8 *frm)
{
    u32 bootp_len, off, opt, mask;

    UART0_PUTC_RAW('a');
    if (get_be32(frm + RXF_BOOTP_XID) == g_eth.xid) {
        UART0_PUTC_RAW('O');
        UART0_PUTC_RAW('f');
        g_eth.own_ip = get_be32(frm + RXF_BOOTP_YIADDR);
        g_eth.server_ip = get_be32(frm + RXF_BOOTP_SIADDR);
        bootp_len = get_be16(frm + RXF_UDP_LEN) - UDP_HDR_LEN;

        /* 0x34a4..0x3510.  Each put_hex32 is followed by a read of the
           chip-id register whose value is dropped. */
        UART0_PUTC_RAW('I');
        UART0_PUTC_RAW('P');
        UART0_PUTC_RAW(' ');
        put_hex32(g_eth.own_ip);
        (void)REG32(TOP_BASE + TOP_CHIP_ID);
        UART0_PUTC_RAW(' ');
        UART0_PUTC_RAW('S');
        UART0_PUTC_RAW('R');
        UART0_PUTC_RAW('V');
        UART0_PUTC_RAW(' ');
        put_hex32(g_eth.server_ip);
        (void)REG32(TOP_BASE + TOP_CHIP_ID);
        put_crlf();

        /* 0x3514..0x35f0: walk the options as (code, len) pairs up to the
           UDP length; mask defaults to /24.  Pad (0) is not special-cased:
           it is skipped as a TLV whose length is the next byte. */
        mask = NET_DEFAULT_MASK;
        for (off = BOOTP_OPTIONS_OFF; off < bootp_len; off += ((opt >> 8) & 0xff) + 2) {
            opt = rd32le(frm + RXF_UDP_DATA + off);
            if ((opt & 0xff) == DHO_SUBNET_MASK) {
                mask = get_be32(frm + RXF_UDP_DATA + off + 2);
                UART0_PUTC_RAW('M');
                UART0_PUTC_RAW('S');
                UART0_PUTC_RAW('K');
                UART0_PUTC_RAW(' ');
                put_hex32(mask);
                put_crlf();
                break;
            }
            if ((opt & 0xff) == DHO_END)
                break;
        }

        /* 0x35f4..0x3618 */
        memcpy8(g_eth.server_mac, frm + RXF_SRC_MAC, sizeof(g_eth.server_mac));
        if (((g_eth.own_ip ^ g_eth.server_ip) & mask) == 0)
            g_eth.state = ETH_ST_OFFERED;
    }
    UART0_PUTC_RAW('b');
}

/*
 * State 4, 0x3684..0x36c8: the DHCPACK.  'c', and a matching xid is all
 * it takes ("Ak", state 5): the message type is not read, so a NAK
 * would do as well, and nothing in the ACK is used.
 */
static void rx_ack(const u8 *frm)
{
    UART0_PUTC_RAW('c');
    if (get_be32(frm + RXF_BOOTP_XID) == g_eth.xid) {
        UART0_PUTC_RAW('A');
        UART0_PUTC_RAW('k');
        g_eth.state = ETH_ST_ARP;
    }
}

/*
 * 0x3708..0x37f8: ARP.  'D'; a request for our IP gets 'a', a reply built
 * in the TX header buffer from the request itself, 'r', the send, 'p'.
 * The reply is sent with a 0x36-byte header length, so its last 12 bytes
 * are whatever the header buffer held from the previous frame.
 */
static void rx_arp(const u8 *frm, u32 own_ip)
{
    u8 sha[8];
    u32 spa, op;

    UART0_PUTC_RAW('D');
    op = get_be16(frm + RXF_ARP_OP);
    if (own_ip != get_be32(frm + RXF_ARP_TPA) || op != ARP_OP_REQUEST)
        return;

    UART0_PUTC_RAW('a');
    spa = rd32le(frm + RXF_ARP_SPA);                    /* kept in wire order */
    memcpy8(sha, frm + RXF_ARP_SHA, 6);
    memcpy8(TXHDR, frm, RXF_ARP_REPLY_COPY);
    memcpy8(TXHDR + TXH_DST_MAC, sha, 6);
    memcpy8(TXHDR + TXH_SRC_MAC, g_eth.own_mac, 6);
    wr16le(TXHDR + TXH_ARP_OP, ARP_OP_REPLY_LE);
    memcpy8(TXHDR + TXH_ARP_SHA, g_eth.own_mac, 6);
    wr32le(TXHDR + TXH_ARP_SPA, __builtin_bswap32(g_eth.own_ip));
    memcpy8(TXHDR + TXH_ARP_THA, sha, 6);
    memcpy8(TXHDR + TXH_ARP_TPA, &spa, 4);
    UART0_PUTC_RAW('r');
    eth_xmit(TX_ARP_REPLY_LEN, NULL, 0);
    UART0_PUTC_RAW('p');
}

/*
 * 0x37fc..0x3990: IPv4 from the server to us: 't', then only UDP to port
 * 2962 carrying TFTP DATA goes further.  Block number printed, ACK sent
 * at once to the server's source port, then the data copied.
 *
 * Quirks kept: the copy is bounded neither by the frame nor by 512, and
 * block 0 passes the "<= 64" test, landing at 0xF07BFE00, just below the
 * image.  Blocks past 64 are acknowledged and dropped.  Any block
 * shorter than 512 bytes ends the transfer, whatever its number.  This
 * runs in any state but 2/3/4 once own_ip is set, not just 8/9.
 */
static void rx_ipv4(const u8 *frm, u32 own_ip)
{
    u32 src, dst, sport, udp_len, blk, data_len;

    src = rd32le(frm + RXF_IP_SRC);
    dst = rd32le(frm + RXF_IP_DST);
    if (__builtin_bswap32(src) != g_eth.server_ip)
        return;
    if (__builtin_bswap32(dst) != own_ip)
        return;

    UART0_PUTC_RAW('t');
    sport = rd16le(frm + RXF_UDP_SPORT);                /* kept in wire order */
    udp_len = get_be16(frm + RXF_UDP_LEN);
    if (get_be16(frm + RXF_UDP_DPORT) != ETH_TFTP_PORT_LOCAL)
        return;
    if (get_be16(frm + RXF_TFTP_OP) != TFTP_OP_DATA)
        return;
    blk = get_be16(frm + RXF_TFTP_BLOCK);
    data_len = udp_len - UDP_HDR_LEN - TFTP_HDR_LEN;
    put_hex32(blk);

    /* 0x38a0..0x3960: the ACK, built on the TFTP template.  The RRQ is
       copied into the payload too and then overwritten by the 4-byte
       ACK (the next 2 bytes of the 4-byte block store are zero). */
    memset32(TXHDR, 0, TX_CLEAR_HDR);
    memcpy8(TXHDR, net_tmpl.tftp_frame, ETH_TXHDR_LEN);
    memcpy8(TXPAY, net_tmpl.tftp_rrq, TFTP_RRQ_LEN);
    memcpy8(TXHDR + TXH_DST_MAC, g_eth.server_mac, 6);
    memcpy8(TXHDR + TXH_SRC_MAC, g_eth.own_mac, 6);
    wr32le(TXHDR + TXH_IP_SRC, __builtin_bswap32(g_eth.own_ip));
    wr32le(TXHDR + TXH_IP_DST, __builtin_bswap32(g_eth.server_ip));
    wr16le(TXHDR + TXH_IP_TOTLEN, TFTP_ACK_IP_TOTLEN_LE);
    wr16le(TXHDR + TXH_UDP_DPORT, sport);
    wr16le(TXHDR + TXH_UDP_LEN, TFTP_ACK_UDP_LEN_LE);
    wr32le(TXPAY + TXP_TFTP_OP, TFTP_ACK_OP_LE);
    wr32le(TXPAY + TXP_TFTP_BLOCK, swap16x2(blk));
    eth_xmit(ETH_TXHDR_LEN, TXPAY, TFTP_ACK_LEN);

    /* 0x3964..0x398c */
    if (blk <= TFTP_MAX_BLOCKS)
        memcpy8((void *)SRAM_TFTP_BLOCK(blk), frm + RXF_TFTP_DATA, data_len);
    if (data_len < TFTP_BLOCK_SIZE)
        g_eth.state = ETH_ST_DONE;
}

/*
 * 0x36cc..0x37fc: every state except 2, 3 and 4.  Nothing happens until
 * an offer has set own_ip; then 'C' per frame and ARP / IPv4 dispatch.
 */
static void rx_other(const u8 *frm)
{
    u32 own_ip = g_eth.own_ip;
    u32 type;

    if (own_ip == 0)
        return;
    UART0_PUTC_RAW('C');
    type = get_be16(frm + RXF_ETHERTYPE);
    if (type == ETHERTYPE_ARP)
        rx_arp(frm, own_ip);
    else if (type == ETHERTYPE_IPV4)
        rx_ipv4(frm, own_ip);
}

/*
 * 0x3400..0x366c: handle every RX descriptor the DMA has given back, in
 * ring order, then return to the main loop.  Each one is recycled (des0
 * buffer, des2 = 0, des3 = OWN|IOC|BUF1V; des1 is left as written back)
 * and the tail re-armed, whatever the frame was.
 */
static void rx_drain(void)
{
    volatile struct eqos_desc *rxd = (volatile struct eqos_desc *)SRAM_ETH_RXDESC;
    const u8 *frm;
    u32 idx;

    for (;;) {
        idx = g_eth.rx_idx;
        if (rxd[idx].des3 & EQOS_RDES3_OWN)
            return;
        frm = (const u8 *)SRAM_ETH_RXBUF(idx);

        switch (g_eth.state) {
        case ETH_ST_WAIT_OFFER:
            rx_offer(frm);
            break;
        case ETH_ST_OFFERED:
            break;
        case ETH_ST_WAIT_ACK:
            rx_ack(frm);
            break;
        default:
            rx_other(frm);
            break;
        }

        /* 0x3628..0x3668 */
        idx = g_eth.rx_idx;
        rxd[idx].des0 = SRAM_ETH_RXBUF(idx);
        rxd[idx].des2 = 0;
        rxd[idx].des3 = EQOS_RDES3_RECYCLE;
        REG32(ETH_DMA_BASE + DMA_CH0_RXDESC_TAIL) = SRAM_ETH_RXDESC_TAIL;
        g_eth.rx_idx = (idx + 1) & (ETH_RING_LEN - 1);
    }
}

/*
 * Common head of "S1" and "S2" (0x39dc..0x3a54, 0x3b08..0x3b78): 'S',
 * clear the TX buffers, the digit, the DHCP templates, our MAC, a new
 * xid, chaddr.
 *
 * memset32 and memcpy32 round their length down to words: 0x156 clears
 * 0x154 bytes and the 0x2a-byte frame template copies only 0x28, so the
 * UDP checksum bytes stay zero from the clear.  chaddr is filled with 8
 * bytes, the last two being own_mac's zero padding.
 */
static void dhcp_build(char digit)
{
    UART0_PUTC_RAW('S');
    memset32(TXHDR, 0, TX_CLEAR_DHCP);
    UART0_PUTC_RAW(digit);
    memcpy32(TXHDR, net_tmpl.dhcp_frame, ETH_TXHDR_LEN);
    memcpy32(TXPAY, net_tmpl.bootp_discover, ETH_TXPAY_SIZE);
    memcpy8(TXHDR + TXH_SRC_MAC, g_eth.own_mac, 6);
    g_eth.xid++;
    SRAM_VAR(u32, SRAM_ETH_TXPAY + TXP_BOOTP_XID) = __builtin_bswap32(g_eth.xid);
    memcpy8(TXPAY + TXP_BOOTP_CHADDR, g_eth.own_mac, 8);
}

/*
 * One step of the send / timer side, 0x3994..0x3ab4 and 0x3dbc..0x3e7c,
 * the switch through the jump table at 0x39a4.  States 4, 8, 9 (and any
 * value > 10) do nothing.
 *
 * no-jump-tables: the original's table is inline in .text; gcc 14 would
 * put a byte-offset table into net.o's .rodata instead, ahead of the
 * frame templates that link.ld expects at 0x7a50.
 */
static void __attribute__((optimize("no-jump-tables"))) net_step(void)
{
    u8 *opt = TXPAY + TXP_BOOTP_OPTIONS;

    switch (g_eth.state) {
    case ETH_ST_INIT:                           /* 0x39d0 */
        g_eth.state = ETH_ST_DISCOVER;
        break;

    case ETH_ST_DISCOVER:                       /* 0x39dc: "S1" DISCOVER */
        dhcp_build('1');
        eth_xmit(ETH_TXHDR_LEN, TXPAY, ETH_TXPAY_SIZE);
        timer_start();
        g_eth.state = ETH_ST_WAIT_OFFER;
        break;

    case ETH_ST_WAIT_OFFER:                     /* 0x3a74 */
        /* 5 s without an offer: back to DISCOVER.  The retry limit,
           (OTP t + 1) * 6, is tested on every pass, not only after a
           timeout; reaching it is "K:". */
        if (timer_now() > ETH_OFFER_TMO) {
            g_eth.state = ETH_ST_DISCOVER;
            timer_stop();
            g_eth.retries++;
        }
        if (g_eth.retries >= g_timeout)
            boot_fail(BOOT_FAIL_TIMEOUT);
        break;

    case ETH_ST_OFFERED:                        /* 0x3afc: "S2" REQUEST */
        /* options 53 = REQUEST, 50 = yiaddr, 54 = siaddr, end.  Then 'a'
           before the send.  (The hardware capture shows "bS2cAk": this
           'a' is missing there, most likely dropped by the UART -- the
           offer path writes ~50 bytes to THR without any FIFO wait.) */
        timer_stop();
        dhcp_build('2');
        wr8(opt + 0, DHO_MSG_TYPE);
        wr8(opt + 1, 1);
        wr8(opt + 2, DHCPREQUEST);
        wr8(opt + 3, DHO_REQUESTED_IP);
        wr8(opt + 4, 4);
        wr32le(opt + 5, __builtin_bswap32(g_eth.own_ip));
        wr8(opt + 9, DHO_SERVER_ID);
        wr8(opt + 10, 4);
        wr32le(opt + 11, __builtin_bswap32(g_eth.server_ip));
        wr8(opt + 15, DHO_END);
        UART0_PUTC_RAW('a');
        eth_xmit(ETH_TXHDR_LEN, TXPAY, ETH_TXPAY_SIZE);
        g_eth.state = ETH_ST_WAIT_ACK;
        break;

    case ETH_ST_ARP:                            /* 0x3c1c: "S3" ARP request */
        UART0_PUTC_RAW('S');
        UART0_PUTC_RAW('3');
        memset32(TXHDR, 0, TX_CLEAR_HDR);
        memcpy8(TXHDR, net_tmpl.arp_request, ETH_TXHDR_LEN);
        memcpy8(TXHDR + TXH_SRC_MAC, g_eth.own_mac, 6);
        memcpy8(TXHDR + TXH_ARP_SHA, g_eth.own_mac, 6);
        wr32le(TXHDR + TXH_ARP_SPA, __builtin_bswap32(g_eth.own_ip));
        wr32le(TXHDR + TXH_ARP_TPA, __builtin_bswap32(g_eth.server_ip));
        eth_xmit(ETH_TXHDR_LEN, NULL, 0);
        g_eth.state = ETH_ST_ARP_SENT;
        break;

    case ETH_ST_ARP_SENT:                       /* 0x3ca8: reply not awaited */
        g_eth.state = ETH_ST_RRQ;
        break;

    case ETH_ST_RRQ:                            /* 0x3dbc: "S4" TFTP RRQ */
        /* to siaddr:69 from port 2962, at the MAC the offer came from;
           sent once, never repeated */
        UART0_PUTC_RAW('S');
        memset32(TXHDR, 0, TX_CLEAR_HDR);
        UART0_PUTC_RAW('4');
        memcpy8(TXHDR, net_tmpl.tftp_frame, ETH_TXHDR_LEN);
        memcpy8(TXPAY, net_tmpl.tftp_rrq, TFTP_RRQ_LEN);
        memcpy8(TXHDR + TXH_DST_MAC, g_eth.server_mac, 6);
        memcpy8(TXHDR + TXH_SRC_MAC, g_eth.own_mac, 6);
        wr32le(TXHDR + TXH_IP_SRC, __builtin_bswap32(g_eth.own_ip));
        wr32le(TXHDR + TXH_IP_DST, __builtin_bswap32(g_eth.server_ip));
        UART0_PUTC_RAW('a');
        eth_xmit(ETH_TXHDR_LEN, TXPAY, TFTP_RRQ_LEN);
        g_eth.state = ETH_ST_WAIT_DATA;
        break;

    case ETH_ST_DONE:                           /* 0x3e60: "Dn", leave */
        g_eth.running = 0;
        UART0_PUTC_RAW('D');
        UART0_PUTC_RAW('n');
        break;

    default:                                    /* 4, 8, 9: wait */
        break;
    }
}

/*
 * 0x3384..0x3af8: run DHCP / ARP / TFTP until "Dn", then hand the image
 * to the common loader check.  Does not return: success jumps to the
 * loader, failure ends in boot_fail() (fatal = 1).
 */
void net_run(void)
{
    u32 t, status;

    /* 0x3384..0x33c8: first xid is the TRNG word (+1 at the first send);
       retry limit (OTP[20:18] + 1) * 6 DISCOVERs of 5 s each */
    g_eth.running = 1;
    g_eth.state = ETH_ST_INIT;
    g_eth.own_ip = 0;
    g_eth.xid = g_eth.rnd;
    g_eth.retries = 0;
    t = (otp_boot_cfg() & OTP_CFG_TMO_MASK) >> OTP_CFG_TMO_SHIFT;
    g_otp_tmo = t;
    g_timeout = (t + 1) * DHCP_RETRIES_PER_T;

    /* 0x33cc..0x3ac0: acknowledge every pending DMA status bit at once;
       "RI\r\n" once per poll that saw RI, however many frames follow */
    do {
        status = REG32(ETH_DMA_BASE + DMA_CH0_STATUS);
        REG32(ETH_DMA_BASE + DMA_CH0_STATUS) = status;
        if (status & DMA_CH0_STATUS_RI) {
            UART0_PUTC_RAW('R');
            UART0_PUTC_RAW('I');
            put_crlf();
            rx_drain();
        }
        net_step();
    } while (g_eth.running);

    /* 0x3ac4..0x3af0: stage the first 0x28 header bytes, set 0xF0FF0020
       bit0 back, check and run (checksum computed by the check itself) */
    memcpy32((void *)SRAM_HDR_STAGE, (const void *)SRAM_IMAGE, HDR_STAGE_COPY);
    REG32(MISC_FF0_BASE + ETH_FF0_CTRL) |= ETH_FF0_CTRL_BIT0;
    check_and_run_loader(1, 0, 0);
}
