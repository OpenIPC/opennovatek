/*
 * eth.h -- the agreed interface between eth.c and net.c, the two halves
 * of the single original function func_053 eth_boot (0x2cb4..0x3e7c).
 * gcc inlined both halves into eth_boot in the original; they are split
 * here only so two people can own them.
 *
 *   eth.c   eth_boot() { eth_hw_init(); net_run(); }
 *           static eth_hw_init(): 0x2cb4..0x3380 -- 'H', E/O/I, pinmux,
 *           clocks/resets, glue or external-PHY reset, DMA SWR (1 s ->
 *           boot_fail(8)), PHY scan, AN0/AN1, speed letters, MAC address
 *           from TRNG, MAC/MTL setup, 4 TX + 4 RX descriptors with
 *           buffers SRAM_ETH_RXBUF(i), 'X', udelay(DLY_500MS),
 *           0xF0FF0020 &= ~1, RX DMA start (DMA_CH0_RX_CTRL |= SR).
 *           On return g_eth.speed/duplex/phy_addr/rnd/own_mac are set and
 *           the RX ring is live; g_eth.rx_idx = 0.
 *
 *   net.c   net_run(): 0x3384..0x3e7c -- g_eth.running = 1, state =
 *           ETH_ST_INIT, xid = rnd, own_ip = retries = 0, g_otp_tmo and
 *           g_timeout = (t+1)*DHCP_RETRIES_PER_T from otp_boot_cfg(); the
 *           RX/timer state machine (functions.txt table, frame templates
 *           data_24..data_28 are net.c's const data); on "Dn":
 *           memcpy32(SRAM_HDR_STAGE, SRAM_IMAGE, HDR_STAGE_COPY),
 *           0xF0FF0020 |= 1, check_and_run_loader(1, 0, 0).  Never
 *           returns: failure ends in boot_fail().
 *
 * net.c touches the EQOS only through eth_xmit() and the DMA status /
 * RX tail registers (DMA_CH0_STATUS, DMA_CH0_RXDESC_TAIL).
 */
#ifndef BOOTROM_NA51089_ETH_H
#define BOOTROM_NA51089_ETH_H

#ifndef __ASSEMBLER__
void net_run(void);
#endif

#endif
