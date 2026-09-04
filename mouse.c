#include "mouse.h"
#include "framebuffer.h"
#include "window.h"
#include <stdint.h>
#include <stddef.h>
extern volatile int g_in_redraw;
static int g_missed_during_redraw = 0;
static uint64_t g_last_tsc = 0;
static inline uint64_t rdtsc_m(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }

// 8042 ports - verified OSDev 8042 PS/2 Controller
#define PORT_KBD_DATA 0x60
#define PORT_KBD_CMD  0x64

// Cursor state - 16x16 save buffer
#define CURSOR_W 12
#define CURSOR_H 12
static int mouse_x = 512; // center
static int mouse_y = 384;
static int mouse_enabled = 0;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];
static uint32_t saved_pixels[16*16];
static int saved_x = -1, saved_y = -1;
static int saved_valid = 0;

// serial helpers
static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex8(uint8_t n){ const char *h="0123456789ABCDEF"; s_putc(h[(n>>4)&0xF]); s_putc(h[n&0xF]); }
static void s_put_dec(int n){
    char b[12]; int i=0; int neg=0;
    if(n==0){s_putc('0');return;}
    if(n<0){neg=1; n=-n;}
    while(n){b[i++]='0'+n%10; n/=10;}
    if(neg) s_putc('-');
    while(i--) s_putc(b[i]);
}

// 8042 helpers - verify each command byte per OSDev
static void wait_input(void){ // wait for IBF clear (bit1 ==0) before writing to 0x64/0x60
    for(int i=0;i<100000;i++) if((inb(PORT_KBD_CMD)&2)==0) return;
}
static void wait_output(void){ // wait for OBF set (bit0==1) before reading 0x60
    for(int i=0;i<100000;i++) if(inb(PORT_KBD_CMD)&1) return;
}
static void flush_output(void){
    for(int i=0;i<10;i++) if(inb(PORT_KBD_CMD)&1) (void)inb(PORT_KBD_DATA); else break;
}

static void mouse_write(uint8_t data){
    wait_input();
    outb(PORT_KBD_CMD, 0xD4); // 0xD4 = Write next byte to AUX (mouse) - verified OSDev
    wait_input();
    outb(PORT_KBD_DATA, data);
}

static uint8_t mouse_read_ack(void){
    wait_output();
    return inb(PORT_KBD_DATA);
}

// Save/restore - simplest approach that leaves no trail: save 16x16 pixels under cursor before drawing, restore on move.
// Why this vs filling with background color: filling would erase whatever was underneath (white rect, text, lines) and leave trail over content.
// Save/restore preserves whatever was underneath, so cursor moving over existing drawings doesn't destroy them.
static void cursor_save(int x, int y){
    for(int dy=0; dy<CURSOR_H; dy++){
        for(int dx=0; dx<CURSOR_W; dx++){
            int sx = x+dx, sy = y+dy;
            if(sx < (int)fb_get_width() && sy < (int)fb_get_height())
                saved_pixels[dy*CURSOR_W+dx] = fb_get_pixel(sx,sy);
            else
                saved_pixels[dy*CURSOR_W+dx] = 0;
        }
    }
    saved_x = x; saved_y = y; saved_valid = 1;
}
static void cursor_restore(void){
    if(!saved_valid) return;
    for(int dy=0; dy<CURSOR_H; dy++){
        for(int dx=0; dx<CURSOR_W; dx++){
            int sx = saved_x+dx, sy = saved_y+dy;
            if(sx < (int)fb_get_width() && sy < (int)fb_get_height())
                fb_put_pixel(sx,sy, saved_pixels[dy*CURSOR_W+dx]);
        }
    }
    saved_valid = 0;
}

// Small filled triangle cursor - right-pointing arrow, white with black border, 12x12
static void draw_cursor_shape(int x, int y){
    // Fill triangle points (0,0) top, (0,11) bottom left, (11,5) tip right - right-pointing
    // Simple: draw filled triangle via scanlines
    for(int dy=0; dy<CURSOR_H; dy++){
        int w;
        if(dy <= 5) w = dy+1; // top half expanding
        else w = 12 - (dy-5); // bottom half contracting, max 6 at middle
        if(w<1) w=1;
        if(w>CURSOR_W) w=CURSOR_W;
        for(int dx=0; dx<w; dx++){
            // white interior
            fb_put_pixel(x+dx, y+dy, 0x00FFFFFF);
        }
        // black border outline for visibility on white background
        fb_put_pixel(x+w, y+dy, 0x00000000);
        fb_put_pixel(x, y+dy, 0x00000000);
    }
    // tip outline
    fb_put_pixel(x+11, y+5, 0x00000000);
    fb_put_pixel(x+10, y+5, 0x00000000);
}

