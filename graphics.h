#ifndef GRAPHICS_H
#define GRAPHICS_H
#include <stdint.h>
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_draw_rect_outline(int x, int y, int w, int h, uint32_t color);
void gfx_draw_circle(int cx, int cy, int r, uint32_t color);
void gfx_draw_filled_circle(int cx, int cy, int r, uint32_t color);
void gfx_draw_char(int x, int y, char c, uint32_t color);
void gfx_draw_string(int x, int y, const char *s, uint32_t color);
void gfx_draw_string_bg(int x, int y, const char *s, uint32_t fg, uint32_t bg);
#endif
