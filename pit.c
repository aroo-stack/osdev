#include "pit.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}

// PIT ports - verified Intel 8253/8254 and OSDev
// 0x40 Channel 0 data (connected to IRQ0)
// 0x43 Mode/Command register
// Command byte trace for 100Hz:
// Bits 7-6 = 00 Channel 0
// Bits 5-4 = 11 Access mode lobyte/hibyte (11 = 11b)
// Bits 3-1 = 011 Mode 3 square wave (011) - also 010 mode 2 rate generator works, we use 011
// Bit 0 = 0 Binary (not BCD)
// So 00 11 011 0 = 00110110 = 0x36
// Divisor = PIT_FREQ / hz = 1193182 / 100 = 11931 = 0x2E9B (low 0x9B high 0x2E)
// Common alternative 11931 vs 11932, both ~100Hz, we use 0x2E9B
#define PIT_FREQ 1193182

void pit_init(uint32_t hz){
    uint32_t divisor = PIT_FREQ / hz;
    if(divisor > 65535) divisor = 65535;
    if(divisor < 1) divisor = 1;
    uint8_t low = divisor & 0xFF;
    uint8_t high = (divisor >> 8) & 0xFF;
    // Command 0x36
    outb(0x43, 0x36);
    outb(0x40, low);
    outb(0x40, high);
}

void pit_set_phase(int hz){ pit_init(hz); }
