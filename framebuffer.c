#include "framebuffer.h"
#include "paging.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t *fb_front = 0; // real visible framebuffer (physical identity)
static uint32_t *fb_back = 0;  // off-screen back buffer
static uint32_t *fb_target = 0; // cached draw target for per-pixel fast path (no branch per pixel)
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint8_t fb_bpp = 0;
static uint8_t fb_type = 0;
static int fb_available = 0;
static int double_buffered = 0;
static uint32_t fb_size_bytes = 0;

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex32(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_dec(uint32_t n){ char b[11]; int i=0; if(n==0){s_putc('0');return;} while(n){b[i++]='0'+n%10; n/=10;} while(i--) s_putc(b[i]); }

int fb_is_available(void){ return fb_available; }
int fb_is_double_buffered(void){ return double_buffered; }
uint32_t fb_get_width(void){ return fb_width; }
uint32_t fb_get_height(void){ return fb_height; }

// Internal helper: cached draw target - set once when double buffering enabled/disabled
// This avoids per-pixel function call + branch (was fb_draw_target() per pixel in put_pixel loops)
static inline uint32_t* fb_draw_target(void){
    return fb_target ? fb_target : fb_front;
}

int fb_init(struct multiboot_info *mbi){
    if(!mbi){ s_puts("FB: no mbi\n"); return 0; }
    if(!(mbi->flags & (1<<12))){
        s_puts("FB: flag bit12 not set -> GRUB did not provide framebuffer, fallback to VGA text\n");
        return 0;
    }
    uint64_t addr64 = ((uint64_t)mbi->framebuffer_addr_high<<32) | mbi->framebuffer_addr_low;
    uint32_t addr = (uint32_t)addr64; // 32-bit kernel
    uint32_t pitch = mbi->framebuffer_pitch;
    uint32_t width = mbi->framebuffer_width;
    uint32_t height = mbi->framebuffer_height;
    uint8_t bpp = mbi->framebuffer_bpp;
    uint8_t type = mbi->framebuffer_type;

    s_puts("FB: GRUB provided addr="); s_put_hex32(addr);
    s_puts(" pitch="); s_put_dec(pitch);
    s_puts(" width="); s_put_dec(width);
    s_puts(" height="); s_put_dec(height);
    s_puts(" bpp="); s_put_dec(bpp);
    s_puts(" type="); s_put_dec(type); s_puts("\n");

    if(type != 1){
        s_puts("FB: type !=1 (not RGB), fallback\n");
        return 0;
    }
    if(bpp != 32){
        s_puts("FB: bpp !=32, got "); s_put_dec(bpp); s_puts(" - trying anyway, may be fallback\n");
    }
    if(width==0 || height==0){
        s_puts("FB: zero dimensions, fallback\n");
        return 0;
    }

    // Map framebuffer physical pages into virtual same (identity)
    uint32_t fb_size = pitch * height;
    uint32_t pages = (fb_size + 0xFFF) >> 12;
    s_puts("FB: mapping "); s_put_dec(pages); s_puts(" pages ("); s_put_dec(fb_size/1024); s_puts(" KB) at "); s_put_hex32(addr); s_puts("\n");
    for(uint32_t i=0;i<pages;i++){
        uint32_t v = addr + i*0x1000;
        uint32_t p = v; // identity
        paging_map(v, p, 0x03);
        volatile uint32_t *probe = (volatile uint32_t*)v;
        (void)*probe;
    }
    fb_front = (uint32_t*)addr;
    fb_pitch = pitch;
    fb_width = width;
    fb_height = height;
    fb_bpp = bpp;
    fb_type = type;
    fb_available = 1;
    fb_size_bytes = fb_size;
    fb_target = fb_front; // draw to front until double buffering enabled

    s_puts("FB: mapped and accessible\n");

    // --- Double buffering: allocate back buffer ---
    // Why via PMM not heap: heap is only 1MB (Phase 6) but back buffer needs ~3MB (1024*768*4=3,145,728)
    // PMM has ~127MB free (32616 frames), so we have space via PMM, not heap.
    // We check heap size vs needed and report.
    uint32_t need = fb_size;
    s_puts("FB: double buffer check heap 1MB vs need "); s_put_dec(need/1024); s_puts(" KB -> ");
    if(need > 1024*1024){
        s_puts("heap too small (1MB), using PMM dedicated region\n");
    } else {
        s_puts("heap would be enough, but still using PMM for dedicated\n");
    }
    s_puts("FB: PMM free before back buffer: "); s_put_dec(pmm_free_frames()); s_puts(" frames\n");

    // Choose virtual address for back buffer beyond heap and framebuffer
    // Heap is 0x00400000-0x00500000 (1MB), framebuffer is 0xFD000000, so 0x00A00000 (10MB) is free virtual
    // This virtual range is currently unmapped (PD 2), so paging_map will allocate tables as needed
    uint32_t back_vaddr = 0x00A00000;
    uint32_t back_pages = (need + 0xFFF) >> 12;
    s_puts("FB: allocating back buffer "); s_put_dec(back_pages); s_puts(" pages at virtual "); s_put_hex32(back_vaddr); s_puts("\n");
    for(uint32_t i=0;i<back_pages;i++){
        uint32_t p = pmm_alloc_frame();
        if(!p){
            s_puts("FB: out of frames for back buffer!\n");
            fb_back = 0;
            double_buffered = 0;
            return 1; // still have front buffer, just no double buffering
        }
        uint32_t v = back_vaddr + i*0x1000;
        paging_map(v, p, 0x03);
        // zero the page via virtual
        volatile uint32_t *ptr = (volatile uint32_t*)v;
        for(int j=0;j<1024;j++) ptr[j]=0;
    }
    fb_back = (uint32_t*)back_vaddr;
    double_buffered = 1;
    fb_target = fb_back; // cache: all future draws go to back, no per-pixel branch
    s_puts("FB: double buffer allocated at "); s_put_hex32(back_vaddr); s_puts(" PMM free after: "); s_put_dec(pmm_free_frames()); s_puts("\n");
    s_puts("FB: double buffering enabled (draw to back, swap to front)\n");
    return 1;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color){
    if(!fb_available) return;
    if(x >= fb_width || y >= fb_height) return;
    uint32_t *target = fb_target ? fb_target : fb_front;
    uint8_t *row = (uint8_t*)target + y * fb_pitch;
    uint32_t *pixel = (uint32_t*)(row + x*4);
    *pixel = color;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y){
    if(!fb_available) return 0;
    if(x >= fb_width || y >= fb_height) return 0;
    uint32_t *target = fb_target ? fb_target : fb_front;
    uint8_t *row = (uint8_t*)target + y * fb_pitch;
    uint32_t *pixel = (uint32_t*)(row + x*4);
    return *pixel;
}

void fb_fill(uint32_t color){
    if(!fb_available) return;
    uint32_t *target = fb_target ? fb_target : fb_front;
    for(uint32_t y=0;y<fb_height;y++){
        uint8_t *row = (uint8_t*)target + y*fb_pitch;
        uint32_t *pixels = (uint32_t*)row;
        for(uint32_t x=0;x<fb_width;x++) pixels[x]=color;
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color){
    if(!fb_available) return;
    uint32_t *target = fb_target ? fb_target : fb_front;
    // Clip to screen
    if(x >= fb_width || y >= fb_height) return;
    if(x + w > fb_width) w = fb_width - x;
    if(y + h > fb_height) h = fb_height - y;
    for(uint32_t dy=0; dy<h; dy++){
        uint8_t *row = (uint8_t*)target + (y+dy) * fb_pitch;
        uint32_t *pixels = (uint32_t*)(row + x*4);
        for(uint32_t dx=0; dx<w; dx++) pixels[dx]=color;
    }
}

static uint32_t fb_swap_count = 0;

// Wait for start of vblank: NOT in retrace -> retrace starts
// 0x3DA bit 3 = vertical retrace (1 = in retrace) - standard VGA status
// With timeout to avoid hang if QEMU VBE doesn't emulate retrace toggling
static inline void wait_for_vblank(void){
    // Wait for NOT in retrace (if already in retrace, wait out)
    for(int i=0;i<100000;i++){
        if((inb(0x3DA) & 0x08)==0) break;
    }
    // Wait for retrace to START
    for(int i=0;i<100000;i++){
        if(inb(0x3DA) & 0x08) break;
    }
}

uint32_t* fb_get_back_buffer(void){ return fb_back; }
uint32_t* fb_get_front_buffer(void){ return fb_front; }
uint32_t fb_get_front_pixel(uint32_t x, uint32_t y){
    if(!fb_available || !fb_front) return 0;
    if(x >= fb_width || y >= fb_height) return 0;
    uint8_t *row = (uint8_t*)fb_front + y * fb_pitch;
    uint32_t *pixel = (uint32_t*)(row + x*4);
    return *pixel;
}
uint32_t fb_get_size_bytes(void){ return fb_size_bytes; }
void fb_blit_from(uint32_t *src){
    if(!fb_available || !double_buffered || !fb_back || !src) return;
    uint32_t *dst = fb_back;
    uint32_t dwords = fb_size_bytes / 4;
    __asm__ volatile("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(dwords) : : "memory");
}

// Known limitation: no hardware vsync/page-flip support, so occasional screen tearing (a thin visible line)
// can appear at the display's refresh boundary during redraws. Attempted 0x3DA vblank polling did not reliably
// eliminate this under QEMU's VBE emulation. Would require hardware page-flipping or a different framebuffer
// architecture to fully fix - out of scope for now.
void fb_swap(void){
    if(!fb_available || !double_buffered || !fb_back || !fb_front) return;
    fb_swap_count++;
    // Diagnostic for leak check: uncomment to log every swap
    // if(fb_swap_count < 20 || (fb_swap_count % 10)==0){
    //     s_puts("FB_SWAP #"); s_put_dec(fb_swap_count);
    //     s_puts(" PMM free "); s_put_dec(pmm_free_frames());
    //     s_puts(" frames\n");
    // }
    // Fast copy back -> front. This is the ONLY write to visible front buffer - eliminates flicker
    // because the display never sees intermediate fb_fill / partial windows.
    // Do NOT unconditionally cli/sti here - caller already holds cli for window/mouse critical sections.
    // Doing cli here when caller already did cli would cause nested cli/sti mismatch:
    // inner sti would re-enable interrupts prematurely while outer still expects disabled, causing re-entrancy
    // and stack overflow after many drag moves (~3 sec). So just do the copy with current interrupt state.
    // --- vsync: wait for start of vblank before copy to reduce tearing ---
    // Standard pattern: wait NOT in retrace, then wait for retrace START, then copy during blank.
    // Timeout prevents hang if QEMU std/VBE doesn't toggle 0x3DA bit 3.
    wait_for_vblank();
    uint32_t *src = fb_back;
    uint32_t *dst = fb_front;
    uint32_t dwords = fb_size_bytes / 4;
    __asm__ volatile(
        "cld; rep movsl"
        : "+S"(src), "+D"(dst), "+c"(dwords)
        :
        : "memory"
    );
}
