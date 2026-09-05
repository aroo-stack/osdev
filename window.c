#include "window.h"
#include "framebuffer.h"
#include "graphics.h"
#include "mouse.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

static void w_strcpy(char *dst, const char *src, int n){
    for(int i=0;i<n-1;i++){ dst[i]=src[i]; if(!src[i]) break; }
    dst[n-1]=0;
}
static int icon_streq(const char *a, const char *b);

struct window windows[MAX_WINDOWS];
int window_count = 0;
int z_order[MAX_WINDOWS]; // indices sorted back->front
// drag state - must be zeroed at boot (BSS) and explicitly reset in window_manager_init
static int dragging = 0;
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;
// resize state - Phase 14
static int resizing = 0;
static int resize_win = -1;
static int resize_off_x = 0, resize_off_y = 0;
static int resize_start_w = 0, resize_start_h = 0;
// instrumentation for Phase 11 double-buffer freeze investigation
volatile int g_in_redraw = 0;
static int g_redraw_count = 0;
static int g_missed_during_redraw = 0;
volatile int g_sched_during_redraw = 0;
static inline uint64_t rdtsc(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }
// deferred redraw flag for Phase 12 fix - heavy window redraw should not run inside IRQ
volatile int g_needs_redraw = 0;

// --- Bliss wallpaper cache (1x framebuffer, ~8.3MB at 1920x1080) to avoid recomputing sky+hills+circles every frame ---
// Trade: PMM has ~127MB free (~32300 frames before), back buffer 2025 pages (~8.3MB at 1920*1080*4), cache also 2025 pages => ~16.6MB total + heap/task stacks ~ few MB => ~20MB well within 127MB
// Virtual layout: heap 0x00400000-0x00500000 (1MB), fb_back 0x00600000-0x00DE9000 (~8.3MB), wallpaper 0x00F00000-0x016E9000 (~8.3MB), task stacks 0x03000000+ below 0xFD000000 fb_front (tried 0x01000000/0x02000000 at 16M/32M but high PD caused slow rep movsl)
static uint32_t *wallpaper_cache = 0;
static uint32_t wallpaper_cache_bytes = 0;
static uint32_t wallpaper_cache_pages = 0;
static int wallpaper_cache_ready = 0;
static uint64_t wallpaper_build_cycles = 0; // cycles to build once
static uint64_t last_wallpaper_cycles = 0; // per-frame wallpaper cost (recompute or blit)
static uint64_t last_windows_cycles = 0;
static uint64_t last_sky_cycles = 0;
static uint64_t last_hill_cycles = 0;
static uint64_t last_cloud_cycles = 0;
static int last_wallpaper_was_blit = 0;
static void wallpaper_cache_build_once(void);

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
static void s_put_hex32(uint32_t n);
static void s_put_cycles(uint64_t n);

// Forward for cursor interaction - mouse.c provides cursor save/restore
extern void window_redraw_with_cursor(void);

static void window_draw_button(struct window *w){
    if(!w->has_button) return;
    // Button absolute position = window x + button x, window y + button y
    // Button y is relative to window origin (0,0 = window top-left), so absolute already includes title bar offset if button placed below it
    // Conversion: ax = w->x + btn.x, ay = w->y + btn.y
    int ax = w->x + w->btn.x;
    int ay = w->y + w->btn.y;
    uint32_t bg = w->btn.pressed ? 0x00999999 : 0x00CCCCCC; // pressed darker
    uint32_t border = 0x00000000;
    uint32_t fg = 0x00000000; // black text on light button
    fb_draw_rect(ax, ay, w->btn.w, w->btn.h, bg);
    gfx_draw_rect_outline(ax, ay, w->btn.w, w->btn.h, border);
    // label centered
    int len = 0; while(w->btn.label[len] && len < 32) len++;
    int tx = ax + (w->btn.w - len*8)/2;
    int ty = ay + (w->btn.h - 8)/2;
    gfx_draw_string(tx, ty, w->btn.label, fg);
}

static void window_draw_textbox(struct window *w){
    if(!w->has_textbox) return;
    int ax = w->x + w->tbox.x;
    int ay = w->y + w->tbox.y;
    uint32_t bg = 0x00FFFFFF; // white
    uint32_t border = w->tbox.focused ? 0x000000FF : 0x00888888; // blue when focused, gray otherwise
    uint32_t textcol = 0x00000000; // black text
    fb_draw_rect(ax, ay, w->tbox.w, w->tbox.h, bg);
    gfx_draw_rect_outline(ax, ay, w->tbox.w, w->tbox.h, border);
    // multi-line wrapping: chars per line based on width, lines based on height
    int chars_per_line = (w->tbox.w - 8) / 8; // 4px padding each side
    if(chars_per_line < 1) chars_per_line = 1;
    int line_height = 10; // 8px font + 2px spacing
    int lines_visible = (w->tbox.h - 8) / line_height; // 4px top/bottom padding
    if(lines_visible < 1) lines_visible = 1;
    int len = w->tbox.len;
    if(len > w->tbox.max_len) len = w->tbox.max_len;
    // total lines needed for current buffer
    int total_lines = (len + chars_per_line - 1) / chars_per_line;
    if(total_lines < 1) total_lines = 1;
    // vertical scrolling: if more lines than fit, show last N lines
    int first_line = 0;
    if(total_lines > lines_visible){
        first_line = total_lines - lines_visible;
    }
    // draw visible lines
    int tx0 = ax + 4;
    int ty0 = ay + 4;
    for(int line = 0; line < lines_visible; line++){
        int src_line = first_line + line;
        int start_idx = src_line * chars_per_line;
        if(start_idx >= len) break; // no more text for this line
        int remaining = len - start_idx;
        int to_draw = remaining < chars_per_line ? remaining : chars_per_line;
        // copy to tmp
        char tmp[65];
        for(int i=0;i<to_draw;i++) tmp[i]=w->tbox.buffer[start_idx+i];
        tmp[to_draw]=0;
        int y = ty0 + line * line_height;
        gfx_draw_string(tx0, y, tmp, textcol);
    }
    // blinking cursor at end of text - calculate wrapped position
    if(w->tbox.focused && w->tbox.cursor_visible){
        int cursor_idx = len; // after last char
        int cursor_line = cursor_idx / chars_per_line;
        int cursor_col = cursor_idx % chars_per_line;
        // adjust for vertical scroll
        int cursor_visible_line = cursor_line - first_line;
        if(cursor_visible_line >= 0 && cursor_visible_line < lines_visible){
            int cx = tx0 + cursor_col * 8;
            int cy = ty0 + cursor_visible_line * line_height;
            if(cx + 1 < ax + w->tbox.w - 1 && cy + 8 < ay + w->tbox.h - 1){
                for(int dy=0; dy<8; dy++) fb_put_pixel(cx, cy+dy, 0x00000000);
                fb_put_pixel(cx+1, cy, 0x00000000);
                fb_put_pixel(cx+1, cy+7, 0x00000000);
            }
        }
    }
}

static void window_draw_minimize_button(struct window *w){
    // small "_" button at top-right of title bar, 16x16, 4px from right edge, 2px from top
    int bx = w->x + w->w - MINIMIZE_BTN_W - 4;
    int by = w->y + 2;
    fb_draw_rect(bx, by, MINIMIZE_BTN_W, MINIMIZE_BTN_H, 0x00CCCCCC);
    gfx_draw_rect_outline(bx, by, MINIMIZE_BTN_W, MINIMIZE_BTN_H, 0x00000000);
    // draw "_" centered
    int tx = bx + (MINIMIZE_BTN_W - 8)/2;
    int ty = by + (MINIMIZE_BTN_H - 8)/2 + 2;
    gfx_draw_string(tx, ty, "_", 0x00000000);
}

static void window_draw_close_button(struct window *w){
    // small "X" button at top-right of title bar, left of minimize (w-40,2), 16x16
    int bx = w->x + w->w - CLOSE_BTN_W - 4 - MINIMIZE_BTN_W - 4; // 4px gap between close and minimize
    int by = w->y + 2;
    fb_draw_rect(bx, by, CLOSE_BTN_W, CLOSE_BTN_H, 0x00FF6666); // reddish for close
    gfx_draw_rect_outline(bx, by, CLOSE_BTN_W, CLOSE_BTN_H, 0x00000000);
    int tx = bx + (CLOSE_BTN_W - 8)/2;
    int ty = by + (CLOSE_BTN_H - 8)/2;
    gfx_draw_string(tx, ty, "X", 0x00000000);
}

