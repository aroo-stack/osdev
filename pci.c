// PCI bus enumeration + RTL8139 detection (detection ONLY: no init, no packets).
// Access mechanism #1 per OSDev wiki "PCI" (verified via search excerpt):
//   address = 0x80000000 | bus<<16 | dev<<11 | fn<<8 | (offset & 0xFC)
//   outl 0xCF8, address; then inl 0xCFC, shift ((offset&3)*8), mask width.
// Bit 31 = enable; bits 1:0 of offset must be 0 (dword-aligned access).
// Header field offsets (standard type-0, cross-checked with U-Boot dump):
//   0x00 vendor[15:0] + device[31:16]; 0x08 rev/prog-if/subclass(23:16)/class(31:24);
//   0x0E header type byte 14 (bit7 = multifunction, unused here - brute force);
//   0x10 BAR0 dword; 0x3C interrupt line byte 0.
// RTL8139 IDs per OSDev wiki "RTL8139" + Linux 8139too table: 0x10EC:0x8139.
// BAR0 decode: bit0=1 -> I/O space, base = bar & 0xFFFFFFFC (QEMU gives 0x..01);
// bit0=0 -> memory, base = bar & 0xFFFFFFF0. SeaBIOS programs these before us.
#include <stdint.h>
#include "pci.h"

#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC
#define PCI_VENDOR_NONE 0xFFFF
#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

static inline void pci_outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline void pci_outl(uint16_t port, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t pci_inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline uint32_t pci_inl(uint16_t port){ uint32_t r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(port)); return r; }
static void s_putc(char c){ while(!(pci_inb(0x3F8+5)&0x20)); pci_outb(0x3F8,(uint8_t)c); }
static void s_puts(const char*s){ for(int i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex16(uint16_t n){ s_puts("0x"); for(int i=12;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_hex32(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_dec(uint32_t n){ char b[11]; int i=0; if(n==0){s_putc('0');return;} while(n){b[i++]='0'+n%10; n/=10;} while(i--) s_putc(b[i]); }

static uint32_t pci_config_read_aligned(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset){
    // Trace: bus=0 dev=3 fn=0 off=0x00 -> 0x80000000|0x000000|0x00001800|0|0 = 0x80001800
    uint32_t address = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)(dev & 0x1F) << 11)
        | ((uint32_t)(fn & 0x07) << 8)
        | (offset & 0xFC);
    pci_outl(PCI_ADDR_PORT, address);
    // Small delay: some hardware latches slowly (same io_wait pattern as RTC).
    pci_outb(0x80, 0);
    return pci_inl(PCI_DATA_PORT);
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset){
    uint32_t v = pci_config_read_aligned(bus, dev, fn, offset);
    return v >> ((offset & 3) * 8); // offset&3==0 for dword reads; shift is no-op
}

uint16_t pci_config_read_word(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset){
    uint32_t v = pci_config_read_aligned(bus, dev, fn, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF); // OSDev form
}

void pci_config_write_word(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t val){
    // Mechanism #1 write: select via 0xCF8, full dword via 0xCFC. RMW keeps
    // the untouched half (e.g. status word when writing command at 0x04).
    uint32_t address = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)(dev & 0x1F) << 11)
        | ((uint32_t)(fn & 0x07) << 8)
        | (offset & 0xFC);
    pci_outl(PCI_ADDR_PORT, address);
    pci_outb(0x80, 0);
    uint32_t cur = pci_inl(PCI_DATA_PORT);
    uint32_t shift = (offset & 2) * 8;
    uint32_t patched = (cur & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_outl(PCI_ADDR_PORT, address); // reselect (reads reset selection to 0xD)
    pci_outb(0x80, 0);
    pci_outl(PCI_DATA_PORT, patched);
}

// RTL8139 findings, filled by the scan.
static int rtl_found = 0;
static int rtl_bus = 0, rtl_dev = 0, rtl_fn = 0;
static uint32_t rtl_bar0 = 0;
static uint8_t rtl_irq = 0;

int pci_rtl8139_present(void){ return rtl_found; }
int pci_rtl8139_bus(void){ return rtl_bus; }
int pci_rtl8139_dev(void){ return rtl_dev; }
int pci_rtl8139_fn(void){ return rtl_fn; }
uint32_t pci_rtl8139_bar0_raw(void){ return rtl_bar0; }
int pci_rtl8139_bar0_is_io(void){ return (rtl_bar0 & 1) != 0; }
uint32_t pci_rtl8139_iobase(void){
    if(!rtl_found || !(rtl_bar0 & 1)) return 0;
    return rtl_bar0 & 0xFFFFFFFCu;
}
uint8_t pci_rtl8139_irq(void){ return rtl_irq; }

void pci_init(void){
    int count = 0;
    s_puts("PCI: brute-force scan bus 0-255 dev 0-31 fn 0-7 (vendor != 0xFFFF)...\n");
    for(int bus=0; bus<256; bus++){
        for(int dev=0; dev<32; dev++){
            for(int fn=0; fn<8; fn++){
                uint16_t vendor = pci_config_read_word((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                if(vendor == PCI_VENDOR_NONE) continue; // no device here
                uint16_t device = pci_config_read_word((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x02);
                uint32_t cls = pci_config_read_dword((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                uint8_t class = (uint8_t)(cls >> 24), subclass = (uint8_t)(cls >> 16);
                s_puts("PCI: bus "); s_put_dec((uint32_t)bus);
                s_puts(" dev "); s_put_dec((uint32_t)dev);
                s_puts(" fn "); s_put_dec((uint32_t)fn);
                s_puts(" vendor "); s_put_hex16(vendor);
                s_puts(" device "); s_put_hex16(device);
                s_puts(" class "); s_put_hex16(class);
                s_puts(" subclass "); s_put_hex16(subclass);
                s_puts("\n");
                count++;
                if(vendor == RTL8139_VENDOR && device == RTL8139_DEVICE && !rtl_found){
                    rtl_found = 1;
                    rtl_bus = bus; rtl_dev = dev; rtl_fn = fn;
                    rtl_bar0 = pci_config_read_dword((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x10);
                    // IRQ line is byte 0 of offset 0x3C: read dword, take low byte.
                    rtl_irq = (uint8_t)(pci_config_read_dword((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x3C) & 0xFF);
                }
            }
        }
    }
    s_puts("PCI: scan done, "); s_put_dec((uint32_t)count); s_puts(" device(s)\n");
    if(rtl_found){
        s_puts("PCI: RTL8139 found at bus "); s_put_dec((uint32_t)rtl_bus);
        s_puts(" dev "); s_put_dec((uint32_t)rtl_dev);
        s_puts(" fn "); s_put_dec((uint32_t)rtl_fn);
        s_puts(" (10EC:8139)\n");
        s_puts("PCI: RTL8139 BAR0 raw "); s_put_hex32(rtl_bar0);
        if(rtl_bar0 & 1){
            s_puts(" -> I/O space, base "); s_put_hex32(rtl_bar0 & 0xFFFFFFFCu); s_puts("\n");
        } else {
            s_puts(" -> memory space, base "); s_put_hex32(rtl_bar0 & 0xFFFFFFF0u); s_puts("\n");
        }
        s_puts("PCI: RTL8139 IRQ line "); s_put_dec(rtl_irq); s_puts("\n");
    } else {
        s_puts("PCI: RTL8139 NOT found (forgot -device rtl8139?)\n");
    }
}