static void cursor_draw(int x, int y){
    cursor_save(x,y);
    draw_cursor_shape(x,y);
}

void mouse_cursor_restore(void){ cursor_restore(); }
void mouse_cursor_draw_current(void){ cursor_draw(mouse_x, mouse_y); }
void mouse_cursor_invalidate(void){ saved_valid = 0; }

static uint8_t prev_buttons = 0;

void mouse_handle_byte(uint8_t data){
    // PS/2 3-byte packet: byte0 buttons/sign/overflow/always1, byte1 dx, byte2 dy
    // Trace byte layout per OSDev PS/2 Mouse:
    // Byte0: bit0 Left, bit1 Right, bit2 Middle, bit3 Always1, bit4 X sign, bit5 Y sign, bit6 X overflow, bit7 Y overflow
    // Byte1: X movement (signed)
    // Byte2: Y movement (signed) - Y positive is up, so screen Y decreases
    if(mouse_cycle==0){
        // expect bit3 ==1 for sync; if not, discard and stay in sync search
        if((data & 0x08)==0){
            return;
        }
        mouse_packet[0]=data;
        mouse_cycle=1;
        return;
    } else if(mouse_cycle==1){
        mouse_packet[1]=data;
        mouse_cycle=2;
        return;
    } else {
        mouse_packet[2]=data;
        mouse_cycle=0;
        // full packet ready
        uint8_t b0 = mouse_packet[0];
        int8_t dx = (int8_t)mouse_packet[1];
        int8_t dy = (int8_t)mouse_packet[2];
        uint8_t buttons = b0 & 0x07;
        // handle overflow - discard movement if overflow set
        if(b0 & 0x40) dx = 0; // X overflow bit6
        if(b0 & 0x80) dy = 0; // Y overflow bit7
        // Y is inverted: PS/2 dy positive = up, screen y increases down
        int new_x = mouse_x + dx;
        int new_y = mouse_y - dy;

        // clamp to screen bounds
        int max_x = fb_is_available() ? (int)fb_get_width() -1 : 1024;
        int max_y = fb_is_available() ? (int)fb_get_height() -1 : 768;
        if(new_x < 0) new_x = 0;
        if(new_y < 0) new_y = 0;
        if(new_x > max_x) new_x = max_x;
        if(new_y > max_y) new_y = max_y;

        // instrumentation for Phase 11 double-buffer freeze: check if IRQ arrived while previous redraw still in progress
        if(g_in_redraw){
            g_missed_during_redraw++;
            if((g_missed_during_redraw % 10)==0){
                s_puts("MOUSE: IRQ12 during redraw! missed="); s_put_dec(g_missed_during_redraw); s_puts("\n");
            }
        }
        uint64_t cur_tsc = rdtsc_m();
        if(g_last_tsc){
            uint64_t delta = cur_tsc - g_last_tsc;
            // log every 20th IRQ interval to see rate vs redraw time
            static int irq_cnt=0;
            if((++irq_cnt % 20)==0){
                s_puts("MOUSE: IRQ interval cycles "); s_put_dec((uint32_t)delta); s_puts("\n");
            }
        }
        g_last_tsc = cur_tsc;

        // serial debug - always, even before visual
        s_puts("MOUSE: dx=");
        s_put_dec(dx);
        s_puts(" dy=");
        s_put_dec(dy);
        s_puts(" buttons=0x");
        s_put_hex8(buttons);
        s_puts(" pos=");
        s_put_dec(new_x); s_putc(','); s_put_dec(new_y);
        s_puts("\n");

        int left_pressed = (buttons & 0x01) && !(prev_buttons & 0x01);
        int left_released = !(buttons & 0x01) && (prev_buttons & 0x01);
        int moved = (new_x != mouse_x || new_y != mouse_y);

        // Phase 11/12: dragging, button, click handling - order matters: drag title bar -> button -> body
        if(window_is_dragging()){
            if(left_released){
                __asm__ volatile("cli");
                window_end_drag();
                if(moved && fb_is_available()){
                    cursor_restore();
                    mouse_x = new_x; mouse_y = new_y;
                    cursor_draw(mouse_x, mouse_y);
                    if(fb_is_double_buffered()) fb_swap();
                } else {
                    mouse_x = new_x; mouse_y = new_y;
                }
                __asm__ volatile("sti");
            } else if(moved){
                __asm__ volatile("cli");
                mouse_x = new_x; mouse_y = new_y;
                window_update_drag(new_x, new_y);
                __asm__ volatile("sti");
            } else {
                mouse_x = new_x; mouse_y = new_y;
            }
        } else if(left_pressed){
            __asm__ volatile("cli");
            mouse_x = new_x; mouse_y = new_y;
            int is_title = 0;
            int hit = window_find_at(new_x, new_y);
            if(hit != -1) is_title = window_is_in_title_bar(hit, new_x, new_y);
            if(is_title){
                window_start_drag(new_x, new_y);
            } else if(window_handle_button_down(new_x, new_y)){
                // button hit - handled (bring to front + pressed visual)
                // window_handle_button_down already did full redraw with cursor
            } else {
                int did_redraw = 0;
                if(fb_is_available()){
                    did_redraw = window_handle_click(new_x, new_y);
                }
                if(!did_redraw && moved && fb_is_available()){
                    cursor_restore();
                    cursor_draw(new_x, new_y);
                    if(fb_is_double_buffered()) fb_swap();
                }
            }
            __asm__ volatile("sti");
        } else if(left_released){
            // button release - check if over same button that was pressed
            __asm__ volatile("cli");
            mouse_x = new_x; mouse_y = new_y;
            // If a button was pressed, handle up (may increment count and redraw)
            // window_handle_button_up will handle pressed state and full redraw if needed
            // It returns 1 if it handled a button, otherwise we handle normal cursor move
            int handled = 0;
            if(fb_is_available()){
                handled = window_handle_button_up(new_x, new_y);
            }
            if(!handled && moved && fb_is_available()){
                cursor_restore();
                cursor_draw(mouse_x, mouse_y);
            } else if(!handled){
                // no button, just update pos (already done)
            }
            __asm__ volatile("sti");
        } else if(moved && fb_is_available()){
            __asm__ volatile("cli");
            cursor_restore();
            mouse_x = new_x;
            mouse_y = new_y;
            cursor_draw(mouse_x, mouse_y);
            if(fb_is_double_buffered()) fb_swap();
            __asm__ volatile("sti");
        } else {
            mouse_x = new_x;
            mouse_y = new_y;
        }
        prev_buttons = buttons;
    }
}