static void window_draw_single(int idx){
    struct window *w = &windows[idx];
    if(!w->visible || w->minimized) return;
    fb_draw_rect(w->x, w->y, w->w, w->h, w->bg_color);
    fb_draw_rect(w->x, w->y, w->w, TITLE_BAR_H, w->title_color);
    gfx_draw_rect_outline(w->x, w->y, w->w, w->h, w->border_color);
    gfx_draw_rect_outline(w->x, w->y, w->w, TITLE_BAR_H, w->border_color);
    gfx_draw_string(w->x+6, w->y+6, w->title, 0x00FFFFFF);
    window_draw_minimize_button(w);
    window_draw_close_button(w);
    window_draw_button(w);
    window_draw_textbox(w);
    if(w->title[0]=='T' && w->title[1]=='a' && w->title[5]=='M'){
        extern void pit_get_task_ticks(int *gui, int *a, int *b);
        extern void pit_get_cpu_percent(int *gui_pct, int *a_pct, int *b_pct);
        extern int task_exists(int id);
        int gui=0,a=0,b=0; int gui_pct=0,a_pct=0,b_pct=0;
        pit_get_task_ticks(&gui,&a,&b);
        pit_get_cpu_percent(&gui_pct,&a_pct,&b_pct);
        // Clicker is task id 1, Notes is task id 2. Killed tasks are not shown.
        int show_clicker = task_exists(1);
        int show_notes = task_exists(2);
        char line1[48]; char line2[48]; char line3[48];
        {
            const char *pfx="GUI: "; int p=0; while(pfx[p]){ line1[p]=pfx[p]; p++; }
            char tmp[16]; int t=0; int n=gui; if(n==0) tmp[t++]='0'; else { char rev[12]; int r=0; while(n){ rev[r++]='0'+n%10; n/=10; } while(r--) tmp[t++]=rev[r]; }
            tmp[t++]=' '; tmp[t++]='('; char pct[4]; int pt=0; int pn=gui_pct; if(pn==0) pct[pt++]='0'; else { char r2[4]; int rr=0; while(pn){ r2[rr++]='0'+pn%10; pn/=10; } while(rr--) pct[pt++]=r2[rr]; } for(int i=0;i<pt && t<15;i++) tmp[t++]=pct[i]; tmp[t++]= '%'; tmp[t++]=')';
            for(int i=0;i<t && p<47;i++) line1[p++]=tmp[i]; line1[p]=0;
        }
        if(show_clicker){
            const char *pfx="Clicker: "; int p=0; while(pfx[p]){ line2[p]=pfx[p]; p++; }
            char tmp[16]; int t=0; int n=a; if(n==0) tmp[t++]='0'; else { char rev[12]; int r=0; while(n){ rev[r++]='0'+n%10; n/=10; } while(r--) tmp[t++]=rev[r]; }
            tmp[t++]=' '; tmp[t++]='('; char pct[4]; int pt=0; int pn=a_pct; if(pn==0) pct[pt++]='0'; else { char r2[4]; int rr=0; while(pn){ r2[rr++]='0'+pn%10; pn/=10; } while(rr--) pct[pt++]=r2[rr]; } for(int i=0;i<pt && t<15;i++) tmp[t++]=pct[i]; tmp[t++]= '%'; tmp[t++]=')';
            for(int i=0;i<t && p<47;i++) line2[p++]=tmp[i]; line2[p]=0;
        } else {
            line2[0]=0; // Clicker task killed
        }
        if(show_notes){
            const char *pfx="Notes: "; int p=0; while(pfx[p]){ line3[p]=pfx[p]; p++; }
            char tmp[16]; int t=0; int n=b; if(n==0) tmp[t++]='0'; else { char rev[12]; int r=0; while(n){ rev[r++]='0'+n%10; n/=10; } while(r--) tmp[t++]=rev[r]; }
            tmp[t++]=' '; tmp[t++]='('; char pct[4]; int pt=0; int pn=b_pct; if(pn==0) pct[pt++]='0'; else { char r2[4]; int rr=0; while(pn){ r2[rr++]='0'+pn%10; pn/=10; } while(rr--) pct[pt++]=r2[rr]; } for(int i=0;i<pt && t<15;i++) tmp[t++]=pct[i]; tmp[t++]= '%'; tmp[t++]=')';
            for(int i=0;i<t && p<47;i++) line3[p++]=tmp[i]; line3[p]=0;
        } else {
            line3[0]=0;
        }
        int tx = w->x + 10;
        int ty = w->y + 30;
        gfx_draw_string(tx, ty, line1, 0x00000000);
        if(show_clicker) gfx_draw_string(tx, ty+12, line2, 0x00000000);
        if(show_notes) gfx_draw_string(tx, ty+24, line3, 0x00000000);
        // Kill buttons for Clicker/Notes only - GUI has no button (structurally no handler for it)
        // Trace: Kill at window-relative (200,42) size 40x12 for Clicker, (200,54) for Notes
        for(int tid=1; tid<=2; tid++){
            if(!task_exists(tid)) continue; // killed task no longer shown, no button
            int kx = w->x + 200; int ky = w->y + (tid==1?42:54); int kw=40, kh=12;
            fb_draw_rect(kx, ky, kw, kh, 0x00FF4444); // red Kill
            gfx_draw_rect_outline(kx, ky, kw, kh, 0x00000000);
            gfx_draw_string(kx+8, ky+2, "Kill", 0x00FFFFFF);
        }
    }
    // Task-driven visible counter inside Clicker and Notes (deferred redraw: task updates window->task_counter + g_needs_redraw, GUI draws)
    // Race: Clicker/Notes task (PIT preempted) writes single-word volatile int task_counter, GUI reads it in window_draw_single.
    // On x86 aligned 32-bit writes are atomic (no torn read), and we use same pattern as g_needs_redraw (single int flag) which is safe for this demo.
    // If we stored a multi-word struct or string, it would need a lock; for a counter it's safe.
    if(icon_streq(w->title, "Clicker")){
        char buf[32]; const char *pfx="Count: "; int p=0; while(pfx[p]){ buf[p]=pfx[p]; p++; }
        char tmp[12]; int t=0; int n=w->task_counter; if(n==0) tmp[t++]='0'; else { char rev[12]; int r=0; while(n){ rev[r++]='0'+n%10; n/=10; } while(r--) tmp[t++]=rev[r]; }
        for(int i=0;i<t && p<31;i++) buf[p++]=tmp[i]; buf[p]=0;
        // Draw in bottom-left of Clicker body, e.g., at window-relative 20, h-30
        int cx = w->x + 20; int cy = w->y + w->h - 30;
        // Clear background for counter
        fb_draw_rect(cx-2, cy-2, 120, 12, w->bg_color);
        gfx_draw_string(cx, cy, buf, 0x00000000);
    } else if(icon_streq(w->title, "Notes")){
        char buf[32]; const char *pfx="Count: "; int p=0; while(pfx[p]){ buf[p]=pfx[p]; p++; }
        char tmp[12]; int t=0; int n=w->task_counter; if(n==0) tmp[t++]='0'; else { char rev[12]; int r=0; while(n){ rev[r++]='0'+n%10; n/=10; } while(r--) tmp[t++]=rev[r]; }
        for(int i=0;i<t && p<31;i++) buf[p++]=tmp[i]; buf[p]=0;
        int cx = w->x + 20; int cy = w->y + 110; // below textbox (textbox at 20,40 360x60, so 110 is just below)
        fb_draw_rect(cx-2, cy-2, 120, 12, w->bg_color);
        gfx_draw_string(cx, cy, buf, 0x00000000);
    }
    {
        int rx = w->x + w->w - RESIZE_HANDLE;
        int ry = w->y + w->h - RESIZE_HANDLE;
        fb_draw_rect(rx, ry, RESIZE_HANDLE, RESIZE_HANDLE, 0x00888888);
        gfx_draw_rect_outline(rx, ry, RESIZE_HANDLE, RESIZE_HANDLE, 0x00000000);
        gfx_draw_line(rx+4, ry+8, rx+8, ry+4, 0x00000000);
        gfx_draw_line(rx+6, ry+8, rx+8, ry+6, 0x00000000);
    }
}

void taskbar_draw(void){
    if(!fb_is_available()) return;
    int fb_h = fb_get_height();
    int fb_w = fb_get_width();
    int ty = fb_h - TASKBAR_H;
    // taskbar background - dark gray
    fb_draw_rect(0, ty, fb_w, TASKBAR_H, 0x00222222);
    gfx_draw_rect_outline(0, ty, fb_w, TASKBAR_H, 0x00000000);
    // tabs for each window
    for(int i=0;i<window_count;i++){
        int tab_x = 5 + i * (150 + 5);
        int tab_y = ty + 3;
        int tab_w = 150;
        int tab_h = TASKBAR_H - 6;
        // color: active front window is lighter, minimized is darker, others medium
        int is_front = (z_order[window_count-1] == i);
        int is_min = windows[i].minimized;
        uint32_t bg;
        if(is_min) bg = 0x00555555; // minimized dark
        else if(is_front) bg = 0x00AAAAFF; // front light blue
        else bg = 0x00888888; // normal gray
        fb_draw_rect(tab_x, tab_y, tab_w, tab_h, bg);
        gfx_draw_rect_outline(tab_x, tab_y, tab_w, tab_h, 0x00000000);
        gfx_draw_string(tab_x+6, tab_y+6, windows[i].title, 0x00FFFFFF);
        // small indicator for minimized: draw "_" at right of tab
        if(is_min){
            gfx_draw_string(tab_x+tab_w-20, tab_y+6, "_", 0x00FFFFFF);
        }
    }
    // "+" button at far right for creating new windows
    {
        int plus_w = 30, plus_h = TASKBAR_H - 6;
        int plus_x = fb_w - plus_w - 5;
        int plus_y = ty + 3;
        int at_max = (window_count >= MAX_WINDOWS);
        uint32_t bg = at_max ? 0x00555555 : 0x00AAAAAA; // gray when disabled, lighter when enabled
        uint32_t border = 0x00000000;
        fb_draw_rect(plus_x, plus_y, plus_w, plus_h, bg);
        gfx_draw_rect_outline(plus_x, plus_y, plus_w, plus_h, border);
        // draw "+" centered
        gfx_draw_string(plus_x+11, plus_y+6, "+", 0x00000000);
        if(at_max){
            // also draw small "X" or dim to indicate disabled
            gfx_draw_string(plus_x+11, plus_y+6, "+", 0x00888888);
        }
    }
}

