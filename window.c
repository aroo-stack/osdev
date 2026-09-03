#include "window.h"
#include "framebuffer.h"
#include "graphics.h"
#include "mouse.h"
#include <stdint.h>
#include <stddef.h>

static void w_strcpy(char *dst, const char *src, int n){
    for(int i=0;i<n-1;i++){ dst[i]=src[i]; if(!src[i]) break; }
    dst[n-1]=0;
}

static struct window windows[MAX_WINDOWS];
static int window_count = 0;
static int z_order[MAX_WINDOWS]; // indices sorted back->front

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_dec(int n){
    char b[12]; int i=0; int neg=0;
    if(n==0){s_putc('0');return;}
    if(n<0){neg=1; n=-n;}
    while(n){b[i++]='0'+n%10; n/=10;}
    if(neg) s_putc('-');
    while(i--) s_putc(b[i]);
}

// Forward for cursor interaction - mouse.c provides cursor save/restore
extern void window_redraw_with_cursor(void);

static void window_draw_single(int idx){
    struct window *w = &windows[idx];
    if(!w->visible) return;
    // filled background
    fb_draw_rect(w->x, w->y, w->w, w->h, w->bg_color);
    // title bar
    fb_draw_rect(w->x, w->y, w->w, TITLE_BAR_H, w->title_color);
    // border outline
    gfx_draw_rect_outline(w->x, w->y, w->w, w->h, w->border_color);
    gfx_draw_rect_outline(w->x, w->y, w->w, TITLE_BAR_H, w->border_color);
    // title text - centered vertically in title bar (8px font, title bar 20px)
    gfx_draw_string(w->x+6, w->y+6, w->title, 0x00FFFFFF);
}

void window_manager_init(void){
    if(!fb_is_available()){
        s_puts("WM: no framebuffer, skip windows\n");
        return;
    }
    // Create 3 overlapping windows - positions chosen to show overlap
    // Window 0 - back
    windows[0].x = 100; windows[0].y = 100; windows[0].w = 400; windows[0].h = 300;
    w_strcpy(windows[0].title, "Window 1", 32);
    windows[0].bg_color = 0x00E0E0E0; // light gray
    windows[0].title_color = 0x00336699; // steel blue
    windows[0].border_color = 0x00000000;
    windows[0].visible = 1;
    windows[0].z = 0;

    windows[1].x = 250; windows[1].y = 180; windows[1].w = 400; windows[1].h = 300;
    w_strcpy(windows[1].title, "Window 2", 32);
    windows[1].bg_color = 0x00D0D0FF; // light blue
    windows[1].title_color = 0x00993333; // reddish
    windows[1].border_color = 0x00000000;
    windows[1].visible = 1;
    windows[1].z = 1;

    windows[2].x = 400; windows[2].y = 250; windows[2].w = 350; windows[2].h = 250;
    w_strcpy(windows[2].title, "Window 3", 32);
    windows[2].bg_color = 0x00FFE0B0; // peach
    windows[2].title_color = 0x00339933; // green
    windows[2].border_color = 0x00000000;
    windows[2].visible = 1;
    windows[2].z = 2;

    window_count = 3;
    // z_order 0..2 back->front corresponds to windows index order initially
    for(int i=0;i<window_count;i++) z_order[i]=i;

    s_puts("WM: created 3 windows\n");
    window_manager_draw_all();
}

void window_manager_draw_all(void){
    if(!fb_is_available()) return;
    // Full screen redraw back-to-front
    // Desktop background
    fb_fill(0x00112244);

    // Draw windows back-to-front via z_order
    // z_order[0] = back, z_order[N-1] = front
    for(int i=0;i<window_count;i++){
        int idx = z_order[i];
        window_draw_single(idx);
    }

    // After full redraw, cursor's saved background is invalid (screen changed)
    // Caller should re-save cursor. We expose via mouse.c's cursor invalidation.
    // Instead, we just invalidate via extern call if needed, or let mouse handler re-save on next move.
    // For now, we don't draw cursor here - mouse.c will draw it after this call
    s_puts("WM: drew windows back->front z=[");
    for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
    s_puts("]\n");
}

int window_find_at(int x, int y){
    // front to back - topmost first
    for(int i=window_count-1; i>=0; i--){
        int idx = z_order[i];
        struct window *w = &windows[idx];
        if(!w->visible) continue;
        if(x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h){
            return idx;
        }
    }
    return -1;
}

