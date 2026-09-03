#include "framebuffer.h"
#include "paging.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t *fb_addr = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint8_t fb_bpp = 0;
static uint8_t fb_type = 0;
static int fb_available = 0;

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex32(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_dec(uint32_t n){ char b[11]; int i=0; if(n==0){s_putc('0');return;} while(n){b[i++]='0'+n%10; n/=10;} while(i--) s_putc(b[i]); }

int fb_is_available(void){ return fb_available; }
uint32_t fb_get_width(void){ return fb_width; }
uint32_t fb_get_height(void){ return fb_height; }

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
        // still try if GRUB gave different mode
    }
    if(width==0 || height==0){
        s_puts("FB: zero dimensions, fallback\n");
        return 0;
    }

    // Map framebuffer physical pages into virtual same (identity)
    uint32_t fb_size = pitch * height;
    // align size to 4K
    uint32_t pages = (fb_size + 0xFFF) >> 12;
    s_puts("FB: mapping "); s_put_dec(pages); s_puts(" pages ("); s_put_dec(fb_size/1024); s_puts(" KB) at "); s_put_hex32(addr); s_puts("\n");
    for(uint32_t i=0;i<pages;i++){
        uint32_t v = addr + i*0x1000;
        uint32_t p = v; // identity
        paging_map(v, p, 0x03);
        // touch to fault early if mapping failed - volatile read
        volatile uint32_t *probe = (volatile uint32_t*)v;
        (void)*probe;
    }
    fb_addr = (uint32_t*)addr;
    fb_pitch = pitch;
    fb_width = width;
    fb_height = height;
    fb_bpp = bpp;
    fb_type = type;
    fb_available = 1;

    s_puts("FB: mapped and accessible\n");
    return 1;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color){
    if(!fb_available) return;
    if(x >= fb_width || y >= fb_height) return;
    // 32bpp: 4 bytes per pixel, pitch is bytes per row
    uint8_t *row = (uint8_t*)fb_addr + y * fb_pitch;
    uint32_t *pixel = (uint32_t*)(row + x*4);
    *pixel = color;
}

void fb_fill(uint32_t color){
    if(!fb_available) return;
    for(uint32_t y=0;y<fb_height;y++){
        uint8_t *row = (uint8_t*)fb_addr + y*fb_pitch;
        uint32_t *pixels = (uint32_t*)row;
        for(uint32_t x=0;x<fb_width;x++) pixels[x]=color;
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color){
    if(!fb_available) return;
    for(uint32_t dy=0; dy<h; dy++){
        for(uint32_t dx=0; dx<w; dx++){
            fb_put_pixel(x+dx, y+dy, color);
        }
    }
}