// --- Desktop icons (Bliss wallpaper layer, under windows) ---
struct desktop_icon desktop_icons[MAX_ICONS];
int desktop_icon_count = 0;
static int selected_icon = -1;
static int last_click_icon = -1;
static int last_click_tick = -1000;
static const int dbl_click_thresh = 50;
void desktop_icons_init(void){
    desktop_icon_count = 0; selected_icon = -1; last_click_icon = -1; last_click_tick = -1000;
    desktop_icons[0].x = 20; desktop_icons[0].y = 40; w_strcpy(desktop_icons[0].label, "New Window", 32); desktop_icons[0].color = 0x004A90D9; desktop_icons[0].selected = 0;
    desktop_icons[1].x = 20; desktop_icons[1].y = 140; w_strcpy(desktop_icons[1].label, "Task Manager", 32); desktop_icons[1].color = 0x0030A030; desktop_icons[1].selected = 0;
    desktop_icons[2].x = 20; desktop_icons[2].y = 240; w_strcpy(desktop_icons[2].label, "Clicker", 32); desktop_icons[2].color = 0x00336699; desktop_icons[2].selected = 0;
    desktop_icons[3].x = 20; desktop_icons[3].y = 340; w_strcpy(desktop_icons[3].label, "Notes", 32); desktop_icons[3].color = 0x00993333; desktop_icons[3].selected = 0;
    desktop_icon_count = 4;
    s_puts("DESKTOP: icons init 4 at (20,40) New Window, (20,140) Task Manager, (20,240) Clicker, (20,340) Notes\n");
}
// Notes icon: white notepad sheet with gray rules, teal top bar, silver spiral
// binding, navy fountain pen, and a solid offset drop shadow. Painted with
// solid primitives only (no alpha blending available); rounded paper corners
// are faked by narrowing the top/bottom rows since the wallpaper behind varies.
static void draw_notes_icon(int gx, int gy){
    uint32_t paper = 0x00F9F9FB;   // off-white sheet
    uint32_t rule = 0x00E2E8F0;    // faint blue-gray rules
    uint32_t teal = 0x000EA5E9;    // top accent bar
    uint32_t silver = 0x00C0C0C0;  // spiral rings + pen nib
    uint32_t navy = 0x001E3A8A;    // pen barrel
    uint32_t shadow = 0x002F3B4C;  // drop shadow
    int px = gx + 6, py = gy + 3;  // 20x26 paper centered in the 32x32 canvas
    // drop shadow behind everything (offset down-right, stays inside canvas)
    fb_draw_rect(px+2, py+2, 20, 26, shadow);
    // paper with rounded corners (narrowed top/bottom rows)
    fb_draw_rect(px+2, py, 16, 1, paper);
    fb_draw_rect(px+1, py+1, 18, 1, paper);
    fb_draw_rect(px, py+2, 20, 22, paper);
    fb_draw_rect(px+1, py+24, 18, 1, paper);
    fb_draw_rect(px+2, py+25, 16, 1, paper);
    // teal accent bar across the top
    fb_draw_rect(px, py+3, 20, 2, teal);
    // horizontal rules
    for(int y = py+8; y <= py+22; y += 4) fb_draw_rect(px+3, y, 14, 1, rule);
    // spiral binding: 4 silver rings straddling the top edge
    for(int k=0;k<4;k++) gfx_draw_circle(px+3+k*5, py+1, 2, silver);
    // pen barrel: 45-degree navy shaft (3 parallel lines for thickness)
    gfx_draw_line(gx+4, gy+27, gx+22, gy+9, navy);
    gfx_draw_line(gx+5, gy+27, gx+23, gy+9, navy);
    gfx_draw_line(gx+4, gy+28, gx+22, gy+10, navy);
    // pen nib: silver V converging to the tip
    gfx_draw_line(gx+22, gy+9, gx+25, gy+6, silver);
    gfx_draw_line(gx+23, gy+11, gx+25, gy+6, silver);
    fb_draw_rect(gx+25, gy+6, 1, 1, silver);
}
// Clicker icon: dark obsidian mouse with glowing cyan left button and click
// ripples radiating from its top-left, plus a solid offset drop shadow.
// Painted with solid primitives only (no alpha); ripples are circle outlines
// drawn first so the body covers their inner parts, leaving outer arcs visible.
static void draw_clicker_icon(int gx, int gy){
    uint32_t body = 0x001E293B;    // matte charcoal
    uint32_t edge = 0x00000000;    // outline
    uint32_t cyan = 0x0006B6D4;    // glowing left button
    uint32_t cyan_hi = 0x0067E8F9; // button highlight
    uint32_t silver = 0x00C0C0C0;  // wheel + divider accents
    uint32_t dark = 0x000F172A;    // divider + deep edge
    uint32_t light = 0x00334155;   // top bevel highlight
    uint32_t shadow = 0x00141824;  // drop shadow
    int bx = gx + 7, by = gy + 4;  // 18x26 body bbox inside the 32x32 canvas
    int cx = bx + 9;               // horizontal center
    // click ripples behind everything (fading cyan arcs, inner parts get covered)
    gfx_draw_circle(bx, by+2, 13, cyan);
    gfx_draw_circle(bx, by+2, 17, 0x000891B2);
    gfx_draw_circle(bx, by+2, 21, 0x000E7490);
    // drop shadow silhouette (offset down-right)
    gfx_draw_filled_circle(cx+2, by+10, 9, shadow);
    fb_draw_rect(bx+2, by+10, 18, 8, shadow);
    gfx_draw_filled_circle(cx+2, by+18, 9, shadow);
    // charcoal body capsule (two domes + middle band)
    gfx_draw_filled_circle(cx, by+8, 9, body);
    fb_draw_rect(bx, by+8, 18, 8, body);
    gfx_draw_filled_circle(cx, by+16, 9, body);
    // glowing cyan left button (rect + dome patch on the left/top)
    fb_draw_rect(bx+1, by+4, 7, 10, cyan);
    gfx_draw_filled_circle(bx+4, by+5, 4, cyan);
    fb_draw_rect(bx+2, by+5, 1, 8, cyan_hi); // inner glow strip
    // center divider + scroll wheel on top of it
    gfx_draw_line(cx, by+1, cx, by+14, dark);
    fb_draw_rect(cx-1, by+9, 3, 5, silver);
    gfx_draw_rect_outline(cx-1, by+9, 3, 5, dark);
    // dark capsule outline + light top bevel
    gfx_draw_circle(cx, by+8, 9, edge);
    gfx_draw_rect_outline(bx, by+8, 18, 8, edge);
    gfx_draw_circle(cx, by+16, 9, edge);
    gfx_draw_line(cx-4, by, cx+4, by, light);
}
void desktop_icons_draw(void){
    if(!fb_is_available()) return;
    for(int i=0;i<desktop_icon_count;i++){
        struct desktop_icon *ic = &desktop_icons[i];
        int ix = ic->x; int iy = ic->y;
        int gx = ix + (ICON_W - ICON_GLYPH)/2; int gy = iy + 4;
        if(i==0){ fb_draw_rect(gx, gy, ICON_GLYPH, ICON_GLYPH, ic->color); gfx_draw_rect_outline(gx, gy, ICON_GLYPH, ICON_GLYPH, 0x00000000); gfx_draw_string(gx+12, gy+12, "+", 0x00FFFFFF); }
        else if(i==1){ gfx_draw_filled_circle(gx + ICON_GLYPH/2, gy + ICON_GLYPH/2, ICON_GLYPH/2, ic->color); gfx_draw_circle(gx + ICON_GLYPH/2, gy + ICON_GLYPH/2, ICON_GLYPH/2, 0x00000000); }
        else if(i==2){ draw_clicker_icon(gx, gy); }
        else { draw_notes_icon(gx, gy); }
        int len=0; while(ic->label[len] && len<32) len++;
        int tx = ix + (ICON_W - len*8)/2; int ty = iy + 4 + ICON_GLYPH + 6;
        if(ic->selected){ int bg_w = len*8 + 6; int bg_h = 10; int bg_x = tx - 3; int bg_y = ty - 1; fb_draw_rect(bg_x, bg_y, bg_w, bg_h, 0x000000FF); gfx_draw_string(tx, ty, ic->label, 0x00FFFFFF); }
        else { gfx_draw_string(tx+1, ty+1, ic->label, 0x00444444); gfx_draw_string(tx, ty, ic->label, 0x00FFFFFF); }
    }
}
int desktop_icon_hit_test(int x, int y){
    for(int i=0;i<desktop_icon_count;i++){ struct desktop_icon *ic = &desktop_icons[i]; if(x >= ic->x && x < ic->x + ICON_W && y >= ic->y && y < ic->y + ICON_H) return i; }
    return -1;
}
void desktop_icon_deselect_all(void){
    int had=0; for(int i=0;i<desktop_icon_count;i++) if(desktop_icons[i].selected) had=1;
    for(int i=0;i<desktop_icon_count;i++) desktop_icons[i].selected=0; selected_icon=-1;
    if(had){ s_puts("DESKTOP: deselect all\n"); g_needs_redraw=1; }
}
static int icon_streq(const char *a, const char *b){ int i=0; while(a[i] && b[i] && a[i]==b[i]) i++; return a[i]==0 && b[i]==0; }
int desktop_icon_handle_click(int x, int y){
    int idx = desktop_icon_hit_test(x,y); if(idx==-1) return 0;
    int now = pit_get_ticks(); s_puts("DESKTOP: click icon "); s_put_dec(idx); s_puts(" '"); s_puts(desktop_icons[idx].label); s_puts("' tick "); s_put_dec(now); s_puts("\n");
    if(last_click_icon==idx && (now - last_click_tick) < dbl_click_thresh){
        s_puts("DESKTOP: double-click icon "); s_put_dec(idx); s_puts("\n");
        for(int i=0;i<desktop_icon_count;i++) desktop_icons[i].selected = (i==idx); selected_icon = idx; last_click_icon = -1; last_click_tick = -1000;
        if(idx==0){ s_puts("DESKTOP: action New Window\n"); window_create_new(); }
        else if(idx==1){ int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Task Manager")) { found=i; break; } if(found!=-1){ s_puts("DESKTOP: action Task Manager bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; s_puts("DESKTOP: unminimize Task Manager\n"); } window_bring_to_front(found); } else { s_puts("DESKTOP: action Task Manager create (was closed)\n"); if(window_count < MAX_WINDOWS){ int nid = window_count; windows[nid].x=600; windows[nid].y=100; windows[nid].w=300; windows[nid].h=200; w_strcpy(windows[nid].title, "Task Manager", 32); windows[nid].bg_color=0x00F0F0F0; windows[nid].title_color=0x00333333; windows[nid].border_color=0x00000000; windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=0; windows[nid].has_textbox=0; windows[nid].task_counter=0; z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i; s_puts("DESKTOP: created Task Manager\n"); g_needs_redraw=1; } else s_puts("DESKTOP: cannot create Task Manager - at max\n"); } }
        else if(idx==2){ // Clicker
            int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Clicker")) { found=i; break; }
            if(found!=-1){ s_puts("DESKTOP: action Clicker bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; } window_bring_to_front(found); }
            else {
                s_puts("DESKTOP: action Clicker create (was closed)\n");
                // Recreate window + task pair fresh - check if Clicker task exists, if not, recreate
                // Clicker task permanently owns id 1 so UI tid mapping, tick slots and stack vbase stay stable
                extern void task_clicker_entry(void);
                extern int task_create_with_id(int want_id, void (*)(void), const char *);
                extern int task_find_by_name(const char *name);
                if(task_find_by_name("Clicker")== -1){
                    s_puts("DESKTOP: recreating Clicker task\n");
                    task_create_with_id(1, task_clicker_entry, "Clicker");
                }
                if(window_count < MAX_WINDOWS){
                    int nid = window_count;
                    windows[nid].x=100; windows[nid].y=100; windows[nid].w=400; windows[nid].h=300;
                    w_strcpy(windows[nid].title, "Clicker", 32);
                    windows[nid].bg_color=0x00E0E0E0; windows[nid].title_color=0x00336699; windows[nid].border_color=0x00000000;
                    windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=1;
                    windows[nid].btn.x=20; windows[nid].btn.y=40; windows[nid].btn.w=120; windows[nid].btn.h=30; w_strcpy(windows[nid].btn.label, "Click Me", 32); windows[nid].btn.pressed=0; windows[nid].btn.clicks=0;
                    windows[nid].has_textbox=0; windows[nid].task_counter=0;
                    z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
                    s_puts("DESKTOP: created Clicker\n"); g_needs_redraw=1;
                } else s_puts("DESKTOP: cannot create Clicker - at max\n");
            }
        }
        else if(idx==3){ // Notes
            int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Notes")) { found=i; break; }
            if(found!=-1){ s_puts("DESKTOP: action Notes bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; } window_bring_to_front(found); }
            else {
                s_puts("DESKTOP: action Notes create (was closed)\n");
                extern void task_notes_entry(void);
                extern int task_create_with_id(int want_id, void (*)(void), const char *);
                extern int task_find_by_name(const char *name);
                if(task_find_by_name("Notes")== -1){
                    s_puts("DESKTOP: recreating Notes task\n");
                    task_create_with_id(2, task_notes_entry, "Notes");
                }
                if(window_count < MAX_WINDOWS){
                    int nid = window_count;
                    windows[nid].x=250; windows[nid].y=180; windows[nid].w=400; windows[nid].h=300;
                    w_strcpy(windows[nid].title, "Notes", 32);
                    windows[nid].bg_color=0x00D0D0FF; windows[nid].title_color=0x00993333; windows[nid].border_color=0x00000000;
                    windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=0; windows[nid].has_textbox=1;
                    windows[nid].tbox.x=20; windows[nid].tbox.y=40; windows[nid].tbox.w=360; windows[nid].tbox.h=60; windows[nid].tbox.max_len=512; windows[nid].tbox.len=0; windows[nid].tbox.buffer[0]=0; windows[nid].tbox.focused=0; windows[nid].tbox.cursor_visible=1; windows[nid].tbox.blink_counter=0;
                    windows[nid].task_counter=0;
                    z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
                    s_puts("DESKTOP: created Notes\n"); g_needs_redraw=1;
                } else s_puts("DESKTOP: cannot create Notes - at max\n");
            }
        }
        g_needs_redraw=1; return 1;
    } else {
        for(int i=0;i<desktop_icon_count;i++) desktop_icons[i].selected = (i==idx); selected_icon = idx; last_click_icon = idx; last_click_tick = now; s_puts("DESKTOP: select icon "); s_put_dec(idx); s_puts("\n"); g_needs_redraw=1; return 1;
    }
}

void window_manager_init(void){
    if(!fb_is_available()){
        s_puts("WM: no framebuffer, skip windows\n");
        return;
    }
    // Defense: explicitly reset BSS-dependent drag/button state even though boot.s now zeroes BSS
    // Without BSS zeroing, these would be garbage and first window_bring_to_front could read stale z_order or dragging flag
    dragging = 0; drag_win = -1; drag_off_x = 0; drag_off_y = 0;
    // Create 2 overlapping windows - positions chosen to show overlap
    // Window 0 - back (Clicker)
    windows[0].x = 100; windows[0].y = 100; windows[0].w = 400; windows[0].h = 300;
    w_strcpy(windows[0].title, "Clicker", 32);
    windows[0].bg_color = 0x00E0E0E0; // light gray
    windows[0].title_color = 0x00336699; // steel blue
    windows[0].border_color = 0x00000000;
    windows[0].visible = 1;
    windows[0].z = 0;
    windows[0].has_button = 1;
    windows[0].btn.x = 20; windows[0].btn.y = 40; windows[0].btn.w = 120; windows[0].btn.h = 30;
    w_strcpy(windows[0].btn.label, "Click Me", 32);
    windows[0].btn.pressed = 0;
    windows[0].btn.clicks = 0;

    windows[1].x = 250; windows[1].y = 180; windows[1].w = 400; windows[1].h = 300;
    w_strcpy(windows[1].title, "Notes", 32);
    windows[1].bg_color = 0x00D0D0FF; // light blue
    windows[1].title_color = 0x00993333; // reddish
    windows[1].border_color = 0x00000000;
    windows[1].visible = 1;
    windows[1].z = 1;
    windows[1].has_button = 0;
    windows[1].has_textbox = 1;
    windows[1].tbox.x = 20; windows[1].tbox.y = 40; windows[1].tbox.w = 360; windows[1].tbox.h = 60; // taller to show 5 lines (was 30 for 2 lines)
    windows[1].tbox.max_len = 512;
    windows[1].tbox.len = 0;
    windows[1].tbox.buffer[0]=0;
    windows[1].tbox.focused = 0;
    windows[1].tbox.cursor_visible = 1;
    windows[1].tbox.blink_counter = 0;
    windows[0].task_counter = 0;
    windows[1].task_counter = 0;

    windows[2].x = 0; windows[2].y = 0; windows[2].w = 0; windows[2].h = 0;
    windows[2].visible = 0; windows[2].minimized = 0; windows[2].task_counter = 0;
    windows[3].x = 0; windows[3].y = 0; windows[3].w = 0; windows[3].h = 0;
    windows[3].visible = 0; windows[3].minimized = 0; windows[3].task_counter = 0;

    window_count = 2;
    // z_order 0..1 back->front corresponds to windows index order initially
    for(int i=0;i<window_count;i++) z_order[i]=i;

    desktop_icons_init();
    s_puts("WM: created 2 windows (Clicker button, Notes textbox) + 4 desktop icons\n");
    // Build wallpaper cache once (draws Bliss then snapshots, measures flat vs Bliss vs blit)
    wallpaper_cache_build_once();
    window_manager_draw_all();
    {
        int w = fb_get_width(); int h = fb_get_height(); int sky_h = h*60/100;
        int black=0, magenta=0; for(int x=0;x<w;x++) { uint32_t p=fb_get_pixel(x,sky_h); if(p==0x000000U) black++; if(p==0x00FF00FFU) magenta++; }
        s_puts("HORIZON FINAL y="); s_put_dec(sky_h); s_puts(" black="); s_put_dec(black); s_puts(" magenta="); s_put_dec(magenta); s_puts("/"); s_put_dec(w);
        s_puts(magenta==w ? " MAGENTA ACTIVE\n" : black==w ? " FULL BLACK LINE!\n" : " no full divider\n");
        // Full scan for ANY full-width black line (fresh look)
        int found_y=-1;
        for(int y=0;y<h;y++){
            int cnt=0;
            for(int x=0;x<w;x++) if(fb_get_pixel(x,y)==0x000000U) cnt++;
            if(cnt==w){ found_y=y; break; }
            if(cnt>w*9/10 && cnt < w){ /* near-full */ }
        }
        if(found_y!=-1){
            s_puts("SCAN found full black line at y="); s_put_dec(found_y); s_puts("\n");
        } else {
            s_puts("SCAN no full black line found (checked all 768 rows)\n");
            // also check front buffer
            int ffound=-1;
            for(int y=0;y<h;y++){
                int cnt=0;
                for(int x=0;x<w;x++) if(fb_get_front_pixel(x,y)==0x000000U) cnt++;
                if(cnt==w){ ffound=y; break; }
            }
            if(ffound!=-1) { s_puts("SCAN FRONT found full black at y="); s_put_dec(ffound); s_puts("\n"); }
            else s_puts("SCAN FRONT also no full black line\n");
        }
    }
    if(fb_is_double_buffered()) fb_swap(); // show windows without cursor yet, mouse will add cursor and swap again
    // Fresh look checks: test pattern overwrite, front vs back, QEMU artifact
    {
        int w = fb_get_width(); int h = fb_get_height(); int sky_h = h*60/100;
        // 1. Test pattern should be overwritten: y=30 green line 0x00FF00 should now be sky, and magenta border at 0,0 should be sky
        uint32_t p30 = fb_get_pixel(512,30); // back buffer after draw (now front after swap, but fb_get_pixel reads back, so check front for actual display)
        uint32_t p30_front = fb_get_front_pixel(512,30);
        uint32_t p00_front = fb_get_front_pixel(0,0);
        uint32_t p00_back = fb_get_pixel(0,0);
        s_puts("CHECK1 test pattern overwrite: y=30 back="); s_put_hex32(p30); s_puts(" front="); s_put_hex32(p30_front);
        s_puts(" (expect sky ~B0E0E6 not 00FF00) "); s_puts(p30==0x0000FF00U ? "STALE!\n" : "overwritten OK\n");
        s_puts(" border 0,0 front="); s_put_hex32(p00_front); s_puts(" back="); s_put_hex32(p00_back); s_puts("\n");
        // 2. Front vs back at horizon - swap must copy every row including y=460
        int mism=0;
        for(int x=0;x<w;x++) if(fb_get_pixel(x,sky_h) != fb_get_front_pixel(x,sky_h)) mism++;
        s_puts("CHECK2 front vs back at y="); s_put_dec(sky_h); s_puts(" mism="); s_put_dec(mism); s_puts("/"); s_put_dec(w);
        s_puts(mism==0 ? " swap full OK\n" : " SWAP INCOMPLETE! stale front row visible\n");
        // Also check cache vs back at horizon (stale cache?)
        if(wallpaper_cache){
            uint32_t c0 = wallpaper_cache[sky_h * w + 0];
            uint32_t b0 = fb_get_pixel(0,sky_h);
            uint32_t c1023 = wallpaper_cache[sky_h * w + 1023];
            uint32_t b1023 = fb_get_pixel(1023,sky_h);
            s_puts("CHECK3 cache vs back at horizon: x0 cache="); s_put_hex32(c0); s_puts(" back="); s_put_hex32(b0);
            s_puts(" x1023 cache="); s_put_hex32(c1023); s_puts(" back="); s_put_hex32(b1023);
            s_puts((c0==b0 && c1023==b1023) ? " cache fresh OK\n" : " STALE CACHE!\n");
        }
        s_puts("CHECK QEMU artifact: width="); s_put_dec(w); s_puts(" height="); s_put_dec(h); s_puts(" pitch="); s_put_dec(fb_get_size_bytes()/h); s_puts(" sky_h="); s_put_dec(sky_h); s_puts("\n");
        // Dump horizon region 455-465 as PPM hex to verify visually - 11 rows
        // Horizon dump removed for clean boot - was 11 rows PPM for debugging seam, now verified clean
    }
}

int window_create_new(void){
    if(window_count >= MAX_WINDOWS){
        s_puts("WM: create window failed - at max "); s_put_dec(MAX_WINDOWS); s_puts("\n");
        return -1;
    }
    int idx = window_count;
    int base_x = 100 + (window_count * 30) % 300;
    int base_y = 100 + (window_count * 30) % 200;
    int w = 350, h = 250;
    int fb_w = fb_get_width();
    int fb_h = fb_get_height();
    if(base_x + w > fb_w - 60) base_x = fb_w - w - 60;
    if(base_y + h > fb_h - TASKBAR_H - 20) base_y = fb_h - TASKBAR_H - h - 20;
    if(base_x < 0) base_x = 0;
    if(base_y < 0) base_y = 0;
    windows[idx].x = base_x;
    windows[idx].y = base_y;
    windows[idx].w = w;
    windows[idx].h = h;
    {
        char buf[32]; const char *pf="Window "; int pp=0; while(pf[pp]){ buf[pp]=pf[pp]; pp++; }
        char nb[8]; int nn=idx+1; int nl=0; char rev[8]; int r=0;
        if(nn==0) rev[r++]='0'; else while(nn){ rev[r++]='0'+nn%10; nn/=10; }
        while(r--) buf[pp++]=rev[r]; buf[pp]=0;
        w_strcpy(windows[idx].title, buf, 32);
    }
    windows[idx].bg_color = 0x00CCCCCC;
    windows[idx].title_color = 0x00444444;
    windows[idx].border_color = 0x00000000;
    windows[idx].visible = 1;
    windows[idx].minimized = 0;
    windows[idx].z = window_count;
    windows[idx].has_button = 0;
    windows[idx].has_textbox = 0;
    windows[idx].task_counter = 0;
    z_order[window_count] = idx;
    window_count++;
    for(int i=0;i<window_count;i++) windows[z_order[i]].z = i;
    s_puts("WM: created Window "); s_put_dec(idx+1); s_puts(" at "); s_put_dec(base_x); s_putc(','); s_put_dec(base_y);
    s_puts(" count now "); s_put_dec(window_count); s_puts("\n");
    g_needs_redraw = 1;
    return idx;
}


static void draw_desktop_gradient(void){
    // Kept for reference, now replaced by draw_bliss_wallpaper
    uint32_t top = 0x00112244;
    uint32_t bot = 0x002A4A6B;
    uint8_t top_r = (top>>16)&0xFF, top_g=(top>>8)&0xFF, top_b=top&0xFF;
    uint8_t bot_r = (bot>>16)&0xFF, bot_g=(bot>>8)&0xFF, bot_b=bot&0xFF;
    int h = fb_get_height();
    int w = fb_get_width();
    for(int y=0; y<h; y++){
        int ratio = (y * 255) / (h-1);
        uint8_t r = top_r + ((bot_r - top_r)*ratio)/255;
        uint8_t g = top_g + ((bot_g - top_g)*ratio)/255;
        uint8_t b = top_b + ((bot_b - top_b)*ratio)/255;
        uint32_t col = (r<<16)|(g<<8)|b;
        fb_draw_rect(0, y, w, 1, col);
    }
}

// --- Wallpaper cache helpers (static, PMM-backed) ---
static void s_put_hex32(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
// 64-bit cycles fit in 32-bit for <4e9 (~1.3s @3GHz); truncate to avoid libgcc __udivdi3 in freestanding
static void s_put_cycles(uint64_t n){ s_put_dec((uint32_t)n); }
static int wallpaper_cache_alloc(void){
    if(wallpaper_cache) return 1;
    if(!fb_is_available()) return 0;
    int w = fb_get_width(); int h = fb_get_height();
    uint32_t need = (uint32_t)w * (uint32_t)h * 4;
    wallpaper_cache_bytes = need;
    wallpaper_cache_pages = (need + 0xFFF) >> 12; // e.g., 768 for 1024x768 (3MB) vs 2025 for 1920x1080 (8.3MB)
    uint32_t vaddr = 0x00F00000; // after fb_back 0x00600000+8.3M=0x00DE9000, so 0x00F00000 (15M) safely after, still low for cache friendliness (was 0x02000000 at 32M, caused slow blit)
    s_puts("WALLPAPER: cache alloc "); s_put_dec(wallpaper_cache_pages); s_puts(" pages need "); s_put_dec(need/1024); s_puts(" KB at "); s_put_hex32(vaddr);
    s_puts(" PMM free before "); s_put_dec(pmm_free_frames()); s_puts("\n");
    for(uint32_t i=0;i<wallpaper_cache_pages;i++){
        uint32_t p = pmm_alloc_frame();
        if(!p){ s_puts("WALLPAPER: out of frames!\n"); return 0; }
        paging_map(vaddr + i*0x1000, p, 0x03);
        volatile uint32_t *ptr=(volatile uint32_t*)(vaddr + i*0x1000);
        for(int j=0;j<1024;j++) ptr[j]=0;
    }
    wallpaper_cache = (uint32_t*)vaddr;
    s_puts("WALLPAPER: cache ready at "); s_put_hex32(vaddr); s_puts(" PMM free after "); s_put_dec(pmm_free_frames()); s_puts("\n");
    return 1;
}
static inline void wallpaper_blit_cached(void){
    if(!wallpaper_cache_ready || !wallpaper_cache) return;
    fb_blit_from(wallpaper_cache); // rep movsl 786432 dwords, no per-pixel sine/isqrt
}

// Bliss wallpaper - sine lookup table, no libm (freestanding)
static const int8_t sin_table[256] = {
       0,    3,    6,    9,   12,   15,   18,   21,   23,   26,   29,   32,   35,   38,   40,   43,
      46,   49,   51,   54,   57,   59,   62,   64,   67,   69,   71,   74,   76,   78,   81,   83,
      85,   87,   89,   91,   93,   95,   96,   98,  100,  101,  103,  104,  106,  107,  108,  110,
     111,  112,  113,  114,  115,  116,  116,  117,  118,  118,  119,  119,  119,  120,  120,  120,
     120,  120,  120,  120,  119,  119,  119,  118,  118,  117,  116,  116,  115,  114,  113,  112,
     111,  110,  108,  107,  106,  104,  103,  101,  100,   98,   96,   95,   93,   91,   89,   87,
      85,   83,   81,   78,   76,   74,   71,   69,   67,   64,   62,   59,   57,   54,   51,   49,
      46,   43,   40,   38,   35,   32,   29,   26,   23,   21,   18,   15,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -15,  -18,  -21,  -23,  -26,  -29,  -32,  -35,  -38,  -40,  -43,
     -46,  -49,  -51,  -54,  -57,  -59,  -62,  -64,  -67,  -69,  -71,  -74,  -76,  -78,  -81,  -83,
     -85,  -87,  -89,  -91,  -93,  -95,  -96,  -98, -100, -101, -103, -104, -106, -107, -108, -110,
    -111, -112, -113, -114, -115, -116, -116, -117, -118, -118, -119, -119, -119, -120, -120, -120,
    -120, -120, -120, -120, -119, -119, -119, -118, -118, -117, -116, -116, -115, -114, -113, -112,
    -111, -110, -108, -107, -106, -104, -103, -101, -100,  -98,  -96,  -95,  -93,  -91,  -89,  -87,
     -85,  -83,  -81,  -78,  -76,  -74,  -71,  -69,  -67,  -64,  -62,  -59,  -57,  -54,  -51,  -49,
     -46,  -43,  -40,  -38,  -35,  -32,  -29,  -26,  -23,  -21,  -18,  -15,  -12,   -9,   -6,   -3,
};
static inline int sin_lookup(int angle){ // angle 0..255 -> 0..2pi
    return sin_table[angle & 0xFF];
}

static void draw_bliss_wallpaper(void){
    int w = fb_get_width();
    int h = fb_get_height();
    int sky_h = h * 60 / 100; // upper 60% sky
    int hill_base = sky_h; // hills start at 60% from top
    // Sky gradient - brighter blue top (0x0087CEEB sky blue) to pale near horizon (0x00B0E0E6 powder blue)
    // Chose vertical per-row lerp: sky_h rows * w cols (e.g., 648*1920=1.2M at 1080p, 460*1024=471k at 768p), sky_h lerps
    uint32_t sky_top = 0x0087CEEB;
    uint32_t sky_bot = 0x00B0E0E6;
    uint8_t top_r = (sky_top>>16)&0xFF, top_g=(sky_top>>8)&0xFF, top_b=sky_top&0xFF;
    uint8_t bot_r = (sky_bot>>16)&0xFF, bot_g=(sky_bot>>8)&0xFF, bot_b=sky_bot&0xFF;
    for(int y=0; y<sky_h; y++){
        int ratio = (y * 255) / (sky_h - 1);
        uint8_t r = top_r + ((bot_r - top_r)*ratio)/255;
        uint8_t g = top_g + ((bot_g - top_g)*ratio)/255;
        uint8_t b = top_b + ((bot_b - top_b)*ratio)/255;
        uint32_t col = (r<<16)|(g<<8)|b;
        fb_draw_rect(0, y, w, 1, col);
    }
    // Fill below horizon with sky bottom before hills - ensures no 35px black gap if hill_base+offset > sky_h
    // Without this, gap sky_h..far_y-1 remains untouched (black) when base>0
    fb_draw_rect(0, sky_h, w, h - sky_h, 0x00B0E0E6); // sky_bot solid under hills
    // Hills will overwrite this sky_bot area from far_y/near_y down, leaving only visible hill silhouette
    // Sun - filled pale yellow/white at upper right, dynamic for width (75% + 120y). At 1024x768 was 800,120; at 1920x1080 ~1696,120
    int sun_x = w - 224; if(sun_x < 0) sun_x = w*3/4;
    int sun_y = 120;
    gfx_draw_filled_circle(sun_x, sun_y, 40, 0x00FFFFE0); // light yellow
    gfx_draw_filled_circle(sun_x, sun_y, 35, 0x00FFFFFF); // white center for highlight

    // Hills - two gentle layers for depth, low frequency for wide rolling (Bliss has 2-3 broad curves, not sawtooth)
    // Trace addresses: hill_base = sky_h = h*60/100, sky GRAD filled y=0..sky_h-1, hills fill y=far_y..h-1
    // Previously period 128 (8 hills) and 85 (12 hills) => sawtooth. Now period scaled with width to keep visual: 600/400 at 1024 => 1.71/2.56 hills
    // At 1920, periods 1125/750 keep same 1.71/2.56 visual (1920/1125≈1.71). Phase = x*256/period &0xFF, hill_y = sky_h + baseOff + amp*sin/128
    // Address trace: fb at 0xFD000000, back 0x00600000, cache 0x00F00000, per-column 1x(h-y) writes w*~h*0.4 avg pixels, no overlap gap
    uint32_t far_green = 0x0090C060; // lighter far
    uint32_t near_green = 0x0030A030; // darker near - Bliss meadow
    int far_period = w * 600 / 1024; if(far_period < 1) far_period = 1;
    int near_period = w * 400 / 1024; if(near_period < 1) near_period = 1;
    for(int x=0; x<w; x++){
        int far_phase = (x * 256 / far_period) & 0xFF; // scaled period keeps ~1.7 hills visual
        int near_phase = (x * 256 / near_period) & 0xFF; // scaled period keeps ~2.5 hills visual
        int far_y = hill_base + 8 + (sin_lookup(far_phase) * 18 >> 7); // base 8, amp 18 => range 460±16, touches horizon
        int near_y = hill_base + 32 + (sin_lookup(near_phase) * 30 >> 7); // base 32, amp 30 => range 460+2..62, in front of far
        // Subtle second wave for organic rolling, different offset/amp per layer to avoid identical alignment
        far_y += (sin_lookup((far_phase+70)&0xFF) * 6 >> 7);  // +±5
        near_y += (sin_lookup((near_phase+50)&0xFF) * 10 >> 7); // +±9
        if(far_y < sky_h) far_y = sky_h;
        if(far_y > h-1) far_y = h-1;
        if(near_y < sky_h) near_y = sky_h;
        if(near_y > h-1) near_y = h-1;
        if(far_y > near_y) far_y = near_y; // far stays behind near
        fb_draw_rect(x, far_y, 1, h - far_y, far_green);
        fb_draw_rect(x, near_y, 1, h - near_y, near_green);
    }
    // Clouds - 3 clusters of 3-4 overlapping white circles, cheap, in sky area (y < sky_h) - x scaled with width
    // Original at 1024: cluster1 180,80 etc., cluster2 500,100, cluster3 850,90. At 1920 scaled ~1.875x keeps similar visual spread.
    int cx1 = w * 180 / 1024; gfx_draw_filled_circle(cx1, 80, 28, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 210 / 1024, 70, 22, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 240 / 1024, 85, 18, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 160 / 1024, 90, 15, 0x00FFFFFF);
    int cx2 = w * 500 / 1024; gfx_draw_filled_circle(cx2, 100, 30, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 530 / 1024, 85, 20, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 470 / 1024, 95, 18, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 550 / 1024, 105, 14, 0x00FFFFFF);
    int cx3 = w * 850 / 1024; gfx_draw_filled_circle(cx3, 90, 26, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 880 / 1024, 75, 18, 0x00FFFFFF);
    gfx_draw_filled_circle(w * 820 / 1024, 80, 16, 0x00FFFFFF);
}

static void wallpaper_cache_build_once(void){
    // Called once at init: draw Bliss wallpaper into fb_back then snapshot to wallpaper_cache
    if(wallpaper_cache_ready) return;
    if(!wallpaper_cache_alloc()) return;
    // Build into back buffer first (fb_back is current fb_target)
    uint64_t t0 = rdtsc();
    draw_bliss_wallpaper();
    uint64_t t1 = rdtsc();
    wallpaper_build_cycles = t1 - t0;
    // Snapshot: back -> cache (rep movsl 786432 dwords)
    uint32_t *src = fb_get_back_buffer();
    uint32_t *dst = wallpaper_cache;
    uint32_t dwords = wallpaper_cache_bytes / 4;
    __asm__ volatile("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(dwords) : : "memory");
    wallpaper_cache_ready = 1;
    s_puts("WALLPAPER: built once cycles "); s_put_cycles(wallpaper_build_cycles);
    s_puts(" (~"); s_put_dec((uint32_t)wallpaper_build_cycles/3000); s_puts(" us @3GHz) bytes "); s_put_dec(wallpaper_cache_bytes); s_puts("\n");
    {
        int w = fb_get_width(); int h = fb_get_height(); int sky_h = h*60/100;
        int black=0, magenta=0; for(int x=0;x<w;x++) { uint32_t p=fb_get_pixel(x,sky_h); if(p==0x000000U) black++; if(p==0x00FF00FFU) magenta++; }
        s_puts("WALLPAPER HORIZON y="); s_put_dec(sky_h); s_puts(" black="); s_put_dec(black); s_puts(" magenta="); s_put_dec(magenta); s_puts("/"); s_put_dec(w);
        s_puts(magenta==w ? " MAGENTA DEBUG LINE ACTIVE\n" : black==w ? " FULL BLACK LINE!\n" : black==0 ? " clean (no divider)\n" : " scattered\n");
        s_puts(" samples y=460 x0="); s_put_hex32(fb_get_pixel(0,sky_h)); s_puts(" x512="); s_put_hex32(fb_get_pixel(512,sky_h)); s_puts("\n");
    }
    // Also measure old flat gradient for comparison (draw to back, then restore wallpaper)
    uint64_t g0 = rdtsc();
    draw_desktop_gradient();
    uint64_t g1 = rdtsc();
    s_puts("WALLPAPER: old flat gradient cycles "); s_put_cycles(g1 - g0);
    s_puts(" (~"); s_put_dec((uint32_t)(g1 - g0)/3000); s_puts(" us)\n");
    // Restore wallpaper to back (since we just overwrote back with flat gradient measurement)
    fb_blit_from(wallpaper_cache);
    // Measure blit cost also
    uint64_t b0 = rdtsc();
    fb_blit_from(wallpaper_cache);
    uint64_t b1 = rdtsc();
    s_puts("WALLPAPER: cached blit cycles "); s_put_cycles(b1 - b0);
    s_puts(" (~"); s_put_dec((uint32_t)(b1 - b0)/3000); s_puts(" us) vs Bliss recompute "); s_put_cycles(wallpaper_build_cycles); s_puts(" -> speedup x"); s_put_dec((uint32_t)wallpaper_build_cycles / ((uint32_t)(b1-b0)==0?1:(uint32_t)(b1-b0))); s_puts("\n");
}

void window_manager_draw_all(void){
    if(!fb_is_available()) return;
    uint64_t t_wall0=0, t_wall1=0;
    if(wallpaper_cache_ready){
        t_wall0 = rdtsc();
        wallpaper_blit_cached();
        t_wall1 = rdtsc();
        last_wallpaper_cycles = t_wall1 - t_wall0;
    } else {
        t_wall0 = rdtsc();
        draw_bliss_wallpaper();
        t_wall1 = rdtsc();
        last_wallpaper_cycles = t_wall1 - t_wall0;
        s_puts("WALLPAPER: uncached draw cycles "); s_put_cycles(last_wallpaper_cycles); s_puts("\n");
    }
    // Desktop icons are part of desktop layer under windows (z-order: wallpaper -> icons -> windows -> taskbar)
    desktop_icons_draw();
    uint64_t t_win0 = rdtsc();
    for(int i=0;i<window_count;i++){
        int idx = z_order[i];
        if(windows[idx].minimized) continue;
        window_draw_single(idx);
    }
    taskbar_draw();
    uint64_t t_win1 = rdtsc();
    last_windows_cycles = t_win1 - t_win0;
    s_puts("WM: drew windows back->front z=[");
    for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
    s_puts("] minimized: ");
    for(int i=0;i<window_count;i++){ s_put_dec(windows[i].minimized); if(i<window_count-1) s_putc(','); }
    s_puts("\n");
    // Per-frame breakdown (wallpaper vs windows) is logged in window_do_redraw, not here to keep serial clean
}



int window_find_at(int x, int y){
    // front to back - topmost first, skip minimized (not drawn, not clickable)
    for(int i=window_count-1; i>=0; i--){
        int idx = z_order[i];
        struct window *w = &windows[idx];
        if(!w->visible || w->minimized) continue;
        if(x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h){
            return idx;
        }
    }
    return -1;
}

int window_bring_to_front(int idx){
    if(idx <0 || idx >= window_count) return 0;
    int pos = -1;
    for(int i=0;i<window_count;i++) if(z_order[i]==idx) pos=i;
    if(pos==-1) return 0;
    if(pos == window_count-1){
        s_puts("WM: window "); s_put_dec(idx+1); s_puts(" already front\n");
        return 0;
    }
    for(int i=pos;i<window_count-1;i++) z_order[i]=z_order[i+1];
    z_order[window_count-1]=idx;
    for(int i=0;i<window_count;i++) windows[z_order[i]].z = i;
    s_puts("WM: bring window "); s_put_dec(idx+1); s_puts(" to front, new z=[");
    for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
    s_puts("]\n");
    g_needs_redraw = 1;
    return 1;
}



void window_set_needs_redraw(void){ g_needs_redraw = 1; }
int window_needs_redraw(void){ return g_needs_redraw; }
void window_do_redraw(void){
    if(g_in_redraw){
        // Re-entrancy detected - this should not happen if main loop is the only caller and it checks g_needs_redraw with cli
        // Log it
        s_puts("WM_REDRAW re-entry!\n");
        return;
    }
    __asm__ volatile("cli");
    if(!g_needs_redraw){
        __asm__ volatile("sti");
        return;
    }
    g_needs_redraw = 0;
    g_in_redraw = 1;
    uint64_t t0 = rdtsc();
    g_redraw_count++;
    mouse_cursor_restore();
    mouse_cursor_invalidate();
    window_manager_draw_all();
    mouse_cursor_draw_current();
    if(fb_is_double_buffered()) fb_swap();
    uint64_t t1 = rdtsc();
    if((g_redraw_count % 10)==0){
        s_puts("WM_REDRAW #"); s_put_dec(g_redraw_count);
        s_puts(" cycles "); s_put_dec((uint32_t)(t1-t0));
        s_puts(" [wallpaper "); s_put_dec((uint32_t)last_wallpaper_cycles);
        s_puts(" win "); s_put_dec((uint32_t)last_windows_cycles);
        s_puts("] PMM free "); s_put_dec(pmm_free_frames());
        s_puts("\n");
    }
    // Log first few drags separately for lag diagnosis (always, not only %10)
    if(g_redraw_count < 6){
        s_puts("WM_REDRAW breakdown #"); s_put_dec(g_redraw_count);
        s_puts(" total "); s_put_dec((uint32_t)(t1-t0));
        s_puts(" wallpaper "); s_put_dec((uint32_t)last_wallpaper_cycles);
        s_puts(" windows "); s_put_dec((uint32_t)last_windows_cycles);
        s_puts("\n");
    }
    g_in_redraw = 0;
    __asm__ volatile("sti");
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

int window_is_in_title_bar(int idx, int x, int y){
    if(idx<0||idx>=window_count) return 0;
    struct window *w = &windows[idx];
    if(w->minimized) return 0;
    return (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + TITLE_BAR_H);
}

int window_is_in_resize_handle(int idx, int x, int y){
    if(idx<0||idx>=window_count) return 0;
    struct window *w = &windows[idx];
    if(w->minimized) return 0;
    int rx = w->x + w->w - RESIZE_HANDLE;
    int ry = w->y + w->h - RESIZE_HANDLE;
    return (x >= rx && x < w->x + w->w && y >= ry && y < w->y + w->h);
}

int window_is_resizing(void){ return resizing; }

int window_start_resize(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1) return 0;
    if(!window_is_in_resize_handle(idx,x,y)) return 0;
    struct window *w = &windows[idx];
    resize_off_x = x - (w->x + w->w);
    resize_off_y = y - (w->y + w->h);
    resize_win = idx;
    resizing = 1;
    resize_start_w = w->w;
    resize_start_h = w->h;
    s_puts("WM: resize start Window "); s_put_dec(idx+1);
    s_puts(" offset "); s_put_dec(resize_off_x); s_putc(','); s_put_dec(resize_off_y);
    s_puts(" size "); s_put_dec(w->w); s_putc('x'); s_put_dec(w->h); s_puts("\n");
    window_bring_to_front(idx);
    return 1;
}

void window_update_resize(int x, int y){
    if(!resizing || resize_win==-1) return;
    struct window *w = &windows[resize_win];
    int new_w = (x - resize_off_x) - w->x;
    int new_h = (y - resize_off_y) - w->y;
    // Enforce minimum - per-window based on content so button/textbox stay inside
    // Stay at same relative offset from top-left, don't scale them; clamp min high enough
    int min_w = WIN_MIN_W;
    int min_h = WIN_MIN_H;
    if(w->has_button){
        int need_w = w->btn.x + w->btn.w + 10;
        if(need_w > min_w) min_w = need_w;
        int need_h = w->btn.y + w->btn.h + 10;
        if(need_h > min_h) min_h = need_h;
    }
    if(w->has_textbox){
        int need_w2 = w->tbox.x + w->tbox.w + 10;
        if(need_w2 > min_w) min_w = need_w2;
        int need_h2 = w->tbox.y + w->tbox.h + 10;
        if(need_h2 > min_h) min_h = need_h2;
    }
    if(new_w < min_w) new_w = min_w;
    if(new_h < min_h) new_h = min_h;
    int max_w = (int)fb_get_width() - w->x;
    int max_h = (int)fb_get_height() - TASKBAR_H - w->y;
    if(new_w > max_w) new_w = max_w;
    if(new_h > max_h) new_h = max_h;
    if(new_w == w->w && new_h == w->h) return;
    w->w = new_w;
    w->h = new_h;
    g_needs_redraw = 1;
}

void window_end_resize(void){
    if(!resizing) return;
    s_puts("WM: resize end Window "); s_put_dec(resize_win+1);
    s_puts(" now "); s_put_dec(windows[resize_win].w); s_putc('x'); s_put_dec(windows[resize_win].h); s_puts("\n");
    resizing = 0;
    resize_win = -1;
}

int window_is_in_close_button(int idx, int x, int y){
    if(idx<0||idx>=window_count) return 0;
    struct window *w=&windows[idx];
    if(w->minimized) return 0;
    int bx = w->x + w->w - CLOSE_BTN_W - 4 - MINIMIZE_BTN_W - 4;
    int by = w->y + 2;
    return (x >= bx && x < bx+CLOSE_BTN_W && y >= by && y < by+CLOSE_BTN_H);
}
int window_handle_taskmanager_kill_click(int x, int y){
    int tm = window_find_by_title("Task Manager");
    if(tm==-1) return 0;
    struct window *w = &windows[tm];
    if(w->minimized || !w->visible) return 0;
    if(x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) return 0;
    for(int tid=1; tid<=2; tid++){
        int kx = w->x + 200; int ky = w->y + (tid==1?42:54); int kw=40, kh=12;
        if(x >= kx && x < kx+kw && y >= ky && y < ky+kh){
            // Kill task and its window - trace: task_kill frees PMM frames at 0x03000000+id*8192 (2 pages), unmaps, shifts task_list, decrements num_tasks
            // window_close for "Clicker" (id1) or "Notes" (id2) shifts windows array and z_order, cleans dangling drag/resize
            extern int task_kill(int id);
            const char *wtitle = (tid==1) ? "Clicker" : "Notes";
            int widx = window_find_by_title(wtitle);
            s_puts("TASKMGR: Kill Task "); s_put_dec(tid); s_puts(" '"); s_puts(wtitle); s_puts("' window idx "); s_put_dec(widx); s_puts("\n");
            task_kill(tid);
            if(widx != -1) window_close(widx);
            g_needs_redraw = 1;
            return 1;
        }
    }
    return 0;
}

int window_handle_minimize_click(int x, int y){
    // Check topmost window's minimize button first (before drag)
    int idx = window_find_at(x,y);
    if(idx==-1) return 0;
    struct window *w = &windows[idx];
    int bx = w->x + w->w - MINIMIZE_BTN_W - 4;
    int by = w->y + 2;
    if(x >= bx && x < bx+MINIMIZE_BTN_W && y >= by && y < by+MINIMIZE_BTN_H){
        w->minimized = 1;
        w->tbox.focused = 0; // unfocus textbox when minimized
        s_puts("WM: minimize Window "); s_put_dec(idx+1); s_puts("\n");
        g_needs_redraw = 1;
        return 1;
    }
    return 0;
}

int window_handle_close_click(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1) return 0;
    if(!window_is_in_close_button(idx,x,y)) return 0;
    s_puts("WM: close click Window "); s_put_dec(idx+1); s_puts("\n");
    window_close(idx);
    return 1;
}

void window_close(int idx){
    if(idx<0||idx>=window_count) return;
    s_puts("WM: closing Window "); s_put_dec(idx+1); s_puts(" title "); s_puts(windows[idx].title); s_puts("\n");
    // Save title for app handling before shift (Clicker/Notes are apps that can be reopened via desktop icon, unlike generic "+" windows)
    char closed_title[32]; for(int i=0;i<32;i++) closed_title[i]=windows[idx].title[i];
    // If closed window was being dragged/resized, cancel
    if(dragging && drag_win==idx){
        dragging=0; drag_win=-1;
        s_puts("WM: cancel drag for closed window\n");
    }
    if(resizing && resize_win==idx){
        resizing=0; resize_win=-1;
        s_puts("WM: cancel resize for closed window\n");
    }
    // If closed window had focused textbox, unfocus (no dangling focused)
    if(windows[idx].has_textbox && windows[idx].tbox.focused){
        s_puts("WM: unfocus textbox of closed window\n");
    }
    // If closed window had pressed button, clear
    if(windows[idx].has_button && windows[idx].btn.pressed){
        windows[idx].btn.pressed=0;
    }
    // Remove from z_order: find pos of idx in z_order, remove it, and adjust indices > idx
    int pos=-1;
    for(int i=0;i<window_count;i++) if(z_order[i]==idx) pos=i;
    if(pos==-1){
        // Defensive: idx not in z_order means window/z_order desync; proceeding would write
        // z_order[-1] (wild write into adjacent .bss). Refuse loudly instead of corrupting.
        s_puts("WM: close desync, idx not in z_order - refusing\n");
        return;
    }
    // Shift windows array down by one from idx+1 to end
    for(int i=idx;i<window_count-1;i++) windows[i]=windows[i+1];
    // Adjust z_order: remove entry at pos, and for any entry > idx, decrement by 1
    for(int i=pos;i<window_count-1;i++) z_order[i]=z_order[i+1];
    // Now window_count decreased by 1, and z_order has one fewer valid entry
    // For remaining z_order entries, if they were > idx, they now point to shifted windows, so decrement
    for(int i=0;i<window_count-1;i++){
        if(z_order[i] > idx) z_order[i]--;
    }
    window_count--;
    // Update z values for debug
    for(int i=0;i<window_count;i++) windows[z_order[i]].z = i;
    // Adjust drag/resize indices if they pointed to windows that shifted
    if(drag_win > idx) drag_win--;
    else if(drag_win == idx) { dragging=0; drag_win=-1; }
    if(resize_win > idx) resize_win--;
    else if(resize_win == idx) { resizing=0; resize_win=-1; }
    // Also need to handle that any window's button pressed state that was for the closed window is gone, but other windows' button states remain
    s_puts("WM: closed, new count "); s_put_dec(window_count);
    s_puts(" z=[");
    for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
    s_puts("]\n");
    // Example trace for spec: windows[3] closed while z_order=[0,2,1,3] count=4
    // Before: windows[0]=W1,1=W2,2=W3,3=TaskMan, z=[0,2,1,3] (W1 back, W3, W2, TaskMan front)
    // Remove idx 3: windows array shifts none (since idx is last), z_order remove pos 3 (value 3) => z=[0,2,1], count 3, no indices >3 to decrement, so z now correctly points to W1(0),W3(1 after shift? Wait W3 was at idx 2, still 2? Actually after shift, windows[2] is still W3, but its old index 2 is now still 2? No, after removing idx 3, windows[0..2] remain W1,W2,W3, indices 0,1,2, and z_order [0,2,1] now correctly points to W1(0), W3(2), W2(1) - W2 is now at idx 1 (was 1), W3 at idx 2 (was 2) - correct.
    // If remove idx 1 (W2) with z=[0,2,1,3]: windows shift: W2 removed, W3 moves from idx2->1, TaskMan from idx3->2. z_order remove pos2 (value1) => [0,2,3] then decrement >1: 2->1,3->2 => [0,1,2] which is W1(0), W3(new1), TaskMan(new2) - correct, no dangling 3.
    // App handling: Clicker and Notes are apps with associated tasks (ids 1/2). Closing them via X should be treated as app close, not permanent destroy like generic windows.
    // For generic windows via "+" (Window 3,4...), just close as before. For Clicker/Notes, also ensure task is handled (killed or left) so reopen via icon can recreate fresh pair.
    // Here we treat close as app close: if closed was Clicker or Notes, also kill its task if it still exists (to keep window/task pair consistent for recreation)
    // This is simpler than trying to preserve exact task state across close, and matches Task Manager Kill behavior (window+task pair)
    if(icon_streq(closed_title, "Clicker")){
        extern int task_kill_by_name(const char *name);
        int killed = task_kill_by_name("Clicker");
        if(killed) s_puts("WM: app Clicker closed, task killed for clean reopen\n");
        else s_puts("WM: app Clicker closed, task already not running\n");
    } else if(icon_streq(closed_title, "Notes")){
        extern int task_kill_by_name(const char *name);
        int killed = task_kill_by_name("Notes");
        if(killed) s_puts("WM: app Notes closed, task killed\n");
        else s_puts("WM: app Notes closed, task already not running\n");
    }
    g_needs_redraw = 1;
}
int window_find_by_title(const char *title){
    for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, title)) return i;
    return -1;
}

int window_handle_taskbar_click(int x, int y){
    if(!fb_is_available()) return 0;
    int ty = fb_get_height() - TASKBAR_H;
    if(y < ty || y >= ty + TASKBAR_H) return 0;
    // Check "+" button at far right first (before window tabs, it's part of taskbar)
    {
        int plus_w = 30, plus_h = TASKBAR_H - 6;
        int plus_x = fb_get_width() - plus_w - 5;
        int plus_y = ty + 3;
        if(x >= plus_x && x < plus_x+plus_w && y >= plus_y && y < plus_y+plus_h){
            if(window_count >= MAX_WINDOWS){
                s_puts("WM: create window failed - at max "); s_put_dec(MAX_WINDOWS); s_puts(" (disabled)\n");
                return 1; // handled (no-op, but don't fall through to window tabs)
            }
            window_create_new();
            return 1;
        }
    }
    // Check each tab
    for(int i=0;i<window_count;i++){
        int tab_x = 5 + i * (150 + 5);
        int tab_y = ty + 3;
        int tab_w = 150;
        int tab_h = TASKBAR_H - 6;
        if(x >= tab_x && x < tab_x+tab_w && y >= tab_y && y < tab_y+tab_h){
            struct window *w = &windows[i];
            if(w->minimized){
                w->minimized = 0;
                s_puts("WM: unminimize Window "); s_put_dec(i+1); s_puts("\n");
            }
            // Bring to front (if not already)
            return window_bring_to_front(i);
        }
    }
    return 0;
}

int window_is_minimized(int idx){
    if(idx<0||idx>=window_count) return 0;
    return windows[idx].minimized;
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
    int min_x = -w->w + 60;
    int max_x = (int)fb_get_width() - 60;
    int min_y = 0;
    int max_y = (int)fb_get_height() - TITLE_BAR_H;
    if(new_x < min_x) new_x = min_x;
    if(new_x > max_x) new_x = max_x;
    if(new_y < min_y) new_y = min_y;
    if(new_y > max_y) new_y = max_y;
    if(new_x == w->x && new_y == w->y) return;
    w->x = new_x;
    w->y = new_y;
    g_needs_redraw = 1;
}


void window_end_drag(void){
    if(!dragging) return;
    s_puts("WM: drag end Window "); s_put_dec(drag_win+1); s_puts("\n");
    dragging = 0;
    drag_win = -1;
}

// --- Phase 12: button ---
static int button_hit_test(int win_idx, int x, int y){
    if(win_idx <0 || win_idx >= window_count) return 0;
    struct window *w = &windows[win_idx];
    if(!w->has_button) return 0;
    int ax = w->x + w->btn.x;
    int ay = w->y + w->btn.y;
    return (x >= ax && x < ax + w->btn.w && y >= ay && y < ay + w->btn.h);
}

int window_handle_button_down(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1) return 0;
    if(!button_hit_test(idx,x,y)) return 0;
    struct window *w = &windows[idx];
    // Bring window to front if not already - defer redraw
    int pos=-1;
    for(int i=0;i<window_count;i++) if(z_order[i]==idx) pos=i;
    if(pos != window_count-1){
        for(int i=pos;i<window_count-1;i++) z_order[i]=z_order[i+1];
        z_order[window_count-1]=idx;
        for(int i=0;i<window_count;i++) windows[z_order[i]].z = i;
        s_puts("WM: bring window "); s_put_dec(idx+1); s_puts(" to front (button) new z=[");
        for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
        s_puts("]\n");
    }
    w->btn.pressed = 1;
    g_needs_redraw = 1;
    s_puts("BTN: down Window "); s_put_dec(idx+1); s_puts(" pressed\n");
    return 1;
}


