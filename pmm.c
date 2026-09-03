#include "pmm.h"
#include "multiboot.h"
#include <stdint.h>
#include <stddef.h>

// Provided by linker.ld
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

// Bitmap: 1=used, 0=free. Covers up to 512MB (131072 frames) = 16KB
#define MAX_MEMORY (512*1024*1024)
#define FRAME_SIZE 4096
#define MAX_FRAMES (MAX_MEMORY / FRAME_SIZE) // 131072
#define BITMAP_U32 (MAX_FRAMES / 32) // 4096

static uint32_t bitmap[BITMAP_U32];
static uint32_t total_frames = 0;
static uint32_t free_count = 0;
static uint32_t kernel_start_frame = 0;
static uint32_t kernel_end_frame = 0;

// serial helpers for pmm prints
static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex32(uint32_t n){
    s_puts("0x");
    for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); }
}
static void s_put_hex64(uint64_t n){
    uint32_t hi=n>>32, lo=(uint32_t)n;
    if(hi) { s_put_hex32(hi); s_puts(""); } s_put_hex32(lo);
}
static void s_put_dec(uint32_t n){
    char b[11]; int i=0; if(n==0){s_putc('0');return;} while(n){b[i++]='0'+n%10; n/=10;} while(i--) s_putc(b[i]);
}

static inline void bitmap_set(uint32_t frame){ bitmap[frame/32] |= (1u<<(frame%32)); }
static inline void bitmap_clear(uint32_t frame){ bitmap[frame/32] &= ~(1u<<(frame%32)); }
static inline int bitmap_test(uint32_t frame){ return bitmap[frame/32] & (1u<<(frame%32)); }

