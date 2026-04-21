#include "vga.h"
#include <stdint.h>
#include <string.h>

#define VRAM ((volatile uint8_t *)0x50000000)

// 對齊記憶體，防止對齊錯誤
static uint8_t vbuf[WIDTH * HEIGHT] __attribute__((aligned(16)));
extern void vga_hw_init();
extern unsigned char hankaku[128][16];

static void vga_set_palette_332(void) {
    volatile uint8_t *dac_index = (volatile uint8_t *)(0x40000000UL + 0x408);
    volatile uint8_t *dac_data = (volatile uint8_t *)(0x40000000UL + 0x409);
    static const uint8_t ui_pal[16][3] = {
        {0x08, 0x09, 0x0B}, {0x10, 0x11, 0x13}, {0x16, 0x18, 0x1B}, {0x1D, 0x20, 0x24},
        {0x25, 0x29, 0x2E}, {0x2E, 0x33, 0x38}, {0x38, 0x3E, 0x44}, {0x43, 0x49, 0x4F},
        {0x4E, 0x55, 0x5B}, {0x5A, 0x61, 0x68}, {0x67, 0x6E, 0x76}, {0x76, 0x7D, 0x85},
        {0x86, 0x8D, 0x95}, {0x98, 0x9E, 0xA5}, {0xB3, 0xB8, 0xBD}, {0xF0, 0xF2, 0xF4}
    };
    *dac_index = 0;
    for (int i = 0; i < 16; i++) {
        *dac_data = ui_pal[i][0];
        *dac_data = ui_pal[i][1];
        *dac_data = ui_pal[i][2];
    }
    for (int i = 0; i < 216; i++) {
        int r = i / 36;
        int g = (i / 6) % 6;
        int b = i % 6;
        *dac_data = (uint8_t)((r * 63) / 5);
        *dac_data = (uint8_t)((g * 63) / 5);
        *dac_data = (uint8_t)((b * 63) / 5);
    }
    for (int i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)((i * 63) / 23);
        *dac_data = v;
        *dac_data = v;
        *dac_data = v;
    }
}

void vga_init() {
    vga_hw_init();
    volatile uint16_t *vbe = (volatile uint16_t *)(0x40000000UL + 0x500);
    vbe[4] = 0x00; vbe[1] = WIDTH; vbe[2] = HEIGHT; vbe[3] = 8; vbe[4] = 0x41;
    vga_set_palette_332();
    memset(vbuf, 0, sizeof(vbuf));
}
uint8_t *vga_get_vbuf(void) { return vbuf; }
void vga_update() { memcpy((void *)VRAM, vbuf, WIDTH * HEIGHT); }

void putpixel(int x, int y, int color) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
        vbuf[x + y * WIDTH] = (uint8_t)color;
    }
}

void draw_rect_fill(int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int ty = y + j;
        if (ty < 0 || ty >= HEIGHT) continue;
        int tx_start = (x < 0) ? 0 : x;
        int tx_end = (x + w > WIDTH) ? WIDTH : (x + w);
        int tw = tx_end - tx_start;
        if (tw > 0) {
            uint8_t *ptr = &vbuf[tx_start + ty * WIDTH];
            memset(ptr, (uint8_t)color, tw);
        }
    }
}

void draw_hline(int x, int y, int w, int color) {
    draw_rect_fill(x, y, w, 1, color);
}

void draw_vline(int x, int y, int h, int color) {
    draw_rect_fill(x, y, 1, h, color);
}

void draw_vertical_gradient(int x, int y, int w, int h, int top_color, int bottom_color) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int c = top_color;
        if (h > 1) c = top_color + ((bottom_color - top_color) * j) / (h - 1);
        draw_rect_fill(x, y + j, w, 1, c);
    }
}

void draw_bevel_rect(int x, int y, int w, int h, int fill, int light, int dark) {
    if (w <= 0 || h <= 0) return;
    draw_rect_fill(x, y, w, h, fill);
    draw_hline(x, y, w, light);
    draw_hline(x, y + 1, w, light);
    draw_vline(x, y, h, light);
    draw_vline(x + 1, y, h, light);
    draw_hline(x, y + h - 2, w, dark);
    draw_hline(x, y + h - 1, w, dark);
    draw_vline(x + w - 2, y, h, dark);
    draw_vline(x + w - 1, y, h, dark);
}

void draw_round_rect_fill(int x, int y, int w, int h, int r, int color) {
    if (r <= 0 || w < 2*r || h < 2*r) { draw_rect_fill(x, y, w, h, color); return; }
    draw_rect_fill(x, y + r, w, h - 2 * r, color);
    draw_rect_fill(x + r, y, w - 2 * r, r, color);
    draw_rect_fill(x + r, y + h - r, w - 2 * r, r, color);
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            if (i*i + j*j <= r*r) {
                putpixel(x + r - 1 - i, y + r - 1 - j, color);
                putpixel(x + w - r + i, y + r - 1 - j, color);
                putpixel(x + r - 1 - i, y + h - r + j, color);
                putpixel(x + w - r + i, y + h - r + j, color);
            }
        }
    }
}

