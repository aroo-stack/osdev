#include "graphics.h"
#include "framebuffer.h"
#include "font.h"
#include <stdint.h>

// Bresenham line - handles all octants
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color){
    int dx = (x1 > x0) ? x1 - x0 : x0 - x1;
    int dy = (y1 > y0) ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + (dy ? -dy : 0); // dx - dy
    // handle dy negative
    dy = -dy;
    while(1){
        fb_put_pixel(x0, y0, color);
        if(x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if(e2 >= dy){ err += dy; x0 += sx; }
        if(e2 <= dx){ err += dx; y0 += sy; }
    }
}

void gfx_draw_rect_outline(int x, int y, int w, int h, uint32_t color){
    if(w<=0 || h<=0) return;
    gfx_draw_line(x, y, x+w-1, y, color); // top
    gfx_draw_line(x, y+h-1, x+w-1, y+h-1, color); // bottom
    gfx_draw_line(x, y, x, y+h-1, color); // left
    gfx_draw_line(x+w-1, y, x+w-1, y+h-1, color); // right
}

// Midpoint circle - 8-way symmetry, outline only
void gfx_draw_circle(int cx, int cy, int r, uint32_t color){
    if(r<=0) return;
    int x = r;
    int y = 0;
    int err = 0;
    while(x >= y){
        fb_put_pixel(cx + x, cy + y, color);
        fb_put_pixel(cx + y, cy + x, color);
        fb_put_pixel(cx - y, cy + x, color);
        fb_put_pixel(cx - x, cy + y, color);
        fb_put_pixel(cx - x, cy - y, color);
        fb_put_pixel(cx - y, cy - x, color);
        fb_put_pixel(cx + y, cy - x, color);
        fb_put_pixel(cx + x, cy - y, color);
        y++;
        err += 1 + 2*y;
        if(2*(err - x) + 1 > 0){
            x--;
            err += 1 - 2*x;
        }
    }
}

void gfx_draw_char(int x, int y, char c, uint32_t color){
    unsigned char uc = (unsigned char)c;
    if(uc >= 128) return;
    const uint8_t *glyph = font8x8[uc];
    for(int row=0; row<8; row++){
        uint8_t bits = glyph[row];
        for(int col=0; col<8; col++){
            // dhepper font encodes LSB = leftmost pixel (verified via 'L' 0x0F -> 00001111 should be left bar)
            // trace 'H' row 0x33 = 00110011: bits 0,1,4,5 set -> x+0,1,4,5 -> "##  ##  " left/right bars, not mirrored
            if(bits & (1 << col)){
                fb_put_pixel(x+col, y+row, color);
            }
        }
    }
}

void gfx_draw_string(int x, int y, const char *s, uint32_t color){
    int cx = x;
    while(*s){
        if(*s=='\n'){ y+=10; cx=x; s++; continue; }
        gfx_draw_char(cx, y, *s, color);
        cx += 8;
        s++;
    }
}

void gfx_draw_string_bg(int x, int y, const char *s, uint32_t fg, uint32_t bg){
    int cx = x;
    while(*s){
        // fill bg 8x8
        for(int dy=0; dy<8; dy++) for(int dx=0; dx<8; dx++) fb_put_pixel(cx+dx, y+dy, bg);
        gfx_draw_char(cx, y, *s, fg);
        cx += 8;
        s++;
    }
}
