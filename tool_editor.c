#include "user_internal.h"

#define TASKBAR_H 30
#define DESKTOP_H (HEIGHT - TASKBAR_H)
#define COL_TEXT UI_C_TEXT

extern void editor_handle_key(struct Window *w, char key);
extern void editor_load_bytes(struct Window *w, const unsigned char *src, uint32_t size);
extern int editor_load_file_window(struct Window *w, uint32_t start_line);
extern void editor_clear(struct Window *w);
extern void editor_set_status(struct Window *w, const char *msg);
extern void editor_render(struct Window *w, int x, int y, int ww, int wh);

void editor_set_status(struct Window *w, const char *msg) {
    if (!msg) msg = "";
    lib_strcpy(w->editor_status, msg);
}

void editor_clear(struct Window *w) {
    w->editor_mode = 0;
    w->editor_line_count = 1;
    w->editor_cursor_row = 0;
    w->editor_cursor_col = 0;
    w->editor_scroll_row = 0;
    w->editor_cmd_len = 0;
    w->editor_cmd_cursor = 0;
    w->editor_dirty = 0;
    w->editor_pending_d = 0;
    w->editor_readonly = 0;
    w->editor_lazy = 0;
    w->editor_loaded_complete = 0;
    w->editor_file_bno = 0;
    w->editor_file_size = 0;
    w->editor_loaded_start_line = 0;
    w->editor_lines[0][0] = '\0';
    w->editor_cmd[0] = '\0';
    w->editor_status[0] = '\0';
}

static int editor_line_len(const char *line) {
    int n = 0;
    while (n < EDITOR_LINE_LEN - 1 && line[n]) n++;
    return n;
}

static void editor_clamp_cursor(struct Window *w) {
    if (w->editor_line_count < 1) w->editor_line_count = 1;
    if (w->editor_line_count > EDITOR_MAX_LINES) w->editor_line_count = EDITOR_MAX_LINES;
    if (w->editor_cursor_row < 0) w->editor_cursor_row = 0;
    if (w->editor_cursor_row >= w->editor_line_count) w->editor_cursor_row = w->editor_line_count - 1;
    int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    if (w->editor_cursor_col < 0) w->editor_cursor_col = 0;
    if (w->editor_cursor_col > len) w->editor_cursor_col = len;
}

static int editor_visible_rows(struct Window *w) {
    int wh = w->maximized ? DESKTOP_H : w->h;
    int char_h = terminal_char_h(w);
    int line_h = char_h + 2;
    int rows = (wh - 56) / line_h;
    if (rows < 1) rows = 1;
    return rows;
}

static void editor_scroll_to_cursor(struct Window *w) {
    int rows = editor_visible_rows(w);
    if (w->editor_cursor_row < w->editor_scroll_row) {
        w->editor_scroll_row = w->editor_cursor_row;
    } else if (w->editor_cursor_row >= w->editor_scroll_row + rows) {
        w->editor_scroll_row = w->editor_cursor_row - rows + 1;
    }
    if (w->editor_scroll_row < 0) w->editor_scroll_row = 0;
    if (w->editor_scroll_row > w->editor_line_count - rows) w->editor_scroll_row = w->editor_line_count - rows;
    if (w->editor_scroll_row < 0) w->editor_scroll_row = 0;
}

static void editor_join_with_previous(struct Window *w) {
    if (w->editor_cursor_row <= 0) return;
    int prev = w->editor_cursor_row - 1;
    int prev_len = editor_line_len(w->editor_lines[prev]);
    int cur_len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    if (prev_len + cur_len >= EDITOR_LINE_LEN - 1) return;
    if (cur_len > 0) {
        memcpy(w->editor_lines[prev] + prev_len, w->editor_lines[w->editor_cursor_row], cur_len);
        w->editor_lines[prev][prev_len + cur_len] = '\0';
    }
    for (int i = w->editor_cursor_row; i < w->editor_line_count - 1; i++) {
        memcpy(w->editor_lines[i], w->editor_lines[i + 1], EDITOR_LINE_LEN);
    }
    w->editor_line_count--;
    if (w->editor_line_count < 1) w->editor_line_count = 1;
    w->editor_cursor_row = prev;
    w->editor_cursor_col = prev_len;
    w->editor_dirty = 1;
}

