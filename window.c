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
// Session-persistent Notes text: lives in .bss for the whole boot session,
// OUTSIDE the window struct that window_close's array shift destroys.
// BSS-zero at boot -> fresh boot (relaunch QEMU) starts empty: session-only
// by construction, no disk involved.
static char notes_saved[513];
static int notes_saved_len = 0;
// Copy live textbox -> session buffer (called on close, before destruction).
static void notes_save_from(struct textbox *tb){
    int len = tb->len;
    if(len > tb->max_len) len = tb->max_len;
    if(len > 512) len = 512;
    for(int i=0;i<len;i++) notes_saved[i] = tb->buffer[i];
    notes_saved[len] = 0;
    notes_saved_len = len;
}
// Copy session buffer -> fresh Notes window textbox. Cursor model is
// append-at-len with no scroll-offset field, so setting len puts the cursor
// at end of text by construction; focus/blink reset to defaults.
static void notes_restore_to(int nid){
    struct textbox *tb = &windows[nid].tbox;
    int len = notes_saved_len;
    if(len > tb->max_len) len = tb->max_len;
    if(len > 512) len = 512;
    for(int i=0;i<len;i++) tb->buffer[i] = notes_saved[i];
    tb->buffer[len] = 0;
    tb->len = len;
    tb->focused = 0;
    tb->cursor_visible = 1;
    tb->blink_counter = 0;
}
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
// Virtual layout (post-photo-blob): kernel image 0x00100000-~0x00970000, heap 0x00B00000-0x00C00000 (1MB), fb_back 0x00C00000-0x014E9000 (~8.3MB), wallpaper 0x01600000-0x01EE9000 (~8.3MB), photo stage 0x02200000-0x02A30000 (~8.3MB spare, allocated on first Photo switch), paint canvas 0x02000000-0x02130000 (~1.2MB), task stacks 0x03000000+ below 0xFD000000 fb_front
static uint32_t *wallpaper_cache = 0;
static uint32_t wallpaper_cache_bytes = 0;
static uint32_t wallpaper_cache_pages = 0;
static int wallpaper_cache_ready = 0;
// Wallpaper preset - session-persistent global (BSS zero = Day at boot).
// Future Settings app contract: read wallpaper_preset directly, change ONLY
// via wallpaper_set_preset()/wallpaper_cycle_preset() (validate + rebuild +
// redraw). Same geometry for all presets, different palettes - cheap to
// generate procedurally, no new assets.
int wallpaper_preset = 0;
// Photo blob (photo.s incbin, .rodata): raw 1920x1080x32 pixels, BGRA byte
// order = 0x00RRGGBB like fb_put_pixel. Pointer-difference gives exact size.
extern uint8_t photo_wallpaper_data[];
extern uint8_t photo_wallpaper_end[];
struct wallpaper_palette {
    const char *name;
    uint32_t sky_top, sky_bot; // gradient + horizon fill
    uint32_t sun_outer, sun_inner; // sun (Day/Sunset) or moon (Night)
    int sun_r_outer, sun_r_inner;
    int sun_near_horizon; // 0: y=120 (high), 1: y=sky_h+30 (low, sunset)
    uint32_t far_green, near_green;
    uint32_t cloud;
};
static const struct wallpaper_palette wallpaper_palettes[WALLPAPER_PRESET_COUNT] = {
    {"Day", 0x0087CEEB,0x00B0E0E6, 0x00FFFFE0,0x00FFFFFF, 40,35,0, 0x0090C060,0x0030A030, 0x00FFFFFF},
    {"Sunset", 0x003B2E6E,0x00FF8C42, 0x00FF5A2A,0x00FFD166, 48,40,1, 0x006B4E71,0x002E4038, 0x00FFB59E},
    {"Night", 0x00050914,0x000B1D3A, 0x00E8EDF2,0x00FFFFFF, 30,24,0, 0x000E2A1A,0x0005130C, 0x003A4A63},
    {"Photo", 0,0, 0,0, 0,0,0, 0,0, 0}, // colors unused: direct blob copy (see rebuild)
};
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

static void window_draw_one_button(struct window *w, int b){
    // Button absolute position = window x + button x, window y + button y
    // Button y is relative to window origin (0,0 = window top-left), so absolute already includes title bar offset if button placed below it
    // Conversion: ax = w->x + btn.x, ay = w->y + btn.y
    int ax = w->x + w->btns[b].x;
    int ay = w->y + w->btns[b].y;
    uint32_t bg = w->btns[b].pressed ? 0x00999999 : 0x00CCCCCC; // pressed darker
    uint32_t border = 0x00000000;
    uint32_t fg = 0x00000000; // black text on light button
    fb_draw_rect(ax, ay, w->btns[b].w, w->btns[b].h, bg);
    gfx_draw_rect_outline(ax, ay, w->btns[b].w, w->btns[b].h, border);
    // label centered
    int len = 0; while(w->btns[b].label[len] && len < 32) len++;
    int tx = ax + (w->btns[b].w - len*8)/2;
    int ty = ay + (w->btns[b].h - 8)/2;
    gfx_draw_string(tx, ty, w->btns[b].label, fg);
}

