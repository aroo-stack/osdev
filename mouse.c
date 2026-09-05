#include "mouse.h"
#include "framebuffer.h"
#include "window.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

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
    for(int dy=0; dy<CURSOR_H; dy++){
        int w;
        if(dy <= 5) w = dy+1; // top half expanding
        else w = 12 - (dy-5); // bottom half contracting, max 6 at middle
        if(w<1) w=1;
        if(w>CURSOR_W) w=CURSOR_W;
        for(int dx=0; dx<w; dx++){
            fb_put_pixel(x+dx, y+dy, 0x00FFFFFF);
        }
        fb_put_pixel(x+w, y+dy, 0x00000000);
        fb_put_pixel(x, y+dy, 0x00000000);
    }
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
static uint64_t g_last_tsc = 0;
extern volatile int g_in_redraw;
static inline uint64_t rdtsc_m(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }

void mouse_handle_byte(uint8_t data){
    mouse_irq_count_inc();
    if(mouse_cycle==0){
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
        uint8_t b0 = mouse_packet[0];
        int8_t dx = (int8_t)mouse_packet[1];
        int8_t dy = (int8_t)mouse_packet[2];
        uint8_t buttons = b0 & 0x07;
        if(b0 & 0x40) dx = 0;
        if(b0 & 0x80) dy = 0;
        int new_x = mouse_x + dx;
        int new_y = mouse_y - dy;
        int max_x = fb_is_available() ? (int)fb_get_width() -1 : 1024;
        int max_y = fb_is_available() ? (int)fb_get_height() -1 : 768;
        if(new_x < 0) new_x = 0;
        if(new_y < 0) new_y = 0;
        if(new_x > max_x) new_x = max_x;
        if(new_y > max_y) new_y = max_y;
        if(g_in_redraw){
            // IRQ arrived while previous redraw still in progress - count but don't re-enter heavy work
            // This is the deferred-redraw fix: heavy work should be outside IRQ, so this should rarely happen now
        }
        uint64_t cur_tsc = rdtsc_m();
        if(g_last_tsc){
            uint64_t delta = cur_tsc - g_last_tsc;
            static int irq_cnt=0;
            if((++irq_cnt % 20)==0){
                s_puts("MOUSE: IRQ interval cycles "); s_put_dec((uint32_t)delta); s_puts("\n");
            }
        }
        g_last_tsc = cur_tsc;
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
        } else if(window_is_resizing()){
            if(left_released){
                __asm__ volatile("cli");
                window_end_resize();
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
                window_update_resize(new_x, new_y);
                __asm__ volatile("sti");
            } else {
                mouse_x = new_x; mouse_y = new_y;
            }
        } else if(left_pressed){
            __asm__ volatile("cli");
            mouse_x = new_x; mouse_y = new_y;
            if(window_handle_close_click(new_x, new_y)){
            } else if(window_handle_minimize_click(new_x, new_y)){
            } else if(window_handle_taskbar_click(new_x, new_y)){
            } else {
                int is_resize = 0;
                int hit2 = window_find_at(new_x, new_y);
                if(hit2 != -1) is_resize = window_is_in_resize_handle(hit2, new_x, new_y);
                int is_title = 0;
                int hit = window_find_at(new_x, new_y);
                if(hit != -1) is_title = window_is_in_title_bar(hit, new_x, new_y);
                if(is_resize){
                    window_start_resize(new_x, new_y);
                } else if(is_title){
                    window_start_drag(new_x, new_y);
                } else if(window_handle_button_down(new_x, new_y)){
                } else if(window_handle_textbox_click(new_x, new_y)){
                } else if(window_handle_taskmanager_kill_click(new_x, new_y)){
                    // Kill Clicker/Notes task and its window - handled (GUI has no Kill button)
                } else {
                    int did_redraw = 0;
                    if(fb_is_available()){
                        did_redraw = window_handle_click(new_x, new_y);
                    }
                    if(did_redraw){
                        // click was inside a window body, icon check skipped (icons are under windows)
                    } else if(fb_is_available() && desktop_icon_handle_click(new_x, new_y)){
                        // icon hit - handled (single select or double-click action), redraw deferred to main loop
                    } else {
                        // empty desktop (not taskbar/window/icon) -> deselect any selected icon
                        if(fb_is_available()) desktop_icon_deselect_all();
                        if(moved && fb_is_available()){
                            cursor_restore();
                            cursor_draw(new_x, new_y);
                            if(fb_is_double_buffered()) fb_swap();
                        }
                    }
                }
            }
            __asm__ volatile("sti");
        } else if(left_released){
            __asm__ volatile("cli");
            mouse_x = new_x; mouse_y = new_y;
            int handled = 0;
            if(fb_is_available()){
                handled = window_handle_button_up(new_x, new_y);
            }
            if(!handled && moved && fb_is_available()){
                cursor_restore();
                cursor_draw(mouse_x, mouse_y);
                if(fb_is_double_buffered()) fb_swap();
            }
            __asm__ volatile("sti");
        } else if(moved && fb_is_available()){
            // Defer even simple moves to main loop to avoid race with window's textbox cursor and double-buffer save/restore
            mouse_x = new_x;
            mouse_y = new_y;
            extern volatile int g_needs_redraw;
            g_needs_redraw = 1;
        } else {
            mouse_x = new_x;
            mouse_y = new_y;
        }
        prev_buttons = buttons;
    }
}

