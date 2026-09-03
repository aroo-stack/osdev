#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>
void mouse_init(void);
void mouse_handle_byte(uint8_t data);
int mouse_is_enabled(void);
void mouse_get_position(int *x, int *y);
// cursor helpers for window manager
void mouse_cursor_restore(void);
void mouse_cursor_draw_current(void);
void mouse_cursor_invalidate(void);
#endif
