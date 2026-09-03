#ifndef WINDOW_H
#define WINDOW_H
#include <stdint.h>

#define MAX_WINDOWS 3
#define TITLE_BAR_H 20

struct window {
    int x, y, w, h;
    char title[32];
    uint32_t bg_color;
    uint32_t title_color;
    uint32_t border_color;
    int visible;
    int z; // 0 = back, higher = front
};

void window_manager_init(void);
void window_manager_draw_all(void);
int window_find_at(int x, int y); // returns index or -1, front to back
int window_bring_to_front(int idx); // returns 1 if redrawn
int window_handle_click(int x, int y); // returns 1 if handled (brought to front)
void window_get_info(int idx, int *x, int *y, int *w, int *h);
// drag handling - Phase 11
int window_start_drag(int x, int y); // check title bar, start drag if hit, returns 1 if started
void window_update_drag(int x, int y); // update dragged window pos to x - offset, clamped, redraw
void window_end_drag(void);
int window_is_dragging(void);
int window_is_in_title_bar(int idx, int x, int y);

#endif
