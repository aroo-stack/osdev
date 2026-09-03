#ifndef PMM_H
#define PMM_H
#include <stdint.h>
#include "multiboot.h"

void pmm_init(uint32_t mbi_addr, uint32_t mmap_addr, uint32_t mmap_length);
uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t frame_addr);
void pmm_print_stats(void);
uint32_t pmm_total_frames(void);
uint32_t pmm_free_frames(void);

#endif
