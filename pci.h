#ifndef PCI_H
#define PCI_H
#include <stdint.h>

// PCI configuration space, access mechanism #1 (ports 0xCF8/0xCFC).
// Detection only in this phase: scan, identify RTL8139, report BAR0 + IRQ.
// The driver phase will use rtl8139_iobase()/rtl8139_irq() + bus/dev/fn.
uint16_t pci_config_read_word(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
uint32_t pci_config_read_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);

// Full brute-force scan (bus 0-255, dev 0-31, fn 0-7, vendor != 0xFFFF):
// logs every device, then reports the RTL8139 (10EC:8139) if present.
void pci_init(void);

// RTL8139 findings (valid only if pci_rtl8139_present() != 0).
int pci_rtl8139_present(void);
int pci_rtl8139_bus(void);
int pci_rtl8139_dev(void);
int pci_rtl8139_fn(void);
uint32_t pci_rtl8139_bar0_raw(void); // raw BAR0 dword
uint32_t pci_rtl8139_iobase(void);   // decoded: I/O base if BAR0 is I/O type, else 0
int pci_rtl8139_bar0_is_io(void);    // BAR0 bit 0: 1 = I/O space, 0 = memory
uint8_t pci_rtl8139_irq(void);       // interrupt line register (0x3C)

#endif
