#include "user_internal.h"
#include <stdarg.h>

void draw_window(struct Window *w) {
    if (!w->active || w->minimized) return;
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    if (w->maximized) { x = 0; y = 0; ww = WIDTH; wh = DESKTOP_H; }
    for(int j=y; j<y+wh && j<DESKTOP_H; j++) { if(j<0) continue; for(int i=x; i<x+ww && i<WIDTH; i++) { if(i<0) continue; sheet_map[j*WIDTH + i] = w->id + 1; } }
    draw_rect_fill(x + 5, y + 6, ww, wh, UI_C_SHADOW);
    draw_round_rect_fill(x, y, ww, wh, UI_RADIUS, COL_WIN_BG);
    draw_round_rect_wire(x, y, ww, wh, UI_RADIUS, COL_WIN_BRD);
    int color = (active_win_idx == w->id) ? COL_TITLE_ACT : COL_TITLE_INACT;
    int title_top = (active_win_idx == w->id) ? 3 : 2;
    int title_bot = (active_win_idx == w->id) ? 2 : 1;
    draw_round_rect_fill(x+2, y+2, ww-4, 26, UI_RADIUS-2, color); 
    draw_vertical_gradient(x + 3, y + 3, ww - 6, 10, title_top, title_bot);
    
    draw_text(x + 10, y + 6, w->title, UI_C_TEXT);
    
    // 按鈕繪製
    int bx = x + ww - 34, by = y + 5;
    draw_bevel_rect(bx, by, 22, 16, 2, 14, 1); draw_text(bx+5, by+2, "X", UI_C_TEXT);
    bx -= 25; draw_bevel_rect(bx, by, 22, 16, 3, 14, 1); draw_text(bx+5, by+2, "M", UI_C_TEXT);
    bx -= 25; draw_bevel_rect(bx, by, 22, 16, 1, 14, 1); draw_text(bx+5, by+2, "-", UI_C_TEXT);

    if (w->kind == WINDOW_KIND_NETSURF) {
        netsurf_render_frame(w, x, y, ww, wh);
        return;
    }
    if (w->kind == WINDOW_KIND_IMAGE) {
        int scale = w->image_scale > 0 ? w->image_scale : 1;
        for (int py = 0; py < w->image_h && py < 240; py++) {
            for (int px = 0; px < w->image_w && px < 320; px++) {
                int r, g, b; rgb_from_rgb565(w->image[py * IMG_MAX_W + px], &r, &g, &b);
                int idx = palette_index_from_rgb(r, g, b);
                putpixel(x + 10 + px*scale, y + 34 + py*scale, idx);
            }
        }
        return;
    }
    if (w->kind == WINDOW_KIND_EDITOR) { editor_render(w, x, y, ww, wh); return; }
    if (w->kind == WINDOW_KIND_DEMO3D) { draw_demo3d_content(w, x, y, ww, wh); return; }

    // 終端機渲染 (簡化版確保不報錯)
    int rv = terminal_visible_rows(w);
    for (int i = 0; i < rv; i++) {
        int idx = w->v_offset + i;
        if (idx < w->total_rows) draw_text(x + 10, y + 40 + (i * 16), w->lines[idx], COL_TEXT);
    }
}

void draw_taskbar() {
    draw_bevel_rect(0, DESKTOP_H, WIDTH, TASKBAR_H, UI_C_PANEL, UI_C_TEXT_MUTED, UI_C_PANEL_DEEP);
    draw_text(18, DESKTOP_H + 8, "[ROOT]", UI_C_TEXT);
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i].active) {
        int tx = taskbar_button_x(i);
        draw_bevel_rect(tx, DESKTOP_H + 5, TASKBAR_BTN_W, 20, UI_C_PANEL, UI_C_TEXT_DIM, UI_C_PANEL_DEEP);
        draw_text(tx + 5, DESKTOP_H + 8, wins[i].title, UI_C_TEXT);
    }
}