int window_handle_button_up(int x, int y){
    int pressed_idx = -1;
    for(int i=0;i<window_count;i++) if(windows[i].has_button && windows[i].btn.pressed) pressed_idx = i;
    if(pressed_idx==-1) return 0;
    struct window *w = &windows[pressed_idx];
    int ax = w->x + w->btn.x;
    int ay = w->y + w->btn.y;
    int inside = (x >= ax && x < ax + w->btn.w && y >= ay && y < ay + w->btn.h);
    w->btn.pressed = 0;
    if(inside){
        w->btn.clicks++;
        char buf[32];
        const char *prefix = "Clicked: ";
        int p=0;
        for(int i=0; prefix[i] && p<31; i++) buf[p++]=prefix[i];
        char num[12]; int n=w->btn.clicks; int len=0;
        if(n==0) num[len++]='0';
        else { char tmp[12]; int t=0; while(n){ tmp[t++]='0'+n%10; n/=10; } while(t--) num[len++]=tmp[t]; }
        for(int i=0;i<len && p<31; i++) buf[p++]=num[i];
        buf[p]=0;
        w_strcpy(w->btn.label, buf, 32);
        s_puts("BTN: click Window "); s_put_dec(pressed_idx+1); s_puts(" count "); s_put_dec(w->btn.clicks); s_puts(" label "); s_puts(buf); s_puts("\n");
    } else {
        s_puts("BTN: release outside Window "); s_put_dec(pressed_idx+1); s_puts("\n");
    }
    g_needs_redraw = 1;
    return 1;
}