static void editor_delete_current_line(struct Window *w) {
    if (w->editor_line_count <= 1) {
        w->editor_line_count = 1;
        w->editor_cursor_row = 0;
        w->editor_cursor_col = 0;
        w->editor_lines[0][0] = '\0';
        w->editor_dirty = 1;
        return;
    }
    int row = w->editor_cursor_row;
    if (row < 0) row = 0;
    if (row >= w->editor_line_count) row = w->editor_line_count - 1;
    for (int i = row; i < w->editor_line_count - 1; i++) {
        memcpy(w->editor_lines[i], w->editor_lines[i + 1], EDITOR_LINE_LEN);
    }
    w->editor_line_count--;
    if (w->editor_cursor_row >= w->editor_line_count) w->editor_cursor_row = w->editor_line_count - 1;
    if (w->editor_cursor_row < 0) w->editor_cursor_row = 0;
    w->editor_cursor_col = 0;
    w->editor_lines[w->editor_cursor_row][0] = '\0';
    w->editor_dirty = 1;
}

static void editor_move_home(struct Window *w) {
    w->editor_cursor_col = 0;
}

static void editor_move_end(struct Window *w) {
    w->editor_cursor_col = editor_line_len(w->editor_lines[w->editor_cursor_row]);
}

static void editor_page_up(struct Window *w) {
    int rows = editor_visible_rows(w);
    if (w->editor_lazy && w->editor_cursor_row == 0 && w->editor_loaded_start_line > 0) {
        uint32_t start = (w->editor_loaded_start_line > (uint32_t)EDITOR_MAX_LINES) ?
                         w->editor_loaded_start_line - EDITOR_MAX_LINES : 0;
        if (editor_load_file_window(w, start) == 0) {
            w->editor_cursor_row = w->editor_line_count - 1;
            w->editor_scroll_row = w->editor_line_count - rows;
            if (w->editor_scroll_row < 0) w->editor_scroll_row = 0;
        }
        return;
    }
    w->editor_cursor_row -= rows;
    if (w->editor_cursor_row < 0) w->editor_cursor_row = 0;
    int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    if (w->editor_cursor_col > len) w->editor_cursor_col = len;
}

static void editor_page_down(struct Window *w) {
    int rows = editor_visible_rows(w);
    if (w->editor_lazy && w->editor_cursor_row + 1 >= w->editor_line_count && !w->editor_loaded_complete) {
        if (editor_load_file_window(w, w->editor_loaded_start_line + (uint32_t)w->editor_line_count) == 0) {
            w->editor_cursor_row = 0;
            w->editor_scroll_row = 0;
        }
        return;
    }
    w->editor_cursor_row += rows;
    if (w->editor_cursor_row >= w->editor_line_count) w->editor_cursor_row = w->editor_line_count - 1;
    int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    if (w->editor_cursor_col > len) w->editor_cursor_col = len;
}

static void editor_insert_char(struct Window *w, char ch) {
    if (w->editor_cursor_row < 0 || w->editor_cursor_row >= EDITOR_MAX_LINES) return;
    int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    if (len >= EDITOR_LINE_LEN - 2) return;
    if (w->editor_cursor_col < 0) w->editor_cursor_col = 0;
    if (w->editor_cursor_col > len) w->editor_cursor_col = len;
    for (int i = len; i >= w->editor_cursor_col; i--) {
        w->editor_lines[w->editor_cursor_row][i + 1] = w->editor_lines[w->editor_cursor_row][i];
    }
    w->editor_lines[w->editor_cursor_row][w->editor_cursor_col] = ch;
    w->editor_cursor_col++;
    w->editor_dirty = 1;
}

static void editor_newline(struct Window *w) {
    if (w->editor_line_count >= EDITOR_MAX_LINES) return;
    int row = w->editor_cursor_row;
    int len = editor_line_len(w->editor_lines[row]);
    int col = w->editor_cursor_col;
    if (col < 0) col = 0;
    if (col > len) col = len;
    char tail[EDITOR_LINE_LEN];
    int tail_len = len - col;
    if (tail_len < 0) tail_len = 0;
    memcpy(tail, w->editor_lines[row] + col, tail_len);
    tail[tail_len] = '\0';
    w->editor_lines[row][col] = '\0';
    for (int i = w->editor_line_count; i > row + 1; i--) {
        memcpy(w->editor_lines[i], w->editor_lines[i - 1], EDITOR_LINE_LEN);
    }
    memcpy(w->editor_lines[row + 1], tail, tail_len + 1);
    w->editor_line_count++;
    w->editor_cursor_row = row + 1;
    w->editor_cursor_col = 0;
    w->editor_dirty = 1;
}

