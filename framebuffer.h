#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H
#include <stdint.h>
#include "multiboot.h"

int fb_init(struct multiboot_info *mbi);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_fill(uint32_t color);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
// dirty-rect support: clip rect intersects all fb_put_pixel/fb_draw_rect output
// (covers every gfx primitive, which all funnel through those two). No clip =
// screen bounds only (previous behavior). Clip is a render-side aid; callers
// must still skip wholly-outside work themselves for the perf win.
void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);
void fb_blit_region(uint32_t *src, int x, int y, int w, int h); // src -> back, clipped rows
void fb_swap_region(int x, int y, int w, int h); // back -> front, clipped rows
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t fb_get_pitch(void); // bytes per row (stride for row math)
int fb_is_available(void);
// double buffering
int fb_is_double_buffered(void);
void fb_swap(void); // copy back buffer to front (visible)
uint32_t* fb_get_back_buffer(void);
uint32_t* fb_get_front_buffer(void);
uint32_t fb_get_front_pixel(uint32_t x, uint32_t y);
uint32_t fb_get_size_bytes(void);
void fb_blit_from(uint32_t *src); // fast copy src (size fb_size_bytes) -> back buffer
uint64_t fb_get_last_swap_cycles(void);

#endif