static void window_draw_button(struct window *w){
    if(!w->has_button) return;
    // Multi-button loop: Clicker has num_btns==1 so this renders pixel-identical
    // to the old single-button path (same rect, colors, centering math).
    int n = w->num_btns;
    if(n > MAX_BTNS) n = MAX_BTNS;
    for(int b=0;b<n;b++) window_draw_one_button(w, b);
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

// Live word count for the Notes textbox. Single linear scan, no extra state:
// separators are space and newline; every 0->1 in_word transition starts a word.
// Trace: "hello world": h starts word 1, space ends it, w starts word 2 -> 2.
// "  hello   world  ": leading spaces keep in_word=0, hello -> 1, gap ends it,
// world -> 2, trailing spaces ignored -> 2. "" -> 0. "hello" -> 1.
// Backspace just shortens len, so recounting from scratch each redraw is always
// consistent (no stale stored count possible).
int textbox_word_count(struct textbox *tb){
    if(!tb) return 0;
    int len = tb->len;
    if(len > tb->max_len) len = tb->max_len;
    if(len > 512) len = 512; // buffer is 513 incl. NUL; never scan past it
    int count = 0, in_word = 0;
    for(int i=0;i<len;i++){
        char c = tb->buffer[i];
        if(c==' ' || c=='\n') in_word = 0;
        else if(!in_word){ in_word = 1; count++; }
    }
    return count;
}

// --- Calculator: purely reactive app, NO background task ---
// Rationale: Clicker/Notes tasks exist to mutate state between interactions
// (ticking counters) and must never touch fb directly (deferred-redraw rule).
// Calculator state changes only inside the mouse button-up path (already GUI
// context) and paints via g_needs_redraw. A task would burn PIT timeslices
// redrawing nothing. Task Manager lists tids 1-2 only, so Calculator is
// correctly absent there - it shows tasks, not windows.
// Geometry (default window 304x264): display at (20,30), width stretches as
// W-40 and height as calc_display_h; grid origin (20, y0=calc_grid_y0(dh)),
// buttons 60x36, gap 8 at default size; layout_sync_window recomputes from
// CURRENT w/h on every redraw (bottom margin 20).
static void calculator_init_window(int nid){
    struct window *w = &windows[nid];
    static const char *labels[4][4] = {{"7","8","9","/"},{"4","5","6","*"},{"1","2","3","-"},{"0","C","=","+"}};
    w->x=500; w->y=140; w->w=304; w->h=264;
    w_strcpy(w->title, "Calculator", 32);
    w->bg_color=0x00E8E8E8; w->title_color=0x00226644; w->border_color=0x00000000;
    w->visible=1; w->minimized=0; w->z=window_count;
    w->has_textbox=0; w->has_settings=0; w->has_paint=0; w->has_about=0;
    w->has_calc=1;
    w->calc.display[0]='0'; w->calc.display[1]='.'; w->calc.display[2]='0'; w->calc.display[3]='0'; w->calc.display[4]=0; // "0.00"
    w->calc.acc=0; w->calc.op=0; w->calc.fresh=1; w->calc.err=0;
    w->has_button=1; w->num_btns=0;
    for(int r=0;r<4;r++) for(int c=0;c<4;c++){
        int b = r*4+c;
        w->btns[b].x = 20+c*68; w->btns[b].y = 76+r*44;
        w->btns[b].w = 60; w->btns[b].h = 36;
        w_strcpy(w->btns[b].label, labels[r][c], 32);
        w->btns[b].pressed = 0; w->btns[b].clicks = 0;
        w->num_btns++;
    }
    w->task_counter=0;
}

// Shared tz preset table: timezone_cycle() steps it, Settings buttons index it
// directly (b-4). City labels live in settings_init_window (display only).
static const int tz_preset_offsets[TIMEZONE_PRESET_COUNT] = {0, 10, -5, 9};
// --- Settings: 4th app, NO background task (same reasoning as Calculator:
// purely reactive - state changes only in the button-up path, paints via
// g_needs_redraw). Reuses the multi-button widget array (8 buttons).
// Layout (window 380x220): "Wallpaper:" label + 3 direct-set buttons
// (Day/Sunset/Night) in a row; "Timezone:" label + 2x2 grid with CITY labels
// (display-only; same offsets/logic as tz presets). Selection highlight reads
// wallpaper_preset / tz_offset_hours live - no duplicate state.
static void settings_init_window(int nid){
    struct window *w = &windows[nid];
    static const char *wp[4] = {"Day", "Sunset", "Night", "Photo"};
    static const char *tz[4] = {"UTC", "Sydney (UTC+10)", "New York (UTC-5)", "Tokyo (UTC+9)"};
    w->x=600; w->y=120; w->w=380; w->h=220;
    w_strcpy(w->title, "Settings", 32);
    w->bg_color=0x00E0E4EA; w->title_color=0x00334155; w->border_color=0x00000000;
    w->visible=1; w->minimized=0; w->z=window_count;
    w->has_textbox=0; w->has_calc=0; w->has_settings=1; w->has_paint=0; w->has_about=0;
    w->has_button=1; w->num_btns=0;
    for(int i=0;i<4;i++){ // wallpaper row: y=52 h=30, x=20/110/200/290 w=80 (ends 370)
        w->btns[i].x = 20+i*90; w->btns[i].y = 52;
        w->btns[i].w = 80; w->btns[i].h = 30;
        w_strcpy(w->btns[i].label, wp[i], 32);
        w->btns[i].pressed = 0; w->btns[i].clicks = 0;
        w->num_btns++;
    }
    for(int i=0;i<4;i++){ // timezone 2x2: x=20/200 w=170, y=122/162 h=30
        int b = 4+i;
        w->btns[b].x = 20+(i%2)*180; w->btns[b].y = 122+(i/2)*40;
        w->btns[b].w = 170; w->btns[b].h = 30;
        w_strcpy(w->btns[b].label, tz[i], 32);
        w->btns[b].pressed = 0; w->btns[b].clicks = 0;
        w->num_btns++;
    }
    w->task_counter=0;
}
// Open-or-refocus shared by desktop icon idx5 and context menu "Open Settings".
static void settings_open_or_focus(void){
    int found = window_find_by_title("Settings");
    if(found != -1){
        s_puts("DESKTOP: action Settings bring to front\n");
        if(windows[found].minimized){ windows[found].minimized = 0; taskbar_tabs_invalidate(); window_invalidate(found); }
        window_bring_to_front(found);
        return;
    }
    s_puts("DESKTOP: action Settings create (was closed)\n");
    if(window_count < MAX_WINDOWS){
        int nid = window_count;
        settings_init_window(nid);
        z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
        s_puts("DESKTOP: created Settings\n"); window_invalidate(nid); taskbar_tabs_invalidate();
    } else s_puts("DESKTOP: cannot create Settings - at max\n");
}
// Direct-SET (not cycle) via the same setters the old menu items called:
// wallpaper_set_preset(b), timezone_set_offset(tz_preset_offsets[b-4]).
// Setters flag redraw themselves, so the desktop updates immediately.
static void settings_handle_button(struct window *w, int b){
    if(b < 4) wallpaper_set_preset(b); // rebuild dirties full screen itself
    else timezone_set_offset(tz_preset_offsets[b-4]);
    // Settings window (highlight follows) + taskbar clock area (tz jumps).
    dirty_add(w->x, w->y, w->w, w->h);
    taskbar_clock_invalidate();
    s_puts("SETTINGS: btn "); s_put_dec(b); s_puts(" wallpaper "); s_put_dec(wallpaper_preset);
    s_puts(" tz "); s_puts(timezone_label()); s_puts("\n");
}
// Paint-bounds + overlap helper for dirty-rect skip checks. Defined early -
// first use is paint_blit below; set per-frame by window_do_redraw, full-screen
// by boot init before its direct draw_all call.
static int dr_x0 = 0, dr_y0 = 0, dr_x1 = 0, dr_y1 = 0;
static int rects_overlap(int x,int y,int w,int h,int ox0,int oy0,int ox1,int oy1){
    if(x+w<=ox0||ox1<=x||y+h<=oy0||oy1<=y) return 0; return 1;
}
// --- Paint: 5th app, NO background task (purely reactive like Calculator).
// Canvas persistence model (differs from every other app): the framebuffer is
// double-buffered and recomposited from wallpaper upward EVERY redraw, so the
// drawing cannot live in any framebuffer - it needs its OWN persistent pixel
// buffer. Like the wallpaper cache: PMM pages at a fixed vaddr (0x02000000,
// 640x480x4 = 300 pages; wallpaper ends ~0x016E9000, task stacks start
// 0x03000000, so no overlap), allocated+cleared once, kept for the session
// (close keeps the drawing, same philosophy as Notes text). Every Paint
// redraw blits the visible canvas portion into the window body; strokes write
// into the buffer + dirty only the brush area (tiny rects, per dirty-rect).
// Canvas content is FIXED 640x480 regardless of window size: resize just shows
// more/less of it (top-left anchored, clipped) - no resampling, no corruption.
#define PAINT_CW 640
#define PAINT_CH 480
#define PAINT_VADDR 0x02000000
#define PAINT_BRUSH_R 3
static uint32_t *paint_canvas = 0; // 640x480 packed rows (stride 640)
static int paint_color_idx = 0; // default black
static const uint32_t paint_colors[5] = {0x00000000, 0x00FF0000, 0x0000AA00, 0x000000FF, 0x00FFD800}; // Black Red Green Blue Yellow
static int paint_last_x = 0, paint_last_y = 0, paint_last_valid = 0; // stroke interpolation state
static void paint_ensure_canvas(void){
    if(paint_canvas) return;
    uint32_t need = PAINT_CW * PAINT_CH * 4;
    uint32_t pages = (need + 0xFFF) >> 12; // 300
    s_puts("PAINT: canvas alloc "); s_put_dec(pages); s_puts(" pages at 0x02000000\n");
    for(uint32_t i=0;i<pages;i++){
        uint32_t p = pmm_alloc_frame();
        if(!p){ s_puts("PAINT: out of frames!\n"); paint_canvas = 0; return; }
        paging_map(PAINT_VADDR + i*0x1000, p, 0x03);
    }
    paint_canvas = (uint32_t*)PAINT_VADDR;
    // Clear to white paper.
    for(uint32_t i=0;i<(uint32_t)PAINT_CW*PAINT_CH;i++) paint_canvas[i] = 0x00FFFFFF;
    s_puts("PAINT: canvas ready (white)\n");
}
static void paint_clear_canvas(void){
    paint_ensure_canvas(); if(!paint_canvas) return;
    for(uint32_t i=0;i<(uint32_t)PAINT_CW*PAINT_CH;i++) paint_canvas[i] = 0x00FFFFFF;
    s_puts("PAINT: cleared\n");
}
int paint_count_nonwhite(void){
    if(!paint_canvas) return 0;
    int n = 0;
    for(uint32_t i=0;i<(uint32_t)PAINT_CW*PAINT_CH;i++) if(paint_canvas[i] != 0x00FFFFFF) n++;
    return n;
}
// Filled-circle brush dot at buffer coords (clipped to buffer).
static void paint_dot(int bx, int by){
    if(!paint_canvas) return;
    uint32_t col = paint_colors[paint_color_idx];
    for(int dy=-PAINT_BRUSH_R; dy<=PAINT_BRUSH_R; dy++)
        for(int dx=-PAINT_BRUSH_R; dx<=PAINT_BRUSH_R; dx++){
            if(dx*dx+dy*dy > PAINT_BRUSH_R*PAINT_BRUSH_R) continue;
            int px = bx+dx, py = by+dy;
            if(px<0||py<0||px>=PAINT_CW||py>=PAINT_CH) continue;
            paint_canvas[py*PAINT_CW+px] = col;
        }
}
// Stroke entry: screen point while left held. Only paints when the topmost
// window at the point IS Paint and the point lands in the buffer; otherwise
// the pen lifts (leaving/entering window or occlusion ends the segment, so
// re-entry doesn't draw a jump line). Always cheap: a few dozen pixels.
void paint_stroke_at(int x, int y){
    int pi = window_find_by_title("Paint");
    if(pi==-1){ paint_last_valid=0; return; }
    if(window_find_at(x,y) != pi){ paint_last_valid=0; return; }
    struct window *w = &windows[pi];
    int bx = x - (w->x+10), by = y - (w->y+66); // canvas origin in window
    if(bx<0||by<0||bx>=PAINT_CW||by>=PAINT_CH){ paint_last_valid=0; return; }
    if(paint_last_valid){
        // Interpolate 2px steps so fast moves draw solid lines, not dots.
        int dx = bx-paint_last_x, dy = by-paint_last_y;
        int steps = (dx<0?-dx:dx) > (dy<0?-dy:dy) ? (dx<0?-dx:dx) : (dy<0?-dy:dy);
        if(steps < 1) steps = 1;
        if(steps > 200) steps = 200; // teleport guard (e.g. window moved mid-stroke)
        for(int s=0;s<=steps;s++)
            paint_dot(paint_last_x + dx*s/steps, paint_last_y + dy*s/steps);
        // Dirty the WHOLE segment bounds (not just the endpoint): the blit only
        // transfers the dirty intersection, and anything outside it stays stale
        // on screen even though the buffer holds the full line (dotted-line bug).
        int sx0 = paint_last_x<bx?paint_last_x:bx, sy0 = paint_last_y<by?paint_last_y:by;
        int sx1 = paint_last_x>bx?paint_last_x:bx, sy1 = paint_last_y>by?paint_last_y:by;
        dirty_add(w->x+10+sx0-5, w->y+66+sy0-5, sx1-sx0+10, sy1-sy0+10);
    } else {
        paint_dot(bx, by);
        dirty_add(x-5, y-5, 10, 10); // brush r=3 + margin (also sets redraw flag)
    }
    paint_last_x = bx; paint_last_y = by; paint_last_valid = 1;
}
void paint_end_stroke(void){ paint_last_valid = 0; }
// Blit visible canvas portion into the window body. Visible rect = buffer
// origin intersected with window client area (10px margins, below palette)
// intersected with the frame's dirty bounds; fb clip guarantees the rest.
// Top-left anchored: resize shows more/less, content never rescales.
static void paint_blit(struct window *w){
    paint_ensure_canvas(); if(!paint_canvas) return;
    int ox = w->x+10, oy = w->y+66; // canvas screen origin
    // Visible canvas rect: window client + screen only (NOT dirty-intersected:
    // the frame below belongs around this, and drawing it around the dirty
    // intersection stamped a hollow box per stroke frame - the square-trail bug).
    int vx0 = ox, vy0 = oy, vx1 = ox+PAINT_CW, vy1 = oy+PAINT_CH;
    if(vx1 > w->x+w->w-10) vx1 = w->x+w->w-10; // window client clip
    if(vy1 > w->y+w->h-10) vy1 = w->y+w->h-10;
    if(vx0 < 0) vx0 = 0; if(vy0 < 0) vy0 = 0; // screen clip
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    if(vx1 > fw) vx1 = fw; if(vy1 > fh) vy1 = fh;
    if(vx0 >= vx1 || vy0 >= vy1) return; // nothing visible
    // Row blits intersect dirty too (this loop bypasses fb_* clip): repaint
    // only the changed portion, not the whole canvas every stroke.
    int x0 = vx0 < dr_x0 ? dr_x0 : vx0, y0 = vy0 < dr_y0 ? dr_y0 : vy0;
    int x1 = vx1 > dr_x1 ? dr_x1 : vx1, y1 = vy1 > dr_y1 ? dr_y1 : vy1;
    if(x0 < x1 && y0 < y1){
        uint32_t *back = fb_get_back_buffer(); if(!back) return;
        uint32_t stride_dw = fb_get_pitch() / 4;
        for(int ry=y0; ry<y1; ry++){
            uint32_t *s = paint_canvas + (uint32_t)(ry-oy) * PAINT_CW + (uint32_t)(x0-ox);
            uint32_t *d = back + (uint32_t)ry * stride_dw + (uint32_t)x0;
            uint32_t dwords = (uint32_t)(x1-x0);
            __asm__ volatile("cld; rep movsl" : "+S"(s), "+D"(d), "+c"(dwords) : : "memory");
        }
    }
    // Frame the VISIBLE canvas (fb clip constrains pixels: no stray boxes even
    // when dirty covers only part of it).
    if(rects_overlap(vx0, vy0, vx1-vx0, vy1-vy0, dr_x0, dr_y0, dr_x1, dr_y1))
        gfx_draw_rect_outline(vx0, vy0, vx1-vx0, vy1-vy0, 0x00000000);
}
// Palette dispatch: buttons 0-4 set color, button 5 = Clear (white + dirty
// the canvas screen area so the erase shows immediately).
static void paint_handle_button(struct window *w, int b){
    if(b < 5){
        paint_color_idx = b;
        s_puts("PAINT: color "); s_put_dec(b); s_puts("\n");
    } else {
        paint_clear_canvas();
        dirty_add(w->x+10, w->y+66, PAINT_CW, PAINT_CH); // clamped to screen inside
    }
}
// Window 440x400 at (700,150): palette row y=30 h=26 (6 x 60px buttons),
// canvas origin (10,66). No task (purely reactive).
static void paint_init_window(int nid){
    struct window *w = &windows[nid];
    static const char *pal[6] = {"Black", "Red", "Green", "Blue", "Yellow", "Clear"};
    w->x=700; w->y=150; w->w=440; w->h=400;
    w_strcpy(w->title, "Paint", 32);
    w->bg_color=0x00E8E8E8; w->title_color=0x00772222; w->border_color=0x00000000;
    w->visible=1; w->minimized=0; w->z=window_count;
    w->has_textbox=0; w->has_calc=0; w->has_settings=0; w->has_paint=1; w->has_about=0;
    w->has_button=1; w->num_btns=0;
    for(int i=0;i<6;i++){
        w->btns[i].x = 10+i*68; w->btns[i].y = 30;
        w->btns[i].w = 60; w->btns[i].h = 26;
        w_strcpy(w->btns[i].label, pal[i], 32);
        w->btns[i].pressed = 0; w->btns[i].clicks = 0;
        w->num_btns++;
    }
    w->task_counter=0;
    paint_ensure_canvas();
}
// Open-or-refocus shared by desktop icon idx6 (window only, no task).
static void paint_open_or_focus(void){
    int found = window_find_by_title("Paint");
    if(found != -1){
        s_puts("DESKTOP: action Paint bring to front\n");
        if(windows[found].minimized){ windows[found].minimized = 0; taskbar_tabs_invalidate(); window_invalidate(found); }
        window_bring_to_front(found);
        return;
    }
    s_puts("DESKTOP: action Paint create (was closed)\n");
    if(window_count < MAX_WINDOWS){
        int nid = window_count;
        paint_init_window(nid);
        z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
        s_puts("DESKTOP: created Paint\n"); window_invalidate(nid); taskbar_tabs_invalidate();
    } else s_puts("DESKTOP: cannot create Paint - at max\n");
}

// FIXED-POINT model (no FPU anywhere: no CR0.EM handling, no FNINIT - out of
// scope). All values stored as integers scaled by 100 (2 decimals): "1.55" is
// the int 155, "10.00" is 1000. Arithmetic is plain integer math; the decimal
// point is inserted only when FORMATTING for display.
// Entry model: whole-number entry ONLY (no decimal-point button - the 4x4 grid
// is full at 16/16). Typing builds an integer string; it is scaled x100 when an
// operator reads it. Decimals are PRODUCED by division (and persist through
// chains via parse-back of the formatted display). E.g. 14/9 -> "1.55".
// Range note: scaled values must fit int32; entry capped at 7 digits
// (9999999 -> 999999900 scaled, fits). Operands beyond ~214,748.36 can overflow
// the (a*100) division step; huge products can wrap - same overflow character
// as the old integer code, documented, out of scope.
// Format scaled int -> "D+.DD" always with 2 decimals (consistent, unambiguous
// about precision; no trim logic). Traces: 155 -> "1.55", 1000 -> "10.00",
// 0 -> "0.00", -55 -> "-0.55", -200 -> "-2.00".
static void calc_format_scaled(int v, char *buf){
    int neg=0; if(v < 0){ neg=1; v = -v; } // INT_MIN edge ignored (see range note)
    int ip = v/100, fp = v%100;
    char tmp[12]; int t=0;
    if(ip==0) tmp[t++]='0'; else while(ip>0){ tmp[t++]=(char)('0'+ip%10); ip/=10; }
    int p=0; if(neg) buf[p++]='-';
    while(t--) buf[p++]=tmp[t];
    buf[p++]='.';
    buf[p++]=(char)('0'+fp/10); buf[p++]=(char)('0'+fp%10);
    buf[p]=0;
}
// Parse display -> scaled int. Handles optional '-', optional '.' with up to 2
// fractional digits (pads short: "1.5" -> 150). Only called on numeric displays,
// never on "Error" (err flag guards those paths). Traces: "14" -> 1400,
// "1.55" -> 155, "10.00" -> 1000, "-2.00" -> -200.
static int calc_parse_scaled(struct calc_state *c){
    char *s = c->display; int i=0, neg=0, v=0;
    if(s[0]=='-'){ neg=1; i=1; }
    while(s[i] && s[i]!='.'){ v = v*10 + (s[i]-'0'); i++; }
    v *= 100;
    if(s[i]=='.'){ i++; int f=0, d=0;
        while(s[i] && d<2){ f = f*10 + (s[i]-'0'); i++; d++; }
        while(d<2){ f*=10; d++; }
        v += f;
    }
    return neg ? -v : v;
}
// Returns 1 and sets *out, or 0 on divide-by-zero (caller latches err).
// Add/sub: both operands x100, direct op stays x100 - no special handling.
// (700+300=1000 -> "10.00".)
// Mul: raw product is x10000, so /100 returns to x100.
// Trace 2.50*3.00: 250*300=75000, /100=750 -> "7.50" (not 75.00, not 0.075).
// Div: (a*100)/b lands back at x100 with 2 decimals instead of truncating.
// Trace 14/9: a=1400, b=900 -> 140000/900 = 155 (truncated, not rounded) -> "1.55".
static int calc_compute(int a, int op, int b, int *out){
    if(op==1) *out = a+b;
    else if(op==2) *out = a-b;
    else if(op==3) *out = (a*b)/100;
    else if(op==4){ if(b==0) return 0; *out = (a*100)/b; }
    else *out = b;
    return 1;
}
static void calc_reset(struct calc_state *c){
    c->display[0]='0'; c->display[1]='.'; c->display[2]='0'; c->display[3]='0'; c->display[4]=0; // "0.00"
    c->acc=0; c->op=0; c->fresh=1; c->err=0;
}
// Dispatch one Calculator button press (label-driven). Fixed-point two-operand,
// no precedence. Traces:
// "7+3=": 7->"7"; +: acc=700,op=+,fresh; 3->"3"; =: 700+300=1000 -> "10.00".
// "14/9=": acc=1400,op=/; 9; =: 140000/900=155 (truncate) -> "1.55".
// "5/0=": div0 -> err, "Error" (no crash/garbage). C: full reset -> "0.00".
static void calculator_handle_button(struct window *w, int b){
    struct calc_state *c = &w->calc;
    char lab = w->btns[b].label[0];
    if(lab=='C'){ calc_reset(c); }
    else if(lab>='0' && lab<='9'){
        if(c->err) calc_reset(c); // digit after Error starts over
        if(c->fresh){ c->display[0]=lab; c->display[1]=0; c->fresh=0; }
        else {
            // Entry is whole digits only; a '.' here means a result is showing
            // (shouldn't happen with fresh=0) - restart entry rather than corrupt.
            int hasdot=0; { int k=0; while(c->display[k]){ if(c->display[k]=='.') hasdot=1; k++; } }
            if(hasdot){ c->display[0]=lab; c->display[1]=0; }
            else {
                int len=0; while(c->display[len] && len<31) len++;
                if(len < 7){ c->display[len]=lab; c->display[len+1]=0; } // 7-digit cap: scaled fits int32
            }
        }
    }
    else if(lab=='+'||lab=='-'||lab=='*'||lab=='/'){
        if(c->err) return; // must C or digit first
        int k = (lab=='+')?1:(lab=='-')?2:(lab=='*')?3:4;
        int cur = calc_parse_scaled(c);
        if(!c->fresh && c->op!=0){
            // chain: "2+3+" computes 2+3 first -> acc=500, display "5.00", then new op
            int r;
            if(!calc_compute(c->acc, c->op, cur, &r)){ c->err=1; w_strcpy(c->display, "Error", 32); return; }
            c->acc = r; calc_format_scaled(r, c->display);
        } else if(!c->fresh && c->op==0){
            c->acc = cur;
        }
        // fresh with pending op (op pressed twice): just change op
        c->op = k; c->fresh = 1;
    }
    else if(lab=='='){
        if(c->err || c->op==0 || c->fresh) return; // nothing to compute
        int r;
        if(!calc_compute(c->acc, c->op, calc_parse_scaled(c), &r)){ c->err=1; w_strcpy(c->display, "Error", 32); return; }
        calc_format_scaled(r, c->display);
        c->acc = r; c->op = 0; c->fresh = 1;
    }
    dirty_add(w->x, w->y, w->w, w->h); // display + shading changes live inside this window
    s_puts("CALC: btn "); s_puts(w->btns[b].label); s_puts(" display "); s_puts(c->display); s_puts("\n");
}

// Calculator display sizing: box height grows with window (H*10/100 in
// [30,72]); digit scale is an integer multiple of the 8px base font so text
// visibly grows: scale=(dh-8)/16 in [1,4] -> dh=30:scale1, dh=40:scale2,
// dh=56:scale3. Grid top follows display bottom + 16.
// Trace default 264x304: dh=30, scale=1, y0=76, bw=60, bh=36 (unchanged).
// Trace 500x400: dh=40, scale=2 (16px digits), y0=86, bh=(400-40-90)/4=67.
static int calc_display_h(int wh){ int dh = wh*10/100; if(dh < 30) dh = 30; if(dh > 72) dh = 72; return dh; }
static int calc_display_scale(int dh){ int s = (dh-8)/16; if(s < 1) s = 1; if(s > 4) s = 4; return s; }
static int calc_grid_y0(int dh){ return 30 + dh + 16; }

// --- About This OS: freestanding number formatters (no snprintf in -nostdlib).
// about_u32 appends decimal digits of v to dst at *p (no NUL). about_u32w pads
// to at least w digits with leading zeros (clock fields). about_ip appends
// dotted-decimal a.b.c.d. All bounds-checked against cap.
static void about_u32(char *dst, int *p, int cap, uint32_t v){
    char rev[12]; int r = 0;
    if(v == 0) rev[r++] = '0';
    else while(v > 0 && r < 12){ rev[r++] = (char)('0' + v % 10); v /= 10; }
    while(r-- > 0 && *p < cap) dst[(*p)++] = rev[r];
}
static void about_u32w(char *dst, int *p, int cap, uint32_t v, int w){
    char rev[12]; int r = 0;
    if(v == 0) rev[r++] = '0';
    else while(v > 0 && r < 12){ rev[r++] = (char)('0' + v % 10); v /= 10; }
    while(r < w && *p < cap){ dst[(*p)++] = '0'; w--; } // leading zeros first
    while(r-- > 0 && *p < cap) dst[(*p)++] = rev[r];
}
static void about_ip(char *dst, int *p, int cap, uint8_t *a){
    for(int i=0;i<4;i++){
        about_u32(dst, p, cap, a[i]);
        if(i < 3 && *p < cap) dst[(*p)++] = '.';
    }
}

// Responsive layout: recompute AND STORE every widget rect from the window's
// CURRENT w/h at the start of every redraw. Hit-testing reads these same stored
// rects, so visuals and hit-test share one source of truth and cannot drift.
// (Between a resize and the next redraw, hit-test uses pre-resize rects - which
// still match the pre-resize pixels on screen. That is the correct invariant.)
// Math (traced):
// - Calculator (W x H, 4x4 grid, gap 8, sides 20, bottom margin 20):
//   bw=(W-64)/4, grid top y0=calc_grid_y0(dh), bh=(H-y0-44)/4.
//   At default 304x264: 60x36 at 20+c*68, 76+r*44.
// - Clicker (single button, proportional + centered): bw=W*30/100 in [120,320],
//   bh=H*10/100 in [30,64], x=(W-bw)/2, y=(H-bh)/2. At 400x300: 120x30@(140,135).
// - Notes (textbox): x=20, y=40, w=W-40; height YIELDS to the bottom-anchored
//   label row (labels at H-32): h = (H-32)-8-40 = H-80. At 400x300: 360x220.
//   Direction matters: labels anchor to the window bottom, textbox fills what
//   remains - so label visibility can never depend on textbox growth.
static void layout_sync_window(struct window *w){
    if(w->has_calc){
        int dh = calc_display_h(w->h);
        int y0 = calc_grid_y0(dh);
        int bw = (w->w - 64)/4; if(bw < 40) bw = 40;
        int bh = (w->h - y0 - 44)/4; if(bh < 24) bh = 24;
        int n = w->num_btns; if(n > MAX_BTNS) n = MAX_BTNS;
        for(int b=0;b<n;b++){
            int c = b%4, r = b/4;
            w->btns[b].x = 20 + c*(bw+8);
            w->btns[b].y = y0 + r*(bh+8);
            w->btns[b].w = bw; w->btns[b].h = bh;
        }
    } else if(w->has_button && !w->has_settings && !w->has_paint){
        // Clicker ONLY (single centered button). Settings/Paint also have_button=1
        // but keep fixed grids from init - excluded or btns[0] gets hijacked.
        int bw = w->w*30/100; if(bw < 120) bw = 120; if(bw > 320) bw = 320; if(bw > w->w-40) bw = w->w-40;
        int bh = w->h*10/100; if(bh < 30) bh = 30; if(bh > 64) bh = 64; if(bh > w->h-60) bh = w->h-60;
        if(bw < 1) bw = 1; if(bh < 1) bh = 1;
        w->btns[0].x = (w->w - bw)/2; w->btns[0].y = (w->h - bh)/2;
        w->btns[0].w = bw; w->btns[0].h = bh;
    }
    if(w->has_textbox){
        // Bottom-anchored labels first: 12px label row 20px above window bottom.
        // Textbox yields to it (h = labels_y - 8 gap - 40 top), never the reverse.
        int labels_y = w->h - 32;
        w->tbox.x = 20; w->tbox.y = 40;
        w->tbox.w = w->w - 40; if(w->tbox.w < 80) w->tbox.w = 80;
        w->tbox.h = labels_y - 8 - w->tbox.y; if(w->tbox.h < 40) w->tbox.h = 40;
    }
}

static void window_draw_single(int idx){
    struct window *w = &windows[idx];
    if(!w->visible || w->minimized) return;
    layout_sync_window(w); // responsive: recompute widget rects from CURRENT w/h first
    fb_draw_rect(w->x, w->y, w->w, w->h, w->bg_color);
    fb_draw_rect(w->x, w->y, w->w, TITLE_BAR_H, w->title_color);
    gfx_draw_rect_outline(w->x, w->y, w->w, w->h, w->border_color);
    gfx_draw_rect_outline(w->x, w->y, w->w, TITLE_BAR_H, w->border_color);
    gfx_draw_string(w->x+6, w->y+6, w->title, 0x00FFFFFF);
    window_draw_minimize_button(w);
    window_draw_close_button(w);
    window_draw_button(w);
    window_draw_textbox(w);
    // Live word count, Notes only, below its textbox (textbox at window-relative
    // 20,40 size 360x60, so bottom edge is y=100; "Words: N" goes at 20,108).
    // Counts words in the buffer (NOT task ticks - the removed in-window counter);
    // recomputed from scratch on every redraw, and every keystroke/backspace sets
    // g_needs_redraw, so the display is always live without storing count state.
    if(w->has_textbox && icon_streq(w->title, "Notes")){
        int wc = textbox_word_count(&w->tbox);
        char line[32]; const char *pfx = "Words: "; int p = 0;
        while(pfx[p]){ line[p] = pfx[p]; p++; }
        char tmp[12]; int t = 0, n = wc;
        if(n == 0) tmp[t++] = '0';
        else { char rev[12]; int r = 0; while(n > 0){ rev[r++] = (char)('0' + n % 10); n /= 10; } while(r > 0){ r--; tmp[t++] = rev[r]; } }
        for(int i = 0; i < t && p < 31; i++) line[p++] = tmp[i];
        line[p] = 0;
        // Anchored below the (live-resized) textbox, not at a fixed offset:
        // wy = tbox bottom + 8. At default 400x300 (tbox 360x220): y=268.
        int wx = w->x + w->tbox.x, wy = w->y + w->tbox.y + w->tbox.h + 8;
        fb_draw_rect(wx - 2, wy - 2, 120, 12, w->bg_color); // clear stale digits ("10" -> "9")
        gfx_draw_string(wx, wy, line, 0x00000000);
        // Session-persistence indicator: text is snapshotted on every close
        // and restored on reopen, until reboot (BSS buffer, no disk).
        gfx_draw_string(wx + 130, wy, "autosaved (session)", 0x00000000);
    }
    // Calculator display: white box at window-relative (20,30); width stretches
    // as W-40, height grows as calc_display_h (30 at default); digits render at
    // integer scale via gfx_draw_string_scaled, right-aligned + vertically centered.
    if(w->has_calc){
        int dx = w->x + 20, dy = w->y + 30, dw = w->w - 40, dh = calc_display_h(w->h);
        if(dw < 80) dw = 80;
        fb_draw_rect(dx, dy, dw, dh, 0x00FFFFFF);
        gfx_draw_rect_outline(dx, dy, dw, dh, 0x00000000);
        int dlen = 0; while(w->calc.display[dlen] && dlen < 32) dlen++;
        int sc = calc_display_scale(dh);
        gfx_draw_string_scaled(dx + dw - 4 - dlen*8*sc, dy + (dh-8*sc)/2, w->calc.display, 0x00000000, sc);
    }
        // Paint canvas blit (palette buttons drawn by generic window_draw_button above).
    if(w->has_paint){
        paint_blit(w);
    }
    // Settings sections: titles + selection highlight. Active option read live    // from wallpaper_preset / tz_offset_hours (no duplicate state): green double
    // outline around the selected button in each section. Setters flag redraw,
    // so highlight follows clicks immediately with no extra frames.
    if(w->has_settings){
        gfx_draw_string(w->x+20, w->y+32, "Wallpaper:", 0x00000000);
        gfx_draw_string(w->x+20, w->y+102, "Timezone:", 0x00000000);
        int n = w->num_btns; if(n > MAX_BTNS) n = MAX_BTNS;
        for(int b=0;b<n;b++){
            int active = (b < 4) ? (wallpaper_preset == b)
                                 : (tz_offset_hours == tz_preset_offsets[b-4]);
            if(!active) continue;
            int ax = w->x + w->btns[b].x, ay = w->y + w->btns[b].y;
            int bw = w->btns[b].w, bh = w->btns[b].h;
            gfx_draw_rect_outline(ax-2, ay-2, bw+4, bh+4, 0x00106428);
            gfx_draw_rect_outline(ax-1, ay-1, bw+2, bh+2, 0x0016A34A);
        }
    }
    // About This OS: live read-only stats, NO background task (purely reactive
    // like Calculator: every value is read from its owner at draw time, no
    // duplicate state stored here). Refreshed by the same periodic
    // window_invalidate in kernel.c's main loop that drives Task Manager.
    if(w->has_about){
        extern const char *about_build_stamp(void);
        extern int pit_get_ticks(void);
        extern void pit_get_task_ticks(int *gui, int *a, int *b);
        extern void pit_get_cpu_percent(int *gui_pct, int *a_pct, int *b_pct);
        extern int task_exists(int id);
        extern int net_configured;
        extern uint8_t net_our_ip[4];
        extern uint8_t net_gw[4];
        int tx = w->x + 12;
        int ty = w->y + 32;
        const int step = 14;
        char line[64]; int p;
        // Build stamp (single source: kernel.c accessor, same string as boot log).
        p = 0; { const char *pfx="Build: "; while(*pfx && p<63) line[p++]=*pfx++;
            const char *s = about_build_stamp(); while(*s && p<63) line[p++]=*s++; }
        line[p]=0; gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
        // Uptime from PIT (100Hz): total seconds -> H:MM:SS, hours unbounded.
        { uint32_t total = (uint32_t)pit_get_ticks() / 100;
          p = 0; { const char *pfx="Uptime: "; while(*pfx && p<63) line[p++]=*pfx++; }
          about_u32(line, &p, 63, total / 3600);
          if(p<63) line[p++]=':';
          about_u32w(line, &p, 63, (total / 60) % 60, 2);
          if(p<63) line[p++]=':';
          about_u32w(line, &p, 63, total % 60, 2);
          line[p]=0; }
        gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
        // Resolution (live framebuffer size).
        { p = 0; { const char *pfx="Res: "; while(*pfx && p<63) line[p++]=*pfx++; }
          about_u32(line, &p, 63, fb_get_width());
          if(p<63) line[p++]='x';
          about_u32(line, &p, 63, fb_get_height());
          line[p]=0; }
        gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
        // PMM frames (owner: pmm.c counters, same values as boot log).
        { p = 0; { const char *pfx="Mem: "; while(*pfx && p<63) line[p++]=*pfx++; }
          about_u32(line, &p, 63, pmm_free_frames());
          { const char *m=" free / "; while(*m && p<63) line[p++]=*m++; }
          about_u32(line, &p, 63, pmm_total_frames());
          { const char *m=" total frames"; while(*m && p<63) line[p++]=*m++; }
          line[p]=0; }
        gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
        // CPU ticks + percent (owner: PIT per-task counters, same source as
        // Task Manager; killed tasks omitted the same way).
        { int gui=0,a=0,b=0,gp=0,ap=0,bp=0;
          pit_get_task_ticks(&gui,&a,&b); pit_get_cpu_percent(&gp,&ap,&bp);
          p = 0; { const char *pfx="CPU GUI "; while(*pfx && p<63) line[p++]=*pfx++; }
          about_u32(line, &p, 63, (uint32_t)gui);
          { const char *m=" ("; while(*m && p<63) line[p++]=*m++; }
          about_u32(line, &p, 63, (uint32_t)gp);
          { const char *m="%)"; while(*m && p<63) line[p++]=*m++; }
          line[p]=0; gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
          if(task_exists(1)){
              p = 0; { const char *pfx="CPU Clicker "; while(*pfx && p<63) line[p++]=*pfx++; }
              about_u32(line, &p, 63, (uint32_t)a);
              { const char *m=" ("; while(*m && p<63) line[p++]=*m++; }
              about_u32(line, &p, 63, (uint32_t)ap);
              { const char *m="%)"; while(*m && p<63) line[p++]=*m++; }
              line[p]=0; gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
          }
          if(task_exists(2)){
              p = 0; { const char *pfx="CPU Notes "; while(*pfx && p<63) line[p++]=*pfx++; }
              about_u32(line, &p, 63, (uint32_t)b);
              { const char *m=" ("; while(*m && p<63) line[p++]=*m++; }
              about_u32(line, &p, 63, (uint32_t)bp);
              { const char *m="%)"; while(*m && p<63) line[p++]=*m++; }
              line[p]=0; gfx_draw_string(tx, ty, line, 0x00000000); ty += step;
          }
        }
        // Network (owner: DHCP globals in rtl8139.c; no copy stored here).
        if(net_configured){
            p = 0; { const char *pfx="Net: "; while(*pfx && p<63) line[p++]=*pfx++; }
            about_ip(line, &p, 63, net_our_ip);
            { const char *m=" gw "; while(*m && p<63) line[p++]=*m++; }
            about_ip(line, &p, 63, net_gw);
            line[p]=0;
        } else {
            p = 0; { const char *m="Net: Not connected"; while(*m && p<63) line[p++]=*m++; }
            line[p]=0;
        }
        gfx_draw_string(tx, ty, line, 0x00000000);
    }
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
    // NOTE: task counters (w->task_counter, updated by Clicker/Notes tasks) are no
    // longer drawn inside the windows themselves - they are only visible in Task
    // Manager, which reads PIT tick/CPU% counters directly. Counting still happens.
    {
        int rx = w->x + w->w - RESIZE_HANDLE;
        int ry = w->y + w->h - RESIZE_HANDLE;
        fb_draw_rect(rx, ry, RESIZE_HANDLE, RESIZE_HANDLE, 0x00888888);
        gfx_draw_rect_outline(rx, ry, RESIZE_HANDLE, RESIZE_HANDLE, 0x00000000);
        gfx_draw_line(rx+4, ry+8, rx+8, ry+4, 0x00000000);
        gfx_draw_line(rx+6, ry+8, rx+8, ry+6, 0x00000000);
    }
}

// Taskbar clock base: RTC seconds-since-midnight, seeded once at boot
// (host/UTC as-is, no timezone conversion). PIT ticks advance from there;
// never re-read (RTC read is boot-only). Unset/negative -> 00:00:00 base.
static int clock_base_sec = 0;
void clock_set_base_seconds(int s){
    clock_base_sec = (s < 0) ? 0 : s;
}
int clock_current_seconds(void){
    // % 86400 wraps past midnight; int % is a single DIV, no libgcc needed.
    return (clock_base_sec + pit_get_ticks() / 100) % 86400;
}
// Timezone display offset: fixed presets, no DST logic, no tz database.
// Presets: UTC+0, +10 Sydney (std; +11 DST not modeled), -5 US Eastern (std),
// +9 Tokyo (no DST). Labels generic ("UTC+10") by design.
// Wraparound traces (mod-then-fix, since C % keeps sign of dividend):
// UTC 23:30 (84600) +10h: (84600+36000)=120600 % 86400 = 34200 -> 09:30 next day.
// UTC 02:00 (7200) -5h: (7200-18000)=-10800 % 86400 = -10800 -> +86400 = 75600 -> 21:00.
// UTC 00:00 +0 -> 0; 23:59:59 (86399) +1h -> 89999%86400=3599 -> 00:59:59.
int tz_offset_hours = 0; // BSS zero = UTC at boot
static int tz_preset_idx = 0;
int clock_apply_tz(int utc_sec, int off_hours){
    int local = (utc_sec + off_hours * 3600) % 86400;
    if(local < 0) local += 86400;
    return local;
}
void timezone_set_offset(int hours){
    if(hours < -12 || hours > 14) return; // outside all real zones: ignore
    tz_offset_hours = hours;
    tz_preset_idx = 0;
    for(int i=0;i<TIMEZONE_PRESET_COUNT;i++) if(tz_preset_offsets[i]==hours) tz_preset_idx = i;
    s_puts("TZ: now "); s_puts(timezone_label()); s_puts("\n");
    // No flag here: caller (Settings button) dirties its window + clock area.
}
void timezone_cycle(void){
    tz_preset_idx = (tz_preset_idx + 1) % TIMEZONE_PRESET_COUNT;
    timezone_set_offset(tz_preset_offsets[tz_preset_idx]);
}
const char *timezone_label(void){
    // "UTC", "UTC+10", "UTC-5" into a static buffer (display + serial share it;
    // use immediately, same pattern as other static scratch in this file).
    static char buf[8];
    int o = tz_offset_hours;
    if(o == 0){ buf[0]='U'; buf[1]='T'; buf[2]='C'; buf[3]=0; return buf; }
    buf[0]='U'; buf[1]='T'; buf[2]='C'; buf[3]=(o<0)?'-':'+';
    int v = (o<0)?-o:o, p = 4;
    char tmp[4]; int t=0;
    while(v>0){ tmp[t++]=(char)('0'+v%10); v/=10; }
    while(t--) buf[p++]=tmp[t];
    buf[p]=0;
    return buf;
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
    // Taskbar clock: HH:MM:SS from RTC seed + PIT advance (see clock_base_sec).
    // ticks/100 = seconds; two digits each with leading zeros ("02:07:05").
    // Right-aligned ending 10px left of "+": 8 chars x 8px = 64px wide.
    // No overlap by construction: 8 tabs max end at 5+8*155-5=1240, clock starts
    // ~1811 at 1920 wide; "+" hit-test starts at plus_x, 10px right of clock end.
    {
        int total = clock_apply_tz(clock_current_seconds(), tz_offset_hours); // UTC base + display offset, wraps at midnight
        int ss = total % 60, mm = (total / 60) % 60, hh = total / 3600;
        char clk[9];
        clk[0]=(char)('0'+(hh/10)%10); clk[1]=(char)('0'+hh%10); clk[2]=':';
        clk[3]=(char)('0'+(mm/10)%10); clk[4]=(char)('0'+mm%10); clk[5]=':';
        clk[6]=(char)('0'+(ss/10)%10); clk[7]=(char)('0'+ss%10); clk[8]=0;
        int plus_x = fb_w - 30 - 5;
        gfx_draw_string(plus_x - 10 - 8*8, ty + 3 + 6, clk, 0x00FFFFFF);
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
    desktop_icons[4].x = 20; desktop_icons[4].y = 440; w_strcpy(desktop_icons[4].label, "Calculator", 32); desktop_icons[4].color = 0x00226644; desktop_icons[4].selected = 0;
    desktop_icons[5].x = 20; desktop_icons[5].y = 540; w_strcpy(desktop_icons[5].label, "Settings", 32); desktop_icons[5].color = 0x00334155; desktop_icons[5].selected = 0;
    desktop_icons[6].x = 20; desktop_icons[6].y = 640; w_strcpy(desktop_icons[6].label, "Paint", 32); desktop_icons[6].color = 0x00772222; desktop_icons[6].selected = 0;
    desktop_icons[7].x = 20; desktop_icons[7].y = 740; w_strcpy(desktop_icons[7].label, "About", 32); desktop_icons[7].color = 0x001F4E79; desktop_icons[7].selected = 0;
    desktop_icon_count = 8;
    s_puts("DESKTOP: icons init 8 at (20,40) New Window, (20,140) Task Manager, (20,240) Clicker, (20,340) Notes, (20,440) Calculator, (20,540) Settings, (20,640) Paint, (20,740) About\n");
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
// Task Manager icon: frosted-glass squircle tile with etched grid and a glowing
// blue performance graph (sharp peaks, bright node dot at the peak - a nod to
// the original blue dot). Solid primitives only; squircle corners and the border
// are faked with narrowed rows since there is no alpha blending.
static void draw_taskmgr_icon(int gx, int gy){
    uint32_t border = 0x001B2A3A;  // dark tile border
    uint32_t glass = 0x0035608A;   // steel-blue frosted glass
    uint32_t glass_hi = 0x008FB8D8;// glassy top highlight
    uint32_t etched = 0x002A4A6B;  // etched grid lines
    uint32_t graph = 0x0038BDF8;   // vibrant performance line
    uint32_t glow = 0x007DD3FC;    // node glow ring
    uint32_t shadow = 0x00101C2A;  // drop shadow
    int tx = gx + 4, ty = gy + 4;  // 24x24 tile centered in the 32x32 canvas
    // drop shadow squircle (offset down-right)
    fb_draw_rect(tx+1, ty+1, 26, 26, shadow);
    // dark border squircle (one px larger all around the glass)
    fb_draw_rect(tx-1+5, ty-1, 16, 1, border);
    fb_draw_rect(tx-1+3, ty, 20, 1, border);
    fb_draw_rect(tx-1+2, ty+1, 22, 1, border);
    fb_draw_rect(tx-1+1, ty+2, 24, 1, border);
    fb_draw_rect(tx-1, ty+3, 26, 18, border);
    fb_draw_rect(tx-1+1, ty+21, 24, 1, border);
    fb_draw_rect(tx-1+2, ty+22, 22, 1, border);
    fb_draw_rect(tx-1+3, ty+23, 20, 1, border);
    fb_draw_rect(tx-1+5, ty+24, 16, 1, border);
    // glass squircle
    fb_draw_rect(tx+4, ty, 16, 1, glass);
    fb_draw_rect(tx+2, ty+1, 20, 1, glass);
    fb_draw_rect(tx+1, ty+2, 22, 1, glass);
    fb_draw_rect(tx, ty+3, 24, 18, glass);
    fb_draw_rect(tx+1, ty+21, 22, 1, glass);
    fb_draw_rect(tx+2, ty+22, 20, 1, glass);
    fb_draw_rect(tx+4, ty+23, 16, 1, glass);
    // glossy top highlight strip
    fb_draw_rect(tx+3, ty+3, 18, 2, glass_hi);
    // etched grid
    fb_draw_rect(tx+9, ty+5, 1, 14, etched);
    fb_draw_rect(tx+15, ty+5, 1, 14, etched);
    fb_draw_rect(tx+4, ty+10, 16, 1, etched);
    fb_draw_rect(tx+4, ty+15, 16, 1, etched);
    // performance graph (2px thick polyline with sharp peaks)
    gfx_draw_line(tx+3, ty+16, tx+6, ty+12, graph);
    gfx_draw_line(tx+3, ty+17, tx+6, ty+13, graph);
    gfx_draw_line(tx+6, ty+12, tx+9, ty+14, graph);
    gfx_draw_line(tx+6, ty+13, tx+9, ty+15, graph);
    gfx_draw_line(tx+9, ty+14, tx+12, ty+6, graph);
    gfx_draw_line(tx+9, ty+15, tx+12, ty+7, graph);
    gfx_draw_line(tx+12, ty+6, tx+15, ty+11, graph);
    gfx_draw_line(tx+12, ty+7, tx+15, ty+12, graph);
    gfx_draw_line(tx+15, ty+11, tx+18, ty+9, graph);
    gfx_draw_line(tx+15, ty+12, tx+18, ty+10, graph);
    gfx_draw_line(tx+18, ty+9, tx+20, ty+13, graph);
    gfx_draw_line(tx+18, ty+10, tx+20, ty+14, graph);
    // glowing node dot at the peak (the original blue dot, kept as tribute)
    gfx_draw_filled_circle(tx+12, ty+6, 3, glow);
    gfx_draw_filled_circle(tx+12, ty+6, 2, graph);
    fb_draw_rect(tx+12, ty+6, 1, 1, 0x00FFFFFF);
}
// Calculator icon: slate calculator body with white display strip and a 3x3
// grid of mint key dots, plus solid offset drop shadow. Same 32x32 canvas
// and solid-primitive style as the Notes/Clicker glyphs.
static void draw_calc_icon(int gx, int gy){
    uint32_t body = 0x002F3B4C;     // slate body
    uint32_t edge = 0x00000000;     // outline
    uint32_t disp = 0x00F1F5F9;     // display strip
    uint32_t mint = 0x006EE7B7;     // keys
    uint32_t eqkey = 0x00F59E0B;    // amber = key accent
    uint32_t shadow = 0x00141824;   // drop shadow
    int bx = gx + 6, by = gy + 3;   // 20x26 body centered in canvas
    fb_draw_rect(bx+2, by+2, 20, 26, shadow);
    fb_draw_rect(bx, by, 20, 26, body);
    gfx_draw_rect_outline(bx, by, 20, 26, edge);
    // display strip
    fb_draw_rect(bx+3, by+3, 14, 5, disp);
    // 3x3 key grid (last key amber)
    for(int r=0;r<3;r++) for(int c=0;c<3;c++){
        int kx = bx+3+c*5, ky = by+11+r*5;
        uint32_t kc = (r==2 && c==2) ? eqkey : mint;
        fb_draw_rect(kx, ky, 3, 3, kc);
    }
}
// Settings icon: dark slate panel with 3 slider rows (track + knob squares)
// in mint/amber/sky accents, plus solid offset drop shadow. Same 32x32 canvas
// and solid-primitive style as the other glyphs.
static void draw_settings_icon(int gx, int gy){
    uint32_t body = 0x00334155;     // slate panel
    uint32_t edge = 0x00000000;
    uint32_t track = 0x0094A3B8;    // slider tracks
    uint32_t shadow = 0x00141824;
    int bx = gx + 6, by = gy + 5;   // 20x22 panel centered in canvas
    fb_draw_rect(bx+2, by+2, 20, 22, shadow);
    fb_draw_rect(bx, by, 20, 22, body);
    gfx_draw_rect_outline(bx, by, 20, 22, edge);
    static const uint32_t knobs[3] = {0x006EE7B7, 0x00F59E0B, 0x0038BDF8};
    for(int r=0;r<3;r++){
        int ry = by + 4 + r*6;
        fb_draw_rect(bx+3, ry+1, 14, 2, track);
        fb_draw_rect(bx+3 + (r*5+2), ry, 4, 4, knobs[r]);
    }
}
// Paint icon: white canvas sheet with red/green/blue paint dots and a brush
// diagonal (brown handle, dark tip), plus solid offset drop shadow. Same 32x32
// canvas and solid-primitive style as the other glyphs.
static void draw_paint_icon(int gx, int gy){
    uint32_t paper = 0x00F9F9FB;
    uint32_t edge = 0x00000000;
    uint32_t shadow = 0x00141824;
    uint32_t handle = 0x008B5A2B;
    int px = gx + 7, py = gy + 4; // 18x24 sheet centered in canvas
    fb_draw_rect(px+2, py+2, 18, 24, shadow);
    fb_draw_rect(px, py, 18, 24, paper);
    gfx_draw_rect_outline(px, py, 18, 24, edge);
    // paint dots: red/green/blue/yellow
    fb_draw_rect(px+3, py+3, 4, 4, 0x00FF0000);
    fb_draw_rect(px+11, py+3, 4, 4, 0x0000AA00);
    fb_draw_rect(px+3, py+11, 4, 4, 0x000000FF);
    fb_draw_rect(px+11, py+11, 4, 4, 0x00FFD800);
    // brush diagonal: brown handle + dark tip
    gfx_draw_line(px+2, py+22, px+14, py+10, handle);
    gfx_draw_line(px+3, py+22, px+15, py+10, handle);
    gfx_draw_line(px+14, py+10, px+16, py+8, 0x00222222);
    fb_draw_rect(px+16, py+8, 1, 1, 0x00222222);
}
// About icon: navy badge tile with a white info disc and navy "i".
// Same 32x32 canvas and solid-primitive style as the other glyphs.
static void draw_about_icon(int gx, int gy){
    uint32_t tile = 0x001F4E79;     // navy badge tile
    uint32_t edge = 0x00000000;
    uint32_t disc = 0x00FFFFFF;     // info disc
    uint32_t shadow = 0x00141824;   // drop shadow
    int bx = gx + 4, by = gy + 4;   // 24x24 tile centered in canvas
    fb_draw_rect(bx+2, by+2, 24, 24, shadow);
    fb_draw_rect(bx, by, 24, 24, tile);
    gfx_draw_rect_outline(bx, by, 24, 24, edge);
    gfx_draw_filled_circle(bx+12, by+13, 8, disc);
    gfx_draw_string(bx+9, by+8, "i", tile);
}
void desktop_icons_draw(void){
    if(!fb_is_available()) return;
    for(int i=0;i<desktop_icon_count;i++){
        struct desktop_icon *ic = &desktop_icons[i];
        int ix = ic->x; int iy = ic->y;
        // Skip icons outside the dirty region (label overflows the 64px box).
        if(!rects_overlap(ix-16, iy, 96, 80, dr_x0, dr_y0, dr_x1, dr_y1)) continue;
        int gx = ix + (ICON_W - ICON_GLYPH)/2; int gy = iy + 4;
        if(i==0){ fb_draw_rect(gx, gy, ICON_GLYPH, ICON_GLYPH, ic->color); gfx_draw_rect_outline(gx, gy, ICON_GLYPH, ICON_GLYPH, 0x00000000); gfx_draw_string(gx+12, gy+12, "+", 0x00FFFFFF); }
        else if(i==1){ draw_taskmgr_icon(gx, gy); }
        else if(i==2){ draw_clicker_icon(gx, gy); }
        else if(i==3){ draw_notes_icon(gx, gy); }
        else if(i==4){ draw_calc_icon(gx, gy); }
        else if(i==5){ draw_settings_icon(gx, gy); }
        else if(i==6){ draw_paint_icon(gx, gy); }
        else if(i==7){ draw_about_icon(gx, gy); }
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
    // Dirty deselected icons' rects (fixes stale highlight with no full frame).
    for(int i=0;i<desktop_icon_count;i++) if(desktop_icons[i].selected)
        dirty_add(desktop_icons[i].x-16, desktop_icons[i].y, 96, 80);
    int had=0; for(int i=0;i<desktop_icon_count;i++) if(desktop_icons[i].selected) had=1;
    for(int i=0;i<desktop_icon_count;i++) desktop_icons[i].selected=0; selected_icon=-1;
    if(had){ s_puts("DESKTOP: deselect all\n"); } // flag already set by dirty_add above
}
static int icon_streq(const char *a, const char *b){ int i=0; while(a[i] && b[i] && a[i]==b[i]) i++; return a[i]==0 && b[i]==0; }
int desktop_icon_handle_click(int x, int y){
    int idx = desktop_icon_hit_test(x,y); if(idx==-1) return 0;
    int now = pit_get_ticks(); s_puts("DESKTOP: click icon "); s_put_dec(idx); s_puts(" '"); s_puts(desktop_icons[idx].label); s_puts("' tick "); s_put_dec(now); s_puts("\n");
    if(last_click_icon==idx && (now - last_click_tick) < dbl_click_thresh){
        s_puts("DESKTOP: double-click icon "); s_put_dec(idx); s_puts("\n");
        for(int i=0;i<desktop_icon_count;i++) desktop_icons[i].selected = (i==idx); selected_icon = idx; last_click_icon = -1; last_click_tick = -1000;
        if(idx==0){ s_puts("DESKTOP: action New Window\n"); window_create_new(); }
        else if(idx==1){ int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Task Manager")) { found=i; break; } if(found!=-1){ s_puts("DESKTOP: action Task Manager bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; s_puts("DESKTOP: unminimize Task Manager\n"); taskbar_tabs_invalidate(); window_invalidate(found); } window_bring_to_front(found); } else { s_puts("DESKTOP: action Task Manager create (was closed)\n"); if(window_count < MAX_WINDOWS){ int nid = window_count; windows[nid].x=600; windows[nid].y=100; windows[nid].w=300; windows[nid].h=200; w_strcpy(windows[nid].title, "Task Manager", 32); windows[nid].bg_color=0x00F0F0F0; windows[nid].title_color=0x00333333; windows[nid].border_color=0x00000000; windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=0; windows[nid].num_btns=0; windows[nid].has_textbox=0; windows[nid].has_calc=0; windows[nid].has_settings=0; windows[nid].has_paint=0; windows[nid].has_about=0; windows[nid].task_counter=0; z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i; s_puts("DESKTOP: created Task Manager\n"); window_invalidate(nid); taskbar_tabs_invalidate(); } else s_puts("DESKTOP: cannot create Task Manager - at max\n"); } }
        else if(idx==2){ // Clicker
            int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Clicker")) { found=i; break; }
            if(found!=-1){ s_puts("DESKTOP: action Clicker bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; taskbar_tabs_invalidate(); window_invalidate(found); } window_bring_to_front(found); }
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
                    windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=1; windows[nid].num_btns=1;
                    windows[nid].btns[0].x=20; windows[nid].btns[0].y=40; windows[nid].btns[0].w=120; windows[nid].btns[0].h=30; w_strcpy(windows[nid].btns[0].label, "Click Me", 32); windows[nid].btns[0].pressed=0; windows[nid].btns[0].clicks=0;
                    windows[nid].has_textbox=0; windows[nid].has_calc=0; windows[nid].has_settings=0; windows[nid].has_paint=0; windows[nid].has_about=0; windows[nid].task_counter=0;
                    z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
                    s_puts("DESKTOP: created Clicker\n"); window_invalidate(nid); taskbar_tabs_invalidate();
                } else s_puts("DESKTOP: cannot create Clicker - at max\n");
            }
        }
        else if(idx==3){ // Notes
            int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Notes")) { found=i; break; }
            if(found!=-1){ s_puts("DESKTOP: action Notes bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; taskbar_tabs_invalidate(); window_invalidate(found); } window_bring_to_front(found); }
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
                    windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=0; windows[nid].num_btns=0; windows[nid].has_textbox=1; windows[nid].has_calc=0; windows[nid].has_settings=0; windows[nid].has_paint=0; windows[nid].has_about=0;
                    windows[nid].tbox.x=20; windows[nid].tbox.y=40; windows[nid].tbox.w=360; windows[nid].tbox.h=60; windows[nid].tbox.max_len=512; notes_restore_to(nid); // placeholder rect: layout_sync recomputes; session text (empty on fresh boot)
                    windows[nid].task_counter=0;
                    z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
                    s_puts("DESKTOP: created Notes\n"); window_invalidate(nid); taskbar_tabs_invalidate();
                } else s_puts("DESKTOP: cannot create Notes - at max\n");
            }
        }
        else if(idx==4){ // Calculator - window only, NO background task (purely reactive)
            int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "Calculator")) { found=i; break; }
            if(found!=-1){ s_puts("DESKTOP: action Calculator bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; taskbar_tabs_invalidate(); window_invalidate(found); } window_bring_to_front(found); }
            else {
                s_puts("DESKTOP: action Calculator create (was closed)\n");
                if(window_count < MAX_WINDOWS){
                    int nid = window_count;
                    calculator_init_window(nid);
                    z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
                    s_puts("DESKTOP: created Calculator\n"); window_invalidate(nid); taskbar_tabs_invalidate();
                } else s_puts("DESKTOP: cannot create Calculator - at max\n");
            }
        }
        else if(idx==5){ // Settings - window only, NO background task (purely reactive)
            settings_open_or_focus();
        }
        else if(idx==6){ // Paint - window only, NO background task (purely reactive)
            paint_open_or_focus();
        }
        else if(idx==7){ // About - window only, NO background task (purely reactive like Calculator)
            int found=-1; for(int i=0;i<window_count;i++) if(icon_streq(windows[i].title, "About")) { found=i; break; }
            if(found!=-1){ s_puts("DESKTOP: action About bring to front\n"); if(windows[found].minimized){ windows[found].minimized=0; taskbar_tabs_invalidate(); window_invalidate(found); } window_bring_to_front(found); }
            else {
                s_puts("DESKTOP: action About create (was closed)\n");
                if(window_count < MAX_WINDOWS){
                    int nid = window_count;
                    windows[nid].x=640; windows[nid].y=140; windows[nid].w=360; windows[nid].h=250;
                    w_strcpy(windows[nid].title, "About", 32);
                    windows[nid].bg_color=0x00F5F5F0; windows[nid].title_color=0x001F4E79; windows[nid].border_color=0x00000000;
                    windows[nid].visible=1; windows[nid].minimized=0; windows[nid].z=window_count; windows[nid].has_button=0; windows[nid].num_btns=0;
                    windows[nid].has_textbox=0; windows[nid].has_calc=0; windows[nid].has_settings=0; windows[nid].has_paint=0; windows[nid].has_about=1; windows[nid].task_counter=0;
                    z_order[window_count]=nid; window_count++; for(int i=0;i<window_count;i++) windows[z_order[i]].z=i;
                    s_puts("DESKTOP: created About\n"); window_invalidate(nid); taskbar_tabs_invalidate();
                } else s_puts("DESKTOP: cannot create About - at max\n");
            }
        }
        // No flag: every create/bring sub-action above invalidates its own rect.
        return 1;
    } else {
        for(int i=0;i<desktop_icon_count;i++) desktop_icons[i].selected = (i==idx); selected_icon = idx; last_click_icon = idx; last_click_tick = now; s_puts("DESKTOP: select icon "); s_put_dec(idx); s_puts("\n"); dirty_add(desktop_icons[idx].x-16, desktop_icons[idx].y, 96, 80); return 1;
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
    windows[0].num_btns = 1; // single button at index 0 (was struct button btn)
    windows[0].has_textbox = 0; windows[0].has_calc = 0; windows[0].has_settings = 0; windows[0].has_paint = 0; windows[0].has_about = 0;
    windows[0].btns[0].x = 20; windows[0].btns[0].y = 40; windows[0].btns[0].w = 120; windows[0].btns[0].h = 30;
    w_strcpy(windows[0].btns[0].label, "Click Me", 32);
    windows[0].btns[0].pressed = 0;
    windows[0].btns[0].clicks = 0;

    windows[1].x = 250; windows[1].y = 180; windows[1].w = 400; windows[1].h = 300;
    w_strcpy(windows[1].title, "Notes", 32);
    windows[1].bg_color = 0x00D0D0FF; // light blue
    windows[1].title_color = 0x00993333; // reddish
    windows[1].border_color = 0x00000000;
    windows[1].visible = 1;
    windows[1].z = 1;
    windows[1].has_button = 0;
    windows[1].num_btns = 0;
    windows[1].has_calc = 0;
    windows[1].has_settings = 0;
    windows[1].has_paint = 0;
    windows[1].has_about = 0;
    windows[1].has_textbox = 1;
    windows[1].tbox.x = 20; windows[1].tbox.y = 40; windows[1].tbox.w = 360; windows[1].tbox.h = 60; // placeholder: layout_sync_window recomputes from w/h on first redraw (-> 360x220 at 400x300)
    windows[1].tbox.max_len = 512;
    windows[1].tbox.len = 0;
    windows[1].tbox.buffer[0]=0;
    windows[1].tbox.focused = 0;
    windows[1].tbox.cursor_visible = 1;
    windows[1].tbox.blink_counter = 0;
    windows[0].task_counter = 0;
    windows[1].task_counter = 0;

    windows[2].x = 0; windows[2].y = 0; windows[2].w = 0; windows[2].h = 0;
    windows[2].visible = 0; windows[2].minimized = 0; windows[2].has_button = 0; windows[2].num_btns = 0; windows[2].has_textbox = 0; windows[2].has_calc = 0; windows[2].has_settings = 0; windows[2].has_paint = 0; windows[2].has_about = 0; windows[2].task_counter = 0;
    windows[3].x = 0; windows[3].y = 0; windows[3].w = 0; windows[3].h = 0;
    windows[3].visible = 0; windows[3].minimized = 0; windows[3].has_button = 0; windows[3].num_btns = 0; windows[3].has_textbox = 0; windows[3].has_calc = 0; windows[3].has_settings = 0; windows[3].has_paint = 0; windows[3].has_about = 0; windows[3].task_counter = 0;

    window_count = 2;
    // z_order 0..1 back->front corresponds to windows index order initially
    for(int i=0;i<window_count;i++) z_order[i]=i;

    desktop_icons_init();
    s_puts("WM: created 2 windows (Clicker button, Notes textbox) + 8 desktop icons\n");
    // Build wallpaper cache once (draws Bliss then snapshots, measures flat vs Bliss vs blit)
    wallpaper_cache_build_once();
    // Boot paints full screen directly (not via do_redraw): explicit full bounds.
    dr_x0=0; dr_y0=0; dr_x1=(int)fb_get_width(); dr_y1=(int)fb_get_height();
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
    windows[idx].num_btns = 0;
    windows[idx].has_textbox = 0;
    windows[idx].has_calc = 0;
    windows[idx].has_settings = 0;
    windows[idx].has_paint = 0;
    windows[idx].has_about = 0;
    windows[idx].task_counter = 0;
    z_order[window_count] = idx;
    window_count++;
    for(int i=0;i<window_count;i++) windows[z_order[i]].z = i;
    s_puts("WM: created Window "); s_put_dec(idx+1); s_puts(" at "); s_put_dec(base_x); s_putc(','); s_put_dec(base_y);
    s_puts(" count now "); s_put_dec(window_count); s_puts("\n");
    window_invalidate(idx); // new window bounds (only new pixels + covered-behind)
    taskbar_tabs_invalidate(); // new tab appended to the strip
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
    uint32_t vaddr = 0x01600000; // 22MB: after fb_back 0x00C00000+8.3M=0x014E9000; paint canvas at 0x02000000 (32M) stays clear (ends ~30.2MB)
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
    // Palette for current preset (guarded: preset is validated by setter,
    // but a stray write must never index out of bounds here).
    int pi = wallpaper_preset;
    if(pi < 0 || pi >= WALLPAPER_PRESET_COUNT) pi = 0;
    const struct wallpaper_palette *pal = &wallpaper_palettes[pi];
    // Sky gradient - preset top/bottom colors, same vertical per-row lerp
    uint32_t sky_top = pal->sky_top;
    uint32_t sky_bot = pal->sky_bot;
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
    fb_draw_rect(0, sky_h, w, h - sky_h, pal->sky_bot); // sky_bot solid under hills
    // Hills will overwrite this sky_bot area from far_y/near_y down, leaving only visible hill silhouette
    // Sun (Day/Sunset) or moon (Night) - same geometry, palette colors/sizes
    int sun_x = w - 224; if(sun_x < 0) sun_x = w*3/4;
    int sun_y = pal->sun_near_horizon ? sky_h + 30 : 120;
    gfx_draw_filled_circle(sun_x, sun_y, pal->sun_r_outer, pal->sun_outer);
    gfx_draw_filled_circle(sun_x, sun_y, pal->sun_r_inner, pal->sun_inner);

    // Hills - two gentle layers for depth, low frequency for wide rolling (Bliss has 2-3 broad curves, not sawtooth)
    // Trace addresses: hill_base = sky_h = h*60/100, sky GRAD filled y=0..sky_h-1, hills fill y=far_y..h-1
    // Previously period 128 (8 hills) and 85 (12 hills) => sawtooth. Now period scaled with width to keep visual: 600/400 at 1024 => 1.71/2.56 hills
    // At 1920, periods 1125/750 keep same 1.71/2.56 visual (1920/1125≈1.71). Phase = x*256/period &0xFF, hill_y = sky_h + baseOff + amp*sin/128
    // Address trace: fb at 0xFD000000, back 0x00C00000, cache 0x01600000, per-column 1x(h-y) writes w*~h*0.4 avg pixels, no overlap gap
    uint32_t far_green = pal->far_green; // lighter far layer
    uint32_t near_green = pal->near_green; // darker near layer
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
    // Clouds - 3 clusters of 3-4 overlapping circles, palette-tinted, cheap,
    // in sky area (y < sky_h) - x scaled with width
    // Original at 1024: cluster1 180,80 etc., cluster2 500,100, cluster3 850,90. At 1920 scaled ~1.875x keeps similar visual spread.
    int cx1 = w * 180 / 1024; gfx_draw_filled_circle(cx1, 80, 28, pal->cloud);
    gfx_draw_filled_circle(w * 210 / 1024, 70, 22, pal->cloud);
    gfx_draw_filled_circle(w * 240 / 1024, 85, 18, pal->cloud);
    gfx_draw_filled_circle(w * 160 / 1024, 90, 15, pal->cloud);
    int cx2 = w * 500 / 1024; gfx_draw_filled_circle(cx2, 100, 30, pal->cloud);
    gfx_draw_filled_circle(w * 530 / 1024, 85, 20, pal->cloud);
    gfx_draw_filled_circle(w * 470 / 1024, 95, 18, pal->cloud);
    gfx_draw_filled_circle(w * 550 / 1024, 105, 14, pal->cloud);
    int cx3 = w * 850 / 1024; gfx_draw_filled_circle(cx3, 90, 26, pal->cloud);
    gfx_draw_filled_circle(w * 880 / 1024, 75, 18, pal->cloud);
    gfx_draw_filled_circle(w * 820 / 1024, 80, 16, pal->cloud);
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

// --- Wallpaper presets: public API (see window.h Settings-app contract) ---
const char *wallpaper_preset_name(int p){
    if(p < 0 || p >= WALLPAPER_PRESET_COUNT) return "?";
    return wallpaper_palettes[p].name;
}
// Invalidate cache + rebuild with current preset, effective on next frame.
// Trace: menu "Change Wallpaper" -> cycle -> set -> rebuild draws Bliss with
// new palette into back buffer, snapshots back->cache, sets ready + redraw flag.
// No reboot: per-frame path blits whatever cache holds. One-time ~47M-cycle
// hitch on switch (same cost as boot build), then back to cheap blits.
// Safe mid-session: back buffer is redrawn from cache+windows on the next
// redraw anyway, so overwriting it here loses nothing.
// --- Photo staged copy: chunked blob->spare transfer + pointer swap ---
// Why staged (option (a) from the plan): while copying, the destination is
// half-old/half-new, and per-frame blits read the LIVE cache - so the copy
// must NOT target the cache. Instead chunks go to a SPARE 8.3MB buffer at
// 0x02200000 (paint ends ~0x02130000, stacks at 0x03000000: clear), and at
// completion the cache/stage POINTERS swap (zero-copy: no second 8MB memcpy,
// no 178M hitch anywhere). Old wallpaper displays untouched until the swap,
// then one dirty_all() repaints with the new image.
// Redraw discipline: chunks set NO flags (nothing visible changes mid-copy),
// so idle stays 1Hz through the transition; exactly one full frame at swap.
// IRQ safety: single writer (main loop); a PIT preemption mid-chunk resumes
// transparently (context switch saves ESI/EDI/ECX).
// Chunk: 1080/20 = 54 rows (~9M cycles, a few ms) -> full image in ~20 main
// loop iterations (~0.2s at ~100Hz wakeups). Snappy but unnoticeable per chunk.
#define PHOTO_STAGE_VADDR 0x02200000
#define PHOTO_CHUNK_ROWS 54
static uint32_t *wallpaper_stage = 0;
static int photo_copy_active = 0;
static int photo_copy_row = 0;
static int photo_copy_t0 = 0;
static int wallpaper_stage_alloc(void){
    if(wallpaper_stage) return 1;
    if(!fb_is_available()) return 0;
    uint32_t need = wallpaper_cache_bytes; // same size as cache by construction
    if(need == 0) return 0;
    uint32_t pages = (need + 0xFFF) >> 12;
    s_puts("WALLPAPER: stage alloc "); s_put_dec(pages); s_puts(" pages at 0x02200000\n");
    for(uint32_t i=0;i<pages;i++){
        uint32_t p = pmm_alloc_frame();
        if(!p){ s_puts("WALLPAPER: stage out of frames!\n"); return 0; }
        paging_map(PHOTO_STAGE_VADDR + i*0x1000, p, 0x03);
    }
    wallpaper_stage = (uint32_t*)PHOTO_STAGE_VADDR;
    return 1;
}
// One chunk per main-loop iteration (called from kernel main loop, NOT from
// the click handler: the click only starts the job and returns instantly).
void wallpaper_copy_poll(void){
    if(!photo_copy_active) return;
    int fh = (int)fb_get_height(), fw = (int)fb_get_width();
    int rows = PHOTO_CHUNK_ROWS;
    if(photo_copy_row + rows > fh) rows = fh - photo_copy_row;
    if(rows > 0 && wallpaper_stage){
        // Blob is width-packed (1920*4 stride); stage mirrors back-buffer
        // layout (pitch stride). Equal on our modes; pitch-correct anyway.
        uint32_t blob_stride_dw = 1920;
        uint32_t stage_stride_dw = fb_get_pitch() / 4;
        for(int r=0; r<rows; r++){
            uint32_t *s = (uint32_t*)photo_wallpaper_data + (uint32_t)(photo_copy_row+r) * blob_stride_dw;
            uint32_t *d = wallpaper_stage + (uint32_t)(photo_copy_row+r) * stage_stride_dw;
            uint32_t dwords = (uint32_t)fw;
            __asm__ volatile("cld; rep movsl" : "+S"(s), "+D"(d), "+c"(dwords) : : "memory");
        }
        photo_copy_row += rows;
    } else {
        photo_copy_row = fh; // degenerate: finish immediately
    }
    if(photo_copy_row >= fh){
        // Swap pointers (zero-copy) + single full repaint with the new image.
        uint32_t *tmp = wallpaper_cache;
        wallpaper_cache = wallpaper_stage;
        wallpaper_stage = tmp;
        photo_copy_active = 0;
        int dt = pit_get_ticks() - photo_copy_t0;
        s_puts("WALLPAPER: photo staged swap ready in "); s_put_dec(dt < 0 ? 0 : dt);
        s_puts(" ticks\n");
        dirty_all();
    }
}

void wallpaper_rebuild(void){
    if(!wallpaper_cache_alloc()) return; // no-op after boot (buffer exists)
    uint64_t t0 = rdtsc();
    if(wallpaper_preset == 3){
        // Photo preset: STAGED copy (see photo staging block below). Validates
        // size, allocates the spare buffer, and starts the background chunk job.
        // Returns immediately (no hitch, no redraw flag: old image stays until
        // the swap). Size mismatch -> Bliss fallback, never garbage.
        uint32_t nbytes = (uint32_t)(photo_wallpaper_end - photo_wallpaper_data);
        if(nbytes != (uint32_t)1920*1080*4 || nbytes != wallpaper_cache_bytes){
            s_puts("WALLPAPER: photo size mismatch (got "); s_put_dec(nbytes);
            s_puts(" want 8294400), Bliss fallback\n");
            draw_bliss_wallpaper();
        } else if(!wallpaper_stage_alloc()){
            s_puts("WALLPAPER: stage alloc failed, sync copy fallback\n");
            uint32_t *src = (uint32_t*)photo_wallpaper_data;
            uint32_t *dst = wallpaper_cache;
            uint32_t dwords = nbytes / 4;
            __asm__ volatile("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(dwords) : : "memory");
        } else {
            photo_copy_active = 1; photo_copy_row = 0;
            photo_copy_t0 = pit_get_ticks();
            s_puts("WALLPAPER: photo staged copy started\n");
            return; // no snapshot/dirty yet: cache still holds the old image
        }
    } else {
        draw_bliss_wallpaper(); // reads wallpaper_preset (0-2)
    }
    uint32_t *src = fb_get_back_buffer();
    uint32_t *dst = wallpaper_cache;
    uint32_t dwords = wallpaper_cache_bytes / 4;
    __asm__ volatile("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(dwords) : : "memory");
    wallpaper_cache_ready = 1;
    uint64_t t1 = rdtsc();
    s_puts("WALLPAPER: rebuilt preset "); s_put_dec(wallpaper_preset);
    s_puts(" "); s_puts(wallpaper_preset_name(wallpaper_preset));
    s_puts(" cycles "); s_put_cycles(t1 - t0); s_puts("\n");
    dirty_all(); // whole screen changed: fall back to full repaint (rare path)
}
void wallpaper_set_preset(int p){
    if(p < 0 || p >= WALLPAPER_PRESET_COUNT) return; // invalid: ignore, keep current
    if(p == wallpaper_preset) return; // same: no work (re-click Photo mid-copy keeps copying)
    if(photo_copy_active){ // switching away mid-copy: abandon staged rows (restart at 0 next time)
        photo_copy_active = 0;
        s_puts("WALLPAPER: staged copy cancelled\n");
    }
    wallpaper_preset = p;
    wallpaper_rebuild();
}
void wallpaper_cycle_preset(void){
    wallpaper_set_preset((wallpaper_preset + 1) % WALLPAPER_PRESET_COUNT);
}

// --- Desktop right-click context menu ---
// Layer: above everything (drawn after taskbar). Redraw discipline (per the
// idle-Hz fix): flags ONLY on open/close/action. While open and idle nothing
// sets flags - the menu repaints as part of whatever redraws already happen
// (1Hz clock), adding ~100 rect+string ops to that frame, zero extra frames.
#define CTX_W 152 // fits "Open Settings" (13ch x 8px) + padding
#define CTX_ROW_H 24
#define CTX_NITEMS 2
#define CTX_H (8 + CTX_NITEMS*CTX_ROW_H) // 56
static int ctx_open = 0;
static int ctx_x = 0, ctx_y = 0; // top-left, clamped on open
// Wallpaper/timezone cycling moved into the Settings app; menu offers it via
// "Open Settings" (same open-or-refocus as the desktop icon).
static const char *ctx_labels[CTX_NITEMS] = {"New Window", "Open Settings"};
void context_menu_open(int x, int y){
    int fb_w = fb_get_width(), fb_h = fb_get_height();
    ctx_x = x; ctx_y = y;
    if(ctx_x + CTX_W > fb_w) ctx_x = fb_w - CTX_W; // clamp near right edge
    if(ctx_x < 0) ctx_x = 0;
    if(ctx_y + CTX_H > fb_h - TASKBAR_H) ctx_y = fb_h - TASKBAR_H - CTX_H; // above taskbar
    if(ctx_y < 0) ctx_y = 0;
    ctx_open = 1;
    s_puts("CTX: open at "); s_put_dec(ctx_x); s_putc(','); s_put_dec(ctx_y); s_puts("\n");
    dirty_add(ctx_x, ctx_y, CTX_W, CTX_H);
}
void context_menu_close(void){
    if(!ctx_open) return;
    // Dirty BEFORE clearing (rect fields survive, but capture-then-clear is the rule).
    dirty_add(ctx_x, ctx_y, CTX_W, CTX_H);
    ctx_open = 0;
    s_puts("CTX: closed\n");
}
int context_menu_is_open(void){ return ctx_open; }
// Returns item index or -1. Same rects as draw (single source: CTX_* + ctx_x/y).
static int context_menu_hit(int x, int y){
    if(!ctx_open) return -1;
    for(int i=0;i<CTX_NITEMS;i++){
        int ix = ctx_x + 4, iy = ctx_y + 4 + i*CTX_ROW_H;
        if(x >= ix && x < ix + CTX_W - 8 && y >= iy && y < iy + CTX_ROW_H) return i;
    }
    return -1;
}
void context_menu_draw(void){
    if(!ctx_open || !fb_is_available()) return;
    fb_draw_rect(ctx_x, ctx_y, CTX_W, CTX_H, 0x00F0F0F0);
    gfx_draw_rect_outline(ctx_x, ctx_y, CTX_W, CTX_H, 0x00000000);
    for(int i=0;i<CTX_NITEMS;i++)
        gfx_draw_string(ctx_x + 12, ctx_y + 4 + i*CTX_ROW_H + 8, ctx_labels[i], 0x00000000);
}
int context_menu_handle_click(int x, int y){
    // Left-click while open: item action + close, or outside-close. Consumed.
    if(!ctx_open) return 0;
    int item = context_menu_hit(x, y);
    if(item == 0){ s_puts("CTX: action New Window\n"); window_create_new(); }
    else if(item == 1){ s_puts("CTX: action Open Settings\n"); settings_open_or_focus(); }
    else s_puts("CTX: click outside, close only\n");
    context_menu_close(); // sets redraw flag (covers action's own flag too)
    return 1;
}
int context_menu_handle_rightclick(int x, int y){
    // Right-press: close open menu first; open only on EMPTY desktop
    // (no window, no icon, not taskbar) - mirrors left-click priority order.
    if(ctx_open) context_menu_close();
    if(window_find_at(x, y) != -1) return 0;
    if(desktop_icon_hit_test(x, y) != -1) return 0;
    if(y >= fb_get_height() - TASKBAR_H) return 0;
    context_menu_open(x, y);
    return 1;
}

// --- Dirty-rectangle tracking: single bounding union (not a region list).
// Exclusive bounds; dirty_valid==0 means FULL SCREEN (initial state, and the
// safety fallback for any flag raised without a rect: missed site = slower
// full frame, never an artifact). dirty_add/all ALWAYS set g_needs_redraw;
// callers never touch the flag directly.
static int dirty_valid = 0;
static int dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = 0, dirty_y1 = 0;
void dirty_add(int x, int y, int w, int h){
    if(w <= 0 || h <= 0) return;
    if(!fb_is_available()) return;
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    if(fw <= 0 || fh <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fw ? fw : x + w, y1 = y + h > fh ? fh : y + h;
    if(x0 >= x1 || y0 >= y1) return; // fully off-screen: nothing
    if(!dirty_valid){ dirty_x0=x0; dirty_y0=y0; dirty_x1=x1; dirty_y1=y1; dirty_valid=1; }
    else { if(x0<dirty_x0)dirty_x0=x0; if(y0<dirty_y0)dirty_y0=y0; if(x1>dirty_x1)dirty_x1=x1; if(y1>dirty_y1)dirty_y1=y1; }
    // Tripwire for oversized unions (permanent diagnostic): legit per-op rects
    // are small (16x16 cursor, window bounds, clock strip); a huge union means
    // many ops accumulated without an intervening redraw (IRQ flood outrunning
    // the main loop). Logs input + result so growth-vs-jump is distinguishable.
    // Threshold 1000x700: above any single window-op rect, below fullscreen.
    if(dirty_x1-dirty_x0 > 1000 || dirty_y1-dirty_y0 > 700){
        s_puts("DIRTYBIG in "); s_put_dec((uint32_t)(x1-x0)); s_putc('x'); s_put_dec((uint32_t)(y1-y0));
        s_puts(" at "); s_put_dec((uint32_t)x0); s_putc(','); s_put_dec((uint32_t)y0);
        s_puts(" -> union "); s_put_dec((uint32_t)(dirty_x1-dirty_x0)); s_putc('x'); s_put_dec((uint32_t)(dirty_y1-dirty_y0));
        s_puts(" at "); s_put_dec((uint32_t)dirty_x0); s_putc(','); s_put_dec((uint32_t)dirty_y0);
        s_puts("\n");
    }
    g_needs_redraw = 1;
}
void dirty_all(void){
    if(!fb_is_available()){ g_needs_redraw = 1; return; }
    dirty_x0=0; dirty_y0=0;
    dirty_x1=(int)fb_get_width(); dirty_y1=(int)fb_get_height();
    dirty_valid=1; g_needs_redraw=1;
}
void window_invalidate(int idx){
    if(idx<0||idx>=window_count) return;
    struct window *w=&windows[idx];
    dirty_add(w->x, w->y, w->w, w->h);
}
void taskbar_clock_invalidate(void){
    // Clock digits: right-aligned ending 10px left of "+", same geometry as draw.
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    int cx = fw - 30 - 5 - 10 - 8*8;
    dirty_add(cx-2, fh-TASKBAR_H+3+6-2, 8*8+4, 12);
}
void taskbar_tabs_invalidate(void){
    // Full tab strip: tabs live at x=5+i*155 (w=150), y=ty+3 (h=24) for up to
    // MAX_WINDOWS slots, so union x=0..5+MAX_WINDOWS*155, y=ty..ty+TASKBAR_H.
    // WHY this exists (traced): every tab-mutating op (create/close/z-shift/
    // minimize recolor) used to dirty only window bounds, never the strip.
    // taskbar_draw runs clipped to the dirty union, so a strip-touching union
    // from an unrelated op repainted CURRENT (shifted) tabs over a sub-rect
    // while pixels outside kept STALE (pre-shift) tabs - half-old/half-new
    // glyphs where the clip edge sliced text ("Click" + outline ghosts that
    // "self-correct" on the next full-strip touch). Unioning the whole strip
    // here makes every tab mutation repaint atomically. Cost ~37k px on
    // discrete clicks only; steady-state (1Hz clock, TaskMan/About refresh)
    // is untouched.
    int fh = (int)fb_get_height();
    int w = 5 + MAX_WINDOWS * (150 + 5);
    dirty_add(0, fh-TASKBAR_H, w, TASKBAR_H);
    s_puts("TASKBAR: tabs dirty (0,"); s_put_dec((uint32_t)(fh-TASKBAR_H));
    s_puts(","); s_put_dec((uint32_t)w); s_puts("x"); s_put_dec((uint32_t)TASKBAR_H);
    s_puts(")\n");
}

void window_manager_draw_all(void){
    if(!fb_is_available()) return;
    uint64_t t_wall0=0, t_wall1=0;
    if(wallpaper_cache_ready){
        t_wall0 = rdtsc();
        // Region blit: only dirty rows/cols (per-frame wallpaper cost now
        // scales with change size, not screen size).
        fb_blit_region(wallpaper_cache, dr_x0, dr_y0, dr_x1-dr_x0, dr_y1-dr_y0);
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
    // Skip icons outside the dirty region (label can overflow the 64px box).
    desktop_icons_draw();
    uint64_t t_win0 = rdtsc();
    for(int i=0;i<window_count;i++){
        int idx = z_order[i];
        if(windows[idx].minimized) continue;
        // Skip windows that don't touch the dirty region: their front-buffer
        // pixels are already correct from a previous frame.
        if(!rects_overlap(windows[idx].x, windows[idx].y, windows[idx].w, windows[idx].h,
                           dr_x0, dr_y0, dr_x1, dr_y1)) continue;
        window_draw_single(idx);
    }
    // Taskbar spans the bottom 30px; skip unless dirty touches it. Clock ticks
    // repaint only the clock rect (taskbar_draw runs, clipped to it).
    if(rects_overlap(0, (int)fb_get_height()-TASKBAR_H, (int)fb_get_width(), TASKBAR_H,
                      dr_x0, dr_y0, dr_x1, dr_y1))
        taskbar_draw();
    if(ctx_open && rects_overlap(ctx_x, ctx_y, CTX_W, CTX_H, dr_x0, dr_y0, dr_x1, dr_y1))
        context_menu_draw(); // topmost layer (above taskbar)
    uint64_t t_win1 = rdtsc();
    last_windows_cycles = t_win1 - t_win0;
    // Throttled frame log (%10, matching the WM_REDRAW summary cadence): this
    // line fired EVERY redraw (~40 bytes ≈ 3.5ms), amplifying starvation under
    // flood - the redraw it logs delays the next redraw. Frequency math for
    // measurements: count these lines x10 over the run duration.
    { static int drew_log_cnt=0;
      if((++drew_log_cnt % 10)==0){
    s_puts("WM: drew windows back->front z=[");
    for(int i=0;i<window_count;i++){ s_put_dec(z_order[i]); if(i<window_count-1) s_putc(','); }
    s_puts("] minimized: ");
    for(int i=0;i<window_count;i++){ s_put_dec(windows[i].minimized); if(i<window_count-1) s_putc(','); }
    s_puts("\n");
      }
    }
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
    // Raised window's rect suffices: it repaints fully (was partially covered),
    // and newly-covered parts of others need no paint (painted over in z-order).
    window_invalidate(idx);
    taskbar_tabs_invalidate(); // front-highlight color moved to another tab
    return 1;
}



void window_set_needs_redraw(void){ dirty_all(); } // legacy entry: no rect known -> full (safe)
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
    // Snapshot the dirty union (full screen if none recorded: safety fallback).
    // Correctness model: back buffer is scratch; every frame recomposes the dirty
    // region from wallpaper upward in z-order, so moved/uncovered areas repaint
    // correctly as long as old+new positions were both unioned by invalidators.
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    int dx0, dy0, dx1, dy1, was_full;
    if(dirty_valid){ dx0=dirty_x0; dy0=dirty_y0; dx1=dirty_x1; dy1=dirty_y1; }
    else { dx0=0; dy0=0; dx1=fw; dy1=fh; }
    dirty_valid = 0;
    was_full = (dx0<=0 && dy0<=0 && dx1>=fw && dy1>=fh);
    // Cursor is deliberately NOT unioned into dirty: a static cursor far from the
    // action would stretch every small region into a huge bounding box (measured:
    // 68x12 clock tick + parked cursor = 917x529 repaint). Instead the cursor box
    // is painted+swapped separately below; restore() first erases the old box in
    // back (saved pixels are always fresh: every repaint path restores first).
    dr_x0=dx0; dr_y0=dy0; dr_x1=dx1; dr_y1=dy1;
    // Capture old cursor box BEFORE restore() clears validity: restore erases
    // it in back, but with partial swap that erasure never reaches front unless
    // we swap the old box explicitly below (missing this = cursor ghosts).
    int old_cx = 0, old_cy = 0, old_cv = 0;
    mouse_get_saved_box(&old_cx, &old_cy, &old_cv);
    mouse_cursor_restore();
    mouse_cursor_invalidate();
    fb_set_clip(dx0, dy0, dx1-dx0, dy1-dy0);
    window_manager_draw_all();
    fb_clear_clip();
    mouse_cursor_draw_current();
    if(fb_is_double_buffered()) fb_swap_region(dx0, dy0, dx1-dx0, dy1-dy0);
    // Cursor box swaps separately (see above: never unioned into dirty).
    { int ccx, ccy; mouse_get_position(&ccx, &ccy);
      if(fb_is_double_buffered()) fb_swap_region(ccx, ccy, 16, 16); }
    // Old cursor box: restore() erased it in back; carry that erasure to front
    // (back there is either restored pixels or dirty-repaint, both correct).
    // Skipped when identical to current (static cursor: no stretch, no waste).
    { int ccx, ccy; mouse_get_position(&ccx, &ccy);
      if(old_cv && (old_cx != ccx || old_cy != ccy) && fb_is_double_buffered())
          fb_swap_region(old_cx, old_cy, 16, 16); }
    uint64_t t1 = rdtsc();
    if((g_redraw_count % 10)==0){
        s_puts("WM_REDRAW #"); s_put_dec(g_redraw_count);
        s_puts(" cycles "); s_put_dec((uint32_t)(t1-t0));
        s_puts(" [wallpaper "); s_put_dec((uint32_t)last_wallpaper_cycles);
        s_puts(" win "); s_put_dec((uint32_t)last_windows_cycles);
        s_puts("] dirty "); s_put_dec((uint32_t)(dx1-dx0)); s_putc('x'); s_put_dec((uint32_t)(dy1-dy0));
        if(was_full) s_puts(" FULL");
        s_puts(" PMM free "); s_put_dec(pmm_free_frames());
        s_puts("\n");
    }
    // Log first few drags separately for lag diagnosis (always, not only %10)
    if(g_redraw_count < 6){
        s_puts("WM_REDRAW breakdown #"); s_put_dec(g_redraw_count);
        s_puts(" total "); s_put_dec((uint32_t)(t1-t0));
        s_puts(" wallpaper "); s_put_dec((uint32_t)last_wallpaper_cycles);
        s_puts(" windows "); s_put_dec((uint32_t)last_windows_cycles);
        s_puts(" dirty "); s_put_dec((uint32_t)(dx1-dx0)); s_putc('x'); s_put_dec((uint32_t)(dy1-dy0));
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
    // Enforce minimum - per-app fixed minimums matched to layout_sync clamps,
    // so widgets never shrink to unusable/overlapping sizes:
    // Calculator grid needs bw>=40/bh>=24 -> 240x240; Clicker button 120x30
    // centered -> 200x140; Notes textbox + labels -> 200x140; Settings fixed
    // grid (380x220, not synced) -> pinned at creation size; Paint palette row
    // needs 410px + margins -> 430x200 (canvas clips below that, by design).
    int min_w = WIN_MIN_W;
    int min_h = WIN_MIN_H;
    if(w->has_paint){ min_w = 430; min_h = 200; }
    else if(w->has_about){ min_w = 320; min_h = 220; }
    else if(w->has_settings){ min_w = 380; min_h = 220; }
    else if(w->has_calc){ min_w = 240; min_h = 240; }
    else if(w->has_textbox){ if(min_w < 200) min_w = 200; if(min_h < 140) min_h = 140; }
    else if(w->has_button){ if(min_w < 200) min_w = 200; if(min_h < 140) min_h = 140; }
    if(new_w < min_w) new_w = min_w;
    if(new_h < min_h) new_h = min_h;
    int max_w = (int)fb_get_width() - w->x;
    int max_h = (int)fb_get_height() - TASKBAR_H - w->y;
    if(new_w > max_w) new_w = max_w;
    if(new_h > max_h) new_h = max_h;
    if(new_w == w->w && new_h == w->h) return;
    // Resize MUST dirty BOTH old and new extents (same ghost rule as drag).
    dirty_add(w->x, w->y, w->w, w->h);
    w->w = new_w;
    w->h = new_h;
    dirty_add(w->x, w->y, w->w, w->h);
}

void window_end_resize(void){
    if(!resizing) return;
    s_puts("WM: resize end Window "); s_put_dec(resize_win+1);
    s_puts(" now "); s_put_dec(windows[resize_win].w); s_putc('x'); s_put_dec(windows[resize_win].h); s_puts("\n");
    resizing = 0;
    resize_win = -1;
    // No flag: every update already dirtied old+new extents.
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
            if(widx != -1) window_close(widx); // close() already invalidated bounds
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
        window_invalidate(idx); // windows behind repaint the hole
        taskbar_tabs_invalidate(); // tab recolored to minimized-dark
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
    // Invalidate BEFORE the array shift destroys the struct: windows behind
    // repaint the hole from wallpaper upward in z-order on next redraw.
    window_invalidate(idx);
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
    // Session persist: snapshot Notes text BEFORE the array shift below
    // destroys the window struct. Every close path (X button, Task Manager
    // Kill) funnels through here, so no path can lose text by forgetting
    // to save - this is why there is no manual Save button.
    if(icon_streq(windows[idx].title, "Notes") && windows[idx].has_textbox){
        notes_save_from(&windows[idx].tbox);
        s_puts("WM: Notes text snapshotted to session buffer len ");
        s_put_dec(notes_saved_len);
        s_puts("\n");
    }
    // If closed window had pressed button(s), clear
    if(windows[idx].has_button){
        int n = windows[idx].num_btns;
        if(n > MAX_BTNS) n = MAX_BTNS;
        for(int b=0;b<n;b++) windows[idx].btns[b].pressed=0;
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
    // No flag here: bounds already invalidated at close entry (above).
    // Strip MUST be dirtied separately: remaining tabs shifted left and the
    // last slot is stale (this was the garbled-tabs root cause).
    taskbar_tabs_invalidate();
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
                taskbar_tabs_invalidate(); // tab recolored (bring_to_front may no-op if already front)
                window_invalidate(i); // reappeared window repaints even if bring_to_front no-ops
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
    // Bottom of window (Notes labels, resize handle) must never slide under the
    // taskbar: those live ~20-32px above the window bottom, so they are the first
    // things swallowed when a tall window is dragged down. If the window is taller
    // than the space above the taskbar, pin top (keeps title bar grabbable).
    int max_y = (int)fb_get_height() - TASKBAR_H - w->h;
    if(max_y < 0) max_y = 0;
    if(new_x < min_x) new_x = min_x;
    if(new_x > max_x) new_x = max_x;
    if(new_y < min_y) new_y = min_y;
    if(new_y > max_y) new_y = max_y;
    if(new_x == w->x && new_y == w->y) return;
    // Drag MUST dirty BOTH old (erase ghost via wallpaper+behind repaint)
    // and new positions - missing either side means artifacts.
    dirty_add(w->x, w->y, w->w, w->h);
    w->x = new_x;
    w->y = new_y;
    dirty_add(w->x, w->y, w->w, w->h);
}


void window_end_drag(void){
    if(!dragging) return;
    s_puts("WM: drag end Window "); s_put_dec(drag_win+1); s_puts("\n");
    dragging = 0;
    drag_win = -1;
    // No flag: every update already dirtied old+new; end changes nothing visual.
}

// --- Phase 12: button (multi-button since Calculator) ---
// Returns button index or -1. Loops all buttons; Clicker (num_btns==1)
// behaves exactly as the old single-button test.
static int button_hit_test(int win_idx, int x, int y){
    if(win_idx <0 || win_idx >= window_count) return -1;
    struct window *w = &windows[win_idx];
    if(!w->has_button) return -1;
    int n = w->num_btns;
    if(n > MAX_BTNS) n = MAX_BTNS;
    for(int b=0;b<n;b++){
        int ax = w->x + w->btns[b].x;
        int ay = w->y + w->btns[b].y;
        if(x >= ax && x < ax + w->btns[b].w && y >= ay && y < ay + w->btns[b].h) return b;
    }
    return -1;
}

int window_handle_button_down(int x, int y){
    int idx = window_find_at(x,y);
    if(idx==-1) return 0;
    int b = button_hit_test(idx,x,y);
    if(b < 0) return 0;
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
    w->btns[b].pressed = 1;
    window_invalidate(idx); // covers inline bring-to-front + pressed shading
    s_puts("BTN: down Window "); s_put_dec(idx+1); s_puts(" btn "); s_put_dec(b); s_puts(" pressed\n");
    return 1;
}


int window_handle_button_up(int x, int y){
    // Find the one pressed button across all windows (down guarantees uniqueness).
    int pressed_idx = -1, pressed_b = -1;
    for(int i=0;i<window_count && pressed_idx==-1;i++){
        if(!windows[i].has_button) continue;
        int n = windows[i].num_btns;
        if(n > MAX_BTNS) n = MAX_BTNS;
        for(int b=0;b<n;b++) if(windows[i].btns[b].pressed){ pressed_idx = i; pressed_b = b; break; }
    }
    if(pressed_idx==-1) return 0;
    struct window *w = &windows[pressed_idx];
    int ax = w->x + w->btns[pressed_b].x;
    int ay = w->y + w->btns[pressed_b].y;
    int inside = (x >= ax && x < ax + w->btns[pressed_b].w && y >= ay && y < ay + w->btns[pressed_b].h);
    w->btns[pressed_b].pressed = 0;
    if(inside){
        // Calculator dispatches to calc logic; every other button window keeps
        // the legacy Clicker behavior on the clicked button (Clicker: btns[0],
        // label "Clicked: N" - pixel- and serial-identical to before).
        if(w->has_calc){
            calculator_handle_button(w, pressed_b);
        } else if(w->has_settings){
            settings_handle_button(w, pressed_b);
        } else if(w->has_paint){
            paint_handle_button(w, pressed_b);
        } else {
            w->btns[pressed_b].clicks++;
        char buf[32];
        const char *prefix = "Clicked: ";
        int p=0;
        for(int i=0; prefix[i] && p<31; i++) buf[p++]=prefix[i];
        char num[12]; int n=w->btns[pressed_b].clicks; int len=0;
        if(n==0) num[len++]='0';
        else { char tmp[12]; int t=0; while(n){ tmp[t++]='0'+n%10; n/=10; } while(t--) num[len++]=tmp[t]; }
        for(int i=0;i<len && p<31; i++) buf[p++]=num[i];
        buf[p]=0;
        w_strcpy(w->btns[pressed_b].label, buf, 32);
        s_puts("BTN: click Window "); s_put_dec(pressed_idx+1); s_puts(" count "); s_put_dec(w->btns[pressed_b].clicks); s_puts(" label "); s_puts(buf); s_puts("\n");
        }
    } else {
        s_puts("BTN: release outside Window "); s_put_dec(pressed_idx+1); s_puts("\n");
    }
    window_invalidate(pressed_idx); // shading/label/display changes live inside this window
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
        // click on desktop -> unfocus all (dirty each focused window first)
        int had=0;
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused){ window_invalidate(i); had=1; }
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox) windows[i].tbox.focused=0;
        if(had){ s_puts("TBOX: unfocus all (desktop)\n"); }
        return 0;
    }
    // check if click inside textbox of that window
    if(!textbox_hit_test(idx,x,y)){
        // click inside window but outside textbox -> unfocus this window's textbox if it was focused? Keep focus only if click inside textbox
        // For Phase 12, clicking elsewhere in window should unfocus textbox (only one focused at a time)
        int had_focus = 0;
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused){ window_invalidate(i); had_focus=1; }
        for(int i=0;i<window_count;i++) if(windows[i].has_textbox) windows[i].tbox.focused=0;
        if(had_focus){ s_puts("TBOX: click outside textbox, unfocus\n"); }
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
    window_invalidate(idx); // focus border + inline bring
    return 1;
}

void window_handle_key(char c){
    // find focused textbox
    int fidx=-1;
    for(int i=0;i<window_count;i++) if(windows[i].has_textbox && windows[i].tbox.focused) fidx=i;
    if(fidx==-1) return;
    struct textbox *tb=&windows[fidx].tbox;
    if(c=='\b'){
        if(tb->len>0){ tb->len--; tb->buffer[tb->len]=0; s_puts("TBOX: backspace len "); s_put_dec(tb->len); s_puts("\n"); window_invalidate(fidx); }
        return;
    }
    if(c=='\n' || c=='\r') return;
    if(tb->len >= tb->max_len) return;
    tb->buffer[tb->len++]=c;
    tb->buffer[tb->len]=0;
    s_puts("TBOX: typed '"); s_putc(c); s_puts("' len "); s_put_dec(tb->len); s_puts(" Window "); s_put_dec(fidx+1); s_puts("\n");
    window_invalidate(fidx);
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
            window_invalidate(i); // cursor pixel change only, but window-granular is fine
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
