#include "user_internal.h"
void clear_window_input_queue(struct Window *w) {
    if (!w) return;
    w->input_head = w->input_tail;
    w->mailbox = 0;
}

void clear_prompt_input(struct Window *w);
void redraw_prompt_line(struct Window *w, int row);
void set_shell_status(struct Window *w, const char *msg);
int terminal_visible_rows(struct Window *w);
void close_window(int idx);
void seed_terminal_history(struct Window *w);

static int terminal_env_find(struct Window *w, const char *name) {
    if (!w || !name || !name[0]) return -1;
    for (int i = 0; i < w->env_count; i++) {
        if (strcmp(w->env_names[i], name) == 0) return i;
    }
    return -1;
}

static int terminal_env_name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) {
        return 0;
    }
    for (int i = 1; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static void terminal_env_copy_value(char *dst, const char *src) {
    int i = 0;
    if (!dst) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i < ENV_VALUE_LEN - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void terminal_env_reset(struct Window *w) {
    if (!w) return;
    w->env_count = 0;
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        w->env_names[i][0] = '\0';
        w->env_values[i][0] = '\0';
    }
}

int terminal_env_set(struct Window *w, const char *name, const char *value) {
    int idx;
    if (!w || !terminal_env_name_ok(name)) return -1;
    idx = terminal_env_find(w, name);
    if (idx < 0) {
        if (w->env_count >= MAX_ENV_VARS) return -1;
        idx = w->env_count++;
    }
    copy_name20(w->env_names[idx], name);
    terminal_env_copy_value(w->env_values[idx], value ? value : "");
    return 0;
}

int terminal_env_unset(struct Window *w, const char *name) {
    int idx = terminal_env_find(w, name);
    if (idx < 0) return -1;
    for (int i = idx; i < w->env_count - 1; i++) {
        copy_name20(w->env_names[i], w->env_names[i + 1]);
        terminal_env_copy_value(w->env_values[i], w->env_values[i + 1]);
    }
    if (w->env_count > 0) {
        w->env_count--;
        w->env_names[w->env_count][0] = '\0';
        w->env_values[w->env_count][0] = '\0';
    }
    return 0;
}

const char *terminal_env_get(struct Window *w, const char *name) {
    int idx = terminal_env_find(w, name);
    if (idx < 0) return "";
    return w->env_values[idx];
}

void terminal_env_bootstrap(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    terminal_env_reset(w);
    terminal_env_set(w, "HOME", "/root");
    terminal_env_set(w, "PWD", w->cwd[0] ? w->cwd : "/root");
    terminal_env_set(w, "TERM", "matrix");
    terminal_env_set(w, "SHELL", "matrix");
    terminal_env_set(w, "USER", "root");
    terminal_env_set(w, "PATH", "/root/bin:/bin:/usr/bin");
}

void terminal_env_sync_pwd(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    terminal_env_set(w, "PWD", w->cwd[0] ? w->cwd : "/root");
}

void terminal_env_expand_command(struct Window *w, const char *src, char *dst, int dst_size) {
    int pos = 0;
    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (*src && pos < dst_size - 1) {
        if (*src != '$') {
            dst[pos++] = *src++;
            continue;
        }
        src++;
        if (*src == '$') {
            dst[pos++] = '$';
            src++;
            continue;
        }
        char name[ENV_NAME_LEN];
        int n = 0;
        if (*src == '{') {
            src++;
            while (*src && *src != '}' && n < ENV_NAME_LEN - 1) {
                name[n++] = *src++;
            }
            if (*src == '}') src++;
        } else if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') || *src == '_') {
            while ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') ||
                   (*src >= '0' && *src <= '9') || *src == '_') {
                if (n < ENV_NAME_LEN - 1) name[n++] = *src;
                src++;
            }
        } else {
            dst[pos++] = '$';
            continue;
        }
        name[n] = '\0';
        const char *value = terminal_env_get(w, name);
        if (!value) value = "";
        for (int i = 0; value[i] && pos < dst_size - 1; i++) {
            dst[pos++] = value[i];
        }
    }
    dst[pos] = '\0';
}