void pmm_init(uint32_t mbi_addr, uint32_t mmap_addr, uint32_t mmap_length) {
    // mark all as used initially
    for(uint32_t i=0;i<BITMAP_U32;i++) bitmap[i]=0xFFFFFFFF;
    total_frames = 0; free_count=0;

    kernel_start_frame = ((uint32_t)&_kernel_start) >> 12;
    kernel_end_frame   = (((uint32_t)&_kernel_end) + FRAME_SIZE-1) >> 12; // exclusive

    s_puts("PMM: kernel "); s_put_hex32((uint32_t)&_kernel_start); s_puts(" - "); s_put_hex32((uint32_t)&_kernel_end);
    s_puts(" frames "); s_put_dec(kernel_start_frame); s_puts(".."); s_put_dec(kernel_end_frame); s_puts("\n");
    s_puts("PMM: mmap_addr="); s_put_hex32(mmap_addr); s_puts(" length="); s_put_dec(mmap_length); s_puts("\n");

    if(mmap_addr==0 || mmap_length==0){
        s_puts("PMM: no mmap!\n");
        return;
    }

    uint32_t offset=0;
    uint32_t usable_bytes=0;
    int region=0;
    while(offset < mmap_length){
        struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry*)(mmap_addr + offset);
        uint64_t base = ((uint64_t)e->base_addr_high<<32)|e->base_addr_low;
        uint64_t len  = ((uint64_t)e->length_high<<32)|e->length_low;
        uint32_t type = e->type;
        uint32_t size = e->size;

        s_puts("  REGION "); s_put_dec(region++); s_puts(": base="); s_put_hex64(base);
        s_puts(" len="); s_put_hex64(len); s_puts(" ("); s_put_dec((uint32_t)(len/1024)); s_puts(" KB) type="); s_put_dec(type);
        if(type==MULTIBOOT_MEMMAP_AVAILABLE) s_puts(" AVAILABLE"); else s_puts(" RESERVED");
        s_puts("\n");

        if(type==MULTIBOOT_MEMMAP_AVAILABLE){
            usable_bytes += (uint32_t)len;
            // clamp to our bitmap max
            uint64_t start = base;
            uint64_t end = base + len;
            if(start < MAX_MEMORY){
                if(end > MAX_MEMORY) end = MAX_MEMORY;
                uint32_t start_frame = (uint32_t)(start >> 12);
                uint32_t end_frame = (uint32_t)(end >> 12);
                // align start up if not page aligned
                if(start & 0xFFF) start_frame++;
                for(uint32_t f=start_frame; f<end_frame; f++){
                    // reserve kernel area and first page (0x0) and reserve <1MB if not already? keep as per map
                    if(f>=kernel_start_frame && f<kernel_end_frame) continue;
                    if(bitmap_test(f)) free_count++;
                    bitmap_clear(f);
                }
            }
        }

        if(size==0) break; // prevent infinite
        offset += size + 4;
        // size already includes fields after size; per spec size is 20 for 24-byte entry
    }

    // ensure frame 0 is reserved (null)
    if(!bitmap_test(0)){ bitmap_set(0); if(free_count) free_count--; }

    // reserve multiboot info and mmap buffer (at 0x10000) from being allocated to heap/framebuffer
    // GRUB places mbi at 0x10000 and mmap buffer nearby; both lie in low available RAM and would otherwise be marked free
    if(mbi_addr){
        uint32_t mbi_frame = mbi_addr >> 12;
        if(mbi_frame < MAX_FRAMES && !bitmap_test(mbi_frame)){
            // already free -> mark used
            bitmap_set(mbi_frame);
            if(free_count) free_count--;
            s_puts("PMM: reserved mbi frame "); s_put_dec(mbi_frame); s_puts(" (0x"); s_put_hex32(mbi_addr); s_puts(")\n");
        } else if(mbi_frame < MAX_FRAMES){
            // already used (e.g., kernel overlap) ensure used
            bitmap_set(mbi_frame);
        }
        // also reserve mmap buffer pages
        if(mmap_addr){
            uint32_t mmap_start = mmap_addr >> 12;
            uint32_t mmap_end = (mmap_addr + mmap_length + 0xFFF) >> 12;
            for(uint32_t f=mmap_start; f<mmap_end && f < MAX_FRAMES; f++){
                if(!bitmap_test(f)){
                    bitmap_set(f);
                    if(free_count) free_count--;
                } else {
                    bitmap_set(f);
                }
            }
            s_puts("PMM: reserved mmap buffer frames "); s_put_dec(mmap_start); s_puts(".."); s_put_dec(mmap_end); s_puts("\n");
        }
    }

    total_frames = free_count;

    // reserve first 1MB area if not marked? The mmap will have type 1 for low mem, but we should ensure frames covering 0..256 (1MB) where BIOS reserved? Actually mmap marks 0x0-0x9FC00 available, 0x9FC00-0x100000 reserved, so our loop already respected it. Keep as is.

    s_puts("PMM: total usable "); s_put_dec(usable_bytes/1024); s_puts(" KB ("); s_put_dec(usable_bytes/(1024*1024)); s_puts(" MB)\n");
    s_puts("PMM: free frames "); s_put_dec(free_count); s_puts(" ("); s_put_dec(free_count*4/1024); s_puts(" MB free) in bitmap 512MB max\n");
}

uint32_t pmm_alloc_frame(void){
    for(uint32_t i=0;i<BITMAP_U32;i++){
        if(bitmap[i]==0xFFFFFFFF) continue;
        for(int b=0;b<32;b++){
            if(!(bitmap[i] & (1u<<b))){
                uint32_t frame = i*32 + b;
                bitmap_set(frame);
                free_count--;
                return frame * FRAME_SIZE;
            }
        }
    }
    return 0; // out of memory (also collides with null page)
}

void pmm_free_frame(uint32_t addr){
    if(addr & 0xFFF) return; // not aligned
    uint32_t frame = addr >> 12;
    if(frame >= MAX_FRAMES) return;
    if(frame >= kernel_start_frame && frame < kernel_end_frame) return; // never free kernel
    if(!bitmap_test(frame)){
        // already free? ignore
        return;
    }
    bitmap_clear(frame);
    free_count++;
}

uint32_t pmm_total_frames(void){ return total_frames; }
uint32_t pmm_free_frames(void){ return free_count; }

void pmm_print_stats(void){
    s_puts("PMM stats: total "); s_put_dec(total_frames); s_puts(" free "); s_put_dec(free_count); s_puts("\n");
}
