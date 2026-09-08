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

#endif