// --- Phase 12 textbox + keyboard ---
static int textbox_hit_test(int win_idx, int x, int y){
    if(win_idx<0||win_idx>=window_count) return 0;
    struct window *w=&windows[win_idx];
    if(!w->has_textbox) return 0;
    int ax=w->x + w->tbox.x;
    int ay=w->y + w->tbox.y;
    return (x>=ax && x<ax+w->tbox.w && y>=ay && y<ay+w->tbox.h);
}

int window_handle_textbox_click(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1) {
        // click on desktop -> unfocus all
        int had=0;
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused) had=1;
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox) windows[i].tbox.focused=0;
        if(had){ s_puts("TBOX: unfocus all (desktop)\n"); g_needs_redraw=1; }
        return 0;
    }
    // check if click inside textbox of that window
    if(!textbox_hit_test(idx,x,y)){
        // click inside window but outside textbox -> unfocus this window's textbox if it was focused? Keep focus only if click inside textbox
        // For Phase 12, clicking elsewhere in window should unfocus textbox (only one focused at a time)
        int had_focus = 0;
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused) had_focus=1;
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox) windows[i].tbox.focused=0;
        if(had_focus){ s_puts("TBOX: click outside textbox, unfocus\n"); g_needs_redraw=1; }
        return 0;
    }
    // hit textbox - focus this, unfocus others
    for(int i=0;i<window_count;i++) if(windows[i].has_textbox) windows[i].tbox.focused=(i==idx);
    windows[idx].tbox.cursor_visible=1;
    windows[idx].tbox.blink_counter=0;
    s_puts("TBOX: focus Window "); s_put_dec(idx+1); s_puts("\n");
    // also bring window to front if not already
    int pos=-1; for(int i=0;i<window_count;i++) if(z_order[i]==idx) pos=i;
    if(pos != window_count-1){
        for(int i=pos;i<window_count-1;i++) z_order[i]=z_order[i+1];
        z_order[window_count-1]=idx;
        for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
    }
    g_needs_redraw=1;
    return 1;
}

