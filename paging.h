#ifndef PAGING_H
#define PAGING_H
#include <stdint.h>
void paging_init(void);
void paging_map(uint32_t vaddr, uint32_t paddr, uint32_t flags);
void paging_unmap(uint32_t vaddr);
void paging_ensure_range(uint32_t vaddr, uint32_t size);
#endif

