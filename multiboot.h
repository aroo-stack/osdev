#ifndef MULTIBOOT_H
#define MULTIBOOT_H
#include <stdint.h>

// Multiboot 1 spec - trace verified against spec Section 3.3
// All structs packed, little endian, 32-bit.

struct multiboot_mmap_entry {
    uint32_t size;          // size of entry excluding this field (20 = 4+4+4+4+4)
    uint32_t base_addr_low; // base_addr 63:0 low
    uint32_t base_addr_high;
    uint32_t length_low;    // length 63:0 low
    uint32_t length_high;
    uint32_t type;          // 1=available, 2=reserved, etc.
} __attribute__((packed));

struct multiboot_info {
    uint32_t flags;         // 0x00
    uint32_t mem_lower;     // 0x04 KB
    uint32_t mem_upper;     // 0x08 KB
    uint32_t boot_device;   // 0x0C
    uint32_t cmdline;       // 0x10
    uint32_t mods_count;    // 0x14
    uint32_t mods_addr;     // 0x18
    uint32_t syms[4];       // 0x1C - 16 bytes union (a.out/ELF) - pad
    uint32_t mmap_length;   // 0x2C (44 decimal) - total size of mmap buffer
    uint32_t mmap_addr;     // 0x30 (48 decimal) - physical address of mmap
    uint32_t drives_length; // 0x34
    uint32_t drives_addr;   // 0x38
    uint32_t config_table;  // 0x3C
    uint32_t boot_loader_name; // 0x40
    uint32_t apm_table;     // 0x44
    uint32_t vbe_control_info; // 0x48
    uint32_t vbe_mode_info; // 0x4C
    uint16_t vbe_mode;      // 0x50
    uint16_t vbe_interface_seg; // 0x52
    uint16_t vbe_interface_off; // 0x54
    uint16_t vbe_interface_len; // 0x56
} __attribute__((packed));

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_MEMMAP_AVAILABLE 1
#define MULTIBOOT_MEMMAP_RESERVED  2

#endif
