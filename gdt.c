#include "gdt.h"

extern void gdt_flush(uint32_t);

struct gdt_entry gdt[3];
struct gdt_ptr gp;

static void gdt_set(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

void gdt_install(void) {
    // null descriptor
    gdt_set(0, 0, 0, 0, 0);
    // code: base 0, limit 4GB, access 0x9A, gran 0xCF -> per trace in AGENTS.md
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    // data: base 0, limit 4GB, access 0x92, gran 0xCF
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    gp.limit = (sizeof(gdt) - 1);
    gp.base  = (uint32_t)&gdt;

    gdt_flush((uint32_t)&gp);
}