static int editor_collect_bytes(struct Window *w, unsigned char *dst, uint32_t max_size, uint32_t *out_size) {
    uint32_t pos = 0;
    for (int row = 0; row < w->editor_line_count; row++) {
        int len = editor_line_len(w->editor_lines[row]);
        if (pos + (uint32_t)len + 1U > max_size) return -1;
        memcpy(dst + pos, w->editor_lines[row], len);
        pos += (uint32_t)len;
        if (row != w->editor_line_count - 1) dst[pos++] = '\n';
    }
    if (w->editor_line_count == 0 && max_size > 0) {
        dst[0] = '\0';
        *out_size = 0;
        return 0;
    }
    *out_size = pos;
    return 0;
}

void editor_load_bytes(struct Window *w, const unsigned char *src, uint32_t size) {
    int row = 0;
    int col = 0;
    memset(w->editor_lines, 0, sizeof(w->editor_lines));
    for (uint32_t i = 0; i < size && row < EDITOR_MAX_LINES; i++) {
        unsigned char ch = src[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            w->editor_lines[row][col] = '\0';
            row++;
            col = 0;
            continue;
        }
        if (col < EDITOR_LINE_LEN - 1) {
            w->editor_lines[row][col++] = (char)ch;
            w->editor_lines[row][col] = '\0';
        }
    }
    if (row < EDITOR_MAX_LINES) {
        w->editor_lines[row][col] = '\0';
        w->editor_line_count = row + 1;
    } else {
        w->editor_line_count = EDITOR_MAX_LINES;
    }
    if (w->editor_line_count < 1) w->editor_line_count = 1;
    w->editor_cursor_row = 0;
    w->editor_cursor_col = 0;
    w->editor_scroll_row = 0;
    w->editor_mode = 0;
    w->editor_cmd_len = 0;
    w->editor_cmd_cursor = 0;
    w->editor_dirty = 0;
    w->editor_pending_d = 0;
    w->editor_readonly = 0;
    w->editor_lazy = 0;
    w->editor_loaded_complete = 1;
    w->editor_loaded_start_line = 0;
    w->editor_status[0] = '\0';
}

static void editor_touch_status(struct Window *w, const char *msg) {
    editor_set_status(w, msg);
}

static int editor_save_file(struct Window *w) {
    unsigned char buf[EDITOR_MAX_LINES * EDITOR_LINE_LEN + EDITOR_MAX_LINES];
    uint32_t size = 0;
    if (w->editor_readonly) {
        editor_touch_status(w, "Read-only truncated view");
        return -3;
    }
    if (editor_collect_bytes(w, buf, sizeof(buf), &size) != 0) {
        editor_touch_status(w, "File too big");
        return -1;
    }
    if (store_file_bytes(w, w->editor_name, buf, size) != 0) {
        editor_touch_status(w, "Save failed");
        return -2;
    }
    w->editor_dirty = 0;
    editor_touch_status(w, "Saved");
    return 0;
}

static void editor_open_insert_mode(struct Window *w) {
    w->editor_pending_d = 0;
    w->editor_mode = 1;
    editor_set_status(w, "-- INSERT --");
}

static void editor_open_command_mode(struct Window *w) {
    w->editor_pending_d = 0;
    w->editor_mode = 2;
    w->editor_cmd_len = 0;
    w->editor_cmd_cursor = 0;
    w->editor_cmd[0] = '\0';
    editor_set_status(w, ":");
}

static void editor_leave_command_mode(struct Window *w) {
    w->editor_mode = 0;
    w->editor_cmd_len = 0;
    w->editor_cmd_cursor = 0;
    w->editor_pending_d = 0;
    if (w->editor_status[0] == '\0') editor_set_status(w, "i=insert  :wq=save+quit  :q=quit");
}

static void editor_move_left(struct Window *w) {
    if (w->editor_cursor_col > 0) w->editor_cursor_col--;
    else if (w->editor_cursor_row > 0) {
        w->editor_cursor_row--;
        w->editor_cursor_col = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    }
}

