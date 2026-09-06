#ifndef WINDOW_H
#define WINDOW_H
#include <stdint.h>

#define MAX_WINDOWS 8
#define TITLE_BAR_H 20
#define TASKBAR_H 30
#define MINIMIZE_BTN_W 16
#define MINIMIZE_BTN_H 16
#define CLOSE_BTN_W 16
#define CLOSE_BTN_H 16
#define RESIZE_HANDLE 12
#define WIN_MIN_W 150
#define WIN_MIN_H 100
#define MAX_ICONS 6
#define ICON_W 64
#define ICON_H 64
#define ICON_GLYPH 32

struct button {
    int x, y, w, h; // relative to parent window origin (0,0 = window top-left)
    char label[32];
    int pressed;
    int clicks;
};
#define MAX_BTNS 20 // Calculator uses 16 (0-9, +, -, *, /, =, C); Clicker uses 1

struct calc_state {
    char display[32]; // shown in display box ("0", "10", "-5", "Error")
    int acc;   // first operand / running result
    int op;    // 0 none, 1 +, 2 -, 3 *, 4 /
    int fresh; // 1 = next digit starts a new entry (after op/= /C)
    int err;   // 1 = divide-by-zero latched, display shows "Error"
};

struct textbox {
    int x, y, w, h; // relative to parent window origin
    char buffer[513]; // 512 + null
    int len;
    int max_len;
    int focused;
    int cursor_visible;
    int blink_counter;
};

struct window {
    int x, y, w, h;
    char title[32];
    uint32_t bg_color;
    uint32_t title_color;
    uint32_t border_color;
    int visible;
    int minimized; // 1 = not drawn on desktop, but taskbar tab still shows
    int z; // 0 = back, higher = front
    struct button btns[MAX_BTNS]; // buttons[0] is Clicker's single button (was struct button btn)
    int num_btns; // 0 = none; Clicker 1, Calculator 16
    int has_button; // == (num_btns > 0), kept as the existing guard
    struct textbox tbox;
    int has_textbox;
    struct calc_state calc; // meaningful only when has_calc == 1
    int has_calc; // 1 = Calculator app window (no background task, purely reactive)
    int has_settings; // 1 = Settings app window (no task; multi-button option grid)
    volatile int task_counter; // owned by Clicker/Notes tasks, drawn by GUI task - single-word atomic
};
extern struct window windows[];
extern int window_count;
extern int z_order[];

// taskbar clock: RTC seed (seconds since midnight, host/UTC as-is, no tz conv)
// read once at boot; PIT ticks advance it from there. -1/unset -> 00:00:00.
void clock_set_base_seconds(int s);
int clock_current_seconds(void); // (base + pit_get_ticks()/100) % 86400
// timezone display offset (hours, fixed presets, no DST logic, no tz database):
// applied ONLY at display formatting; UTC tracking (RTC seed + PIT) untouched.
// Future Settings app contract: read tz_offset_hours, write via setter/cycle.
#define TIMEZONE_PRESET_COUNT 4
extern int tz_offset_hours;
void timezone_set_offset(int hours); // validated to -12..+14, triggers redraw
void timezone_cycle(void); // step through {UTC, UTC+10, UTC-5, UTC+9}
const char *timezone_label(void); // "UTC", "UTC+10", "UTC-5", ... (static buf)
int clock_apply_tz(int utc_sec, int off_hours); // pure wrap math, testable
// wallpaper presets - session global for a future Settings app:
// read via wallpaper_preset, change ONLY via wallpaper_set_preset/cycle
// (setter validates + rebuilds cache + redraws). 0=Day 1=Sunset 2=Night.
#define WALLPAPER_PRESET_COUNT 3
extern int wallpaper_preset;
void wallpaper_set_preset(int p);
void wallpaper_cycle_preset(void);
const char *wallpaper_preset_name(int p);
// desktop right-click context menu (empty desktop only)
void context_menu_open(int x, int y);
void context_menu_close(void);
int context_menu_is_open(void);
int context_menu_handle_click(int x, int y); // left-click: item action + close, or outside-close; 1 if menu was open
int context_menu_handle_rightclick(int x, int y); // right-press: close if open, open if empty desktop
void context_menu_draw(void); // drawn last (above taskbar)

// desktop icons - part of desktop layer under windows
struct desktop_icon {
    int x, y; // absolute desktop position (top-left of hit box)
    char label[32];
    uint32_t color; // glyph color
    int selected; // 1 = highlighted
};
extern struct desktop_icon desktop_icons[];
extern int desktop_icon_count;
void desktop_icons_init(void);
void desktop_icons_draw(void);
int desktop_icon_hit_test(int x, int y); // returns icon index or -1
int desktop_icon_handle_click(int x, int y); // single/double-click logic, returns 1 if hit icon
void desktop_icon_deselect_all(void);

void window_manager_init(void);
void window_manager_draw_all(void);
int window_find_at(int x, int y); // returns index or -1, front to back
int window_bring_to_front(int idx); // returns 1 if redrawn
int window_handle_click(int x, int y); // returns 1 if handled (brought to front)
void window_get_info(int idx, int *x, int *y, int *w, int *h);
// drag handling - Phase 11
int window_start_drag(int x, int y); // check title bar, start drag if hit, returns 1 if started
void window_update_drag(int x, int y); // update dragged window pos to x - offset, clamped, redraw (now deferred)
void window_end_drag(void);
int window_is_dragging(void);
int window_is_in_title_bar(int idx, int x, int y);
// deferred redraw - Phase 11 fix: heavy redraw should run outside IRQ
void window_set_needs_redraw(void);
int window_needs_redraw(void);
void window_do_redraw(void);
extern volatile int g_needs_redraw;
extern volatile int g_in_redraw;
// button - Phase 12
int window_handle_button_down(int x, int y); // returns 1 if hit button
int window_handle_button_up(int x, int y); // returns 1 if click completed (incremented)
// textbox - Phase 12
int window_handle_textbox_click(int x, int y); // focus handling, returns 1 if hit
void window_handle_key(char c); // append char if focused textbox exists
void window_handle_backspace(void);
int textbox_word_count(struct textbox *tb); // live word count for Notes, recomputed per redraw
void window_tick_cursor(void); // called from main loop to blink
int window_handle_scancode(uint8_t scancode); // translate scancode -> key, returns 1 if handled (focused textbox existed)
// taskbar + minimize - Phase 13
void taskbar_draw(void);
int window_handle_minimize_click(int x, int y); // hit minimize "_" in title bar, toggles minimized
int window_handle_taskbar_click(int x, int y); // hit taskbar tab, unminimize or bring to front
int window_is_minimized(int idx);
// resize - Phase 14
int window_is_in_resize_handle(int idx, int x, int y);
int window_start_resize(int x, int y);
void window_update_resize(int x, int y);
void window_end_resize(void);
int window_is_resizing(void);
int window_create_new(void);
// close - Phase 15
int window_handle_close_click(int x, int y);
void window_close(int idx);
int window_is_in_close_button(int idx, int x, int y);
int window_find_by_title(const char *title);
int window_handle_taskmanager_kill_click(int x, int y); // Kill Clicker/Notes tasks and their windows (GUI has none)

#endif