void window_handle_key(char c){
    // find focused textbox
    int fidx=-1;
    for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused) fidx=i;
    if(fidx==-1) return;
    struct textbox *tb=&windows[fidx].tbox;
    if(c=='\b'){
        if(tb->len>0){ tb->len--; tb->buffer[tb->len]=0; s_puts("TBOX: backspace len "); s_put_dec(tb->len); s_puts("\n"); g_needs_redraw=1; }
        return;
    }
    if(c=='\n' || c=='\r') return;
    if(tb->len >= tb->max_len) return;
    tb->buffer[tb->len++]=c;
    tb->buffer[tb->len]=0;
    s_puts("TBOX: typed '"); s_putc(c); s_puts("' len "); s_put_dec(tb->len); s_puts(" Window "); s_put_dec(fidx+1); s_puts("\n");
    g_needs_redraw=1;
}

void window_handle_backspace(void){
    window_handle_key('\b');
}

void window_tick_cursor(void){
    // Called from main loop to blink - toggle every N redraws
    // We use a simple counter: toggle every 20 ticks (~0.5s if called at ~40Hz main loop)
    // Choice: redraw-cycle vs PIT ticks - we don't have PIT unmasked, so use main loop iterations
    // Main loop runs hlt + check, wakes on any IRQ (mouse/keyboard). For idle blink, we need periodic wakeups.
    // Since PIT is masked (master 0xF9), no periodic wakeup. So we blink on every redraw that happens due to input.
    // For true idle blink, we would need PIT. For now, blink every 20 redraws is visible during typing/movement.
    for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused){
        windows[i].tbox.blink_counter++;
        if(windows[i].tbox.blink_counter >= 20){
            windows[i].tbox.blink_counter=0;
            windows[i].tbox.cursor_visible ^= 1;
            g_needs_redraw=1;
        }
    }
}

static const char scancode_map[128] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5', [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t', [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m',
    [0x39]=' ', [0x0E]=0, // backspace handled separately
};
// Use scancode_map for translation

int window_handle_scancode(uint8_t scancode){
    // Check if any textbox is focused - if none, let caller fall through to scancode logging
    int has_focused = 0;
    for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused) { has_focused=1; break; }
    if(!has_focused) return 0;
    // Ignore release (high bit set) - only translate press
    if(scancode & 0x80) return 1; // still considered handled (focused, but release ignored)
    if(scancode == 0x0E){ // backspace make
        window_handle_backspace();
        return 1;
    }
    char c = 0;
    if(scancode < 128) c = scancode_map[scancode];
    if(!c) return 1; // unmapped but focused, consume it (don't log as KEY)
    window_handle_key(c);
    return 1;
}