void redraw_prompt_line(struct Window *w, int row) {
    if (row < 0 || row >= ROWS) return;
    lib_strcpy(w->lines[row], PROMPT);
    lib_strcat(w->lines[row], w->cmd_buf);
    w->cur_col = PROMPT_LEN + w->cursor_pos;
}
void clear_prompt_input(struct Window *w) {
    w->cmd_buf[0] = '\0';
    w->edit_len = 0;
    w->cursor_pos = 0;
    w->has_saved_cmd = 0;
}

void seed_terminal_history(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (w->hist_count > 0) return;
    lib_strcpy(w->history[0], "python3 server_https.py");
    lib_strcpy(w->history[1], "wrp set http://192.168.123.100:9999");
    lib_strcpy(w->history[2], "netsurf https://www.google.com");
    lib_strcpy(w->history[3], "netsurf https://example.com");
    lib_strcpy(w->history[4], "wget https://192.168.123.100/test.bin test.bin");
    w->hist_count = 5;
    w->hist_idx = -1;
}

void set_shell_status(struct Window *w, const char *msg) {
    if (!w) return;
    if (!msg || !msg[0]) {
        w->shell_status[0] = '\0';
        return;
    }
    lib_strcpy(w->shell_status, msg);
}

int terminal_font_scale(struct Window *w) {
    if (!w) return 1;
    if (w->term_font_scale < TERM_FONT_SCALE_MIN) return TERM_FONT_SCALE_MIN;
    if (w->term_font_scale > TERM_FONT_SCALE_MAX) return TERM_FONT_SCALE_MAX;
    return w->term_font_scale;
}

int terminal_char_w(struct Window *w) {
    return 8 * terminal_font_scale(w);
}

int terminal_char_h(struct Window *w) {
    return 16 * terminal_font_scale(w);
}

int terminal_line_h(struct Window *w) {
    return terminal_char_h(w) + 2;
}

int window_input_empty(struct Window *w) {
    return w->input_head == w->input_tail;
}

void window_input_push(struct Window *w, char key) {
    int next = (w->input_tail + 1) % INPUT_MAILBOX_SIZE;
    if (next == w->input_head) {
        w->input_head = (w->input_head + 1) % INPUT_MAILBOX_SIZE;
    }
    w->input_q[w->input_tail] = key;
    w->input_tail = next;
}

int window_input_pop(struct Window *w, char *key) {
    if (window_input_empty(w)) return 0;
    *key = w->input_q[w->input_head];
    w->input_head = (w->input_head + 1) % INPUT_MAILBOX_SIZE;
    return 1;
}

void wake_terminal_worker_for_window(int win_idx) {
    if (win_idx < 0) return;
    int worker = win_idx % TERMINAL_WORKERS;
    if (worker >= 0 && worker < TERMINAL_WORKERS && terminal_worker_task_ids[worker] >= 0) {
        task_wake(terminal_worker_task_ids[worker]);
    }
}

void wake_network_task(void) {
    if (network_task_id >= 0) task_wake(network_task_id);
}

void network_task_notify(void) {
    wake_network_task();
}

void broadcast_ctrl_c(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].active || wins[i].kind != WINDOW_KIND_TERMINAL) continue;
        if (wins[i].executing_cmd || wins[i].waiting_wget) {
            wins[i].cancel_requested = 1;
        }
    }
}

void append_terminal_line(struct Window *w, const char *text) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (w->total_rows >= ROWS) {
        for (int i = 1; i < ROWS; i++) {
            lib_strcpy(w->lines[i - 1], w->lines[i]);
        }
        w->total_rows = ROWS - 1;
        if (w->v_offset > 0) w->v_offset--;
    }
    lib_strcpy(w->lines[w->total_rows], text);
    w->total_rows++;
    int rv = terminal_visible_rows(w);
    if (w->total_rows > rv) w->v_offset = w->total_rows - rv;
}

int terminal_visible_rows(struct Window *w) {
    int wh = w->maximized ? DESKTOP_H : w->h;
    int rv = (wh - 40) / terminal_line_h(w);
    if (rv < 1) rv = 1;
    return rv;
}