int window_bring_to_front(int idx){
    if(idx <0 || idx >= window_count) return 0;
    // find current position in z_order
    int pos = -1;
    for(int i=0;i<window_count;i++) if(z_order[i]==idx) pos=i;
    if(pos==-1) return 0;
    if(pos == window_count-1){
        s_puts("WM: window "); s_put_dec(idx+1); s_puts(" already front\n");
        return 0; // already front, no redraw
    }
    // Redraw strategy: full screen redraw on z-order change
    // Cursor save/restore assumes static background, but windows reordering changes background.
    // So restore old cursor, invalidate, then full redraw, then draw cursor at new position.
    mouse_cursor_restore();
    mouse_cursor_invalidate();
    // move to front: shift elements between pos+1..end down one, put idx at end
    for(int i=pos;i<window_count-1;i++) z_order[i]=z_order[i+1];
    z_order[window_count-1]=idx;
    // update z values for debug
    for(int i=0;i<window_count;i++) windows[z_order[i]].z = i;
    s_puts("WM: bring window "); s_put_dec(idx+1); s_puts(" to front, new z=[");
    for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
    s_puts("]\n");
    window_manager_draw_all();
    // After full redraw, draw cursor at current mouse position with fresh save
    mouse_cursor_draw_current();
    return 1;
}

int window_handle_click(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1){
        s_puts("WM: click at "); s_put_dec(x); s_putc(','); s_put_dec(y); s_puts(" on desktop\n");
        return 0;
    }
    s_puts("WM: click at "); s_put_dec(x); s_putc(','); s_put_dec(y); s_puts(" inside Window "); s_put_dec(idx+1); s_puts("\n");
    return window_bring_to_front(idx);
}

void window_get_info(int idx, int *x, int *y, int *w, int *h){
    if(idx<0||idx>=window_count) return;
    if(x) *x=windows[idx].x;
    if(y) *y=windows[idx].y;
    if(w) *w=windows[idx].w;
    if(h) *h=windows[idx].h;
}

// --- Phase 11: dragging ---
static int dragging = 0;
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;

int window_is_in_title_bar(int idx, int x, int y){
    if(idx<0||idx>=window_count) return 0;
    struct window *w = &windows[idx];
    return (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + TITLE_BAR_H);
}

int window_is_dragging(void){ return dragging; }

int window_start_drag(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1) return 0;
    if(!window_is_in_title_bar(idx,x,y)) return 0;
    // Bring to front first (if not already) - this does full redraw with cursor handling
    // But we need offset before bringing to front? Offset should be based on window pos before bring to front (pos doesn't change on bring to front, only z)
    struct window *w = &windows[idx];
    drag_off_x = x - w->x;
    drag_off_y = y - w->y;
    drag_win = idx;
    dragging = 1;
    s_puts("WM: drag start Window "); s_put_dec(idx+1); s_puts(" offset "); s_put_dec(drag_off_x); s_putc(','); s_put_dec(drag_off_y); s_puts("\n");
    // Ensure window is front - this will do full redraw and cursor draw at current pos
    window_bring_to_front(idx);
    return 1;
}

void window_update_drag(int x, int y){
    if(!dragging || drag_win==-1) return;
    struct window *w = &windows[drag_win];
    int new_x = x - drag_off_x;
    int new_y = y - drag_off_y;

    // Bounds: allow partially off-screen but keep title bar visible so it can be dragged back
    // Chosen: keep at least 60px of width visible and title bar 20px visible
    // So x in [-w+60, width-60], y in [0, height - TITLE_BAR_H]
    // Alternative fully on-screen would be 0..width-w, 0..height-h - we chose partially for usability
    int min_x = -w->w + 60;
    int max_x = (int)fb_get_width() - 60;
    int min_y = 0;
    int max_y = (int)fb_get_height() - TITLE_BAR_H;
    if(new_x < min_x) new_x = min_x;
    if(new_x > max_x) new_x = max_x;
    if(new_y < min_y) new_y = min_y;
    if(new_y > max_y) new_y = max_y;

    if(new_x == w->x && new_y == w->y) return; // no move

    // Redraw strategy for dragging: full screen redraw each move
    // Why full redraw vs incremental (only erase old window rect and draw new)?
    // Full redraw is simplest and guarantees correct z-order overlap for all windows.
    // Incremental would need to handle clipping where dragged window overlaps others, and where
    // other windows become exposed when dragged window moves away - complex to get right without artifacts.
    // With only 3 windows at 1024x768, full redraw is ~3MB fill + 3*~120K windows = ~3.4M pixels per move.
    // At 60-100Hz mouse moves, that's ~200-340M pixels/sec, well within QEMU's ~1GB/sec, no flicker on modern host.
    // So we choose full redraw each drag move for correctness over micro-optimization.
    mouse_cursor_restore();
    mouse_cursor_invalidate();
    w->x = new_x;
    w->y = new_y;
    window_manager_draw_all();
    mouse_cursor_draw_current();
}

void window_end_drag(void){
    if(!dragging) return;
    s_puts("WM: drag end Window "); s_put_dec(drag_win+1); s_puts("\n");
    dragging = 0;
    drag_win = -1;
}