void draw_round_rect_wire(int x, int y, int w, int h, int r, int color) {
    if (r <= 0 || w < 2*r || h < 2*r) return;
    draw_rect_fill(x + r, y, w - 2 * r, 2, color);
    draw_rect_fill(x + r, y + h - 2, w - 2 * r, 2, color);
    draw_rect_fill(x, y + r, 2, h - 2 * r, color);
    draw_rect_fill(x + w - 2, y + r, 2, h - 2 * r, color);
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int d2 = i*i + j*j;
            if (d2 <= r*r && d2 > (r-2)*(r-2)) {
                putpixel(x + r - 1 - i, y + r - 1 - j, color);
                putpixel(x + w - r + i, y + r - 1 - j, color);
                putpixel(x + r - 1 - i, y + h - r + j, color);
                putpixel(x + w - r + i, y + h - r + j, color);
            }
        }
    }
}

void draw_char(int x, int y, char c, int color) {
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return; // 嚴格過濾非法字元
    unsigned char *bitmap = hankaku[uc];
    for (int i = 0; i < 16; i++) {
        int ty = y + i; if (ty < 0 || ty >= HEIGHT) continue;
        for (int j = 0; j < 8; j++) {
            int tx = x + j; if (tx < 0 || tx >= WIDTH) continue;
            if (bitmap[i] & (0x80 >> j)) vbuf[tx + ty * WIDTH] = (uint8_t)color;
        }
    }
}
void draw_char_scaled(int x, int y, char c, int color, int scale) {
    if (scale <= 1) {
        draw_char(x, y, c, color);
        return;
    }
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return;
    unsigned char *bitmap = hankaku[uc];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 8; j++) {
            if (!(bitmap[i] & (0x80 >> j))) continue;
            draw_rect_fill(x + j * scale, y + i * scale, scale, scale, color);
        }
    }
}
void draw_char_scaled_clipped(int x, int y, char c, int color, int scale, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    if (scale <= 1) scale = 1;
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return;
    unsigned char *bitmap = hankaku[uc];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 8; j++) {
            if (!(bitmap[i] & (0x80 >> j))) continue;
            int px0 = x + j * scale;
            int py0 = y + i * scale;
            for (int sy = 0; sy < scale; sy++) {
                int py = py0 + sy;
                if (py < clip_y0 || py >= clip_y1 || py < 0 || py >= HEIGHT) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = px0 + sx;
                    if (px < clip_x0 || px >= clip_x1 || px < 0 || px >= WIDTH) continue;
                    vbuf[px + py * WIDTH] = (uint8_t)color;
                }
            }
        }
    }
}
void draw_text(int x, int y, const char *s, int color) {
    if (!s) return;
    while (*s) { draw_char(x, y, *s++, color); x += 8; }
}
void draw_text_scaled(int x, int y, const char *s, int color, int scale) {
    if (!s) return;
    if (scale <= 1) {
        draw_text(x, y, s, color);
        return;
    }
    while (*s) {
        draw_char_scaled(x, y, *s++, color, scale);
        x += 8 * scale;
    }
}
void draw_text_scaled_clipped(int x, int y, const char *s, int color, int scale, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    if (!s) return;
    if (scale <= 1) scale = 1;
    while (*s) {
        draw_char_scaled_clipped(x, y, *s++, color, scale, clip_x0, clip_y0, clip_x1, clip_y1);
        x += 8 * scale;
    }
}
void draw_cursor(int x, int y, int color, int mode) {
    switch (mode) {
    case CURSOR_SIZE_H:
        for (int i = -7; i <= 7; i++) putpixel(x + i, y, color);
        for (int i = -3; i <= 3; i++) { putpixel(x - 7 + i, y + i, color); putpixel(x - 7 + i, y - i, color); }
        for (int i = -3; i <= 3; i++) { putpixel(x + 7 - i, y + i, color); putpixel(x + 7 - i, y - i, color); }
        break;
    case CURSOR_SIZE_V:
        for (int i = -7; i <= 7; i++) putpixel(x, y + i, color);
        for (int i = -3; i <= 3; i++) { putpixel(x + i, y - 7 + i, color); putpixel(x - i, y - 7 + i, color); }
        for (int i = -3; i <= 3; i++) { putpixel(x + i, y + 7 - i, color); putpixel(x - i, y + 7 - i, color); }
        break;
    case CURSOR_SIZE_D1:
        for (int i = -6; i <= 6; i++) putpixel(x + i, y + i, color);
        for (int i = 0; i <= 3; i++) {
            putpixel(x - 6 + i, y - 6, color);
            putpixel(x - 6, y - 6 + i, color);
            putpixel(x + 6 - i, y + 6, color);
            putpixel(x + 6, y + 6 - i, color);
        }
        break;
    case CURSOR_SIZE_D2:
        for (int i = -6; i <= 6; i++) putpixel(x + i, y - i, color);
        for (int i = 0; i <= 3; i++) {
            putpixel(x - 6 + i, y + 6, color);
            putpixel(x - 6, y + 6 - i, color);
            putpixel(x + 6 - i, y - 6, color);
            putpixel(x + 6, y - 6 + i, color);
        }
        break;
    default:
        for (int row = 0; row < 11; row++) {
            int w = (row < 6) ? (row + 1) : (11 - row);
            if (w < 1) w = 1;
            for (int col = 0; col < w; col++) {
                putpixel(x + col, y + row, color);
            }
        }
        for (int i = 0; i < 6; i++) {
            putpixel(x + 4, y + 4 + i, color);
        }
        break;
    }
}