int terminal_visible_cols(struct Window *w) {
    int ww = w->maximized ? WIDTH : w->w;
    int has_scroll = (w->total_rows > terminal_visible_rows(w));
    int usable_w = ww - 20 - (has_scroll ? 12 : 0);
    int cols = usable_w / terminal_char_w(w);
    if (cols < 1) cols = 1;
    if (cols > COLS - 1) cols = COLS - 1;
    return cols;
}
void terminal_clamp_v_offset(struct Window *w) {
    int max_off = w->total_rows - terminal_visible_rows(w);
    if (max_off < 0) max_off = 0;
    if (w->v_offset < 0) w->v_offset = 0;
    if (w->v_offset > max_off) w->v_offset = max_off;
}

int terminal_scroll_max(struct Window *w) {
    int max_off = w->total_rows - terminal_visible_rows(w);
    if (max_off < 0) max_off = 0;
    return max_off;
}

void terminal_append_prompt(struct Window *w) {
    if (!w) return;
    if (w->total_rows >= ROWS) {
        for (int i = 1; i < ROWS; i++) {
            lib_strcpy(w->lines[i - 1], w->lines[i]);
        }
        w->total_rows = ROWS - 1;
        if (w->v_offset > 0) w->v_offset--;
    }
    if (w->total_rows < ROWS) {
        lib_strcpy(w->lines[w->total_rows], PROMPT);
        w->total_rows++;
        w->cur_col = PROMPT_LEN;
    }
}

int terminal_is_at_bottom(struct Window *w) {
    return w->v_offset >= terminal_scroll_max(w);
}

void terminal_scroll_to_bottom(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    w->v_offset = terminal_scroll_max(w);
}

int line_text_len(const char *s) {
    int n = 0;
    while (n < COLS - 1 && s[n]) n++;
    return n;
}

void terminal_clear_selection(struct Window *w) {
    if (!w) return;
    w->selecting = 0;
    w->has_selection = 0;
    w->sel_start_row = w->sel_start_col = 0;
    w->sel_end_row = w->sel_end_col = 0;
}

void terminal_selection_normalized(struct Window *w, int *sr, int *sc, int *er, int *ec) {
    *sr = w->sel_start_row;
    *sc = w->sel_start_col;
    *er = w->sel_end_row;
    *ec = w->sel_end_col;
    if (*sr > *er || (*sr == *er && *sc > *ec)) {
        int tr = *sr, tc = *sc;
        *sr = *er; *sc = *ec;
        *er = tr; *ec = tc;
    }
}

int terminal_mouse_to_cell(struct Window *w, int mx, int my, int *row, int *col) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return 0;
    int x = w->maximized ? 0 : w->x;
    int y = w->maximized ? 0 : w->y;
    int ww = w->maximized ? WIDTH : w->w;
    int wh = w->maximized ? DESKTOP_H : w->h;
    int left = x + 10;
    int top = y + 40;
    int inner_h = wh - 40;
    if (mx < left || mx >= x + ww - 15 || my < top || my >= top + inner_h) return 0;
    int r = (my - top) / terminal_line_h(w);
    int c = (mx - left) / terminal_char_w(w);
    if (r < 0) r = 0;
    if (r >= terminal_visible_rows(w)) r = terminal_visible_rows(w) - 1;
    if (c < 0) c = 0;
    if (c > terminal_visible_cols(w)) c = terminal_visible_cols(w);
    r += w->v_offset;
    if (r < 0) r = 0;
    if (r >= w->total_rows) r = w->total_rows - 1;
    *row = r;
    *col = c;
    return 1;
}

void terminal_copy_selection(struct Window *w) {
    if (!w || !w->has_selection) return;
    int sr, sc, er, ec;
    int pos = 0;
    terminal_selection_normalized(w, &sr, &sc, &er, &ec);
    terminal_clipboard[0] = '\0';
    for (int r = sr; r <= er && pos < (int)sizeof(terminal_clipboard) - 1; r++) {
        int len = line_text_len(w->lines[r]);
        int c0 = (r == sr) ? sc : 0;
        int c1 = (r == er) ? ec : len;
        if (c0 < 0) c0 = 0;
        if (c0 > len) c0 = len;
        if (c1 < c0) c1 = c0;
        if (c1 > len) c1 = len;
        for (int c = c0; c < c1 && pos < (int)sizeof(terminal_clipboard) - 1; c++) {
            terminal_clipboard[pos++] = w->lines[r][c];
        }
        if (r != er && pos < (int)sizeof(terminal_clipboard) - 1) terminal_clipboard[pos++] = '\n';
    }
    terminal_clipboard[pos] = '\0';
}

