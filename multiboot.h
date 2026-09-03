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
    // Framebuffer info - Multiboot spec amendment, only valid if flags & (1<<12)
    // Trace: offset 0x58 (88) framebuffer_addr 64-bit (low then high)
    //        0x60 (96) pitch 4B, 0x64 (100) width 4B, 0x68 (104) height 4B
    //        0x6C (108) bpp 1B, 0x6D (109) type 1B, 0x6E (110) reserved + palette
    uint32_t framebuffer_addr_low;  // 0x58
    uint32_t framebuffer_addr_high; // 0x5C
    uint32_t framebuffer_pitch;     // 0x60
    uint32_t framebuffer_width;     // 0x64
    uint32_t framebuffer_height;    // 0x68
    uint8_t  framebuffer_bpp;       // 0x6C
    uint8_t  framebuffer_type;      // 0x6D 0=indexed 1=RGB 2=EGA text
    uint8_t  framebuffer_reserved;  // 0x6E
    uint8_t  framebuffer_palette;   // 0x6F+ varies
} __attribute__((packed));

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_MEMMAP_AVAILABLE 1
#define MULTIBOOT_MEMMAP_RESERVED  2

#endif
