#ifndef WINDOW_H
#define WINDOW_H
#include <stdint.h>

#define MAX_WINDOWS 3
#define TITLE_BAR_H 20
#define TASKBAR_H 30
#define MINIMIZE_BTN_W 16
#define MINIMIZE_BTN_H 16
#define RESIZE_HANDLE 12
#define WIN_MIN_W 150
#define WIN_MIN_H 100

struct button {
    int x, y, w, h; // relative to parent window origin (0,0 = window top-left)
    char label[32];
    int pressed;
    int clicks;
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
    struct button btn;
    int has_button;
    struct textbox tbox;
    int has_textbox;
};

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
// button - Phase 12
int window_handle_button_down(int x, int y); // returns 1 if hit button
int window_handle_button_up(int x, int y); // returns 1 if click completed (incremented)
// textbox - Phase 12
int window_handle_textbox_click(int x, int y); // focus handling, returns 1 if hit
void window_handle_key(char c); // append char if focused textbox exists
void window_handle_backspace(void);
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

#endif