void terminal_paste_clipboard(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL || !terminal_clipboard[0]) return;
    for (int i = 0; terminal_clipboard[i]; i++) {
        char ch = terminal_clipboard[i];
        if (ch == '\n') ch = 10;
        window_input_push(w, ch);
    }
    w->mailbox = 1;
    wake_terminal_worker_for_window(w->id);
}

const char *terminal_clipboard_text(void) {
    return terminal_clipboard;
}

void resize_terminal_font(struct Window *w, int new_scale) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (new_scale < TERM_FONT_SCALE_MIN) new_scale = TERM_FONT_SCALE_MIN;
    if (new_scale > TERM_FONT_SCALE_MAX) new_scale = TERM_FONT_SCALE_MAX;
    if (new_scale == w->term_font_scale) return;
    w->term_font_scale = new_scale;
    terminal_clamp_v_offset(w);
}
void handle_window_mailbox(struct Window *w) {
    char key;
    if (!window_input_pop(w, &key)) {
        w->mailbox = 0;
        return;
    }
    if (w->kind == WINDOW_KIND_EDITOR) {
        editor_handle_key(w, key);
        return;
    }
    if (w->kind == WINDOW_KIND_IMAGE) {
        if (key == '+' || key == '=') resize_image_window(w, w->image_scale + 1);
        else if (key == '-') resize_image_window(w, w->image_scale - 1);
        return;
    }
    if (key == 3) {
        if (w->executing_cmd || w->waiting_wget) {
            w->cancel_requested = 1;
        } else {
            w->submit_locked = 0;
            w->cancel_requested = 0;
            clear_window_input_queue(w);
            clear_prompt_input(w);
            redraw_prompt_line(w, (w->total_rows > 0) ? (w->total_rows - 1) : 0);
        }
        return;
    }
    if (w->executing_cmd && !app_running) {
        w->executing_cmd = 0;
        w->cancel_requested = 0;
        if (key == 10) return;
    }
    if (key >= 32) {
        w->submit_locked = 0;
        if (w->shell_status[0]) {
            w->shell_status[0] = '\0';
            if (w->total_rows > 0) {
                redraw_prompt_line(w, w->total_rows - 1);
            }
        }
    }
    if (w->executing_cmd) {
        if (key == 3) w->cancel_requested = 1;
        return;
    }
    int r = (w->total_rows > 0) ? (w->total_rows - 1) : 0;
    if (r >= ROWS) r = ROWS - 1;

    if (key == 10) {
        if (w->waiting_wget) return;
        if (w->submit_locked) return;
        int is_clear = (strncmp(w->cmd_buf, "clear", 5) == 0);
        if (w->edit_len > 0) {
            w->submit_locked = 1;
            run_command(w);
        }
        w->input_head = w->input_tail;
        if (w->waiting_wget) return;
        clear_prompt_input(w);
        if (!is_clear && w->total_rows < ROWS) {
            w->total_rows++;
            lib_strcpy(w->lines[w->total_rows-1], PROMPT);
            w->cur_col = PROMPT_LEN;
        } else if (is_clear) {
            redraw_prompt_line(w, 0);
        }
        terminal_scroll_to_bottom(w);
    } else if (w->waiting_wget) {
        if (key == 3) w->cancel_requested = 1;
        return;
    } else if (key == 0x10) {
        w->submit_locked = 0;
        if (w->hist_idx < w->hist_count-1) {
            if (w->hist_idx == -1) {
                lib_strcpy(w->saved_cmd, w->cmd_buf);
                w->has_saved_cmd = 1;
            }
            w->hist_idx++;
            load_edit_buffer(w, w->history[w->hist_idx]);
            redraw_prompt_line(w, r);
        }
    } else if (key == 0x11) {
        w->submit_locked = 0;
        if (w->hist_idx > 0) {
            w->hist_idx--;
            load_edit_buffer(w, w->history[w->hist_idx]);
            redraw_prompt_line(w, r);
        } else if (w->hist_idx == 0) {
            w->hist_idx = -1;
            if (w->has_saved_cmd) load_edit_buffer(w, w->saved_cmd);
            else load_edit_buffer(w, "");
            w->has_saved_cmd = 0;
            redraw_prompt_line(w, r);
        }
    } else if (key == 0x12) {
        if (w->cursor_pos > 0) {
            w->cursor_pos--;
            redraw_prompt_line(w, r);
        }
    } else if (key == 0x13) {
        if (w->cursor_pos < w->edit_len) {
            w->cursor_pos++;
            redraw_prompt_line(w, r);
        }
    } else if (key == 0x15) {
        if (w->cursor_pos != 0) {
            w->cursor_pos = 0;
            redraw_prompt_line(w, r);
        }
    } else if (key == 0x16) {
        if (w->cursor_pos != w->edit_len) {
            w->cursor_pos = w->edit_len;
            redraw_prompt_line(w, r);
        }
    } else if (key == 0x14) {
        if (w->cursor_pos < w->edit_len) {
            for (int i = w->cursor_pos; i < w->edit_len; i++) {
                w->cmd_buf[i] = w->cmd_buf[i + 1];
            }
            w->edit_len--;
            redraw_prompt_line(w, r);
        }
    } else if (key == 8) {
        if (w->cursor_pos > 0 && w->edit_len > 0) {
            for (int i = w->cursor_pos - 1; i < w->edit_len; i++) {
                w->cmd_buf[i] = w->cmd_buf[i + 1];
            }
            w->edit_len--;
            w->cursor_pos--;
            redraw_prompt_line(w, r);
        }
    } else if (key == '\t') {
        try_tab_complete(w, r);
    } else if (key >= 32) {
        if (w->edit_len < COLS - PROMPT_LEN - 1) {
            for (int i = w->edit_len; i >= w->cursor_pos; i--) {
                w->cmd_buf[i + 1] = w->cmd_buf[i];
            }
            w->cmd_buf[w->cursor_pos] = key;
            w->edit_len++;
            w->cursor_pos++;
            redraw_prompt_line(w, r);
        }
    }

}