static void editor_move_right(struct Window *w) {
    int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
    if (w->editor_cursor_col < len) w->editor_cursor_col++;
    else if (w->editor_cursor_row + 1 < w->editor_line_count) {
        w->editor_cursor_row++;
        w->editor_cursor_col = 0;
    }
}

static void editor_move_up(struct Window *w) {
    if (w->editor_cursor_row > 0) {
        w->editor_cursor_row--;
        int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
        if (w->editor_cursor_col > len) w->editor_cursor_col = len;
    } else if (w->editor_lazy && w->editor_loaded_start_line > 0) {
        uint32_t start = (w->editor_loaded_start_line > (uint32_t)EDITOR_MAX_LINES) ?
                         w->editor_loaded_start_line - EDITOR_MAX_LINES : 0;
        if (editor_load_file_window(w, start) == 0) {
            w->editor_cursor_row = w->editor_line_count - 1;
            w->editor_cursor_col = 0;
        }
    }
}

static void editor_move_down(struct Window *w) {
    if (w->editor_cursor_row + 1 < w->editor_line_count) {
        w->editor_cursor_row++;
        int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
        if (w->editor_cursor_col > len) w->editor_cursor_col = len;
    } else if (w->editor_lazy && !w->editor_loaded_complete) {
        if (editor_load_file_window(w, w->editor_loaded_start_line + (uint32_t)w->editor_line_count) == 0) {
            w->editor_cursor_row = 0;
            w->editor_cursor_col = 0;
        }
    }
}

static int editor_parse_positive_int(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int seen = 0;
    if (!s || !out) return 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') {
        seen = 1;
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    while (*s == ' ') s++;
    if (!seen || *s != '\0') return 0;
    *out = v;
    return 1;
}

static void editor_apply_command(struct Window *w) {
    w->editor_cmd[w->editor_cmd_len] = '\0';
    if (strcmp(w->editor_cmd, "w") == 0) {
        editor_save_file(w);
    } else if (strcmp(w->editor_cmd, "wq") == 0 || strcmp(w->editor_cmd, "x") == 0) {
        if (editor_save_file(w) == 0) close_window(w->id);
    } else if (strcmp(w->editor_cmd, "q") == 0) {
        if (!w->editor_dirty) close_window(w->id);
        else editor_set_status(w, "No write since last change");
    } else if (strcmp(w->editor_cmd, "q!") == 0) {
        close_window(w->id);
    } else if (w->editor_cmd[0] != '\0') {
        uint32_t line_no = 0;
        if (editor_parse_positive_int(w->editor_cmd, &line_no) && line_no > 0) {
            uint32_t target = line_no - 1U;
            if (w->editor_lazy) {
                uint32_t start = (target > (uint32_t)(EDITOR_MAX_LINES / 2)) ?
                                 target - (uint32_t)(EDITOR_MAX_LINES / 2) : 0;
                if (editor_load_file_window(w, start) == 0) {
                    uint32_t rel = target - w->editor_loaded_start_line;
                    if (rel >= (uint32_t)w->editor_line_count) rel = (uint32_t)(w->editor_line_count - 1);
                    w->editor_cursor_row = (int)rel;
                    w->editor_cursor_col = 0;
                    editor_scroll_to_cursor(w);
                }
            } else {
                if (target >= (uint32_t)w->editor_line_count) target = (uint32_t)(w->editor_line_count - 1);
                w->editor_cursor_row = (int)target;
                w->editor_cursor_col = 0;
                editor_scroll_to_cursor(w);
            }
        } else {
            editor_set_status(w, "Unknown command");
        }
    }
    if (w->active && w->kind == WINDOW_KIND_EDITOR) {
        w->editor_mode = 0;
        w->editor_cmd_len = 0;
        w->editor_cmd_cursor = 0;
    }
}

