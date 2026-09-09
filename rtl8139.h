#ifndef RTL8139_H
#define RTL8139_H
#include <stdint.h>

// RTL8139 bring-up, Phase 2 (detection is in pci.c; packets are Phase 3).
// rtl8139_init(): bus master, power on, soft reset, RX buffer, IMR/RCR,
// RE|TE, IRQ unmask. Safe no-op (with log) if the card wasn't found.
void rtl8139_init(void);
// Vector-43 (IRQ11) handler body: EOI is sent by the idt.c dispatcher first
// (established pattern), this only reads/logs/acks ISR.
void rtl8139_irq_handler(void);
// I/O base for Phase 3 (0 if absent or BAR0 isn't I/O type).
uint32_t rtl8139_iobase(void);
// Phase 3: build + transmit one broadcast test frame, poll TOK. Safe no-op
// if init didn't complete. Enables IMR TOK so IRQ43 also fires.
void rtl8139_send_test(void);
// Phase 4 trigger: DHCP discover broadcast (elicits DHCPOFFER from QEMU
// user-net). Uses TX descriptor pair 1 (pair 0 consumed by send_test).
void rtl8139_send_dhcp_discover(void);
// Phase 5: parsed network configuration ("poor man's DHCP": offer parsed +
// stored, no REQUEST/ACK exchange - noted as simplification in code).
// Valid only when net_configured != 0. Octets in network order.
extern uint8_t net_our_ip[4];   // yiaddr from the offer
extern uint8_t net_mask[4];     // option 1 (subnet mask)
extern uint8_t net_gw[4];       // option 3 (first router)
extern uint8_t net_server[4];   // option 54 (DHCP server id)
extern int net_configured;
// Deferred RX drain: call each main-loop iteration. Drains queued packets
// once their DMA has settled (>=1 PIT tick after the IRQ), else no-op.
void rtl8139_poll_rx(void);

#endif