void mouse_init(void){
    s_puts("MOUSE: init via 8042...\n");
    // Defense: explicitly reset BSS-dependent state even if bootloader did not zero BSS
    // Without this, first packet's mouse_cycle could be garbage -> desync and first move glitch,
    // and saved_valid garbage -> first cursor_restore reads garbage pixels -> flicker
    mouse_cycle = 0;
    saved_valid = 0;
    saved_x = -1; saved_y = -1;
    // Flush any pending data
    flush_output();

    // Step 1: Enable AUX port - 0xA8 - verified OSDev 8042
    wait_input();
    outb(PORT_KBD_CMD, 0xA8);
    // Small delay and flush any spurious byte that 0xA8 may generate on some QEMU configs (observed 0x41 with double buffer timing)
    for(int i=0;i<10000;i++) __asm__ volatile("nop");
    flush_output();
    s_puts("MOUSE: sent 0xA8 enable AUX\n");

    // Step 2: Enable IRQ12 in command byte
    // Read command byte via 0x20
    wait_input();
    outb(PORT_KBD_CMD, 0x20); // 0x20 = Read command byte
    wait_output();
    uint8_t cmd = inb(PORT_KBD_DATA);
    s_puts("MOUSE: cmd byte before=0x"); s_put_hex8(cmd); s_puts("\n");
    cmd |= 0x02; // bit1 = enable IRQ12 (second port)
    cmd &= ~0x20; // bit5 = 0 enable second port clock (0=enable, 1=disable)
    cmd |= 0x01; // ensure IRQ1 keyboard also enabled
    // keep translation etc as is
    wait_input();
    outb(PORT_KBD_CMD, 0x60); // 0x60 = Write command byte
    wait_input();
    outb(PORT_KBD_DATA, cmd);
    s_puts("MOUSE: cmd byte after=0x"); s_put_hex8(cmd); s_puts("\n");

    // Step 3: Enable mouse data reporting - via 0xD4 + 0xF4
    // 0xD4 = Write next byte to AUX
    // Mouse command 0xF4 = Enable data reporting
    mouse_write(0xF4);
    uint8_t ack = mouse_read_ack();
    s_puts("MOUSE: enable 0xF4 ack=0x"); s_put_hex8(ack); s_puts("\n");
    if(ack != 0xFA){
        s_puts("MOUSE: warning ack != 0xFA, retrying once\n");
        mouse_write(0xF4);
        ack = mouse_read_ack();
        s_puts("MOUSE: retry ack=0x"); s_put_hex8(ack); s_puts("\n");
    }
    // Flush any extra bytes that may have arrived (e.g., mouse may send 0xAA after reset, but not after 0xF4)
    // Ensure output buffer empty before unmasking IRQs, otherwise stray 0xFA could be delivered as IRQ1 (keyboard) and show as spurious KEY 0xFA
    for(int i=0;i<10;i++){
        if(inb(PORT_KBD_CMD) & 1){
            uint8_t extra = inb(PORT_KBD_DATA);
            s_puts("MOUSE: flushed extra 0x"); s_put_hex8(extra); s_puts("\n");
        } else break;
    }

    // Set defaults: sample rate 100, resolution 2? Optional, but not needed for 3-byte mode
    // Mouse now in streaming mode, will send packets on movement

    // Unmask IRQ12 on PIC - master 0x21, slave 0xA1
    // Master offset 0x20 (32) 0-7, slave 0x28 (40) 8-15, IRQ12 = slave 4 => vector 44 = 0x20+12 = 0x28+4
    // Need to enable slave cascade on master (IRQ2) and IRQ12 on slave
    uint8_t master_mask = inb(0x21);
    uint8_t slave_mask = inb(0xA1);
    s_puts("MOUSE: PIC masks before master=0x"); s_put_hex8(master_mask); s_puts(" slave=0x"); s_put_hex8(slave_mask); s_puts("\n");
    master_mask &= ~0x04; // clear bit2 = enable cascade (IRQ2)
    // keep keyboard IRQ1 enabled (bit1=0), others as before but ensure IRQ2 enabled
    // Our previous mask was 0xF9 (11111001) for keyboard+mouse; keep that
    master_mask = 0xF9; // enable IRQ1+IRQ2, mask others (including timer masked for simplicity)
    slave_mask &= ~0x10; // clear bit4 = enable IRQ12
    slave_mask = 0xEF; // 11101111 enable 12 only
    outb(0x21, master_mask);
    outb(0xA1, slave_mask);
    s_puts("MOUSE: PIC masks after master=0x"); s_put_hex8(master_mask); s_puts(" slave=0x"); s_put_hex8(slave_mask); s_puts("\n");

    // Flush any byte that arrived between unmask and now (would otherwise fire as spurious IRQ1 0xFA after sti)
    for(int i=0;i<100;i++){
        if(inb(PORT_KBD_CMD) & 1){
            uint8_t extra = inb(PORT_KBD_DATA);
            s_puts("MOUSE: post-unmask flush 0x"); s_put_hex8(extra); s_puts("\n");
        } else break;
        for(int j=0;j<1000;j++) __asm__ volatile("nop");
    }

    // Init cursor position center and draw
    if(fb_is_available()){
        mouse_x = fb_get_width()/2;
        mouse_y = fb_get_height()/2;
        cursor_draw(mouse_x, mouse_y);
        if(fb_is_double_buffered()) fb_swap();
        s_puts("MOUSE: cursor drawn at center\n");
    }
    mouse_enabled = 1;
    s_puts("MOUSE: enabled, IRQ12 vector 44 (0x2C) ready\n");
}

int mouse_is_enabled(void){ return mouse_enabled; }
void mouse_get_position(int *x,int *y){ if(x) *x=mouse_x; if(y) *y=mouse_y; }