void terminal_worker_task(void) {
    static int worker_counter = 0;
    int worker_id = worker_counter++;
    while (1) {
        int did_work = 0;
        release_finished_app_commands();
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if ((i % TERMINAL_WORKERS) != worker_id) continue;
            struct Window *w = &wins[i];
            if (!w->active || window_input_empty(w)) continue;
            do {
                handle_window_mailbox(w);
                did_work = 1;
            } while (!window_input_empty(w));
        }
        if (!did_work) task_sleep_current();
        else task_os();
    }
}
void network_task(void) {
    extern void virtio_net_rx_loop2();
    extern int virtio_net_has_pending_irq();
    extern int virtio_net_has_rx_ready();
    extern int virtio_net_rx_pending_count();
    while (1) {
        int did_work = 0;
        int gui_busy = gui_clicked || gui_click_pending || gui_wheel != 0 || gui_key != 0;
        int rx_pending = virtio_net_rx_pending_count();
        uint32_t save_available = wget_job.body_len - wget_save.written;
        int can_stream_save = wget_job.save_pending &&
                              wget_job.success &&
                              (wget_save.active ||
                               save_available >= FS_DATA_PAYLOAD ||
                               wget_job.done);
        if (wget_job.owner_win_id >= 0 && wget_job.owner_win_id < MAX_WINDOWS &&
            wins[wget_job.owner_win_id].waiting_wget && wins[wget_job.owner_win_id].cancel_requested &&
            (wget_job.request_pending || wget_job.active)) {
            wins[wget_job.owner_win_id].cancel_requested = 0;
            wget_close_pcb(1);
            wget_finish(0, "INTERRUPTED");
            did_work = 1;
        }
        if (!did_work && wget_job.request_pending && !wget_job.active) {
            wget_kick_if_needed();
            did_work = 1;
        }
        if (!did_work && wget_timeout_step()) {
            did_work = 1;
        }
        if (!did_work && (virtio_net_has_pending_irq() || rx_pending > 0)) {
            int rx_budget = 1;
            if (!gui_busy) {
                if (rx_pending >= 8) rx_budget = 8;
                else if (rx_pending >= 4) rx_budget = 4;
                else if (rx_pending >= 2) rx_budget = 2;
            }
            while (rx_budget-- > 0 && (virtio_net_has_pending_irq() || virtio_net_has_rx_ready())) {
                virtio_net_rx_loop2();
            }
            did_work = 1;
        }
        if (!did_work && can_stream_save && (wget_job.done || wget_save_turn) &&
            !virtio_net_has_pending_irq() && !virtio_net_has_rx_ready()) {
            if (wget_job.success) {
                int rc;
                int save_budget = 1;
                if (wget_job.done && !gui_busy) save_budget = 24;
                else if (!gui_busy && save_available >= FS_DATA_PAYLOAD * 4U) save_budget = 4;
                if (!wget_save.active) {
                    rc = wget_save_begin();
                    if (rc < 0) wget_save.failed = 1;
                }
                while (!wget_save.failed && wget_save.active && save_budget-- > 0) {
                    rc = wget_save_step();
                    if (rc < 0) {
                        wget_save.failed = 1;
                        break;
                    }
                    if (!wget_job.done) break;
                }
                if (wget_save.failed || !wget_save.active) {
                    wget_job.save_pending = 0;
                    if (wget_job.owner_win_id >= 0 && wget_job.owner_win_id < MAX_WINDOWS &&
                        wins[wget_job.owner_win_id].active && wins[wget_job.owner_win_id].kind == WINDOW_KIND_TERMINAL) {
                        struct Window *ow = &wins[wget_job.owner_win_id];
                        int follow_bottom = terminal_is_at_bottom(ow);
                        if (!wget_save.failed) append_terminal_line(ow, ">> Downloaded.");
                        else append_terminal_line(ow, "ERR: Save Failed.");
                        ow->waiting_wget = 0;
                        clear_prompt_input(ow);
                        terminal_append_prompt(ow);
                        if (follow_bottom) terminal_scroll_to_bottom(ow);
                    }
                }
            } else if (wget_job.owner_win_id >= 0 && wget_job.owner_win_id < MAX_WINDOWS &&
                       wins[wget_job.owner_win_id].active && wins[wget_job.owner_win_id].kind == WINDOW_KIND_TERMINAL) {
                wget_job.save_pending = 0;
                char line[COLS];
                struct Window *ow = &wins[wget_job.owner_win_id];
                int follow_bottom = terminal_is_at_bottom(ow);
                line[0] = '\0';
                if (strcmp(wget_job.err, "INTERRUPTED") == 0) {
                    lib_strcpy(line, "^C");
                } else {
                    lib_strcpy(line, "ERR: ");
                    lib_strcat(line, wget_job.err[0] ? wget_job.err : "wget fail");
                }
                append_terminal_line(ow, line);
                ow->waiting_wget = 0;
                clear_prompt_input(ow);
                terminal_append_prompt(ow);
                if (follow_bottom) terminal_scroll_to_bottom(ow);
            }
            did_work = 1;
        }
        if (!did_work && wget_job.done && wget_job.save_pending && !wget_job.success &&
            wget_job.owner_win_id >= 0 && wget_job.owner_win_id < MAX_WINDOWS &&
            wins[wget_job.owner_win_id].active && wins[wget_job.owner_win_id].kind == WINDOW_KIND_TERMINAL) {
            wget_job.save_pending = 0;
            char line[COLS];
            struct Window *ow = &wins[wget_job.owner_win_id];
            int follow_bottom = terminal_is_at_bottom(ow);
            line[0] = '\0';
            if (strcmp(wget_job.err, "INTERRUPTED") == 0) {
                lib_strcpy(line, "^C");
            } else {
                lib_strcpy(line, "ERR: ");
                lib_strcat(line, wget_job.err[0] ? wget_job.err : "wget fail");
            }
            append_terminal_line(ow, line);
            ow->waiting_wget = 0;
            clear_prompt_input(ow);
            terminal_append_prompt(ow);
            if (follow_bottom) terminal_scroll_to_bottom(ow);
            did_work = 1;
        }
        if (!wget_job.done) {
            if (can_stream_save) wget_save_turn ^= 1;
            else wget_save_turn = 1;
        } else {
            wget_save_turn = 1;
        }
        sys_check_timeouts();
        if (!did_work &&
            !wget_job.request_pending && !wget_job.active && !wget_job.save_pending &&
            !virtio_net_has_pending_irq() && !virtio_net_has_rx_ready()) {
            task_sleep_current();
        } else {
            task_os();
        }
    }
}