void mouse_init(void){
    s_puts("MOUSE: init via 8042...\n");
    mouse_cycle = 0;
    saved_valid = 0;
    saved_x = -1; saved_y = -1;
    flush_output();
    wait_input();
    outb(PORT_KBD_CMD, 0xA8);
    for(int i=0;i<10000;i++) __asm__ volatile("nop");
    flush_output();
    s_puts("MOUSE: sent 0xA8 enable AUX\n");
    wait_input();
    outb(PORT_KBD_CMD, 0x20);
    wait_output();
    uint8_t cmd = inb(PORT_KBD_DATA);
    s_puts("MOUSE: cmd byte before=0x"); s_put_hex8(cmd); s_puts("\n");
    cmd |= 0x02;
    cmd &= ~0x20;
    cmd |= 0x01;
    wait_input();
    outb(PORT_KBD_CMD, 0x60);
    wait_input();
    outb(PORT_KBD_DATA, cmd);
    s_puts("MOUSE: cmd byte after=0x"); s_put_hex8(cmd); s_puts("\n");
    mouse_write(0xF4);
    uint8_t ack = mouse_read_ack();
    s_puts("MOUSE: enable 0xF4 ack=0x"); s_put_hex8(ack); s_puts("\n");
    if(ack != 0xFA){
        s_puts("MOUSE: warning ack != 0xFA, retrying once\n");
        mouse_write(0xF4);
        ack = mouse_read_ack();
        s_puts("MOUSE: retry ack=0x"); s_put_hex8(ack); s_puts("\n");
    }
    for(int i=0;i<10;i++){
        if(inb(PORT_KBD_CMD) & 1){
            uint8_t extra = inb(PORT_KBD_DATA);
            s_puts("MOUSE: flushed extra 0x"); s_put_hex8(extra); s_puts("\n");
        } else break;
    }
    uint8_t master_mask = inb(0x21);
    uint8_t slave_mask = inb(0xA1);
    s_puts("MOUSE: PIC masks before master=0x"); s_put_hex8(master_mask); s_puts(" slave=0x"); s_put_hex8(slave_mask); s_puts("\n");
    master_mask &= ~0x04;
    master_mask = 0xF9;
    slave_mask &= ~0x10;
    slave_mask = 0xEF;
    outb(0x21, master_mask);
    outb(0xA1, slave_mask);
    s_puts("MOUSE: PIC masks after master=0x"); s_put_hex8(master_mask); s_puts(" slave=0x"); s_put_hex8(slave_mask); s_puts("\n");
    for(int i=0;i<100;i++){
        if(inb(PORT_KBD_CMD) & 1){
            uint8_t extra = inb(PORT_KBD_DATA);
            s_puts("MOUSE: post-unmask flush 0x"); s_put_hex8(extra); s_puts("\n");
        } else break;
        for(int j=0;j<1000;j++) __asm__ volatile("nop");
    }
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