static void editor_find_next(struct Window *w, int forward) {
    if (w->editor_cmd_len <= 0) return;
    w->editor_cmd[w->editor_cmd_len] = '\0';
    int start_row = w->editor_cursor_row, start_col = w->editor_cursor_col + (forward ? 1 : -1);
    for (int i = 0; i < w->editor_line_count; i++) {
        int r = (start_row + (forward ? i : -i) + w->editor_line_count) % w->editor_line_count;
        char *line = w->editor_lines[r]; int len = editor_line_len(line);
        int c_start = (i == 0) ? start_col : (forward ? 0 : len - 1);
        if (forward) {
            for (int c = c_start; c <= len - w->editor_cmd_len; c++) {
                if (strncmp(line + c, w->editor_cmd, w->editor_cmd_len) == 0) {
                    w->editor_cursor_row = r; w->editor_cursor_col = c; return;
                }
            }
        } else {
            for (int c = c_start; c >= 0; c--) {
                if (c <= len - w->editor_cmd_len && strncmp(line + c, w->editor_cmd, w->editor_cmd_len) == 0) {
                    w->editor_cursor_row = r; w->editor_cursor_col = c; return;
                }
            }
        }
    }
}

void editor_handle_key(struct Window *w, char key) {
    if (w->editor_mode == 2 || w->editor_mode == 3) {
        w->editor_pending_d = 0;
        if (key == 27) {
            w->editor_mode = 0; w->editor_cmd_len = 0;
            editor_set_status(w, "i=insert  :wq=save+quit  :q=quit"); return;
        }
        if (key == 10 || key == '\r') {
            if (w->editor_mode == 3) { editor_find_next(w, 1); w->editor_mode = 0; }
            else editor_apply_command(w);
            editor_clamp_cursor(w); editor_scroll_to_cursor(w); return;
        }
        if (key == 8 || key == 127) {
            if (w->editor_cmd_len > 0) w->editor_cmd_len--;
            w->editor_cmd[w->editor_cmd_len] = '\0';
            editor_set_status(w, (w->editor_mode == 3) ? "/" : ":"); lib_strcat(w->editor_status, w->editor_cmd); return;
        }
        if (key >= 32 && key < 127 && w->editor_cmd_len < COLS - 2) {
            w->editor_cmd[w->editor_cmd_len++] = key; w->editor_cmd[w->editor_cmd_len] = '\0';
            editor_set_status(w, (w->editor_mode == 3) ? "/" : ":"); lib_strcat(w->editor_status, w->editor_cmd);
        }
        return;
    }

    if (key == 27) {
        w->editor_mode = 0; w->editor_pending_d = 0;
        editor_set_status(w, "i=insert  :wq=save+quit  :q=quit"); return;
    }
    if (w->editor_mode == 0) {
        if (w->editor_pending_d) {
            if (key == 'd') { editor_delete_current_line(w); w->editor_pending_d = 0; }
            else w->editor_pending_d = 0;
            editor_clamp_cursor(w); editor_scroll_to_cursor(w); return;
        }
        if (key == 'i') { editor_open_insert_mode(w); }
        else if (key == ':') { editor_open_command_mode(w); }
        else if (key == '/') { w->editor_mode = 3; w->editor_cmd_len = 0; w->editor_cmd[0] = '\0'; editor_set_status(w, "/"); return; }
        else if (key == 'n') { editor_find_next(w, 1); editor_clamp_cursor(w); editor_scroll_to_cursor(w); return; }
        else if (key == 'N') { editor_find_next(w, 0); editor_clamp_cursor(w); editor_scroll_to_cursor(w); return; }
        else if (key == 'h') { editor_move_left(w); }
        else if (key == 'l') { editor_move_right(w); } else if (key == 'k') {
            editor_move_up(w);
        } else if (key == 'j') {
            editor_move_down(w);
        } else if (key == 'o') {
            if (w->editor_line_count < EDITOR_MAX_LINES) {
                if (w->editor_cursor_row + 1 < w->editor_line_count) {
                    for (int i = w->editor_line_count; i > w->editor_cursor_row + 1; i--) {
                        memcpy(w->editor_lines[i], w->editor_lines[i - 1], EDITOR_LINE_LEN);
                    }
                    w->editor_line_count++;
                } else {
                    w->editor_line_count++;
                }
                w->editor_cursor_row++;
                w->editor_cursor_col = 0;
                w->editor_lines[w->editor_cursor_row][0] = '\0';
                editor_open_insert_mode(w);
                w->editor_dirty = 1;
            }
        } else if (key == 'O') {
            if (w->editor_line_count < EDITOR_MAX_LINES) {
                for (int i = w->editor_line_count; i > w->editor_cursor_row; i--) {
                    memcpy(w->editor_lines[i], w->editor_lines[i - 1], EDITOR_LINE_LEN);
                }
                w->editor_line_count++;
                w->editor_lines[w->editor_cursor_row][0] = '\0';
                w->editor_cursor_col = 0;
                editor_open_insert_mode(w);
                w->editor_dirty = 1;
            }
        } else if (key == 'x') {
            int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
            if (w->editor_cursor_col < len) {
                for (int i = w->editor_cursor_col; i < len; i++) {
                    w->editor_lines[w->editor_cursor_row][i] = w->editor_lines[w->editor_cursor_row][i + 1];
                }
                w->editor_dirty = 1;
            }
        } else if (key == 'd') {
            w->editor_pending_d = 1;
            editor_set_status(w, "dd=delete line");
            return;
        } else if (key == 0x10) {
            editor_move_up(w);
        } else if (key == 0x11) {
            editor_move_down(w);
        } else if (key == 0x12) {
            editor_move_left(w);
        } else if (key == 0x13) {
            editor_move_right(w);
        } else if (key == 0x14) {
            int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
            if (w->editor_cursor_col < len) {
                for (int i = w->editor_cursor_col; i < len; i++) {
                    w->editor_lines[w->editor_cursor_row][i] = w->editor_lines[w->editor_cursor_row][i + 1];
                }
                w->editor_dirty = 1;
            }
        } else if (key == 0x15) { // Home
            editor_move_home(w);
        } else if (key == 0x16) { // End
            editor_move_end(w);
        } else if (key == 0x17) { // PageUp
            editor_page_up(w);
        } else if (key == 0x18) { // PageDown
            editor_page_down(w);
        } else if (key == 0x19) {
            editor_open_insert_mode(w);
        }
        editor_clamp_cursor(w);
        editor_scroll_to_cursor(w);
        return;
    }

    if (gui_ctrl_pressed && (key == 'v' || key == 'V')) {
        const char *clip = terminal_clipboard_text();
        if (clip) {
            while (*clip) {
                if (*clip == '\n') editor_newline(w);
                else if (*clip != '\r') editor_insert_char(w, *clip);
                clip++;
            }
        }
        editor_clamp_cursor(w);
        editor_scroll_to_cursor(w);
        return;
    }

    if (key == 0x15) { // Home in Insert Mode
        editor_move_home(w);
    } else if (key == 0x16) { // End in Insert Mode
        editor_move_end(w);
    } else if (key == 0x17) { // PageUp in Insert Mode
        editor_page_up(w);
    } else if (key == 0x18) { // PageDown in Insert Mode
        editor_page_down(w);
    } else if (key == 0x10) { // Up in Insert Mode
        editor_move_up(w);
    } else if (key == 0x11) { // Down in Insert Mode
        editor_move_down(w);
    } else if (key == 0x12) { // Left in Insert Mode
        editor_move_left(w);
    } else if (key == 0x13) { // Right in Insert Mode
        editor_move_right(w);
    } else if (key == 8 || key == 127) {
        if (w->editor_cursor_col > 0) {
            editor_move_left(w);
            int len = editor_line_len(w->editor_lines[w->editor_cursor_row]);
            for (int i = w->editor_cursor_col; i < len; i++) {
                w->editor_lines[w->editor_cursor_row][i] = w->editor_lines[w->editor_cursor_row][i + 1];
            }
            w->editor_dirty = 1;
        } else {
            editor_join_with_previous(w);
        }
    } else if (key == 10 || key == '\r') {
        editor_newline(w);
    } else if (key >= 32 && key < 127) {
        editor_insert_char(w, key);
    }
    editor_clamp_cursor(w);
    editor_scroll_to_cursor(w);
}

