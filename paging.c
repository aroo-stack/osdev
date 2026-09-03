#include "paging.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

// Page structures must be 4096-aligned - Intel requires low 12 bits zero
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_table_0[1024] __attribute__((aligned(4096)));
static uint32_t page_table_1[1024] __attribute__((aligned(4096))); // for heap 4MB..8MB
static uint32_t page_table_high[1024] __attribute__((aligned(4096))); // reserved high half if needed

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex32(uint32_t n){
    s_puts("0x");
    for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); }
}

void paging_map(uint32_t vaddr, uint32_t paddr, uint32_t flags){
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    uint32_t pd = page_directory[pd_idx];
    uint32_t *table;
    if(!(pd & 0x1)){
        // need new table - try preallocated heap tables first
        if(pd_idx==1) table = page_table_1;
        else if(pd_idx==0) table = page_table_0;
        else if(pd_idx==768) table = page_table_high;
        else {
            // dynamic: allocate frame from pmm (must be <4MB to be identity mapped for table access)
            uint32_t tframe = pmm_alloc_frame();
            if(!tframe) return;
            // use identity-mapped address to zero it (tframe <4MB guaranteed for first few allocs, but ensure)
            table = (uint32_t*)tframe;
            // if tframe >=0x400000, its virtual not mapped; fallback to preallocated fb table
            // for framebuffer at 0xE0000000 etc., pmm may return high frame not mapped - handle via temp map using page_table_0 region?
            // Simplified: assume low alloc, zero via identity
            for(int i=0;i<1024;i++) table[i]=0;
            page_directory[pd_idx] = (tframe & 0xFFFFF000) | 0x03;
            __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
            // now set entry
            table[pt_idx] = (paddr & 0xFFFFF000) | (flags & 0xFFF) | 0x01;
            __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
            return;
        }
        // for preallocated, ensure zeroed already and install if needed
        if(!(page_directory[pd_idx] & 0x1)){
            uint32_t taddr = (uint32_t)table & 0xFFFFF000;
            page_directory[pd_idx] = taddr | 0x03;
        }
        // re-read table pointer after install
        pd = page_directory[pd_idx];
    }
    table = (uint32_t*)(pd & 0xFFFFF000);
    // table is physical low address, virtual identity mapped ( <4MB ), so direct access works
    table[pt_idx] = (paddr & 0xFFFFF000) | (flags & 0xFFF) | 0x01; // ensure P
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

void paging_unmap(uint32_t vaddr){
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    uint32_t pd = page_directory[pd_idx];
    if(!(pd & 0x1)) return;
    uint32_t *table = (uint32_t*)(pd & 0xFFFFF000);
    table[pt_idx]=0;
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

void paging_init(void){
    // Zero directory and tables
    for(int i=0;i<1024;i++) page_directory[i]=0;
    for(int i=0;i<1024;i++) page_table_0[i]=0;
    for(int i=0;i<1024;i++) page_table_1[i]=0;
    for(int i=0;i<1024;i++) page_table_high[i]=0;

    // Identity map first 4MB: each PTE maps 4KB page VA==PA
    // PTE flags: P=1 RW=1 US=0 => 0x03 (present + writable, supervisor)
    for(int i=0;i<1024;i++){
        uint32_t frame = i * 0x1000;
        page_table_0[i] = frame | 0x03; // 0b11
    }

    // PDE 0 points to page_table_0
    // PDE flags: P=1 RW=1 US=0 PWT=0 PCD=0 A=0 PS=0 (4KB) => also 0x03
    page_directory[0] = ((uint32_t)page_table_0 & 0xFFFFF000) | 0x03;

    // Ensure CR4.PSE = 0 (disable 4MB pages) for 4KB mode; BIOS leaves 0
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~ (1u<<4); // clear PSE
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    // Load CR3 with physical address of directory (identity mapped, VA==PA)
    __asm__ volatile("mov %0, %%cr3" :: "r"(page_directory) : "memory");

    // Enable paging: set CR0.PG bit 31
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    // Flush pipeline / prefetch - far jump not strictly needed for 4KB, but do short jmp
    __asm__ volatile("jmp 1f \n 1:");

    s_puts("PAGING: PD="); s_put_hex32((uint32_t)page_directory);
    s_puts(" PT0="); s_put_hex32((uint32_t)page_table_0);
    s_puts(" CR3="); s_put_hex32((uint32_t)page_directory);
    s_puts(" enabled\n");
}
