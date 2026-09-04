#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "idt.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "framebuffer.h"
#include "graphics.h"
#include "mouse.h"
#include "window.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static int serial_transmit_empty(void) {
    return inb(0x3F8 + 5) & 0x20;
}

static void serial_putc(char c) {
    while (!serial_transmit_empty());
    outb(0x3F8, c);
}

static void serial_puts(const char* s) {
    for (size_t i = 0; s[i] != '\0'; i++) serial_putc(s[i]);
}

static void vga_puts(const char* s) {
    volatile uint16_t* vga = (volatile uint16_t*) 0xB8000;
    for (size_t i = 0; s[i] != '\0'; i++) {
        vga[i] = (uint16_t) s[i] | (uint16_t) 0x0F00;
    }
}

void kernel_main(uint32_t magic, uint32_t mbi_addr) {
    serial_init();
    serial_puts("Hello, OS - serial console working\n");
    vga_puts("Hello, OS");
    serial_puts("GDT: installing...\n");
    gdt_install();
    serial_puts("GDT: loaded OK, selectors reloaded\n");
    vga_puts("Hello, OS - GDT OK");
    serial_puts("Hello, OS - GDT OK\n");

    serial_puts("IDT: installing...\n");
    idt_install();
    serial_puts("IDT: loaded OK\n");
    vga_puts("Hello, OS - IDT OK");
    serial_puts("PIC: remapped to 0x20/0x28, keyboard IRQ1 enabled\n");
    serial_puts("IDT ready: press keys to see scancodes, or trigger #DE to test\n");

    // Check multiboot magic from boot.s (EBX preservation)
    serial_puts("BOOT: magic=");
    for(int i=28;i>=0;i-=4){ uint8_t v=(magic>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
    serial_puts(" mbi=");
    for(int i=28;i>=0;i-=4){ uint8_t v=(mbi_addr>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
    serial_puts("\n");
    if(magic != 0x2BADB002){
        serial_puts("BOOT: bad magic! expected 0x2BADB002\n");
    } else {
        serial_puts("BOOT: multiboot magic OK\n");
    }

    if(mbi_addr){
        struct multiboot_info *mbi = (struct multiboot_info*)mbi_addr;
        serial_puts("MBI: flags=");
        for(int i=28;i>=0;i-=4){ uint8_t v=(mbi->flags>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
        serial_puts("\n");
        if(mbi->flags & (1<<6)){
            // mmap present
            pmm_init(mbi_addr, mbi->mmap_addr, mbi->mmap_length);
        } else {
            serial_puts("MBI: no mmap flag!\n");
        }
    } else {
        serial_puts("BOOT: mbi_addr is 0!\n");
    }

    // quick allocator sanity test
    {
        uint32_t f1 = pmm_alloc_frame();
        uint32_t f2 = pmm_alloc_frame();
        uint32_t f3 = pmm_alloc_frame();
        serial_puts("PMM test: alloc ");
        for(int i=28;i>=0;i-=4){ uint8_t v=(f1>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
        serial_puts(" ");
        for(int i=28;i>=0;i-=4){ uint8_t v=(f2>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
        serial_puts(" ");
        for(int i=28;i>=0;i-=4){ uint8_t v=(f3>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
        serial_puts("\n");
        if(f1 && f2 && f3) serial_puts("PMM test: alloc OK\n"); else serial_puts("PMM test: alloc FAILED\n");
        pmm_free_frame(f2);
        uint32_t f4 = pmm_alloc_frame();
        serial_puts("PMM test: free f2 then alloc -> ");
        for(int i=28;i>=0;i-=4){ uint8_t v=(f4>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
        serial_puts("\n");
        if(f4==f2) serial_puts("PMM test: reuse OK\n"); else serial_puts("PMM test: reuse mismatch (expected reuse)\n");
        pmm_free_frame(f1); pmm_free_frame(f3); pmm_free_frame(f4);
        pmm_print_stats();
    }

    serial_puts("PAGING: installing identity map 0..4MB...\n");
    paging_init();
    serial_puts("PAGING: enabled OK\n");
    // strengthened r/w test: volatile, multiple addresses including edge of 4MB
    {
        struct { uint32_t addr; uint32_t pattern; const char *name; } tests[] = {
            {0x00001000, 0x11111111, "low 0x00001000"},
            {0x00102000, 0x22222222, "mid 0x00102000"},
            {0x003FF000, 0xDEADBEEF, "edge 0x003FF000 (last page start)"},
            {0x003FFFFC, 0xCAFEBABE, "edge 0x003FFFFC (last dword of 4MB)"},
        };
        int ok=1;
        for(int i=0;i<4;i++){
            volatile uint32_t *p = (volatile uint32_t*)tests[i].addr;
            uint32_t old = *p;
            __asm__ volatile("" ::: "memory");
            *p = tests[i].pattern;
            __asm__ volatile("" ::: "memory");
            uint32_t got = *p;
            __asm__ volatile("" ::: "memory");
            if(got != tests[i].pattern){
                serial_puts("PAGING: r/w FAILED at "); serial_puts(tests[i].name);
                serial_puts(" got 0x");
                for(int b=28;b>=0;b-=4){ uint8_t v=(got>>b)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
                serial_puts("\n");
                ok=0;
            } else {
                serial_puts("PAGING: r/w OK at "); serial_puts(tests[i].name); serial_puts("\n");
            }
            *p = old;
            __asm__ volatile("" ::: "memory");
        }
        if(ok) serial_puts("PAGING: r/w test OK (all 4 addresses)\n");
        else   serial_puts("PAGING: r/w test FAILED\n");
    }
    {
        uint32_t f = pmm_alloc_frame();
        serial_puts("PAGING: post-paging alloc 0x");
        for(int i=28;i>=0;i-=4){ uint8_t v=(f>>i)&0xF; char c=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,c); }
        serial_puts("\n");
        if(f) pmm_free_frame(f);
    }

    serial_puts("HEAP: init...\n");
    heap_init();
    heap_print_stats();
    // Phase 6 hand-traced test: distinct patterns, free/reuse, corruption check
    {
        serial_puts("HEAP test: alloc a=32 b=64 c=16\n");
        uint8_t *a = (uint8_t*)kmalloc(32);
        uint8_t *b = (uint8_t*)kmalloc(64);
        uint8_t *c = (uint8_t*)kmalloc(16);
        serial_puts("  a="); for(int i=28;i>=0;i-=4){ uint8_t v=(((uint32_t)a)>>i)&0xF; char ch=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,ch); } serial_puts(" b="); for(int i=28;i>=0;i-=4){ uint8_t v=(((uint32_t)b)>>i)&0xF; char ch=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,ch); } serial_puts(" c="); for(int i=28;i>=0;i-=4){ uint8_t v=(((uint32_t)c)>>i)&0xF; char ch=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,ch); } serial_puts("\n");
        if(!a||!b||!c){ serial_puts("HEAP test: alloc FAILED\n"); } else {
            for(int i=0;i<32;i++) a[i]=0xAA;
            for(int i=0;i<64;i++) b[i]=0xBB;
            for(int i=0;i<16;i++) c[i]=0xCC;
            serial_puts("  filled a=0xAA b=0xBB c=0xCC\n");
            // verify distinct
            int ok=1;
            for(int i=0;i<32;i++) if(a[i]!=0xAA) ok=0;
            for(int i=0;i<64;i++) if(b[i]!=0xBB) ok=0;
            for(int i=0;i<16;i++) if(c[i]!=0xCC) ok=0;
            serial_puts(ok?"  verify distinct OK\n":"  verify distinct FAILED (overlap?)\n");
            // free b
            uint32_t b_addr=(uint32_t)b;
            serial_puts("  free b\n");
            kfree(b);
            heap_print_stats();
            // alloc d=48 should reuse b's 64-byte block
            uint8_t *d = (uint8_t*)kmalloc(48);
            serial_puts("  d=kmalloc(48) -> "); for(int i=28;i>=0;i-=4){ uint8_t v=(((uint32_t)d)>>i)&0xF; char ch=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,ch); } serial_puts("\n");
            if((uint32_t)d == b_addr) serial_puts("  reuse OK (d==b)\n"); else serial_puts("  reuse FAILED (d!=b)\n");
            // check a and c not corrupted by reuse
            int ok2=1;
            for(int i=0;i<32;i++) if(a[i]!=0xAA) ok2=0;
            for(int i=0;i<16;i++) if(c[i]!=0xCC) ok2=0;
            serial_puts(ok2?"  a/c intact after reuse OK\n":"  a/c corrupted after reuse FAILED\n");
            // fill d with distinct pattern
            for(int i=0;i<48;i++) d[i]=0xDD;
            // second reuse test: free a, alloc e=24 should reuse a
            uint32_t a_addr=(uint32_t)a;
            kfree(a);
            uint8_t *e = (uint8_t*)kmalloc(24);
            serial_puts("  e=kmalloc(24) after free a -> "); for(int i=28;i>=0;i-=4){ uint8_t v=(((uint32_t)e)>>i)&0xF; char ch=v<10?'0'+v:'A'+v-10; while(!(inb(0x3F8+5)&0x20)); outb(0x3F8,ch); } serial_puts("\n");
            if((uint32_t)e == a_addr) serial_puts("  reuse2 OK (e==a)\n"); else serial_puts("  reuse2 FAILED\n");
            heap_print_stats();
            if(ok && ok2) serial_puts("HEAP test: PASS\n"); else serial_puts("HEAP test: FAIL\n");
        }
    }

    // Phase 7: framebuffer - requires paging for high fb address mapping
    {
        struct multiboot_info *mbi2 = (struct multiboot_info*)mbi_addr;
        if(mbi2 && (mbi2->flags & (1<<12))){
            serial_puts("FB: attempting init...\n");
            if(fb_init(mbi2)){
                serial_puts("FB: init OK, drawing test pattern\n");
                fb_fill(0x00112244); // dark blue background
                fb_draw_rect(100, 100, 300, 200, 0x00FFFFFF); // white rectangle
                fb_draw_rect(0, 0, fb_get_width(), 20, 0x00FF0000); // red top bar
                // draw border - fixed: was only top/bottom 2px at screen edge (thin, easily clipped by display scaling)
                // now full 4px border on all sides for visibility
                fb_draw_rect(0, 0, fb_get_width(), 4, 0x00FF00FF);
                fb_draw_rect(0, fb_get_height()-4, fb_get_width(), 4, 0x00FF00FF);
                fb_draw_rect(0, 0, 4, fb_get_height(), 0x00FF00FF);
                fb_draw_rect(fb_get_width()-4, 0, 4, fb_get_height(), 0x00FF00FF);
                serial_puts("FB: test pattern drawn (border fixed to 4px all sides)\n");
            } else {
                serial_puts("FB: init failed, staying in VGA text mode\n");
                vga_puts("FB fallback");
            }
        } else {
            serial_puts("FB: not available from GRUB (flags bit12 clear), fallback VGA text\n");
            vga_puts("FB fallback - no framebuffer");
        }
    }

    // Phase 8: 2D primitives + text on framebuffer
    if(fb_is_available()){
        serial_puts("GFX: testing primitives...\n");
        gfx_draw_line(0, 30, fb_get_width()-1, 30, 0x0000FF00); // horizontal green
        gfx_draw_line(50, 50, 200, 300, 0x00FFFF00); // diagonal yellow
        gfx_draw_line(300, 50, 150, 280, 0x00FF8000); // other diagonal orange
        gfx_draw_rect_outline(450, 100, 200, 150, 0x0000FFFF); // cyan outline (unfilled)
        gfx_draw_rect_outline(10, 400, 100, 80, 0x00FF00FF); // small magenta outline
        gfx_draw_circle(512, 384, 80, 0x00FFFFFF); // white circle center
        gfx_draw_circle(600, 500, 40, 0x0000FF00); // green circle
        serial_puts("GFX: lines/rects/circles drawn\n");
        gfx_draw_string(10, 40, "Hello, Framebuffer!", 0x00FFFFFF);
        gfx_draw_string(10, 50, "Line, Rect, Circle, Text OK", 0x00FFFFFF);
        gfx_draw_string(10, 60, "0123456789 ABCDEFG", 0x00FFFF00);
        gfx_draw_string(450, 280, "Outline Rect 200x150", 0x0000FFFF);
        serial_puts("GFX: text drawn\n");
        serial_puts("GFX: Phase 8 OK\n");
    } else {
        serial_puts("GFX: skipped - no framebuffer\n");
    }

    // Phase 10: Window manager (before mouse cursor, so windows under cursor)
    serial_puts("WM: init...\n");
    window_manager_init();
    serial_puts("WM: ready - 3 overlapping windows, click to focus\n");

    // Phase 9: PS/2 mouse + cursor (after windows, so cursor on top)
    serial_puts("MOUSE: init (IRQ12 vector 44)...\n");
    mouse_init();
    serial_puts("MOUSE: ready - move mouse over QEMU window and click\n");

    __asm__ volatile ("sti");

    // Uncomment to test exception handling (should print EXCEPTION and halt):
    // volatile int a = 1; volatile int b = 0; volatile int c = a / b; (void)c;

    // Main loop - deferred window redraw runs outside IRQ (fixes freeze)
    // Cursor blink handled here: window_tick_cursor toggles every ~20 redraws/idle ticks and sets needs_redraw
    for (;;) {
        window_tick_cursor();
        if(window_needs_redraw()){
            window_do_redraw();
        }
        __asm__ volatile ("hlt");
    }
}