void editor_render(struct Window *w, int x, int y, int ww, int wh) {
    extern void draw_rect_fill(int,int,int,int,int);
    extern void draw_vline(int,int,int,int);
    int char_w = terminal_char_w(w);
    int char_h = terminal_char_h(w);
    int line_h = char_h + 2;
    int scale = terminal_font_scale(w);
    
    // 行號區域寬度: 4個字元 + 左右間距
    int gutter_w = (4 * char_w) + 8;
    int text_x = x + 10 + gutter_w;
    int text_y = y + 36;
    int status_y = y + wh - 18;
    int rows = editor_visible_rows(w);
    char display_path[64];
    display_path[0] = '\0';
    if (w->editor_path[0]) {
        lib_strcpy(display_path, w->editor_path);
    } else if (w->editor_cwd[0] && w->editor_name[0]) {
        lib_strcpy(display_path, w->editor_cwd);
        if (display_path[strlen(display_path) - 1] != '/') {
            lib_strcat(display_path, "/");
        }
        lib_strcat(display_path, w->editor_name);
    } else if (w->editor_name[0]) {
        lib_strcpy(display_path, w->editor_name);
    } else {
        lib_strcpy(display_path, "untitled.txt");
    }
    
    editor_scroll_to_cursor(w);
    
    // 繪製分隔線
    draw_vline(text_x - 4, text_y, rows * line_h, UI_C_BORDER);

    for (int i = 0; i < rows; i++) {
        int row = w->editor_scroll_row + i;
        if (row >= w->editor_line_count) break;
        
        // 1. 繪製行號
        char num_str[8];
        lib_itoa(w->editor_loaded_start_line + (uint32_t)row + 1U, num_str);
        // 右對齊處理 (簡單版: 根據位數偏移)
        int num_len = 0; while(num_str[num_len]) num_len++;
        int num_offset = (4 - num_len) * char_w;
        draw_text_scaled(x + 10 + num_offset, text_y + i * line_h, num_str, UI_C_TEXT_DIM, scale);

        // 2. 繪製內容
        char clip[EDITOR_LINE_LEN];
        int len = editor_line_len(w->editor_lines[row]);
        draw_text_scaled(text_x, text_y + i * line_h, w->editor_lines[row], COL_TEXT, scale);

        // 搜尋高亮
        if (w->editor_cmd_len > 0 && w->editor_mode != 3) {
            for (int c = 0; c <= len - w->editor_cmd_len; c++) {
                if (strncmp(w->editor_lines[row] + c, w->editor_cmd, w->editor_cmd_len) == 0) {
                    draw_rect_fill(text_x + c * char_w, text_y + i * line_h, w->editor_cmd_len * char_w, char_h, UI_C_SELECTION);
                    char match[COLS];
                    memcpy(match, w->editor_lines[row] + c, w->editor_cmd_len);
                    match[w->editor_cmd_len] = '\0';
                    draw_text_scaled(text_x + c * char_w, text_y + i * line_h, match, UI_C_TEXT, scale);
                }
            }
        }

        // 3. 繪製游標 (加入閃爍邏輯)
        if (row == w->editor_cursor_row && active_win_idx == w->id) {
            if (((sys_now() / 200U) & 1U) == 0U) { // 每 200ms 閃爍一次
                int cx = text_x + w->editor_cursor_col * char_w;
                int cy = text_y + i * line_h;
                // 確保游標不超出視窗右邊界
                if (cx < x + ww - char_w) {
                    draw_rect_fill(cx, cy, char_w, char_h, COL_TEXT);
                }
            }
        }
    }
    draw_rect_fill(x + 3, status_y, ww - 6, 14, UI_C_SELECTION);
    if (w->editor_mode == 2) {
        char status[EDITOR_STATUS_LEN + COLS];
        status[0] = ':';
        status[1] = '\0';
        lib_strcat(status, w->editor_cmd);
        draw_text(x + 10, status_y + 1, status, UI_C_TEXT);
    } else {
        char status[EDITOR_STATUS_LEN + 32];
        if (w->editor_pending_d) lib_strcpy(status, "dd=delete line");
        else lib_strcpy(status, w->editor_status[0] ? w->editor_status : "i=insert  :wq=save+quit  :q=quit");
        if (w->editor_lazy) {
            char ln[12];
            lib_strcat(status, "  lines ");
            lib_itoa(w->editor_loaded_start_line + 1U, ln);
            lib_strcat(status, ln);
            lib_strcat(status, "-");
            lib_itoa(w->editor_loaded_start_line + (uint32_t)w->editor_line_count, ln);
            lib_strcat(status, ln);
        }
        if (w->editor_dirty) lib_strcat(status, "  [modified]");
        draw_text(x + 10, status_y + 1, status, UI_C_TEXT);
    }
}
