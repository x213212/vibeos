#include "user.h"
#include "user_wget.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

extern volatile int need_resched;
extern int gui_mx, gui_my, gui_clicked, gui_click_pending, gui_right_clicked, gui_right_click_pending, gui_wheel, gui_shortcut_new_task, gui_shortcut_close_task, gui_shortcut_switch_task, gui_ctrl_pressed;
extern char gui_key;
extern int os_debug;
extern uint32_t APP_START, APP_END, APP_SIZE;

#define TERMINAL_WORKERS 3
#define PROMPT "matrix:~$ > "
#define PROMPT_LEN 12
#define TASKBAR_ID 255
#define DEMO3D_SHAPE_POLY 0
#define DEMO3D_SHAPE_CUBE 1
#define RESIZE_NONE 0
#define RESIZE_LEFT 1
#define RESIZE_RIGHT 2
#define RESIZE_TOP 4
#define RESIZE_BOTTOM 8
#define FS_MAX_BLOCKS 65536u
#define ASM_MAX_LINES 256
#define ASM_MAX_LABELS 64
#define ASM_LINE_LEN 192
#define RESIZE_MARGIN 10
#define MIN_WIN_W 220
#define MIN_WIN_H 140
#define TASKBAR_H 30
#define DESKTOP_H (HEIGHT - TASKBAR_H)
#define TASKBAR_START_X 110
#define TASKBAR_BTN_W 64
#define TASKBAR_BTN_GAP 4
#define TERM_FONT_SCALE_MIN 1
#define TERM_FONT_SCALE_MAX 3

#define COL_DESKTOP UI_C_DESKTOP
#define COL_WIN_BG UI_C_PANEL_DARK
#define COL_WIN_BRD UI_C_BORDER
#define COL_TITLE_ACT UI_C_PANEL_ACTIVE
#define COL_TITLE_INACT UI_C_PANEL
#define COL_TEXT UI_C_TEXT
#define COL_DIR UI_C_TEXT_DIM
#define UI_RADIUS 10

static uint32_t demo3d_fps_last_ms = 0;
static uint32_t demo3d_fps_frames = 0;
static uint32_t demo3d_fps_value = 0;
static uint32_t demo3d_last_frame_ms = 0;
volatile int gui_redraw_needed = 0;
static int gui_task_id = -1;
static void format_hms(uint32_t total, char *out);
int app_owner_win_id = -1;
unsigned char file_io_buf[WGET_MAX_FILE_SIZE];
int gui_cursor_mode = CURSOR_NORMAL;
void (*loaded_app_entry)(void) = 0;
reg_t loaded_app_resume_pc = 0;
reg_t loaded_app_user_sp = 0;
reg_t loaded_app_heap_lo = 0;
reg_t loaded_app_heap_hi = 0;
reg_t loaded_app_heap_cur = 0;
reg_t loaded_app_satp = 0;
int loaded_app_exit_code = 0;
int app_task_id = -1;
int app_running = 0;
uint32_t app_root_pt[1024] __attribute__((aligned(4096), section(".apppt")));
uint32_t app_l2_pt[4][1024] __attribute__((aligned(4096), section(".apppt")));

void mode_to_str(uint32_t mode, uint16_t type, char *s) {
    s[0] = (type == 1) ? 'd' : '-';
    s[1] = (mode & 4) ? 'r' : '-'; s[2] = (mode & 2) ? 'w' : '-'; s[3] = (mode & 1) ? 'x' : '-'; s[4] = '\0';
}

static void path_set_child(char *cwd, const char *name) {
    int len = strlen(cwd);
    if (len > 1 && cwd[len - 1] != '/') {
        lib_strcat(cwd, "/");
    }
    lib_strncat(cwd, name, 19);
}

static void path_set_parent(char *cwd) {
    int len = strlen(cwd);
    if (len <= 1) {
        lib_strcpy(cwd, "/");
        return;
    }
    while (len > 1 && cwd[len - 1] == '/') {
        cwd[--len] = '\0';
    }
    while (len > 1 && cwd[len - 1] != '/') {
        cwd[--len] = '\0';
    }
    if (len > 1) {
        cwd[len - 1] = '\0';
    } else {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
}

static void build_editor_path(char *dst, const char *cwd, const char *name) {
    int pos = 0;
    int i = 0;
    if (!dst) return;
    dst[0] = '\0';
    if (cwd && cwd[0]) {
        while (cwd[i] && pos < 127) {
            dst[pos++] = cwd[i++];
        }
    } else {
        const char *root = "/root";
        while (root[i] && pos < 127) {
            dst[pos++] = root[i++];
        }
    }
    if (pos > 0 && dst[pos - 1] != '/' && pos < 127) {
        dst[pos++] = '/';
    }
    i = 0;
    while (name && name[i] && pos < 127) {
        dst[pos++] = name[i++];
    }
    dst[pos] = '\0';
}

static void shorten_path_for_title(char *dst, const char *src, int max_len) {
    int src_len = 0;
    if (!dst || max_len <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (src[src_len]) src_len++;
    if (src_len <= max_len) {
        int i = 0;
        for (; i < max_len && src[i]; i++) dst[i] = src[i];
        dst[i] = '\0';
        return;
    }
    if (max_len <= 3) {
        for (int i = 0; i < max_len; i++) dst[i] = '.';
        dst[max_len] = '\0';
        return;
    }
    dst[0] = '.';
    dst[1] = '.';
    dst[2] = '.';
    int keep = max_len - 3;
    int start = src_len - keep;
    if (start < 0) start = 0;
    int len = 0;
    for (len = 0; len < keep && src[start + len]; len++) {
        dst[3 + len] = src[start + len];
    }
    dst[3 + len] = '\0';
}

void copy_name20(char *dst, const char *src) {
    int i = 0;
    if (!dst) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; i < 19 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void append_out_str(char *out, int out_max, const char *s) {
    int len = strlen(out);
    if (len >= out_max - 1) return;
    while (*s && len < out_max - 1) {
        out[len++] = *s++;
    }
    out[len] = '\0';
}

static void append_out_pad(char *out, int out_max, const char *s, int width) {
    int len = strlen(out);
    int used = 0;
    if (width < 0) width = 0;
    while (s && *s && len < out_max - 1 && used < width) {
        out[len++] = *s++;
        used++;
    }
    while (len < out_max - 1 && used < width) {
        out[len++] = ' ';
        used++;
    }
    out[len] = '\0';
}

static const char *path_basename(const char *p) {
    const char *base = p;
    if (!p) return "";
    while (*p) {
        if (*p == '/' || *p == '\\') base = p + 1;
        p++;
    }
    return base;
}

static int resolve_editor_target(struct Window *term, const char *input, uint32_t *dir_bno_out, char *cwd_out, char *leaf_out) {
    extern struct Window fs_tmp_window;
    struct Window *tmp = &fs_tmp_window;
    const char *p;
    char seg[20];
    int seg_len = 0;
    char expanded[128];
    int ends_with_slash = 0;

    if (!term || !input || !dir_bno_out || !cwd_out || !leaf_out) return -1;

    terminal_expand_home_path(term, input, expanded, sizeof(expanded));
    p = expanded;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return -1;
    {
        int x = 0;
        while (expanded[x]) x++;
        while (x > 0 && (expanded[x - 1] == ' ' || expanded[x - 1] == '\t')) x--;
        ends_with_slash = (x > 0 && expanded[x - 1] == '/');
    }

    memset(tmp, 0, sizeof(*tmp));
    tmp->cwd_bno = term->cwd_bno ? term->cwd_bno : 1;
    if (term->cwd[0]) lib_strcpy(tmp->cwd, term->cwd);
    else lib_strcpy(tmp->cwd, "/root");

    if (p[0] == '/') {
        tmp->cwd_bno = 1;
        lib_strcpy(tmp->cwd, "/root");
        while (*p == '/') p++;
        if (strncmp(p, "root/", 5) == 0) p += 5;
        else if (strcmp(p, "root") == 0) p += 4;
        while (*p == '/') p++;
    }

    while (*p) {
        seg_len = 0;
        while (p[seg_len] && p[seg_len] != '/') {
            if (seg_len >= (int)sizeof(seg) - 1) return -1;
            seg[seg_len] = p[seg_len];
            seg_len++;
        }
        seg[seg_len] = '\0';
        p += seg_len;
        while (*p == '/') p++;
        if (seg_len == 0 || strcmp(seg, ".") == 0) {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            struct dir_block *db = load_current_dir(tmp);
            if (!db) return -1;
            tmp->cwd_bno = db->parent_bno ? db->parent_bno : 1;
            if (tmp->cwd_bno == 1) lib_strcpy(tmp->cwd, "/root");
            else path_set_parent(tmp->cwd);
            continue;
        }
        if (*p == '\0') {
            if (ends_with_slash) {
                struct dir_block *db = load_current_dir(tmp);
                if (!db) return -1;
                int idx = find_entry_index(db, seg, 1);
                if (idx == -1) return -1;
                tmp->cwd_bno = db->entries[idx].bno;
                path_set_child(tmp->cwd, db->entries[idx].name);
                copy_name20(leaf_out, "");
                *dir_bno_out = tmp->cwd_bno ? tmp->cwd_bno : 1;
                lib_strcpy(cwd_out, tmp->cwd[0] ? tmp->cwd : "/root");
                return 0;
            }
            copy_name20(leaf_out, seg);
            *dir_bno_out = tmp->cwd_bno ? tmp->cwd_bno : 1;
            lib_strcpy(cwd_out, tmp->cwd[0] ? tmp->cwd : "/root");
            return 0;
        }
        struct dir_block *db = load_current_dir(tmp);
        if (!db) return -1;
        int idx = find_entry_index(db, seg, 1);
        if (idx == -1) return -1;
        tmp->cwd_bno = db->entries[idx].bno;
        path_set_child(tmp->cwd, db->entries[idx].name);
    }

    *dir_bno_out = tmp->cwd_bno ? tmp->cwd_bno : 1;
    lib_strcpy(cwd_out, tmp->cwd[0] ? tmp->cwd : "/root");
    copy_name20(leaf_out, "");
    return 0;
}

static void clear_window_input_queue(struct Window *w) {
    if (!w) return;
    w->input_head = w->input_tail;
    w->mailbox = 0;
}

struct Window wins[MAX_WINDOWS];
static void clear_prompt_input(struct Window *w);
static int terminal_is_ssh_auth_request(const char *cmd);
static void terminal_begin_ssh_auth(struct Window *w);
static void terminal_refresh_ssh_auth_prompt(struct Window *w);
static void terminal_cancel_ssh_auth(struct Window *w);
static void terminal_submit_ssh_auth(struct Window *w);
static void append_terminal_line(struct Window *w, const char *text);
static void terminal_app_stdout_flush(struct Window *w);
void redraw_prompt_line(struct Window *w, int row);
void terminal_append_prompt(struct Window *w);
static void set_shell_status(struct Window *w, const char *msg);
static int terminal_visible_rows(struct Window *w);
void close_window(int idx);
static void seed_terminal_history(struct Window *w);
static void release_app_owner_command_state(void) {
    if (app_owner_win_id < 0 || app_owner_win_id >= MAX_WINDOWS) {
        app_owner_win_id = -1;
        return;
    }
    struct Window *w = &wins[app_owner_win_id];
    if (w->active && w->kind == WINDOW_KIND_TERMINAL) {
        w->executing_cmd = 0;
        w->cancel_requested = 0;
        w->submit_locked = 0;
        w->waiting_wget = 0;
        clear_window_input_queue(w);
        clear_prompt_input(w);
        if (w->total_rows > 0) {
            redraw_prompt_line(w, w->total_rows - 1);
        }
    }
    app_owner_win_id = -1;
}
static void release_finished_app_commands(void) {
    if (app_running) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].active) continue;
        if (wins[i].kind != WINDOW_KIND_TERMINAL) continue;
        if (wins[i].executing_cmd) {
            wins[i].executing_cmd = 0;
            wins[i].cancel_requested = 0;
        }
    }
}

void app_exit_trampoline(void) {
    int exit_code = loaded_app_exit_code;
    int owner = app_owner_win_id;
    appfs_close_all();
    if (owner >= 0 && owner < MAX_WINDOWS) {
        terminal_app_stdout_flush(&wins[owner]);
    }
    release_app_owner_command_state();
    app_running = 0;
    loaded_app_entry = 0;
    loaded_app_resume_pc = 0;
    loaded_app_user_sp = 0;
    loaded_app_heap_lo = 0;
    loaded_app_heap_hi = 0;
    loaded_app_heap_cur = 0;
    loaded_app_satp = 0;
    loaded_app_exit_code = 0;
    app_owner_win_id = -1;
    memset(app_root_pt, 0, sizeof(app_root_pt));
    memset(app_l2_pt, 0, sizeof(app_l2_pt));
    if (owner >= 0 && owner < MAX_WINDOWS && wins[owner].active && wins[owner].kind == WINDOW_KIND_TERMINAL) {
        char num[16];
        char status[24];
        struct Window *w = &wins[owner];
        lib_strcpy(status, "exit=");
        lib_itoa((uint32_t)exit_code, num);
        lib_strcat(status, num);
        set_shell_status(w, status);
        clear_prompt_input(w);
        redraw_prompt_line(w, w->total_rows - 1);
    }
    for (;;) {
        task_sleep_current();
    }
}
static int z_order[MAX_WINDOWS];
int active_win_idx = -1;
static uint8_t sheet_map[WIDTH * HEIGHT] __attribute__((aligned(16)));
static char terminal_clipboard[2048];
static int terminal_worker_task_ids[TERMINAL_WORKERS];
static int network_task_id = -1;

static int taskbar_button_x(int idx) {
    return TASKBAR_START_X + idx * (TASKBAR_BTN_W + TASKBAR_BTN_GAP);
}

static void set_window_title(struct Window *w, int idx) {
    lib_strcpy(w->title, "Term #");
    char num[12];
    lib_itoa((uint32_t)idx, num);
    lib_strcat(w->title, num);
}

int palette_index_from_rgb(unsigned char r, unsigned char g, unsigned char b) {
    int ri = (r * 5) / 255;
    int gi = (g * 5) / 255;
    int bi = (b * 5) / 255;
    return 16 + (ri * 36) + (gi * 6) + bi;
}

void quantize_rgb_to_palette(int r, int g, int b, int *idx, int *qr, int *qg, int *qb) {
    int ri = (r * 5) / 255;
    int gi = (g * 5) / 255;
    int bi = (b * 5) / 255;
    if (ri < 0) ri = 0; if (ri > 5) ri = 5;
    if (gi < 0) gi = 0; if (gi > 5) gi = 5;
    if (bi < 0) bi = 0; if (bi > 5) bi = 5;
    *idx = 16 + (ri * 36) + (gi * 6) + bi;
    *qr = (ri * 255) / 5;
    *qg = (gi * 255) / 5;
    *qb = (bi * 255) / 5;
}

uint16_t rgb565_from_rgb(unsigned char r, unsigned char g, unsigned char b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void rgb_from_rgb565(uint16_t c, int *r, int *g, int *b) {
    *r = ((c >> 11) & 0x1F) * 255 / 31;
    *g = ((c >> 5) & 0x3F) * 255 / 63;
    *b = (c & 0x1F) * 255 / 31;
}

static int dither_err_r0[WIDTH + 2], dither_err_g0[WIDTH + 2], dither_err_b0[WIDTH + 2];
static int dither_err_r1[WIDTH + 2], dither_err_g1[WIDTH + 2], dither_err_b1[WIDTH + 2];

static void request_gui_redraw(void) {
    gui_redraw_needed = 1;
    need_resched = 1;
    if (gui_task_id >= 0) task_wake(gui_task_id);
}

void redraw_prompt_line(struct Window *w, int row) {
    if (row < 0 || row >= ROWS) return;
    lib_strcpy(w->lines[row], PROMPT);
    lib_strcat(w->lines[row], w->cmd_buf);
    w->cur_col = PROMPT_LEN + w->cursor_pos;
    request_gui_redraw();
}

static void load_edit_buffer(struct Window *w, const char *src) {
    int i = 0;
    while (i < COLS - PROMPT_LEN - 1 && src[i]) {
        w->cmd_buf[i] = src[i];
        i++;
    }
    w->cmd_buf[i] = '\0';
    w->edit_len = i;
    w->cursor_pos = i;
}

static void clear_prompt_input(struct Window *w) {
    w->cmd_buf[0] = '\0';
    w->edit_len = 0;
    w->cursor_pos = 0;
    w->has_saved_cmd = 0;
}

static int terminal_is_ssh_auth_request(const char *cmd) {
    const char *p;
    if (!cmd) return 0;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (strncmp(cmd, "ssh auth", 8) != 0) return 0;
    p = cmd + 8;
    while (*p == ' ' || *p == '\t') p++;
    return (*p == '\0');
}

static void terminal_refresh_ssh_auth_prompt(struct Window *w) {
    char display[COLS];
    int i;
    if (!w || !w->ssh_auth_mode) return;
    lib_strcpy(display, "ssh auth ");
    for (i = 0; i < w->ssh_auth_len && (9 + i) < COLS - 1; i++) {
        display[9 + i] = '*';
    }
    display[9 + i] = '\0';
    lib_strcpy(w->cmd_buf, display);
    w->edit_len = 9 + i;
    w->cursor_pos = w->edit_len;
    if (w->total_rows > 0) redraw_prompt_line(w, w->total_rows - 1);
}

static void terminal_begin_ssh_auth(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    w->ssh_auth_mode = 1;
    w->ssh_auth_len = 0;
    w->ssh_auth_buf[0] = '\0';
    w->submit_locked = 0;
    clear_prompt_input(w);
    terminal_refresh_ssh_auth_prompt(w);
}

static void terminal_cancel_ssh_auth(struct Window *w) {
    if (!w || !w->ssh_auth_mode) return;
    w->ssh_auth_mode = 0;
    w->ssh_auth_len = 0;
    w->ssh_auth_buf[0] = '\0';
    w->submit_locked = 0;
    clear_prompt_input(w);
    if (w->total_rows > 0) redraw_prompt_line(w, w->total_rows - 1);
}

static void terminal_submit_ssh_auth(struct Window *w) {
    char msg[OUT_BUF_SIZE];
    if (!w || !w->ssh_auth_mode) return;
    msg[0] = '\0';
    if (ssh_client_set_password(w->ssh_auth_buf, msg, OUT_BUF_SIZE) == 0) {
        append_terminal_line(w, msg[0] ? msg : "OK: ssh password stored.");
    } else {
        if (msg[0] == '\0') lib_strcpy(msg, "ERR: ssh auth <password>.");
        append_terminal_line(w, msg);
    }
    w->ssh_auth_mode = 0;
    w->ssh_auth_len = 0;
    w->ssh_auth_buf[0] = '\0';
    w->submit_locked = 0;
    clear_prompt_input(w);
    terminal_append_prompt(w);
}

static void seed_terminal_history(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (w->hist_count > 0) return;
    lib_strcpy(w->history[0], "env");
    lib_strcpy(w->history[1], "alias ll=ls -all");
    lib_strcpy(w->history[2], "alias c=clear");
    lib_strcpy(w->history[3], "source ~/.bashrc");
    lib_strcpy(w->history[4], ". ~/.bashrc");
    lib_strcpy(w->history[5], "cd ~");
    lib_strcpy(w->history[6], "export WSL_IP=<WSL_IP>");
    lib_strcpy(w->history[7], "ssh status");
    lib_strcpy(w->history[8], "ssh root@192.168.123.100:2221");
    lib_strcpy(w->history[9], "ssh auth");
    lib_strcpy(w->history[10], "ssh exec uname -a");
    lib_strcpy(w->history[11], "ssh exec ls");
    lib_strcpy(w->history[12], "wrp set http://192.168.123.100:9999");
    lib_strcpy(w->history[13], "netsurf https://duckduckgo.com");
    lib_strcpy(w->history[14], "netsurf https://example.com");
    lib_strcpy(w->history[15], "wget https://192.168.123.100/lez.gb lez.gb");
    lib_strcpy(w->history[16], "gbemu lez.gb");
    w->hist_count = 17;
    w->hist_idx = -1;
}

static void terminal_source_script(struct Window *w, const char *path);
static int terminal_alias_set(struct Window *w, const char *name, const char *value);
static int terminal_alias_unset(struct Window *w, const char *name);
static const char *terminal_alias_get(struct Window *w, const char *name);
static void list_dir_contents(uint32_t bno, char *out);
static int terminal_source_depth = 0;

static void terminal_apply_bashrc_line(struct Window *w, const char *line) {
    char buf[COLS];
    char *p;
    char *eq;
    char name[ENV_NAME_LEN];
    char value[ENV_VALUE_LEN];
    char path[64];
    int n;

    if (!w || !line) return;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;
    n = 0;
    while (line[n] && n < (int)sizeof(buf) - 1) {
        buf[n] = line[n];
        n++;
    }
    buf[n] = '\0';
    p = buf;
    while (p > buf && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t')) {
        *--p = '\0';
    }
    if (buf[0] == '\0' || buf[0] == '#') return;
    if (strncmp(buf, "export ", 7) == 0) {
        p = buf + 7;
    } else {
        p = buf;
    }
    eq = strchr(p, '=');
    if (!eq) {
        if (strncmp(buf, "alias", 5) == 0 && (buf[5] == '\0' || buf[5] == ' ')) {
            char *spec = buf + 5;
            char *alias_eq;
            while (*spec == ' ') spec++;
            if (*spec) {
                alias_eq = strchr(spec, '=');
                if (alias_eq) {
                    *alias_eq++ = '\0';
                    while (*alias_eq == ' ' || *alias_eq == '\t') alias_eq++;
                    
                    // Strip quotes if they exist
                    char *val = alias_eq;
                    int vlen = (int)strlen(val);
                    if (vlen >= 2 && ((val[0] == '\'' && val[vlen-1] == '\'') || (val[0] == '"' && val[vlen-1] == '"'))) {
                        val[vlen-1] = '\0';
                        val++;
                    }
                    if (*spec && val[0]) terminal_alias_set(w, spec, val);
                }
            }
        } else if (strncmp(buf, "unalias ", 8) == 0) {
            char *namep = buf + 8;
            while (*namep == ' ') namep++;
            if (*namep) terminal_alias_unset(w, namep);
        } else if (strncmp(buf, "source ", 7) == 0) {
            char *src = buf + 7;
            while (*src == ' ') src++;
            if (*src) terminal_source_script(w, src);
        } else if (strcmp(buf, ".") == 0) {
            /* no-op for bare dot */
        } else if (strncmp(buf, ". ", 2) == 0) {
            char *src = buf + 2;
            while (*src == ' ') src++;
            if (*src) terminal_source_script(w, src);
        } else if (strncmp(buf, "cd ", 3) == 0) {
            char *dir = buf + 3;
            while (*dir == ' ') dir++;
            if (*dir == '\0' || strcmp(dir, "/root") == 0 || strcmp(dir, "/") == 0) {
                w->cwd_bno = 1;
                lib_strcpy(w->cwd, "/root");
                terminal_env_sync_pwd(w);
            } else {
                char expanded[64];
                terminal_expand_home_path(w, dir, expanded, sizeof(expanded));
                if (expanded[0]) {
                    uint32_t parent_bno = 0;
                    char parent_cwd[32];
                    char leaf[20];
                    extern struct Window fs_tmp_window;
                    struct Window *tmp = &fs_tmp_window;
                    struct dir_block *db;
                    int idx;
                    if (resolve_editor_target(w, expanded, &parent_bno, parent_cwd, leaf) == 0) {
                        memset(tmp, 0, sizeof(*tmp));
                        tmp->cwd_bno = parent_bno ? parent_bno : 1;
                        lib_strcpy(tmp->cwd, parent_cwd[0] ? parent_cwd : "/root");
                        db = load_current_dir(tmp);
                        if (db) {
                            idx = find_entry_index(db, leaf, 1);
                            if (idx >= 0) {
                                w->cwd_bno = db->entries[idx].bno;
                                lib_strcpy(w->cwd, tmp->cwd[0] ? tmp->cwd : "/root");
                                path_set_child(w->cwd, db->entries[idx].name);
                                terminal_env_sync_pwd(w);
                            }
                        }
                    }
                }
            }
        }
        if (strncmp(buf, "unset ", 6) == 0) {
            char *namep = buf + 6;
            while (*namep == ' ') namep++;
            if (*namep) terminal_env_unset(w, namep);
        }
        return;
    }
    n = 0;
    while (p < eq && *p == ' ') p++;
    while (p < eq && n < ENV_NAME_LEN - 1) {
        char c = *p++;
        if (c == ' ' || c == '\t') break;
        name[n++] = c;
    }
    name[n] = '\0';
    n = 0;
    eq++;
    while (*eq == ' ') eq++;
    while (eq[n] && n < ENV_VALUE_LEN - 1) {
        value[n] = eq[n];
        n++;
    }
    value[n] = '\0';
    if (name[0]) terminal_env_set(w, name, value);
}

static void terminal_source_script(struct Window *w, const char *path) {
    unsigned char *buf = 0;
    uint32_t size = 0;
    char expanded[64];
    if (!w || !path || !path[0]) return;
    if (terminal_source_depth >= 4) {
        lib_printf("[bashrc] skip recursive source '%s'\n", path);
        return;
    }
    terminal_source_depth++;
    terminal_expand_home_path(w, path, expanded, sizeof(expanded));
    if (load_file_bytes_alloc(w, expanded, &buf, &size) != 0 || !buf) {
        if (buf) free(buf);
        terminal_source_depth--;
        return;
    }
    {
        uint32_t i = 0;
        uint32_t start = 0;
        while (i <= size) {
            if (i == size || buf[i] == '\n') {
                char line[COLS];
                uint32_t len = i - start;
                if (len >= sizeof(line)) len = sizeof(line) - 1;
                memcpy(line, buf + start, len);
                line[len] = '\0';
                terminal_apply_bashrc_line(w, line);
                start = i + 1;
            }
            i++;
        }
    }
    free(buf);
    terminal_source_depth--;
}

static void terminal_load_bashrc(struct Window *w) {
    unsigned char *probe_buf = 0;
    uint32_t probe_size = 0;
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    
    // Always use home-relative path for consistency
    if (load_file_bytes_alloc(w, "~/.bashrc", &probe_buf, &probe_size) == 0 && probe_buf) {
        free(probe_buf);
    } else {
        const char *def =
            "# matrix shell defaults\n"
            "alias ll='ls -all'\n"
            "alias l='ls'\n"
            "alias c='clear'\n"
            "alias n='netsurf'\n"
            "alias wrpopen='wrp open'\n"
            "alias ..='cd ..'\n";
        store_file_bytes(w, "~/.bashrc", (const unsigned char *)def, (uint32_t)strlen(def));
    }
    terminal_source_script(w, "~/.bashrc");
}

static int terminal_env_find(struct Window *w, const char *name) {
    if (!w || !name || !name[0]) return -1;
    for (int i = 0; i < w->env_count; i++) {
        if (strcmp(w->env_names[i], name) == 0) return i;
    }
    return -1;
}

static int terminal_env_name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return 0;
    for (int i = 1; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
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

static int terminal_alias_find(struct Window *w, const char *name) {
    if (!w || !name || !name[0]) return -1;
    for (int i = 0; i < w->alias_count; i++) {
        if (strcmp(w->alias_names[i], name) == 0) return i;
    }
    return -1;
}

static int terminal_alias_name_ok(const char *name) {
    return terminal_env_name_ok(name);
}

static void terminal_alias_copy_value(char *dst, const char *src) {
    terminal_env_copy_value(dst, src);
}

static void terminal_alias_reset(struct Window *w) {
    if (!w) return;
    w->alias_count = 0;
    for (int i = 0; i < MAX_ALIAS_VARS; i++) {
        w->alias_names[i][0] = '\0';
        w->alias_values[i][0] = '\0';
    }
}

static int terminal_alias_set(struct Window *w, const char *name, const char *value) {
    int idx;
    if (!w || !terminal_alias_name_ok(name)) return -1;
    idx = terminal_alias_find(w, name);
    if (idx < 0) {
        if (w->alias_count >= MAX_ALIAS_VARS) return -1;
        idx = w->alias_count++;
    }
    copy_name20(w->alias_names[idx], name);
    terminal_alias_copy_value(w->alias_values[idx], value ? value : "");
    return 0;
}

static int terminal_alias_unset(struct Window *w, const char *name) {
    int idx = terminal_alias_find(w, name);
    if (idx < 0) return -1;
    for (int i = idx; i < w->alias_count - 1; i++) {
        copy_name20(w->alias_names[i], w->alias_names[i + 1]);
        terminal_alias_copy_value(w->alias_values[i], w->alias_values[i + 1]);
    }
    if (w->alias_count > 0) {
        w->alias_count--;
        w->alias_names[w->alias_count][0] = '\0';
        w->alias_values[w->alias_count][0] = '\0';
    }
    return 0;
}

static const char *terminal_alias_get(struct Window *w, const char *name) {
    int idx = terminal_alias_find(w, name);
    if (idx < 0) return "";
    return w->alias_values[idx];
}

void terminal_expand_home_path(struct Window *w, const char *src, char *dst, int dst_size) {
    const char *home;
    int pos = 0;
    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    home = terminal_env_get(w, "HOME");
    if (!home || !home[0]) home = "/root";
    if (src[0] != '~') {
        int i = 0;
        while (src[i] && i < dst_size - 1) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
        return;
    }
    if (src[1] != '\0' && src[1] != '/') {
        int i = 0;
        while (src[i] && i < dst_size - 1) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
        return;
    }
    while (home[pos] && pos < dst_size - 1) {
        dst[pos] = home[pos];
        pos++;
    }
    src++;
    if (*src == '/') src++;
    if (pos > 0 && pos < dst_size - 1 && dst[pos - 1] == '/' && *src == '/') src++;
    if (pos > 0 && pos < dst_size - 1 && dst[pos - 1] != '/') {
        dst[pos++] = '/';
    }
    while (*src && pos < dst_size - 1) {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
}

static void terminal_expand_alias_command(struct Window *w, const char *src, char *dst, int dst_size) {
    char token[ENV_NAME_LEN];
    int i = 0;
    int token_len = 0;
    const char *alias;
    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (src[i] == ' ' || src[i] == '\t') i++;
    while (src[i] && src[i] != ' ' && src[i] != '\t' && token_len < ENV_NAME_LEN - 1) {
        token[token_len++] = src[i++];
    }
    token[token_len] = '\0';
    alias = terminal_alias_get(w, token);
    if (!alias || !alias[0]) {
        int pos = 0;
        while (src[pos] && pos < dst_size - 1) {
            dst[pos] = src[pos];
            pos++;
        }
        dst[pos] = '\0';
        return;
    }
    int pos = 0;
    for (int j = 0; alias[j] && pos < dst_size - 1; j++) dst[pos++] = alias[j];
    while (src[i] == ' ' || src[i] == '\t') i++;
    if (src[i] && pos < dst_size - 1) dst[pos++] = ' ';
    while (src[i] && pos < dst_size - 1) dst[pos++] = src[i++];
    dst[pos] = '\0';
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
    terminal_alias_reset(w);
    terminal_env_set(w, "HOME", "/root");
    terminal_env_set(w, "PWD", w->cwd[0] ? w->cwd : "/root");
    terminal_env_set(w, "TERM", "matrix");
    terminal_env_set(w, "SHELL", "matrix");
    terminal_env_set(w, "USER", "root");
    terminal_env_set(w, "PATH", "/root/bin:/bin:/usr/bin");
    terminal_alias_set(w, "ll", "ls -all");
    terminal_alias_set(w, "l", "ls");
    terminal_alias_set(w, "c", "clear");
    terminal_alias_set(w, "n", "netsurf");
    terminal_alias_set(w, "wrpopen", "wrp open");
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

static void set_shell_status(struct Window *w, const char *msg) {
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

static int terminal_line_h(struct Window *w) {
    return terminal_char_h(w) + 2;
}

static int window_input_empty(struct Window *w) {
    return w->input_head == w->input_tail;
}

static void window_input_push(struct Window *w, char key) {
    int next = (w->input_tail + 1) % INPUT_MAILBOX_SIZE;
    if (next == w->input_head) {
        w->input_head = (w->input_head + 1) % INPUT_MAILBOX_SIZE;
    }
    w->input_q[w->input_tail] = key;
    w->input_tail = next;
}

static int window_input_pop(struct Window *w, char *key) {
    if (window_input_empty(w)) return 0;
    *key = w->input_q[w->input_head];
    w->input_head = (w->input_head + 1) % INPUT_MAILBOX_SIZE;
    return 1;
}

static void wake_terminal_worker_for_window(int win_idx) {
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

static void broadcast_ctrl_c(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].active || wins[i].kind != WINDOW_KIND_TERMINAL) continue;
        if (wins[i].executing_cmd || wins[i].waiting_wget) {
            wins[i].cancel_requested = 1;
        }
    }
}


static uint32_t crc32_bytes(const unsigned char *buf, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            if (crc & 1U) crc = (crc >> 1) ^ 0xEDB88320U;
            else crc >>= 1;
        }
    }
    return ~crc;
}

static void append_terminal_line(struct Window *w, const char *text) {
    if (!w || !text || w->kind != WINDOW_KIND_TERMINAL) return;
    const char *p = text;
    char line_buf[COLS];
    int col = 0;
    while (*p) {
        if (*p == '\n') {
            line_buf[col] = '\0';
            if (w->total_rows >= ROWS) {
                for (int i = 1; i < ROWS; i++) lib_strcpy(w->lines[i - 1], w->lines[i]);
                w->total_rows = ROWS - 1;
                if (w->v_offset > 0) w->v_offset--;
            }
            lib_strcpy(w->lines[w->total_rows], line_buf);
            w->total_rows++;
            int rv = terminal_visible_rows(w);
            if (w->total_rows > rv) w->v_offset = w->total_rows - rv;
            col = 0; p++; continue;
        }
        if (col < COLS - 1) line_buf[col++] = *p;
        p++;
    }
    if (col > 0) {
        line_buf[col] = '\0';
        if (w->total_rows >= ROWS) {
            for (int i = 1; i < ROWS; i++) lib_strcpy(w->lines[i - 1], w->lines[i]);
            w->total_rows = ROWS - 1;
            if (w->v_offset > 0) w->v_offset--;
        }
        lib_strcpy(w->lines[w->total_rows], line_buf);
        w->total_rows++;
        int rv = terminal_visible_rows(w);
        if (w->total_rows > rv) w->v_offset = w->total_rows - rv;
    }
    request_gui_redraw();
}

static void terminal_app_stdout_flush(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (w->app_stdout_len <= 0) return;
    w->app_stdout[w->app_stdout_len] = '\0';
    append_terminal_line(w, w->app_stdout);
    w->app_stdout_len = 0;
    w->app_stdout[0] = '\0';
}

void terminal_app_stdout_flush_win(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return;
    terminal_app_stdout_flush(&wins[win_id]);
}

void terminal_app_stdout_putc(int win_id, char ch) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return;
    struct Window *w = &wins[win_id];
    if (!w->active || w->kind != WINDOW_KIND_TERMINAL) return;
    if (ch == '\r') return;
    if (ch == '\n') {
        terminal_app_stdout_flush(w);
        return;
    }
    if (w->app_stdout_len >= (int)sizeof(w->app_stdout) - 1) {
        terminal_app_stdout_flush(w);
    }
    if (w->app_stdout_len < (int)sizeof(w->app_stdout) - 1) {
        w->app_stdout[w->app_stdout_len++] = ch;
        w->app_stdout[w->app_stdout_len] = '\0';
    }
}

void terminal_app_stdout_puts(int win_id, const char *s) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !s) return;
    while (*s) {
        terminal_app_stdout_putc(win_id, *s++);
    }
    if (win_id >= 0 && win_id < MAX_WINDOWS) {
        terminal_app_stdout_flush(&wins[win_id]);
    }
}

static int terminal_visible_rows(struct Window *w) {
    int wh = w->maximized ? DESKTOP_H : w->h;
    int rv = (wh - 40) / terminal_line_h(w);
    if (rv < 1) rv = 1;
    return rv;
}

static int terminal_visible_cols(struct Window *w) {
    int ww = w->maximized ? WIDTH : w->w;
    int has_scroll = (w->total_rows > terminal_visible_rows(w));
    int usable_w = ww - 20 - (has_scroll ? 12 : 0);
    int cols = usable_w / terminal_char_w(w);
    if (cols < 1) cols = 1;
    if (cols > COLS - 1) cols = COLS - 1;
    return cols;
}

static int str_starts_with(const char *s, const char *prefix);

static void copy_range_str(char *dst, int dst_size, const char *begin, const char *end) {
    int i = 0;
    if (dst_size <= 0) return;
    while (begin < end && i < dst_size - 1) dst[i++] = *begin++;
    dst[i] = '\0';
}

static void derive_filename_from_path(const char *path, char *out, int out_size) {
    const char *name = path;
    const char *p = path;
    if (out_size <= 0) return;
    while (*p) {
        if (*p == '/') name = p + 1;
        else if (*p == '?' || *p == '#') break;
        p++;
    }
    if (*name == '\0') {
        lib_strcpy(out, "index.html");
        return;
    }
    copy_range_str(out, out_size, name, p);
}

static void terminal_clamp_v_offset(struct Window *w) {
    int max_off = w->total_rows - terminal_visible_rows(w);
    if (max_off < 0) max_off = 0;
    if (w->v_offset < 0) w->v_offset = 0;
    if (w->v_offset > max_off) w->v_offset = max_off;
}

static int terminal_scroll_max(struct Window *w) {
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
    request_gui_redraw();
}

static int terminal_is_at_bottom(struct Window *w) {
    return w->v_offset >= terminal_scroll_max(w);
}

static void terminal_scroll_to_bottom(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    w->v_offset = terminal_scroll_max(w);
}

static int line_text_len(const char *s) {
    int n = 0;
    while (n < COLS - 1 && s[n]) n++;
    return n;
}

static void append_dir_entries_sorted(struct dir_block *db, const char *title, const char *name_prefix, int type_filter, char *out) {
    int printed = 0;
    out[0] = '\0';
    if (title && title[0]) lib_strcat(out, title);
    for (int pass = 0; pass < 2; pass++) {
        int want_type = (pass == 0) ? 1 : 0;
        for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
            if (!db->entries[i].name[0]) continue;
            if (type_filter != -1 && db->entries[i].type != type_filter) continue;
            if (db->entries[i].type != want_type) continue;
            if (name_prefix && name_prefix[0] && !str_starts_with(db->entries[i].name, name_prefix)) continue;
            char row[64];
            snprintf(row, sizeof(row), "  %c %s\n", (db->entries[i].type == 1 ? 'd' : 'f'), db->entries[i].name);
            if (strlen(out) + strlen(row) < OUT_BUF_SIZE - 2) {
                lib_strcat(out, row);
                printed = 1;
            } else {
                return;
            }
        }
    }
    if (!printed) lib_strcat(out, " (empty)");
}

static void append_single_entry_line(struct file_entry *e, char *out, int all) {
    if (!e || !out) return;
    out[0] = '\0';
    if (all) {
        char ms[5], sz[16], ts[20], name[32];
        mode_to_str(e->mode, e->type, ms);
        format_size_human(e->size, sz);
        format_hms(e->mtime, ts);
        copy_name20(name, e->name);
        char row[128];
        snprintf(row, sizeof(row), "%5s %8s %8s %s %s\n",
                 ms, sz, ts, (e->type == 1 ? "d" : "f"), name);
        lib_strcat(out, row);
    } else {
        char row[64];
        snprintf(row, sizeof(row), "  %c %s\n", (e->type == 1 ? 'd' : 'f'), e->name);
        lib_strcat(out, row);
    }
}

static void terminal_clear_selection(struct Window *w) {
    if (!w) return;
    w->selecting = 0;
    w->has_selection = 0;
    w->sel_start_row = w->sel_start_col = 0;
    w->sel_end_row = w->sel_end_col = 0;
}

static void terminal_selection_normalized(struct Window *w, int *sr, int *sc, int *er, int *ec) {
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

static int terminal_mouse_to_cell(struct Window *w, int mx, int my, int *row, int *col) {
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

static void terminal_copy_selection(struct Window *w) {
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

static void terminal_paste_clipboard(struct Window *w) {
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

static void resize_terminal_font(struct Window *w, int new_scale) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (new_scale < TERM_FONT_SCALE_MIN) new_scale = TERM_FONT_SCALE_MIN;
    if (new_scale > TERM_FONT_SCALE_MAX) new_scale = TERM_FONT_SCALE_MAX;
    if (new_scale == w->term_font_scale) return;
    w->term_font_scale = new_scale;
    terminal_clamp_v_offset(w);
}

static void append_hex32(char *dst, uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    while (*dst) dst++;
    dst[0] = '0';
    dst[1] = 'x';
    for (int i = 0; i < 8; i++) {
        dst[2 + i] = hex[(v >> (28 - i * 4)) & 0xF];
    }
    dst[10] = '\0';
}

static void append_hex8(char *dst, unsigned char v) {
    static const char hex[] = "0123456789abcdef";
    while (*dst) dst++;
    dst[0] = hex[(v >> 4) & 0xF];
    dst[1] = hex[v & 0xF];
    dst[2] = '\0';
}

void format_size_human(uint32_t bytes, char *out) {
    const char *unit = "B";
    uint32_t whole = bytes;
    uint32_t frac = 0;
    if (bytes >= 1024U * 1024U * 1024U) {
        unit = "GB";
        whole = bytes / (1024U * 1024U * 1024U);
        frac = (bytes % (1024U * 1024U * 1024U)) * 10U / (1024U * 1024U * 1024U);
    } else if (bytes >= 1024U * 1024U) {
        unit = "MB";
        whole = bytes / (1024U * 1024U);
        frac = (bytes % (1024U * 1024U)) * 10U / (1024U * 1024U);
    } else if (bytes >= 1024U) {
        unit = "KB";
        whole = bytes / 1024U;
        frac = (bytes % 1024U) * 10U / 1024U;
    }
    lib_itoa(whole, out);
    if (unit[0] != 'B') {
        while (*out) out++;
        out[0] = '.';
        out[1] = (char)('0' + frac);
        out[2] = '\0';
    }
    lib_strcat(out, unit);
}

static int is_mostly_text(const unsigned char *buf, uint32_t size) {
    if (size == 0) return 1;
    uint32_t printable = 0;
    for (uint32_t i = 0; i < size; i++) {
        unsigned char c = buf[i];
        if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') printable++;
    }
    return printable * 100U >= size * 85U;
}

static void render_hex_dump(char *out, const unsigned char *buf, uint32_t size) {
    out[0] = '\0';
    uint32_t limit = size;
    if (limit > 128U) limit = 128U;
    for (uint32_t i = 0; i < limit; i += 16U) {
        char off[12];
        off[0] = '\0';
        append_hex32(off, i);
        lib_strcat(out, off);
        lib_strcat(out, ": ");
        for (uint32_t j = 0; j < 16U && i + j < limit; j++) {
            append_hex8(out, buf[i + j]);
            lib_strcat(out, " ");
        }
        if (i + 16U < limit) lib_strcat(out, "\n");
    }
    if (size > limit) lib_strcat(out, "\n...");
}


static int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int is_name_completion_cmd(const char *cmd) {
    return strcmp(cmd, "cd") == 0 ||
           strcmp(cmd, "ls") == 0 ||
           strcmp(cmd, "find") == 0 ||
           strcmp(cmd, "cat") == 0 ||
           strcmp(cmd, "env") == 0 ||
           strcmp(cmd, "echo") == 0 ||
           strcmp(cmd, "open") == 0 ||
           strcmp(cmd, "export") == 0 ||
           strcmp(cmd, "alias") == 0 ||
           strcmp(cmd, "unalias") == 0 ||
           strcmp(cmd, "source") == 0 ||
           strcmp(cmd, "run") == 0 ||
           strcmp(cmd, "ssh") == 0 ||
           strcmp(cmd, "wget") == 0 ||
           strcmp(cmd, "unset") == 0 ||
           strcmp(cmd, "wrp") == 0 ||
           strcmp(cmd, "write") == 0 ||
           strcmp(cmd, "mkdir") == 0 ||
           strcmp(cmd, "rm") == 0 ||
           strcmp(cmd, "touch") == 0 ||
           strcmp(cmd, "vim") == 0 ||
           strcmp(cmd, "mv") == 0 ||
           strcmp(cmd, "rename") == 0;
}

static int terminal_first_token(const char *line, char *out, int out_size) {
    int i = 0;
    int j = 0;
    if (!line || !out || out_size <= 0) return 0;
    while (line[i] == ' ') i++;
    while (line[i] && line[i] != ' ' && j < out_size - 1) {
        out[j++] = line[i++];
    }
    out[j] = '\0';
    return j > 0;
}

static int terminal_command_uses_path_completion(const char *cmd) {
    if (!cmd || !cmd[0]) return 0;
    return is_name_completion_cmd(cmd) ||
           strcmp(cmd, "vim") == 0 ||
           strcmp(cmd, "cat") == 0 ||
           strcmp(cmd, "cd") == 0 ||
           strcmp(cmd, "mkdir") == 0 ||
           strcmp(cmd, "rm") == 0 ||
           strcmp(cmd, "touch") == 0 ||
           strcmp(cmd, "write") == 0 ||
           strcmp(cmd, "open") == 0 ||
           strcmp(cmd, "run") == 0 ||
           strcmp(cmd, "source") == 0;
}

static int fuzzy_match_ordered(const char *cand, const char *pat) {
    if (!pat || !pat[0]) return 0;
    while (*cand && *pat) {
        if (tolower((int)*cand) == tolower((int)*pat)) pat++;
        cand++;
    }
    return *pat == '\0';
}

static void apply_completion(struct Window *w, int row, int token_start, int token_end, const char *replacement, int add_space) {
    char newbuf[COLS];
    int pos = 0;
    (void)add_space;
    for (int i = 0; i < token_start && pos < COLS - 1; i++) newbuf[pos++] = w->cmd_buf[i];
    for (int i = 0; replacement[i] && pos < COLS - 1; i++) newbuf[pos++] = replacement[i];
    for (int i = token_end; w->cmd_buf[i] && pos < COLS - 1; i++) newbuf[pos++] = w->cmd_buf[i];
    newbuf[pos] = '\0';
    load_edit_buffer(w, newbuf);
    w->cursor_pos = token_start + strlen(replacement);
    redraw_prompt_line(w, row);
}

static int tab_complete_command(struct Window *w, int row, int token_start, int token_end) {
    static const char *cmds[] = {
        "pwd", "format", "cd", "ls", "mkdir", "rm", "touch", "write", "cat", "wget", "open", "run", "gbemu", "demo3d", "frankenstein", "help", "clear", "mv", "rename", "netsurf", "ssh", "wrp", "find", "mem", "df", "du", "alias", "unalias", "export", "unset", "source"
    };
    char prefix[COLS];
    int plen = token_end - token_start;
    if (plen < 0) plen = 0;
    if (plen >= COLS) plen = COLS - 1;
    for (int i = 0; i < plen; i++) prefix[i] = w->cmd_buf[token_start + i];
    prefix[plen] = '\0';

    int matches = 0;
    int fuzzy_matches = 0;
    const char *best = 0;
    const char *fuzzy_best = 0;
    char common[COLS];
    common[0] = '\0';
    for (unsigned int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (str_starts_with(cmds[i], prefix)) {
            if (matches == 0) {
                lib_strcpy(common, cmds[i]);
                best = cmds[i];
            } else {
                int j = 0;
                while (common[j] && cmds[i][j] && common[j] == cmds[i][j]) j++;
                common[j] = '\0';
            }
            matches++;
        } else if (fuzzy_match_ordered(cmds[i], prefix)) {
            if (!fuzzy_best || strlen(cmds[i]) < strlen(fuzzy_best)) {
                fuzzy_best = cmds[i];
            }
            fuzzy_matches++;
        }
    }
    if (matches > 0) {
        apply_completion(w, row, token_start, token_end, (matches == 1) ? best : common, matches == 1);
        return 1;
    }
    if (fuzzy_matches > 0 && fuzzy_best) {
        apply_completion(w, row, token_start, token_end, fuzzy_best, 1);
        return 1;
    }
    return 0;
}

static int tab_complete_name(struct Window *w, int row, int token_start, int token_end, int type_filter) {
    char prefix[COLS];
    int plen = token_end - token_start;
    if (plen < 0) plen = 0; if (plen >= COLS) plen = COLS - 1;
    for (int i = 0; i < plen; i++) prefix[i] = w->cmd_buf[token_start + i];
    prefix[plen] = '\0';

    uint32_t target_dir_bno = w->cwd_bno;
    char leaf_prefix[32]; leaf_prefix[0] = '\0';
    char dir_part[128]; dir_part[0] = '\0';

    if (strchr(prefix, '/') || prefix[0] == '~') {
        char *last_slash = strrchr(prefix, '/');
        if (last_slash) {
            int dlen = (int)(last_slash - prefix + 1);
            memcpy(dir_part, prefix, dlen); dir_part[dlen] = '\0';
            lib_strcpy(leaf_prefix, last_slash + 1);
        } else if (prefix[0] == '~') {
            lib_strcpy(dir_part, "~/"); lib_strcpy(leaf_prefix, prefix + 1);
        } else {
            lib_strcpy(dir_part, "./"); lib_strcpy(leaf_prefix, prefix);
        }

        uint32_t p_bno = 0; char p_cwd[128], leaf_unused[20];
        char expanded[128];
        terminal_expand_home_path(w, dir_part, expanded, sizeof(expanded));
        if (resolve_editor_target(w, expanded, &p_bno, p_cwd, leaf_unused) == 0) {
            target_dir_bno = p_bno;
        } else return 0;
    } else {
        lib_strcpy(leaf_prefix, prefix);
    }

    extern struct Window fs_tmp_window; struct Window *ctx = &fs_tmp_window;
    memset(ctx, 0, sizeof(*ctx)); ctx->cwd_bno = target_dir_bno;
    struct dir_block *db = load_current_dir(ctx);
    if (!db) return 0;

    // Smart listing: If leaf_prefix is empty (path ends in /) or exact dir match
    if (leaf_prefix[0] == '\0') {
        char list_out[OUT_BUF_SIZE];
        list_dir_contents(target_dir_bno, list_out);
        append_terminal_line(w, list_out);
        terminal_append_prompt(w);
        terminal_scroll_to_bottom(w);
        redraw_prompt_line(w, (w->total_rows > 0) ? (w->total_rows - 1) : 0);
        w->mailbox = 1;
        gui_redraw_needed = 1;
        need_resched = 1;
        if (gui_task_id >= 0) task_wake(gui_task_id);
        extern void wake_terminal_worker_for_window(int id);
        wake_terminal_worker_for_window(w->id);
        return 1;
    }

    int matches = 0;
    char best[20], common[20];
    common[0] = '\0'; best[0] = '\0';
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (!db->entries[i].name[0]) continue;
        if (type_filter != -1 && db->entries[i].type != type_filter) continue;
        if (str_starts_with(db->entries[i].name, leaf_prefix)) {
            if (matches == 0) {
                lib_strcpy(common, db->entries[i].name); lib_strcpy(best, db->entries[i].name);
            } else {
                int j = 0; while (common[j] && db->entries[i].name[j] && common[j] == db->entries[i].name[j]) j++;
                common[j] = '\0';
            }
            matches++;
        }
    }
    
    if (matches > 1) {
        char list_out[OUT_BUF_SIZE]; list_out[0] = '\0';
        append_dir_entries_sorted(db, "Possibilities:\n", leaf_prefix, type_filter, list_out);
        // 1. Append the file list
        append_terminal_line(w, list_out);

        // 2. Create the new prompt line and force it to be the visible input line
        terminal_append_prompt(w);
        terminal_scroll_to_bottom(w);
        redraw_prompt_line(w, (w->total_rows > 0) ? (w->total_rows - 1) : 0);
        // 3. Force a GUI redraw pulse for the new bottom prompt
        w->mailbox = 1;
        gui_redraw_needed = 1;
        need_resched = 1;
        if (gui_task_id >= 0) task_wake(gui_task_id);
        extern void wake_terminal_worker_for_window(int id);
        wake_terminal_worker_for_window(w->id);
        return 1;
    }

    if (matches == 1) {
        char full_replacement[160];
        lib_strcpy(full_replacement, dir_part);
        lib_strcat(full_replacement, best);
        int idx = find_entry_index(db, (char *)best, 1);
        // Add slash if it's a directory to allow immediate recursion on next Tab
        if (idx >= 0) lib_strcat(full_replacement, "/");
        apply_completion(w, row, token_start, token_end, full_replacement, 0);
        return 1;
    }
    
    // If no matches but leaf_prefix itself IS a directory, add a slash
    int idx = find_entry_index(db, leaf_prefix, 1);
    if (idx >= 0) {
        char full_replacement[160];
        lib_strcpy(full_replacement, dir_part);
        lib_strcat(full_replacement, leaf_prefix);
        lib_strcat(full_replacement, "/");
        apply_completion(w, row, token_start, token_end, full_replacement, 0);
        return 1;
    }

    return 0;
}

static void try_tab_complete(struct Window *w, int row) {
    if (w->cursor_pos < 0 || w->cursor_pos > w->edit_len) return;
    int token_start = w->cursor_pos;
    while (token_start > 0 && w->cmd_buf[token_start - 1] != ' ') token_start--;
    int token_end = w->cursor_pos;
    while (token_end < w->edit_len && w->cmd_buf[token_end] != ' ') token_end++;

    char prefix[COLS];
    int plen = token_end - token_start;
    if (plen < 0) plen = 0; if (plen >= COLS) plen = COLS - 1;
    for (int i = 0; i < plen; i++) prefix[i] = w->cmd_buf[token_start + i];
    prefix[plen] = '\0';

    char resolved_cmd[COLS];
    char expanded_line[COLS];
    terminal_expand_alias_command(w, w->cmd_buf, expanded_line, sizeof(expanded_line));
    if (!terminal_first_token(expanded_line, resolved_cmd, sizeof(resolved_cmd))) {
        resolved_cmd[0] = '\0';
    }

    // If there's a space before and no prefix, or prefix has path chars, use path completion
    if (token_start > 0) {
        if (terminal_command_uses_path_completion(resolved_cmd) ||
            strchr(prefix, '/') || prefix[0] == '~' || prefix[0] == '.') {
            if (tab_complete_name(w, row, token_start, token_end, -1)) return;
        }
    }

    if (strchr(prefix, '/') || prefix[0] == '~' || prefix[0] == '.') {
        if (tab_complete_name(w, row, token_start, token_end, -1)) return;
    }

    // Default to command completion for the first token
    int first_end = 0;
    while (first_end < w->edit_len && w->cmd_buf[first_end] != ' ') first_end++;
    if (token_start < first_end || token_start == 0) {
        tab_complete_command(w, row, token_start, token_end);
    }
}

int find_entry_index(struct dir_block *db, const char *name, int type_filter) {
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (!db->entries[i].name[0]) continue;
        if (type_filter != -1 && db->entries[i].type != type_filter) continue;
        if (strcmp(db->entries[i].name, name) == 0) return i;
    }
    return -1;
}

static int launch_app_file(struct Window *w, const char *name, const char *argstr, char *out, int out_max) {
    unsigned char *buf = 0;
    uint32_t size = 0;
    if (app_running) {
        lib_strcpy(out, "ERR: App Busy (previous app still running).");
        return -9;
    }
    int rc = load_file_bytes_alloc(w, name, &buf, &size);
    uint8_t *app_base = (uint8_t *)(uintptr_t)APP_START;

    if (rc != 0) {
        if (buf) free(buf);
        if (rc == -2) lib_strcpy(out, "ERR: File Not Found.");
        else if (rc == -4) lib_strcpy(out, "ERR: No Memory.");
        else lib_strcpy(out, "ERR: Load Failed.");
        return rc;
    }

    loaded_app_entry = 0;
    appfs_set_cwd(w->cwd_bno, w->cwd);
    memset(app_base, 0, APP_SIZE);

    if (size < sizeof(struct elf32_ehdr) ||
        ((struct elf32_ehdr *)buf)->e_ident[0] != ELF32_MAG0 ||
        ((struct elf32_ehdr *)buf)->e_ident[1] != ELF32_MAG1 ||
        ((struct elf32_ehdr *)buf)->e_ident[2] != ELF32_MAG2 ||
        ((struct elf32_ehdr *)buf)->e_ident[3] != ELF32_MAG3) {
        free(buf);
        lib_strcpy(out, "ERR: ELF Only.");
        return -11;
    }

    if (app_load_elf_image(buf, size, out) != 0) {
        free(buf);
        return -11;
    }

    if (!loaded_app_entry) {
        free(buf);
        lib_strcpy(out, "ERR: Bad App Entry.");
        return -8;
    }

    if (APP_SIZE <= APP_STACK_SIZE + APP_HEAP_GUARD) {
        lib_strcpy(out, "ERR: App Mem Too Small.");
        return -9;
    }
    loaded_app_user_sp = ((reg_t)(uintptr_t)APP_END & ~(reg_t)0xF) - 16;
    if (loaded_app_user_sp <= (reg_t)(uintptr_t)APP_START + APP_HEAP_GUARD) {
        free(buf);
        lib_strcpy(out, "ERR: App Heap Too Small.");
        return -10;
    }

    if (app_prepare_argv_stack(name, argstr, &loaded_app_user_sp) != 0) {
        free(buf);
        lib_strcpy(out, "ERR: Bad Args.");
        return -12;
    }

    app_running = 1;
    app_owner_win_id = w->id;
    clear_window_input_queue(w);
    app_vm_reset();
    app_heap_reset((reg_t)(uintptr_t)APP_START + APP_HEAP_GUARD, loaded_app_user_sp - APP_HEAP_GUARD);
    if (app_task_id < 0) {
        app_task_id = task_create(&app_bootstrap, 0, 1);
    } else {
        task_reset(app_task_id, &app_bootstrap, 0, 1);
    }
    task_wake(app_task_id);
    free(buf);
    lib_strcpy(out, ">> App Started.");
    return 0;
}

static int try_launch_elf_command(struct Window *w, char *cmd, char *out) {
    char tmp[COLS];
    char *fn = tmp;
    char *args = 0;
    char *sp;
    size_t len;
    int i = 0;

    while (cmd[i] == ' ') i++;
    if (cmd[i] == '\0') return 0;
    while (cmd[i] && i < COLS - 1) {
        tmp[i] = cmd[i];
        i++;
    }
    tmp[i] = '\0';

    while (*fn == ' ') fn++;
    if (*fn == '\0') return 0;
    sp = fn;
    while (*sp && *sp != ' ') sp++;
    if (*sp == ' ') {
        *sp++ = '\0';
        while (*sp == ' ') sp++;
        if (*sp == '\0') sp = 0;
    } else {
        sp = 0;
    }
    len = strlen(fn);
    if (len < 4 || strcmp(fn + len - 4, ".elf") != 0) {
        return 0;
    }
    args = sp;
    if (launch_app_file(w, fn, args, out, OUT_BUF_SIZE) != 0) return 1;
    return 1;
}


void reset_window(struct Window *w, int idx) {
    memset(w, 0, sizeof(*w));
    w->id = idx;
    w->cwd_bno = 1;
    w->term_font_scale = 1;
    lib_strcpy(w->cwd, "/root");
}

void close_window(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    if (wins[idx].active && wins[idx].kind == WINDOW_KIND_NETSURF) {
        extern void netsurf_release_window(int win_id);
        netsurf_release_window(idx);
    }
    if (active_win_idx == idx) {
        active_win_idx = -1;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (z_order[i] != idx) continue;
        for (int k = i; k < MAX_WINDOWS - 1; k++) {
            z_order[k] = z_order[k + 1];
        }
        z_order[MAX_WINDOWS - 1] = idx;
        break;
    }
    reset_window(&wins[idx], idx);
    wins[idx].taskbar_anim = 0;
    wins[idx].selecting = 0;
    wins[idx].has_selection = 0;
    if (active_win_idx == -1) {
        cycle_active_window();
    }
}

static int active_window_valid(void) {
    if (active_win_idx < 0 || active_win_idx >= MAX_WINDOWS) return 0;
    if (!wins[active_win_idx].active) return 0;
    if (wins[active_win_idx].minimized) return 0;
    return 1;
}

static int any_window_active(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].active && !wins[i].minimized) return 1;
    }
    return 0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void append_u2(char *out, unsigned int v) {
    out[0] = (char)('0' + ((v / 10U) % 10U));
    out[1] = (char)('0' + (v % 10U));
}

static void append_u4(char *out, unsigned int v) {
    out[0] = (char)('0' + ((v / 1000U) % 10U));
    out[1] = (char)('0' + ((v / 100U) % 10U));
    out[2] = (char)('0' + ((v / 10U) % 10U));
    out[3] = (char)('0' + (v % 10U));
}

static void format_ymd_hms(char *out, unsigned int year, unsigned int month, unsigned int day,
                           unsigned int hour, unsigned int min, unsigned int sec) {
    append_u4(out, year);
    out[4] = '-';
    append_u2(out + 5, month);
    out[7] = '-';
    append_u2(out + 8, day);
    out[10] = ' ';
    append_u2(out + 11, hour);
    out[13] = ':';
    append_u2(out + 14, min);
    out[16] = ':';
    append_u2(out + 17, sec);
    out[19] = '\0';
}

static void format_clock(char *out) {
    unsigned int total = get_wall_clock_seconds();
    unsigned int sec = total % 60U;
    unsigned int min = (total / 60U) % 60U;
    unsigned int hour = (total / 3600U) % 24U;
    format_ymd_hms(out,
                   (unsigned int)HOST_BUILD_YEAR,
                   (unsigned int)HOST_BUILD_MONTH,
                   (unsigned int)HOST_BUILD_DAY,
                   hour, min, sec);
}

uint32_t current_fs_time(void) {
    return get_wall_clock_seconds();
}

static void format_hms(uint32_t total, char *out) {
    total %= 86400U;
    unsigned int sec = total % 60U;
    unsigned int min = (total / 60U) % 60U;
    unsigned int hour = (total / 3600U) % 24U;
    format_ymd_hms(out,
                   (unsigned int)HOST_BUILD_YEAR,
                   (unsigned int)HOST_BUILD_MONTH,
                   (unsigned int)HOST_BUILD_DAY,
                   hour, min, sec);
}

static int cursor_mode_for_resize_dir(int dir) {
    if ((dir & RESIZE_LEFT) && (dir & RESIZE_TOP)) return CURSOR_SIZE_D2;
    if ((dir & RESIZE_RIGHT) && (dir & RESIZE_BOTTOM)) return CURSOR_SIZE_D2;
    if ((dir & RESIZE_RIGHT) && (dir & RESIZE_TOP)) return CURSOR_SIZE_D1;
    if ((dir & RESIZE_LEFT) && (dir & RESIZE_BOTTOM)) return CURSOR_SIZE_D1;
    if (dir & (RESIZE_LEFT | RESIZE_RIGHT)) return CURSOR_SIZE_H;
    if (dir & (RESIZE_TOP | RESIZE_BOTTOM)) return CURSOR_SIZE_V;
    return CURSOR_NORMAL;
}

static int open_image_file(struct Window *term, const char *name) {
    uint32_t size = 0;
    if (load_file_bytes(term, name, file_io_buf, sizeof(file_io_buf), &size) != 0) return -1;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].active) continue;
        reset_window(&wins[i], i);
        wins[i].active = 1;
        wins[i].maximized = 0;
        wins[i].minimized = 0;
        wins[i].x = 140 + (i * 28);
        wins[i].y = 90 + (i * 22);
        int img_w = 0, img_h = 0;
        if (decode_image_to_rgb565(file_io_buf, size, wins[i].image, IMG_MAX_W, IMG_MAX_H, &img_w, &img_h) != 0) {
            reset_window(&wins[i], i);
            return -2;
        }
        wins[i].kind = WINDOW_KIND_IMAGE;
        wins[i].image_w = img_w;
        wins[i].image_h = img_h;
        wins[i].image_scale = 1;
        wins[i].w = img_w + 20;
        if (wins[i].w < 220) wins[i].w = 220;
        wins[i].h = img_h + 46;
        if (wins[i].h < 140) wins[i].h = 140;
        lib_strcpy(wins[i].title, "Image: ");
        lib_strncat(wins[i].title, name, 15);
        wins[i].x = (WIDTH - wins[i].w) / 2;
        if (wins[i].x < 0) wins[i].x = 0;
        wins[i].y = (DESKTOP_H - wins[i].h) / 2;
        if (wins[i].y < 0) wins[i].y = 0;
        bring_to_front(i);
        return 0;
    }
    return -3;
}

int open_text_editor(struct Window *term, const char *name) {
    uint32_t size = 0;
    int rc = -1;
    char display_name[64];
    char requested[64];
    char absolute_path[128];
    uint32_t dir_bno = term ? term->cwd_bno : 1;
    char cwd_copy[128];
    char leaf[32];
    int use_resolve = 0;

    if (!term) return -1;
    if (!name || name[0] == '\0') return -1;

    // Clear all local buffers
    memset(leaf, 0, sizeof(leaf));
    memset(absolute_path, 0, sizeof(absolute_path));
    memset(display_name, 0, sizeof(display_name));
    memset(requested, 0, sizeof(requested));
    memset(cwd_copy, 0, sizeof(cwd_copy));
    
    strncpy(requested, name, sizeof(requested)-1);

    if (strchr(requested, '/') || strstr(requested, "..") || requested[0] == '~') use_resolve = 1;
    lib_strcpy(cwd_copy, term->cwd[0] ? term->cwd : "/root");
    
    // Default absolute path construction
    build_editor_path(absolute_path, cwd_copy, requested);

    if (use_resolve && resolve_editor_target(term, name, &dir_bno, cwd_copy, leaf) == 0) {
        extern struct Window fs_tmp_window;
        struct Window *load_ctx = &fs_tmp_window;
        memset(load_ctx, 0, sizeof(*load_ctx));
        load_ctx->cwd_bno = dir_bno;
        lib_strcpy(load_ctx->cwd, cwd_copy);
        rc = load_file_bytes(load_ctx, leaf, file_io_buf, sizeof(file_io_buf), &size);
        build_editor_path(absolute_path, cwd_copy, leaf);
        strncpy(display_name, leaf, sizeof(display_name)-1);
    } else {
        rc = load_file_bytes(term, requested, file_io_buf, sizeof(file_io_buf), &size);
        const char *base = path_basename(requested);
        strncpy(display_name, base, sizeof(display_name)-1);
    }
    
    if (rc == -3) return -4;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].active) continue;
        reset_window(&wins[i], i);
        wins[i].active = 1;
        wins[i].kind = WINDOW_KIND_EDITOR;
        wins[i].maximized = 1;
        wins[i].x = 0; wins[i].y = 0; wins[i].w = WIDTH; wins[i].h = DESKTOP_H;
        
        strncpy(wins[i].editor_name, display_name, 19);
        lib_strcpy(wins[i].editor_path, absolute_path);
        lib_strcpy(wins[i].editor_cwd, cwd_copy[0] ? cwd_copy : "/root");
        wins[i].cwd_bno = dir_bno ? dir_bno : 1;
        
        if (rc == 0) editor_load_bytes(&wins[i], file_io_buf, size);
        else editor_clear(&wins[i]);
        
        memset(wins[i].title, 0, sizeof(wins[i].title));
        lib_strcpy(wins[i].title, "Vim: ");
        shorten_path_for_title(wins[i].title + 5, wins[i].editor_path, 55);
        
        editor_set_status(&wins[i], "i=insert  :wq=save+quit  :q=quit");
        bring_to_front(i);
        return 0;
    }
    return -5;
}

static void resize_image_window(struct Window *w, int new_scale) {
    if (w->kind != WINDOW_KIND_IMAGE) return;
    if (new_scale < 1) new_scale = 1;
    if (new_scale > 12) new_scale = 12;
    if (new_scale == w->image_scale) return;

    int center_x = w->x + w->w / 2;
    int center_y = w->y + w->h / 2;

    w->image_scale = new_scale;
    w->w = w->image_w * new_scale + 20;
    if (w->w < 220) w->w = 220;
    if (w->w > WIDTH) w->w = WIDTH;
    w->h = w->image_h * new_scale + 46;
    if (w->h < 140) w->h = 140;
    if (w->h > DESKTOP_H) w->h = DESKTOP_H;

    w->x = center_x - w->w / 2;
    w->y = center_y - w->h / 2;
    w->x = clamp_int(w->x, 0, WIDTH - w->w);
    w->y = clamp_int(w->y, 0, DESKTOP_H - w->h);
}

void draw_line_clipped(int x0, int y0, int x1, int y1, int color,
                       int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int dx = x1 - x0;
    int sx = dx >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    int dy = y1 - y0;
    int sy = dy >= 0 ? 1 : -1;
    if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : -dy) / 2;
    for (;;) {
        if (x0 >= clip_x0 && x0 < clip_x1 && y0 >= clip_y0 && y0 < clip_y1) {
            putpixel(x0, y0, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

static int edge_fn(int ax, int ay, int bx, int by, int px, int py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void draw_triangle_filled_clipped(int x0, int y0, int x1, int y1, int x2, int y2, int color,
                                  int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (x1 < minx) minx = x1; if (x2 < minx) minx = x2;
    if (x1 > maxx) maxx = x1; if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1; if (y2 < miny) miny = y2;
    if (y1 > maxy) maxy = y1; if (y2 > maxy) maxy = y2;
    if (minx < clip_x0) minx = clip_x0;
    if (miny < clip_y0) miny = clip_y0;
    if (maxx >= clip_x1) maxx = clip_x1 - 1;
    if (maxy >= clip_y1) maxy = clip_y1 - 1;
    int area = edge_fn(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;
    int sign = (area > 0) ? 1 : -1;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            int w0 = sign * edge_fn(x1, y1, x2, y2, px, py);
            int w1 = sign * edge_fn(x2, y2, x0, y0, px, py);
            int w2 = sign * edge_fn(x0, y0, x1, y1, px, py);
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                putpixel(px, py, color);
            }
        }
    }
}

int sin_deg(int deg) {
    static const unsigned short sin_q[91] = {
        0,4,9,13,18,22,27,31,36,40,44,49,53,58,62,66,71,75,79,83,88,92,96,100,104,108,112,116,120,124,
        128,132,135,139,143,147,150,154,157,161,164,167,171,174,177,181,184,187,190,193,195,198,201,204,
        206,209,211,214,216,218,221,223,225,227,229,231,232,234,236,237,239,240,242,243,244,245,246,247,
        248,249,250,251,251,252,253,253,254,254,254,255,255,255,255,255,256
    };
    while (deg < 0) deg += 360;
    deg %= 360;
    if (deg <= 90) return sin_q[deg];
    if (deg <= 180) return sin_q[180 - deg];
    if (deg <= 270) return -((int)sin_q[deg - 180]);
    return -((int)sin_q[360 - deg]);
}

int cos_deg(int deg) {
    return sin_deg(deg + 90);
}

static int hit_resize_zone(struct Window *w, int mx, int my) {
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
    int dir = RESIZE_NONE;
    if (w->maximized || w->minimized) return RESIZE_NONE;
    if (mx < wx || mx >= wx + ww || my < wy || my >= wy + wh) return RESIZE_NONE;
    if (mx < wx + RESIZE_MARGIN) dir |= RESIZE_LEFT;
    else if (mx >= wx + ww - RESIZE_MARGIN) dir |= RESIZE_RIGHT;
    if (my < wy + RESIZE_MARGIN) dir |= RESIZE_TOP;
    else if (my >= wy + wh - RESIZE_MARGIN) dir |= RESIZE_BOTTOM;
    return dir;
}

static void begin_resize(struct Window *w, int dir, int mx, int my) {
    w->resizing = 1;
    w->resize_dir = dir;
    w->resize_start_mx = mx;
    w->resize_start_my = my;
    w->resize_start_x = w->x;
    w->resize_start_y = w->y;
    w->resize_start_w = w->w;
    w->resize_start_h = w->h;
}

static void apply_resize(struct Window *w, int mx, int my) {
    int dx = mx - w->resize_start_mx;
    int dy = my - w->resize_start_my;
    int new_x = w->resize_start_x;
    int new_y = w->resize_start_y;
    int new_w = w->resize_start_w;
    int new_h = w->resize_start_h;

    if (w->resize_dir & RESIZE_RIGHT) {
        new_w = w->resize_start_w + dx;
    }
    if (w->resize_dir & RESIZE_BOTTOM) {
        new_h = w->resize_start_h + dy;
    }
    if (w->resize_dir & RESIZE_LEFT) {
        new_x = w->resize_start_x + dx;
        new_w = w->resize_start_w - dx;
        if (new_w < MIN_WIN_W) {
            new_x -= (MIN_WIN_W - new_w);
            new_w = MIN_WIN_W;
        }
    }
    if (w->resize_dir & RESIZE_TOP) {
        new_y = w->resize_start_y + dy;
        new_h = w->resize_start_h - dy;
        if (new_h < MIN_WIN_H) {
            new_y -= (MIN_WIN_H - new_h);
            new_h = MIN_WIN_H;
        }
    }

    new_w = clamp_int(new_w, MIN_WIN_W, WIDTH);
    new_h = clamp_int(new_h, MIN_WIN_H, DESKTOP_H);
    new_x = clamp_int(new_x, 0, WIDTH - new_w);
    new_y = clamp_int(new_y, 0, DESKTOP_H - new_h);

    w->x = new_x;
    w->y = new_y;
    w->w = new_w;
    w->h = new_h;
    if (w->kind == WINDOW_KIND_TERMINAL) terminal_clamp_v_offset(w);
    if (w->kind == WINDOW_KIND_NETSURF) {
        w->v_offset = 0;
        w->ns_h_offset = 0;
        w->ns_input_active = 0;
        w->ns_resize_pending = 1;
        w->ns_resize_last_ms = sys_now();
        extern void netsurf_invalidate_layout(int win_id);
        netsurf_invalidate_layout(w->id);
    }
}

void bring_to_front(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    int p = -1; for(int i=0; i<MAX_WINDOWS; i++) if(z_order[i] == idx) { p = i; break; }
    if(p != -1) {
        for(int k=p; k<MAX_WINDOWS-1; k++) z_order[k] = z_order[k+1];
        z_order[MAX_WINDOWS-1] = idx;
        active_win_idx = idx;
        wins[idx].taskbar_anim = 8;
    }
}

void cycle_active_window() {
    int start_pos = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (z_order[i] == active_win_idx) {
            start_pos = i;
            break;
        }
    }

    if (start_pos == -1) {
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            int cid = z_order[i];
            if (cid >= 0 && cid < MAX_WINDOWS && wins[cid].active && !wins[cid].minimized) {
                active_win_idx = cid;
                return;
            }
        }
        active_win_idx = -1;
        return;
    }

    int current = z_order[start_pos];
    if (current < 0 || current >= MAX_WINDOWS) {
        active_win_idx = -1;
        return;
    }

    // Rotate the current top window to the back so Shift+Tab cycles all jobs,
    // instead of bouncing between only the last two.
    for (int i = start_pos; i > 0; i--) {
        z_order[i] = z_order[i - 1];
    }
    z_order[0] = current;

    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        int cid = z_order[i];
        if (cid >= 0 && cid < MAX_WINDOWS && wins[cid].active && !wins[cid].minimized) {
            active_win_idx = cid;
            return;
        }
    }

    active_win_idx = -1;
}

static void recursive_find(struct Window *w, uint32_t bno, const char *path, const char *search, char *out) {
    if (bno == 0 || bno >= FS_MAX_BLOCKS) return;
    
    // Allocate a block buffer on the heap to avoid stack overflow
    struct blk *b = malloc(sizeof(struct blk));
    if (!b) return;
    
    memset(b, 0, sizeof(struct blk));
    b->blockno = bno;
    virtio_disk_rw(b, 0);
    struct dir_block *db = (struct dir_block *)b->data;
    if (db->magic != FS_MAGIC) {
        free(b);
        return;
    }
    
    // Copy necessary info to minimize stack usage during recursion
    struct file_entry entries[MAX_DIR_ENTRIES];
    memcpy(entries, db->entries, sizeof(entries));
    free(b); // Done with the disk block
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (entries[i].name[0] == '\0') continue;
        
        char full_path[256];
        lib_strcpy(full_path, path);
        if (full_path[strlen(full_path)-1] != '/') lib_strcat(full_path, "/");
        lib_strcat(full_path, entries[i].name);
        
        if (strstr(entries[i].name, search)) {
            char row[300];
            snprintf(row, sizeof(row), "%s %s\n", (entries[i].type == 1 ? "[DIR]" : "[FILE]"), full_path);
            if (strlen(out) + strlen(row) < OUT_BUF_SIZE - 2) {
                lib_strcat(out, row);
            }
        }
        
        if (entries[i].type == 1) {
            recursive_find(w, entries[i].bno, full_path, search, out);
        }
    }
}

static uint32_t calculate_dir_size(uint32_t bno, int curr_depth, int max_depth) {
    if (bno == 0 || bno >= FS_MAX_BLOCKS || curr_depth > max_depth) return 0;
    
    struct blk *b = malloc(sizeof(struct blk));
    if (!b) return 0;
    
    memset(b, 0, sizeof(struct blk));
    b->blockno = bno;
    virtio_disk_rw(b, 0);
    struct dir_block *db = (struct dir_block *)b->data;
    
    if (db->magic != FS_MAGIC) {
        free(b);
        return 0;
    }
    
    uint32_t total = 0;
    uint32_t child_bnos[MAX_DIR_ENTRIES];
    int count = 0;

    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (db->entries[i].name[0] == '\0') continue;
        if (db->entries[i].type == 1) { // Directory
            if (count < MAX_DIR_ENTRIES) {
                child_bnos[count++] = db->entries[i].bno;
            }
        } else if (db->entries[i].type == 0) { // File
            total += db->entries[i].size;
        }
    }

    free(b);
    
    // Only recurse if we haven't reached max depth
    if (curr_depth < max_depth) {
        for (int i = 0; i < count; i++) {
            total += calculate_dir_size(child_bnos[i], curr_depth + 1, max_depth);
        }
    }
    return total;
}

static void list_dir_contents(uint32_t bno, char *out) {
    extern struct Window fs_tmp_window;
    struct Window *ctx = &fs_tmp_window;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cwd_bno = bno;
    struct dir_block *db = load_current_dir(ctx);
    if (!db) { lib_strcpy(out, "ERR: Directory unreadable."); return; }

    append_dir_entries_sorted(db, "Directory contents:\n", 0, -1, out);
}

static int check_dir_and_list(struct Window *w, const char *path, char *out) {
    if (!path || path[0] == '\0') return 0;
    char expanded[128]; uint32_t p_bno = 0; char p_cwd[128], leaf[20];
    terminal_expand_home_path(w, path, expanded, sizeof(expanded));
    if (resolve_editor_target(w, expanded, &p_bno, p_cwd, leaf) == 0) {
        uint32_t target_bno = 0;
        if (leaf[0] == '\0' || strcmp(leaf, ".") == 0) target_bno = p_bno;
        else {
            extern struct Window fs_tmp_window; struct Window *tmp = &fs_tmp_window;
            memset(tmp, 0, sizeof(*tmp)); tmp->cwd_bno = p_bno;
            struct dir_block *db = load_current_dir(tmp);
            int idx = find_entry_index(db, leaf, -1);
            if (idx >= 0 && db->entries[idx].type == 1) target_bno = db->entries[idx].bno;
        }
        if (target_bno != 0) { list_dir_contents(target_bno, out); return 1; }
    }
    return 0;
}

void exec_single_cmd(struct Window *w, char *cmd) {
    char *out = w->out_buf; out[0] = '\0';
    if (strncmp(cmd, "pwd", 3) == 0) { 
        if (cmd[3] == ' ' && (cmd[4] == 'h' || cmd[4] == '-')) { lib_strcpy(out, "usage: pwd"); return; }
        lib_strcpy(out, w->cwd); 
    }
    else if (strncmp(cmd, "df", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')) {
        if (cmd[2] == ' ' && (cmd[3] == 'h' || cmd[3] == '-')) { lib_strcpy(out, "usage: df"); return; }
        uint32_t total, used, free_blks;
        fs_usage_info(&total, &used, &free_blks);
        char row[128], sz_total[16], sz_used[16], sz_free[16];
        format_size_human(total * BSIZE, sz_total);
        format_size_human(used * BSIZE, sz_used);
        format_size_human(free_blks * BSIZE, sz_free);
        snprintf(row, sizeof(row), "Disk Usage (BlockSize=%d):\n  Total: %8s (%u blocks)\n  Used:  %8s (%u blocks)\n  Free:  %8s (%u blocks)\n",
                 BSIZE, sz_total, total, sz_used, used, sz_free, free_blks);
        lib_strcpy(out, row);
    } else if (strncmp(cmd, "du", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')) {
        char *path_arg = cmd + 2; while (*path_arg == ' ') path_arg++;
        if (*path_arg == 'h' && (*(path_arg+1) == '\0' || *(path_arg+1) == ' ')) { lib_strcpy(out, "usage: du [-maxd N] [path]"); return; }
        int max_depth = 20; char *mpos = strstr(cmd, "-maxd");
        if (mpos) {
            char *dstr = mpos + 5; while (*dstr == ' ') dstr++;
            if (*dstr >= '0' && *dstr <= '9') {
                max_depth = 0; while (*dstr >= '0' && *dstr <= '9') { max_depth = max_depth * 10 + (*dstr - '0'); dstr++; }
            }
            path_arg = dstr; while (*path_arg == ' ') path_arg++;
        }
        uint32_t target_bno = w->cwd_bno; const char *dpath = w->cwd; char expanded[128];
        if (*path_arg != '\0') {
            uint32_t p_bno = 0; char p_cwd[128], leaf[20];
            terminal_expand_home_path(w, path_arg, expanded, sizeof(expanded));
            if (resolve_editor_target(w, expanded, &p_bno, p_cwd, leaf) == 0) {
                if (leaf[0] == '\0' || strcmp(leaf, ".") == 0) { target_bno = p_bno; dpath = (expanded[0] ? expanded : w->cwd); }
                else {
                    extern struct Window fs_tmp_window; struct Window *tmp = &fs_tmp_window;
                    memset(tmp, 0, sizeof(*tmp)); tmp->cwd_bno = p_bno;
                    struct dir_block *db = load_current_dir(tmp);
                    if (db) {
                        int idx = find_entry_index(db, leaf, 1);
                        if (idx >= 0) { target_bno = db->entries[idx].bno; dpath = expanded; }
                        else { lib_strcpy(out, "ERR: Dir Not Found."); return; }
                    }
                }
            } else { lib_strcpy(out, "ERR: Invalid Path."); return; }
        }
        uint32_t size = calculate_dir_size(target_bno, 1, max_depth);
        char sz_str[16]; format_size_human(size, sz_str);
        snprintf(out, OUT_BUF_SIZE, "Size of %s (maxdepth %d): %s (%u bytes)", dpath, max_depth, sz_str, size);
    } else if (strncmp(cmd, "format", 6) == 0) {
        char *arg = cmd + 6;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "YES_I_AM_SURE") == 0) {
            fs_format_root(w);
            fs_reset_window_cwd(w);
            terminal_env_sync_pwd(w);
            lib_strcpy(out, ">> Matrix Rebuilt. All data wiped.");
        } else {
            lib_strcpy(out, "DANGER: This will wipe EVERYTHING.\nType: format YES_I_AM_SURE");
        }
    } else if (strncmp(cmd, "cd ", 3) == 0 || strcmp(cmd, "cd") == 0) {
        char *target = cmd + 2;
        while (*target == ' ') target++;
        if (*target == 'h' && (*(target+1) == '\0' || *(target+1) == ' ')) {
            lib_strcpy(out, "usage: cd ~ | cd .. | cd /root/subdir | cd subdir");
            return;
        }
        if (*target == '\0' || strcmp(target, "/root") == 0 || strcmp(target, "/") == 0) {
            w->cwd_bno = 1;
            lib_strcpy(w->cwd, "/root");
            terminal_env_sync_pwd(w);
        } else if (strcmp(target, "..") == 0) {
            struct dir_block *db = load_current_dir(w);
            if (!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
            w->cwd_bno = db->parent_bno ? db->parent_bno : 1;
            if (w->cwd_bno == 1) lib_strcpy(w->cwd, "/root");
            else path_set_parent(w->cwd);
            terminal_env_sync_pwd(w);
        } else {
            char expanded[128];
            uint32_t parent_bno = 0;
            char parent_cwd[128];
            char leaf[20];
            extern struct Window fs_tmp_window;
            struct Window *tmp = &fs_tmp_window;
            struct dir_block *db;
            int idx;
            terminal_expand_home_path(w, target, expanded, sizeof(expanded));
            if (expanded[0] == '\0') { lib_strcpy(out, "ERR: Dir Not Found."); return; }
            if (resolve_editor_target(w, expanded, &parent_bno, parent_cwd, leaf) != 0) {
                lib_strcpy(out, "ERR: Dir Not Found.");
                return;
            }
            memset(tmp, 0, sizeof(*tmp));
            tmp->cwd_bno = parent_bno ? parent_bno : 1;
            lib_strcpy(tmp->cwd, parent_cwd[0] ? parent_cwd : "/root");
            db = load_current_dir(tmp);
            if (!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
            idx = find_entry_index(db, leaf, 1);
            if (idx < 0) { lib_strcpy(out, "ERR: Dir Not Found."); return; }
            w->cwd_bno = db->entries[idx].bno;
            lib_strcpy(w->cwd, tmp->cwd[0] ? tmp->cwd : "/root");
            path_set_child(w->cwd, db->entries[idx].name);
            terminal_env_sync_pwd(w);
        }
    } else if (strncmp(cmd, "alias", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        char *spec = cmd + 5;
        while (*spec == ' ') spec++;
        if (*spec == 'h' && (*(spec+1) == '\0' || *(spec+1) == ' ')) {
            lib_strcpy(out, "usage: alias name=value | alias (to list all)");
            return;
        }
        char *eq;
        char row[OUT_BUF_SIZE];
        while (*spec == ' ') spec++;
        if (*spec == '\0') {
            out[0] = '\0';
            for (int i = 0; i < w->alias_count; i++) {
                append_out_str(out, OUT_BUF_SIZE, "alias ");
                append_out_str(out, OUT_BUF_SIZE, w->alias_names[i]);
                append_out_str(out, OUT_BUF_SIZE, "='");
                append_out_str(out, OUT_BUF_SIZE, w->alias_values[i]);
                append_out_str(out, OUT_BUF_SIZE, "'");
                if (i + 1 < w->alias_count) append_out_str(out, OUT_BUF_SIZE, "\n");
            }
            if (w->alias_count == 0) lib_strcpy(out, "(empty)");
        } else {
            eq = strchr(spec, '=');
            if (!eq) {
                const char *val = terminal_alias_get(w, spec);
                if (val && val[0]) {
                    lib_strcpy(row, "alias ");
                    lib_strcat(row, spec);
                    lib_strcat(row, "='");
                    lib_strcat(row, val);
                    lib_strcat(row, "'");
                    lib_strcpy(out, row);
                } else {
                    lib_strcpy(out, "ERR: alias not found.");
                }
            } else {
                *eq++ = '\0';
                while (*eq == ' ') eq++;
                if (*spec == '\0' || *eq == '\0') {
                    lib_strcpy(out, "ERR: alias name=value.");
                } else if (terminal_alias_set(w, spec, eq) != 0) {
                    lib_strcpy(out, "ERR: alias failed.");
                }
            }
        }
    } else if (strncmp(cmd, "unalias", 7) == 0 && (cmd[7] == '\0' || cmd[7] == ' ')) {
        char *name = cmd + 7;
        while (*name == ' ') name++;
        if (*name == '\0') {
            lib_strcpy(out, "ERR: unalias NAME.");
        } else if (terminal_alias_unset(w, name) != 0) {
            lib_strcpy(out, "ERR: unalias failed.");
        }
    } else if (strncmp(cmd, "source", 6) == 0 && (cmd[6] == '\0' || cmd[6] == ' ')) {
        char *path = cmd + 6;
        while (*path == ' ') path++;
        if (*path == 'h' && (*(path+1) == '\0' || *(path+1) == ' ')) {
            lib_strcpy(out, "usage: source <file> | . <file>");
            return;
        }
        if (*path == '\0') {
            lib_strcpy(out, "ERR: source <file>.");
        } else {
            terminal_source_script(w, path);
        }
    } else if (cmd[0] == '.' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        char *path = cmd + 1;
        while (*path == ' ') path++;
        if (*path == '\0') {
            lib_strcpy(out, "ERR: source <file>.");
        } else {
            terminal_source_script(w, path);
        }
    } else if (strncmp(cmd, "export", 6) == 0 && (cmd[6] == '\0' || cmd[6] == ' ')) {
        char *spec = cmd + 6;
        while (*spec == ' ') spec++;
        if (*spec == 'h' && (*(spec+1) == '\0' || *(spec+1) == ' ')) {
            lib_strcpy(out, "usage: export NAME=value");
            return;
        }
        char name[ENV_NAME_LEN];
        char value[ENV_VALUE_LEN];
        char *eq;
        int n = 0;
        while (*spec == ' ') spec++;
        if (*spec == '\0') {
            lib_strcpy(out, "ERR: export NAME=value.");
        } else {
            eq = strchr(spec, '=');
            if (!eq) {
                while (spec[n] && spec[n] != ' ' && n < ENV_NAME_LEN - 1) {
                    name[n] = spec[n];
                    n++;
                }
                name[n] = '\0';
                value[0] = '\0';
            } else {
                while (spec[n] && spec + n < eq && n < ENV_NAME_LEN - 1) {
                    name[n] = spec[n];
                    n++;
                }
                name[n] = '\0';
                n = 0;
                eq++;
                while (*eq == ' ') eq++;
                while (eq[n] && n < ENV_VALUE_LEN - 1) {
                    value[n] = eq[n];
                    n++;
                }
                value[n] = '\0';
            }
            if (terminal_env_set(w, name, value) != 0) {
                lib_strcpy(out, "ERR: export failed.");
            }
        }
    } else if (strncmp(cmd, "unset", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        char *name = cmd + 5;
        while (*name == ' ') name++;
        if (*name == '\0') {
            lib_strcpy(out, "ERR: unset NAME.");
        } else if (terminal_env_unset(w, name) != 0) {
            lib_strcpy(out, "ERR: unset failed.");
        }
    } else if (strcmp(cmd, "env") == 0 || strncmp(cmd, "printenv", 8) == 0) {
        out[0] = '\0';
        for (int i = 0; i < w->env_count; i++) {
            append_out_str(out, OUT_BUF_SIZE, w->env_names[i]);
            append_out_str(out, OUT_BUF_SIZE, "=");
            append_out_str(out, OUT_BUF_SIZE, w->env_values[i]);
            if (i + 1 < w->env_count) append_out_str(out, OUT_BUF_SIZE, "\n");
        }
        if (w->env_count == 0) {
            lib_strcpy(out, "(empty)");
        }
    } else if (strncmp(cmd, "find ", 5) == 0) {
        char *target = cmd + 5;
        while (*target == ' ') target++;
        if (*target == '\0') { lib_strcpy(out, "usage: find <filename>"); return; }
        out[0] = '\0';
        recursive_find(w, 1, "/root", target, out);
        if (out[0] == '\0') lib_strcpy(out, "No files found.");
    } else if (strncmp(cmd, "ls", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')) {
        char *h_ptr = cmd + 2; while (*h_ptr == ' ') h_ptr++;
        if (strcmp(h_ptr, "h") == 0 || strcmp(h_ptr, "-h") == 0) {
            lib_strcpy(out, "usage: ls [-all] [path]"); return;
        }
        int all = (strstr(cmd, "-all") != 0);
        char *path_arg = cmd + 2; while (*path_arg == ' ') path_arg++;
        if (all) {
            char *all_ptr = strstr(cmd, "-all");
            path_arg = all_ptr + 4; while (*path_arg == ' ') path_arg++;
            if (*path_arg == '\0') {
                path_arg = cmd + 2; while (*path_arg == ' ' || *path_arg == '-') path_arg++;
                if (strncmp(path_arg, "all", 3) == 0) { path_arg += 3; while (*path_arg == ' ') path_arg++; }
            }
        }
        uint32_t target_bno = w->cwd_bno;
        char single_out[OUT_BUF_SIZE];
        single_out[0] = '\0';
        if (*path_arg != '\0' && strcmp(path_arg, "-all") != 0) {
            uint32_t p_bno = 0; char p_cwd[128], leaf[20];
            if (resolve_fs_target(w, path_arg, &p_bno, p_cwd, leaf) == 0) {
                if (leaf[0] == '\0' || strcmp(leaf, ".") == 0) { target_bno = p_bno; }
                else {
                    extern struct Window fs_tmp_window; struct Window *ltmp = &fs_tmp_window;
                    memset(ltmp, 0, sizeof(*ltmp)); ltmp->cwd_bno = p_bno;
                    struct dir_block *db = load_current_dir(ltmp);
                    if (db) {
                        int idx_file = find_entry_index(db, leaf, 0);
                        int idx_dir = find_entry_index(db, leaf, 1);
                        if (idx_file >= 0) {
                            append_single_entry_line(&db->entries[idx_file], single_out, all);
                            lib_strcpy(out, single_out);
                            return;
                        }
                        if (idx_dir >= 0) target_bno = db->entries[idx_dir].bno;
                        else { lib_strcpy(out, "ERR: Dir Not Found."); return; }
                    }
                }
            } else { lib_strcpy(out, "ERR: Invalid Path."); return; }
        }
        extern struct Window fs_tmp_window; struct Window *ctx = &fs_tmp_window;
        memset(ctx, 0, sizeof(*ctx)); ctx->cwd_bno = target_bno;
        struct dir_block *db = load_current_dir(ctx);
        if(!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
        int printed = 0; out[0] = '\0';
        for(int i=0; i<MAX_DIR_ENTRIES; i++) if(db->entries[i].name[0]) {
            if(!all && db->entries[i].name[0] == '.' && strcmp(db->entries[i].name, ".bashrc") != 0) continue;
            char row[OUT_BUF_SIZE]; row[0] = '\0';
            if(all) {
                char ms[5], sz[16], ts[10], name[32];
                mode_to_str(db->entries[i].mode, db->entries[i].type, ms);
                format_size_human(db->entries[i].size, sz); format_hms(db->entries[i].mtime, ts);
                copy_name20(name, db->entries[i].name);
                append_out_pad(row, OUT_BUF_SIZE, ms, 5); append_out_str(row, OUT_BUF_SIZE, " ");
                append_out_pad(row, OUT_BUF_SIZE, sz, 8); append_out_str(row, OUT_BUF_SIZE, " ");
                append_out_pad(row, OUT_BUF_SIZE, ts, 19); append_out_str(row, OUT_BUF_SIZE, " ");
                append_out_str(row, OUT_BUF_SIZE, name);
            } else {
                append_out_str(row, OUT_BUF_SIZE, (db->entries[i].type==1) ? "d " : "f ");
                append_out_str(row, OUT_BUF_SIZE, db->entries[i].name);
            }
            append_out_str(out, OUT_BUF_SIZE, row);
            append_out_str(out, OUT_BUF_SIZE, "\n");
            printed = 1;
        }
        if (!printed) lib_strcpy(out, "(empty)");
    } else if (strncmp(cmd, "ssh", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        char *arg = cmd + 3;
        while (*arg == ' ') arg++;
        if (*arg == 'h' && (*(arg+1) == '\0' || *(arg+1) == ' ')) {
            lib_strcpy(out, "usage: ssh status | ssh set <user@host[:port]> | ssh auth [password] | ssh exec <cmd>");
            return;
        }
        if (*arg == '\0' || strcmp(arg, "status") == 0) {
            char target[96];
            char wrp_url[160];
            out[0] = '\0';
            lib_strcat(out, "ssh: ");
            if (ssh_client_get_target(target, sizeof(target)) == 0) {
                lib_strcat(out, target);
            } else {
                lib_strcat(out, "(unset)");
            }
            lib_strcat(out, " wrp: ");
            if (ssh_client_get_wrp_url(wrp_url, sizeof(wrp_url)) == 0) {
                lib_strcat(out, wrp_url);
            } else {
                lib_strcat(out, "(unset)");
            }
        } else if (strchr(arg, '@') != NULL && strchr(arg, ' ') == NULL) {
            if (ssh_client_set_target(arg, out, OUT_BUF_SIZE) < 0 && out[0] == '\0') {
                lib_strcpy(out, "ERR: ssh set <user@host[:port]>.");
            }
        } else if (strncmp(arg, "set ", 4) == 0) {
            char *spec = arg + 4;
            while (*spec == ' ') spec++;
            if (ssh_client_set_target(spec, out, OUT_BUF_SIZE) < 0 && out[0] == '\0') {
                lib_strcpy(out, "ERR: ssh set <user@host[:port]>.");
            }
        } else if (strncmp(arg, "auth ", 5) == 0) {
            char *password = arg + 5;
            while (*password == ' ') password++;
            if (ssh_client_set_password(password, out, OUT_BUF_SIZE) < 0 && out[0] == '\0') {
                lib_strcpy(out, "ERR: ssh auth <password>.");
            }
        } else if (strncmp(arg, "exec ", 5) == 0) {
            char *remote = arg + 5;
            while (*remote == ' ') remote++;
            if (*remote == '\0') {
                lib_strcpy(out, "ERR: ssh exec <command>.");
            } else {
                ssh_client_exec_remote(remote, out, OUT_BUF_SIZE);
            }
        } else {
            lib_strcpy(out, "ERR: ssh status | ssh set <user@host[:port]> | ssh auth [password] | ssh exec <cmd>");
        }
    } else if (strncmp(cmd, "wrp", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        char *arg = cmd + 3;
        while (*arg == ' ') arg++;
        if (*arg == 'h' && (*(arg+1) == '\0' || *(arg+1) == ' ')) {
            lib_strcpy(out, "usage: wrp status | wrp set <http://host:8080> | wrp open");
            return;
        }
        if (*arg == '\0' || strcmp(arg, "status") == 0) {
            char wrp_url[160];
            out[0] = '\0';
            lib_strcat(out, "wrp: ");
            if (ssh_client_get_wrp_url(wrp_url, sizeof(wrp_url)) == 0) {
                lib_strcat(out, wrp_url);
            } else {
                lib_strcat(out, "(unset)");
            }
        } else if (strncmp(arg, "set ", 4) == 0) {
            char *url = arg + 4;
            while (*url == ' ') url++;
            if (ssh_client_set_wrp_url(url, out, OUT_BUF_SIZE) < 0 && out[0] == '\0') {
                lib_strcpy(out, "ERR: wrp set <http://host:8080>.");
            }
        } else if (strcmp(arg, "open") == 0) {
            char wrp_url[160];
            if (ssh_client_get_wrp_url(wrp_url, sizeof(wrp_url)) < 0) {
                lib_strcpy(out, "ERR: wrp url not set. Use wrp set <http://host:8080>.");
            } else {
                char wcmd[208];
                lib_strcpy(wcmd, "netsurf ");
                lib_strcat(wcmd, wrp_url);
                exec_single_cmd(w, wcmd);
                lib_strcpy(out, ">> WRP Opened.");
            }
        } else {
            lib_strcpy(out, "ERR: wrp status | wrp set <http://host:8080> | wrp open");
        }
    } else if (strncmp(cmd, "mkdir ", 6) == 0) {
        char *path = cmd + 6; while (*path == ' ') path++;
        if (strcmp(path, "h") == 0) { lib_strcpy(out, "usage: mkdir <path>"); return; }
        if (check_dir_and_list(w, path, out)) return;
        char expanded[128]; uint32_t p_bno = 0; char p_cwd[128], leaf[20];
        terminal_expand_home_path(w, path, expanded, sizeof(expanded));
        if (resolve_editor_target(w, expanded, &p_bno, p_cwd, leaf) != 0 || leaf[0] == '\0') { lib_strcpy(out, "ERR: Invalid Path."); return; }
        extern struct Window fs_tmp_window; struct Window *ctx = &fs_tmp_window;
        memset(ctx, 0, sizeof(*ctx)); ctx->cwd_bno = p_bno;
        struct dir_block *db = load_current_dir(ctx);
        if (!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
        if (find_entry_index(db, leaf, -1) != -1) { lib_strcpy(out, "ERR: Already Exists."); return; }
        for(int i=0; i<MAX_DIR_ENTRIES; i++) if(db->entries[i].name[0]==0) {
            uint32_t nb = balloc(); memset(&db->entries[i], 0, sizeof(db->entries[i]));
            copy_name20(db->entries[i].name, leaf); db->entries[i].bno = nb; db->entries[i].type = 1; db->entries[i].mode = 7;
            db->entries[i].ctime = db->entries[i].mtime = current_fs_time();
            virtio_disk_rw(&ctx->local_b, 1);
            struct blk nb_blk; memset(&nb_blk, 0, sizeof(nb_blk)); nb_blk.blockno = nb;
            struct dir_block *nd = (struct dir_block *)nb_blk.data; nd->magic = FS_MAGIC; nd->parent_bno = p_bno;
            virtio_disk_rw(&nb_blk, 1); lib_strcpy(out, ">> Directory Created."); return;
        }
        lib_strcpy(out, "ERR: Dir Full.");
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        char *target = cmd + 3; while (*target == ' ') target++;
        if (strcmp(target, "h") == 0) { lib_strcpy(out, "usage: rm <path>"); return; }
        if (check_dir_and_list(w, target, out)) return;
        if (remove_entry_named(w, target) == 0) lib_strcpy(out, ">> Removed."); else lib_strcpy(out, "ERR: Remove Failed.");
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        char *fn = cmd + 6; while (*fn == ' ') fn++;
        if (strcmp(fn, "h") == 0) { lib_strcpy(out, "usage: touch <path>"); return; }
        if (check_dir_and_list(w, fn, out)) return;
        if (store_file_bytes(w, fn, (const unsigned char *)"", 0) == 0) lib_strcpy(out, ">> Touched."); else lib_strcpy(out, "ERR: Touch Failed.");
    } else if (strncmp(cmd, "write ", 6) == 0) {
        char *fn = cmd + 6; char *data = strchr(fn, ' ');
        if (data) { 
            *data++ = '\0'; while (*data == ' ') data++;
            if (strcmp(fn, "h") == 0) { lib_strcpy(out, "usage: write <path> <text>"); return; }
            if (check_dir_and_list(w, fn, out)) return;
            if (store_file_bytes(w, fn, (const unsigned char *)data, strlen(data)) == 0) lib_strcpy(out, ">> Written."); else lib_strcpy(out, "ERR: Write Failed."); 
        } else lib_strcpy(out, "usage: write <path> <text>");
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        char *fn = cmd + 4; while (*fn == ' ') fn++;
        if (strcmp(fn, "h") == 0) { lib_strcpy(out, "usage: cat <path>"); return; }
        
        // Path resolution for smart behavior
        char expanded[128]; uint32_t p_bno = 0; char p_cwd[128], leaf[20];
        terminal_expand_home_path(w, fn, expanded, sizeof(expanded));
        if (resolve_editor_target(w, expanded, &p_bno, p_cwd, leaf) == 0) {
            if (leaf[0] == '\0' || strcmp(leaf, ".") == 0) {
                list_dir_contents(p_bno, out); return;
            } else {
                extern struct Window fs_tmp_window; struct Window *ltmp = &fs_tmp_window;
                memset(ltmp, 0, sizeof(*ltmp)); ltmp->cwd_bno = p_bno;
                struct dir_block *db = load_current_dir(ltmp);
                int idx = find_entry_index(db, leaf, -1);
                if (idx >= 0 && db->entries[idx].type == 1) {
                    list_dir_contents(db->entries[idx].bno, out); return;
                }
            }
        }
        
        uint32_t sz; if (load_file_bytes(w, fn, (unsigned char *)out, OUT_BUF_SIZE - 1, &sz) == 0) out[sz] = '\0'; else lib_strcpy(out, "ERR: Cat Failed.");
    } else if (strncmp(cmd, "wget status", 11) == 0) {
        if (wget_job.request_pending || wget_job.active) {
            lib_strcpy(out, "wget: ");
            lib_strcat(out, wget_stage_name());
            lib_strcat(out, " ");
            char sz[12];
            lib_itoa(wget_job.body_len, sz);
            lib_strcat(out, sz);
            lib_strcat(out, "B");
            if (wget_job.wait_started_ms != 0) {
                lib_strcat(out, " ");
                char ms[16];
                lib_itoa((uint32_t)(sys_now() - wget_job.wait_started_ms), ms);
                lib_strcat(out, ms);
                lib_strcat(out, "ms");
            }
        } else if (wget_job.done && wget_job.success) {
            lib_strcpy(out, "wget: done ");
            lib_strcat(out, wget_job.filename);
            lib_strcat(out, " ");
            char sz[16];
            format_size_human(wget_job.body_len, sz);
            lib_strcat(out, sz);
        } else if (wget_job.done) {
            lib_strcpy(out, "wget: fail ");
            lib_strcat(out, wget_job.err[0] ? wget_job.err : "-");
        } else {
            lib_strcpy(out, "wget: idle");
        }
    } else if (strncmp(cmd, "wget ", 5) == 0) {
        char *arg1 = cmd + 5;
        while (*arg1 == ' ') arg1++;
        if (*arg1 == 'h' && (*(arg1+1) == '\0' || *(arg1+1) == ' ')) {
            lib_strcpy(out, "usage: wget <ip> <path> <file>\n"
                            "       wget http(s)://host[:port]/path [file]");
            return;
        }
        char *arg2 = 0;
        char *arg3 = 0;
        uint32_t progress_row = (w->total_rows > 0) ? (uint32_t)(w->total_rows - 1) : 0;

        if (*arg1 == '\0') { lib_strcpy(out, "ERR: wget <ip> <path> <file>"); return; }
        arg2 = strchr(arg1, ' ');
        if (arg2) {
            *arg2++ = '\0';
            while (*arg2 == ' ') arg2++;
            if (*arg2 == '\0') arg2 = 0;
        }
        if (arg2) {
            arg3 = strchr(arg2, ' ');
            if (arg3) {
                *arg3++ = '\0';
                while (*arg3 == ' ') arg3++;
                if (*arg3 == '\0') arg3 = 0;
            }
        }
        if (wget_job.active || wget_job.request_pending) { lib_strcpy(out, "ERR: wget busy."); return; }

        if (str_starts_with(arg1, "http://") || str_starts_with(arg1, "https://")) {
            char host[32];
            char path[96];
            char file[20];
            uint16_t port = 0;
            int use_tls = str_starts_with(arg1, "https://");
            if (!parse_wget_url(arg1, host, &port, path)) { lib_strcpy(out, "ERR: wget <ip> <path> <file>"); return; }
            if (path[0] != '/') { lib_strcpy(out, "ERR: wget <ip> <path> <file>"); return; }
            if (arg2 && *arg2) copy_name20(file, arg2);
            else derive_filename_from_path(path, file, sizeof(file));
            if (file[0] == '\0') { lib_strcpy(out, "ERR: wget <ip> <path> <file>"); return; }
            if (strlen(file) > 19) { lib_strcpy(out, "ERR: File Name Too Long."); return; }
            wget_queue_request(host, path, file, port, use_tls);
        } else {
            char *host = arg1;
            char *path = arg2;
            char *name = arg3;
            if (!path || !name) { lib_strcpy(out, "ERR: wget <ip> <path> <file>"); return; }
            if (*host == '\0' || *path == '\0' || *name == '\0') { lib_strcpy(out, "ERR: wget <ip> <path> <file>"); return; }
            if (strlen(name) > 19) { lib_strcpy(out, "ERR: File Name Too Long."); return; }
            wget_queue_request(host, path, name, 80, 0);
        }

        wget_job.save_pending = 1;
        wget_job.owner_win_id = w->id;
        wget_job.progress_row = progress_row;
        wget_job.target_cwd_bno = w->cwd_bno;
        lib_strcpy(wget_job.target_cwd, w->cwd);
        w->waiting_wget = 1;
        set_wget_progress_line(w, progress_row);
        out[0] = '\0';
    } else if (strncmp(cmd, "demo3d", 6) == 0) {
        int rc;
        char *spec = cmd + 6;
        while (*spec == ' ') spec++;
        if (*spec) {
            if (strcmp(spec, "cube") == 0) {
                rc = open_demo3d_cube_window();
            } else {
            int pts[8][3];
            int count = 0;
            rc = parse_demo3d_points(spec, pts, &count);
            if (rc == 0) rc = open_demo3d_window_points(pts, count);
            else if (rc == -2) { lib_strcpy(out, "ERR: Max 8 Points."); return; }
            else { lib_strcpy(out, "ERR: demo3d x,y,z x,y,z ..."); return; }
            }
        } else {
            rc = open_demo3d_window();
        }
        if (rc == 0) lib_strcpy(out, ">> 3D Demo Opened.");
        else lib_strcpy(out, "ERR: No Free Window.");
    } else if (strncmp(cmd, "frankenstein", 12) == 0) {
        int rc = open_frankenstein_window();
        if (rc == 0) lib_strcpy(out, ">> Frankenstein Opened.");
        else lib_strcpy(out, "ERR: No Free Window.");
    } else if (strncmp(cmd, "open ", 5) == 0) {
        char *fn = cmd + 5;
        while (*fn == ' ') fn++;
        if (*fn == '\0') { lib_strcpy(out, "ERR: Missing File."); return; }
        int rc = open_image_file(w, fn);
        if (rc == 0) lib_strcpy(out, ">> Image Opened.");
        else if (rc == -2) lib_strcpy(out, "ERR: Only 24-bit BMP.");
        else if (rc == -3) lib_strcpy(out, "ERR: No Free Window.");
        else lib_strcpy(out, "ERR: Open Failed.");
    } else if (strncmp(cmd, "run ", 4) == 0) {
        char *fn = cmd + 4;
        char *args = 0;
        while (*fn == ' ') fn++;
        if (*fn == '\0') { lib_strcpy(out, "ERR: Missing File."); return; }
        args = fn;
        while (*args && *args != ' ') args++;
        if (*args == ' ') {
            *args++ = '\0';
            while (*args == ' ') args++;
            if (*args == '\0') args = 0;
        } else {
            args = 0;
        }
        if (launch_app_file(w, fn, args, out, OUT_BUF_SIZE) != 0) return;
    } else if (strncmp(cmd, "gbemu", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        char *path = cmd + 5;
        char *rom = 0;
        while (*path == ' ') path++;
        if (*path == '\0') {
            rom = "lez.gb";
        } else {
            rom = path;
        }
        if (strlen(rom) < 3 || (strcmp(rom + strlen(rom) - 3, ".gb") != 0 && strcmp(rom + strlen(rom) - 4, ".gbc") != 0)) {
            lib_strcpy(out, "ERR: gbemu <rom.gb|rom.gbc>");
            return;
        }
        int rc = open_gbemu_window(w, rom);
        if (rc == 0) lib_strcpy(out, ">> GBEMU Opened.");
        else if (rc == -3) lib_strcpy(out, "ERR: No Free Window.");
        else if (rc == -2) lib_strcpy(out, "ERR: Out of memory.");
        else if (rc == -1) lib_strcpy(out, "ERR: Invalid ROM.");
        else lib_strcpy(out, "ERR: GBEMU Failed.");
    } else if (try_launch_elf_command(w, cmd, out)) {
        return;
    } else if (strncmp(cmd, "vim", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        char *fn = cmd + 3;
        while (*fn == ' ') fn++;
        if (*fn == '\0') {
            lib_printf("[vim] missing file cmd='%s'\n", cmd);
            lib_strcpy(out, "ERR: Missing File.");
            return;
        }
        lib_printf("[vim] run_command cmd='%s' fn='%s'\n", cmd, fn);
        int rc = open_text_editor(w, fn);
        if (rc == 0) lib_strcpy(out, ">> Vim Opened.");
        else if (rc == -4) lib_strcpy(out, "ERR: No Free Window.");
        else lib_strcpy(out, "ERR: Vim Failed.");
    } else if (strncmp(cmd, "asm", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        char *fn = cmd + 3;
        while (*fn == ' ') fn++;
        if (*fn == '\0') {
            lib_strcpy(out, "ERR: asm <file>");
            return;
        }
        if (run_asm_file(w, fn) == 0) {
            lib_strcpy(out, w->out_buf);
        } else if (w->out_buf[0] == '\0') {
            lib_strcpy(out, "ERR: asm failed.");
        } else {
            lib_strcpy(out, w->out_buf);
        }
    } else if (strncmp(cmd, "netsurf", 7) == 0) {
        char *url = cmd + 7; while (*url == ' ') url++;
        char launch_url[512];
        char clean_url[256];
        if (*url == '\0') url = "https://192.168.123.100/test.html";
        netsurf_normalize_target_url(url, clean_url, sizeof(clean_url));
        if (clean_url[0] == '\0' || netsurf_prepare_launch_url(clean_url, 400, 420, launch_url, sizeof(launch_url)) < 0) {
            lib_strcpy(out, "ERR: Unable to open NetSurf URL.");
            return;
        }
        int nid = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].active) {
                reset_window(&wins[i], i);
                wins[i].active = 1; wins[i].kind = WINDOW_KIND_NETSURF;
                wins[i].x = 50; wins[i].y = 50; wins[i].w = 400; wins[i].h = 420;
                lib_strcpy(wins[i].title, "NetSurf Browser");
                lib_strcpy(wins[i].ns_url, launch_url);
                lib_strcpy(wins[i].ns_target_url, clean_url);
                lib_strcpy(wins[i].ns_history[0], launch_url);
                wins[i].ns_history_count = 1; wins[i].ns_history_pos = 0;
                extern void netsurf_init_engine(struct Window *w);
                netsurf_init_engine(&wins[i]);
                extern void netsurf_begin_navigation(int win_id);
                netsurf_begin_navigation(i);
                extern void netsurf_invalidate_layout(int win_id);
                netsurf_invalidate_layout(i);
                bring_to_front(i); nid = i; break;
            }
        }
        if (nid != -1) {
            lib_strcpy(wins[nid].ns_url, launch_url);
            lib_strcpy(wins[nid].ns_target_url, clean_url);
            lib_strcpy(wins[nid].ns_history[0], launch_url);
            wins[nid].ns_history_count = 1;
            wins[nid].ns_history_pos = 0;
            extern void netsurf_begin_navigation(int win_id);
            netsurf_begin_navigation(nid);
            if (netsurf_queue_wrapped_request(clean_url, wins[nid].w, wins[nid].h, nid) < 0) {
                lib_strcpy(out, "ERR: Unable to open NetSurf URL.");
                return;
            }
        } else lib_strcpy(out, "ERR: No Free Window.");
    } else if (strncmp(cmd, "mem", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        uint32_t total, used, free_pg, m_calls, f_calls;
        mem_usage_info(&total, &used, &free_pg, &m_calls, &f_calls);
        char row[128];
        snprintf(row, sizeof(row), "RAM: total=%uKB, used=%uKB, free=%uKB\nActivity: malloc=%u, free=%u", 
                 total*4, used*4, free_pg*4, m_calls, f_calls);
        lib_strcpy(out, row);
    } else if (strncmp(cmd, "help", 4) == 0) {
        lib_strcpy(out, "Commands: ls, find, mem, df, du, mkdir, rm, touch, cd, pwd, write, cat, wget, open, run, gbemu, vim, asm, demo3d, frankenstein, netsurf, ssh, wrp, format, clear, env, export, unset, alias, unalias, source, ., echo\nType '<cmd> h' for usage.");
    } else { lib_strcpy(out, "Invalid signal."); }
}
void run_command(struct Window *w) {
    char *full_cmd = w->cmd_buf;
    w->shell_status[0] = '\0';

    // 捲動邏輯：滿了就往上推，而不是直接清空
    if (w->total_rows >= ROWS) {
        for (int i = 1; i < ROWS; i++) lib_strcpy(w->lines[i-1], w->lines[i]);
        w->total_rows = ROWS - 1;
    }

    w->executing_cmd = 1;
    w->cancel_requested = 0;

    // 指令紀錄與清除處理
    if (w->edit_len > 0) {
        if (w->hist_count == 0 || strcmp(w->history[0], full_cmd) != 0) {
            for(int i=MAX_HIST-1; i>0; i--) lib_strcpy(w->history[i], w->history[i-1]);
            lib_strcpy(w->history[0], full_cmd); 
            if(w->hist_count < MAX_HIST) w->hist_count++;
        }
    }
    if (strncmp(full_cmd, "clear", 5) == 0) {
        w->total_rows = 1; w->v_offset = 0; 
        lib_strcpy(w->lines[0], PROMPT); w->cur_col = PROMPT_LEN;
        w->executing_cmd = 0; w->submit_locked = 0;
        return;
    }

    // 執行指令
    exec_single_cmd(w, full_cmd);

    // 安全地將輸出印到螢幕 (嚴格邊界檢查)
    char *res = w->out_buf;
    int len = strlen(res);
    int pos = 0;
    int max_c = terminal_visible_cols(w);

    while(pos < len && w->total_rows < ROWS) {
        int t = 0;
        while(pos+t < len && res[pos+t] != '\n' && t < max_c) t++;
        
        if (w->total_rows < ROWS) {
            for(int i=0; i<t; i++) w->lines[w->total_rows][i] = res[pos+i];
            w->lines[w->total_rows][t] = '\0';
            w->total_rows++;
        }
        pos += (pos + t < len && res[pos+t] == '\n') ? (t + 1) : t;
    }

    terminal_scroll_to_bottom(w);
    if (!app_running) { 
        w->executing_cmd = 0; 
        w->submit_locked = 0; 
    }
}

static void handle_window_mailbox(struct Window *w) {
    char key;
    if (!window_input_pop(w, &key)) {
        w->mailbox = 0;
        return;
    }

    // SANITY CHECK: Fix the '1.6 billion' memory corruption bug
    if (w->edit_len < 0 || w->edit_len >= COLS) {
        lib_printf("[FIX] Detected corrupted edit_len=%d, resetting to 0\n", w->edit_len);
        w->edit_len = 0;
        w->cursor_pos = 0;
        w->cmd_buf[0] = '\0';
    }

    // DEBUG LOGGING
    if (w->kind == WINDOW_KIND_EDITOR) {
        editor_handle_key(w, key);
        return;
    }
    if (w->kind == WINDOW_KIND_IMAGE) {
        if (key == '+' || key == '=') resize_image_window(w, w->image_scale + 1);
        else if (key == '-') resize_image_window(w, w->image_scale - 1);
        return;
    }
    if (w->kind == WINDOW_KIND_TERMINAL && w->ssh_auth_mode) {
        if (key == 3 || key == 27) {
            terminal_cancel_ssh_auth(w);
            return;
        }
        if (key == 10 || key == '\r') {
            terminal_submit_ssh_auth(w);
            return;
        }
        if (key == 8 || key == 127) {
            if (w->ssh_auth_len > 0) {
                w->ssh_auth_len--;
                w->ssh_auth_buf[w->ssh_auth_len] = '\0';
                terminal_refresh_ssh_auth_prompt(w);
            }
            return;
        }
        if (key >= 32 && key < 127) {
            if (w->ssh_auth_len < (int)sizeof(w->ssh_auth_buf) - 1) {
                w->ssh_auth_buf[w->ssh_auth_len++] = key;
                w->ssh_auth_buf[w->ssh_auth_len] = '\0';
                terminal_refresh_ssh_auth_prompt(w);
            }
            return;
        }
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

        int q_len = (w->input_tail >= w->input_head) ? 
                    (w->input_tail - w->input_head) : 
                    (INPUT_MAILBOX_SIZE - w->input_head + w->input_tail);
        lib_printf("[DEBUG] Enter logic START, Q_len=%d, edit_len=%d\n", q_len, w->edit_len);

        int is_clear = (strncmp(w->cmd_buf, "clear", 5) == 0);
        int had_input = (w->edit_len > 0);
        
        if (had_input) {
            if (terminal_is_ssh_auth_request(w->cmd_buf)) {
                terminal_begin_ssh_auth(w);
                return;
            }
            w->submit_locked = 1;
            run_command(w);
        }
        
        if (w->waiting_wget) return;
        clear_prompt_input(w);
        if (!is_clear && !app_running && w->total_rows < ROWS) {
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
            lib_printf("[DEBUG] Storing char '%c'\n", key);
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
void create_new_task() {
    for(int i=0; i<MAX_WINDOWS; i++) if(!wins[i].active) {
        // 1. COMPLETELY ZERO OUT THE STRUCT PHYSICALLY
        memset(&wins[i], 0, sizeof(struct Window));
        
        reset_window(&wins[i], i);
        wins[i].active=1; wins[i].maximized=0; wins[i].minimized=0;
        wins[i].kind = WINDOW_KIND_TERMINAL;
        wins[i].x=120+i*40; wins[i].y=70+i*30; wins[i].w=520; wins[i].h=320;
        
        // 2. FORCE RESET CRITICAL COUNTERS TO ZERO
        wins[i].total_rows = 1;
        wins[i].cur_col = 12;
        wins[i].edit_len = 0;    // FIX: Reclaim from garbage values
        wins[i].cursor_pos = 0;
        wins[i].submit_locked = 0;
        wins[i].executing_cmd = 0;
        memset(wins[i].cmd_buf, 0, sizeof(wins[i].cmd_buf));
        lib_strcpy(wins[i].lines[0], PROMPT);

        terminal_env_bootstrap(&wins[i]);
        terminal_load_bashrc(&wins[i]);
        
        // 3. FINAL SANITY CHECK - Ensure bashrc didn't mess up state
        wins[i].edit_len = 0;
        wins[i].cursor_pos = 0;
        wins[i].cmd_buf[0] = '\0';
        
        set_window_title(&wins[i], i);
        seed_terminal_history(&wins[i]);
        bring_to_front(i); 
        active_win_idx = i;
        break;
    }
}

void draw_window(struct Window *w) {
    if (!w->active || w->minimized) return;
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    if (w->maximized) { x = 0; y = 0; ww = WIDTH; wh = DESKTOP_H; }
    for(int j=y; j<y+wh && j<DESKTOP_H; j++) { if(j<0) continue; for(int i=x; i<x+ww && i<WIDTH; i++) { if(i<0) continue; sheet_map[j*WIDTH + i] = w->id + 1; } }
    extern void draw_round_rect_fill(int,int,int,int,int,int);
    extern void draw_round_rect_wire(int,int,int,int,int,int);
    extern void draw_rect_fill(int,int,int,int,int);
    extern void draw_vertical_gradient(int,int,int,int,int,int);
    extern void draw_bevel_rect(int,int,int,int,int,int,int);
    draw_rect_fill(x + 5, y + 6, ww, wh, UI_C_SHADOW);
    draw_round_rect_fill(x, y, ww, wh, UI_RADIUS, COL_WIN_BG);
    draw_round_rect_wire(x, y, ww, wh, UI_RADIUS, COL_WIN_BRD);
    int color = (active_win_idx == w->id) ? COL_TITLE_ACT : COL_TITLE_INACT;
    int title_top = (active_win_idx == w->id) ? 3 : 2;
    int title_bot = (active_win_idx == w->id) ? 2 : 1;
    draw_round_rect_fill(x+2, y+2, ww-4, 26, UI_RADIUS-2, color); 
    draw_vertical_gradient(x + 3, y + 3, ww - 6, 10, title_top, title_bot);
    int title_max = (ww - 110) / 8;
    if (title_max < 6) title_max = 6;
    if (title_max > 60) title_max = 60;
    if (w->kind == WINDOW_KIND_TERMINAL) {
        char title[80];
        lib_strcpy(title, w->title);
        lib_strcat(title, " [x");
        char num[4];
        lib_itoa((uint32_t)terminal_font_scale(w), num);
        lib_strcat(title, num);
        lib_strcat(title, "]");
        title[title_max] = '\0';
        draw_text(x + 10, y + 6, title, UI_C_TEXT);
    } else if (w->kind == WINDOW_KIND_DEMO3D) {
        char title[80];
        char num[12];
        lib_strcpy(title, w->title);
        lib_strcat(title, " [");
        lib_itoa(demo3d_fps_value, num);
        lib_strcat(title, num);
        lib_strcat(title, " FPS]");
        title[title_max] = '\0';
        draw_text(x + 10, y + 6, title, UI_C_TEXT);
    } else {
        char title[80];
        lib_strcpy(title, w->title);
        title[title_max] = '\0';
        draw_text(x + 10, y + 6, title, UI_C_TEXT);
    }
    {
        int bx = x + ww - 34, by = y + 5;
        int hover_x = (gui_mx >= bx && gui_mx < bx + 22 && gui_my >= by && gui_my < by + 16);
        int close_fill = hover_x ? 4 : 2;
        int close_light = hover_x ? 15 : 14;
        int close_dark = hover_x ? 1 : 1;
        draw_bevel_rect(bx, by, 22, 16, close_fill, close_light, close_dark);
        draw_text(x + ww - 29, y + 7, "X", UI_C_TEXT);
    }
    {
        int bx = x + ww - 59, by = y + 5;
        int hover_x = (gui_mx >= bx && gui_mx < bx + 22 && gui_my >= by && gui_my < by + 16);
        int max_fill = hover_x ? 5 : 3;
        int max_light = hover_x ? 15 : 14;
        int max_dark = hover_x ? 1 : 1;
        draw_bevel_rect(bx, by, 22, 16, max_fill, max_light, max_dark);
        draw_text(x + ww - 54, y + 7, "M", UI_C_TEXT);
    }
    {
        int bx = x + ww - 84, by = y + 5;
        int hover_x = (gui_mx >= bx && gui_mx < bx + 22 && gui_my >= by && gui_my < by + 16);
        int min_fill = hover_x ? 4 : 1;
        int min_light = hover_x ? 15 : 14;
        int min_dark = hover_x ? 1 : 1;
        draw_bevel_rect(bx, by, 22, 16, min_fill, min_light, min_dark);
        draw_text(x + ww - 79, y + 7, "-", UI_C_TEXT);
    }
    if (w->kind == WINDOW_KIND_IMAGE) {
        int scale = w->image_scale > 0 ? w->image_scale : 1;
        int draw_w = w->image_w * scale;
        int draw_h = w->image_h * scale;
        int inner_w = ww - 20;
        int inner_h = wh - 36;
        if (inner_w < 0) inner_w = 0;
        if (inner_h < 0) inner_h = 0;
        if (draw_w > inner_w) draw_w = inner_w;
        if (draw_h > inner_h) draw_h = inner_h;
        if (draw_w > WIDTH) draw_w = WIDTH;
        if (draw_h > HEIGHT) draw_h = HEIGHT;
        memset(dither_err_r0, 0, sizeof(dither_err_r0));
        memset(dither_err_g0, 0, sizeof(dither_err_g0));
        memset(dither_err_b0, 0, sizeof(dither_err_b0));
        memset(dither_err_r1, 0, sizeof(dither_err_r1));
        memset(dither_err_g1, 0, sizeof(dither_err_g1));
        memset(dither_err_b1, 0, sizeof(dither_err_b1));
        if (scale == 1) {
            for (int py = 0; py < w->image_h; py++) {
                memset(dither_err_r1, 0, sizeof(dither_err_r1));
                memset(dither_err_g1, 0, sizeof(dither_err_g1));
                memset(dither_err_b1, 0, sizeof(dither_err_b1));
                for (int px = 0; px < w->image_w; px++) {
                    int r, g, b;
                    rgb_from_rgb565(w->image[py * IMG_MAX_W + px], &r, &g, &b);
                    r += dither_err_r0[px] >> 4;
                    g += dither_err_g0[px] >> 4;
                    b += dither_err_b0[px] >> 4;
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    if (g < 0) g = 0; else if (g > 255) g = 255;
                    if (b < 0) b = 0; else if (b > 255) b = 255;
                    int idx, qr, qg, qb;
                    quantize_rgb_to_palette(r, g, b, &idx, &qr, &qg, &qb);
                    int er = (r - qr), eg = (g - qg), eb = (b - qb);
                    dither_err_r0[px + 1] += er * 7;
                    dither_err_g0[px + 1] += eg * 7;
                    dither_err_b0[px + 1] += eb * 7;
                    dither_err_r1[px - (px > 0 ? 1 : 0)] += er * (px > 0 ? 3 : 0);
                    dither_err_g1[px - (px > 0 ? 1 : 0)] += eg * (px > 0 ? 3 : 0);
                    dither_err_b1[px - (px > 0 ? 1 : 0)] += eb * (px > 0 ? 3 : 0);
                    dither_err_r1[px] += er * 5;
                    dither_err_g1[px] += eg * 5;
                    dither_err_b1[px] += eb * 5;
                    dither_err_r1[px + 1] += er;
                    dither_err_g1[px + 1] += eg;
                    dither_err_b1[px + 1] += eb;
                    putpixel(x + 10 + px, y + 34 + py, idx);
                }
                memcpy(dither_err_r0, dither_err_r1, sizeof(dither_err_r0));
                memcpy(dither_err_g0, dither_err_g1, sizeof(dither_err_g0));
                memcpy(dither_err_b0, dither_err_b1, sizeof(dither_err_b0));
            }
        } else {
            for (int dy = 0; dy < draw_h; dy++) {
                memset(dither_err_r1, 0, sizeof(dither_err_r1));
                memset(dither_err_g1, 0, sizeof(dither_err_g1));
                memset(dither_err_b1, 0, sizeof(dither_err_b1));
                int fy = dy * 256 / scale;
                int sy0 = fy >> 8;
                int wy = fy & 0xFF;
                int sy1 = sy0 + 1;
                if (sy0 < 0) sy0 = 0;
                if (sy0 >= w->image_h) sy0 = w->image_h - 1;
                if (sy1 >= w->image_h) sy1 = w->image_h - 1;
                for (int dx = 0; dx < draw_w; dx++) {
                    int fx = dx * 256 / scale;
                    int sx0 = fx >> 8;
                    int wx = fx & 0xFF;
                    int sx1 = sx0 + 1;
                    if (sx0 < 0) sx0 = 0;
                    if (sx0 >= w->image_w) sx0 = w->image_w - 1;
                    if (sx1 >= w->image_w) sx1 = w->image_w - 1;

                    uint16_t c00 = w->image[sy0 * IMG_MAX_W + sx0];
                    uint16_t c10 = w->image[sy0 * IMG_MAX_W + sx1];
                    uint16_t c01 = w->image[sy1 * IMG_MAX_W + sx0];
                    uint16_t c11 = w->image[sy1 * IMG_MAX_W + sx1];
                    int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
                    rgb_from_rgb565(c00, &r00, &g00, &b00);
                    rgb_from_rgb565(c10, &r10, &g10, &b10);
                    rgb_from_rgb565(c01, &r01, &g01, &b01);
                    rgb_from_rgb565(c11, &r11, &g11, &b11);

                    int w00 = (256 - wx) * (256 - wy);
                    int w10 = wx * (256 - wy);
                    int w01 = (256 - wx) * wy;
                    int w11 = wx * wy;
                    int r = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11) >> 16;
                    int g = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11) >> 16;
                    int b = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11) >> 16;
                    r += dither_err_r0[dx] >> 4;
                    g += dither_err_g0[dx] >> 4;
                    b += dither_err_b0[dx] >> 4;
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    if (g < 0) g = 0; else if (g > 255) g = 255;
                    if (b < 0) b = 0; else if (b > 255) b = 255;
                    int idx, qr, qg, qb;
                    quantize_rgb_to_palette(r, g, b, &idx, &qr, &qg, &qb);
                    int er = (r - qr), eg = (g - qg), eb = (b - qb);
                    dither_err_r0[dx + 1] += er * 7;
                    dither_err_g0[dx + 1] += eg * 7;
                    dither_err_b0[dx + 1] += eb * 7;
                    dither_err_r1[dx - (dx > 0 ? 1 : 0)] += er * (dx > 0 ? 3 : 0);
                    dither_err_g1[dx - (dx > 0 ? 1 : 0)] += eg * (dx > 0 ? 3 : 0);
                    dither_err_b1[dx - (dx > 0 ? 1 : 0)] += eb * (dx > 0 ? 3 : 0);
                    dither_err_r1[dx] += er * 5;
                    dither_err_g1[dx] += eg * 5;
                    dither_err_b1[dx] += eb * 5;
                    dither_err_r1[dx + 1] += er;
                    dither_err_g1[dx + 1] += eg;
                    dither_err_b1[dx + 1] += eb;
                    putpixel(x + 10 + dx, y + 34 + dy, idx);
                }
                memcpy(dither_err_r0, dither_err_r1, sizeof(dither_err_r0));
                memcpy(dither_err_g0, dither_err_g1, sizeof(dither_err_g0));
                memcpy(dither_err_b0, dither_err_b1, sizeof(dither_err_b0));
            }
        }
        return;
    }
    if (w->kind == WINDOW_KIND_GBEMU) {
        draw_gbemu_content(w, x, y, ww, wh);
        return;
    }
    if (w->kind == WINDOW_KIND_EDITOR) {
        editor_render(w, x, y, ww, wh);
        return;
    }
    if (w->kind == WINDOW_KIND_FPS_GAME) {
        draw_fps_content(w, x, y, ww, wh);
        return;
    }
    if (w->kind == WINDOW_KIND_DEMO3D) {
        draw_demo3d_content(w, x, y, ww, wh);
        return;
    }
    if (w->kind == WINDOW_KIND_NETSURF) {
        // 導覽列 UI
        draw_rect_fill(x, y + 30, ww, 34, UI_C_PANEL);
        draw_rect_fill(x, y + 64, ww, 1, UI_C_BORDER);
        draw_bevel_rect(x + 10, y + 38, 24, 20, (w->ns_history_pos > 0 ? 3 : 1), 14, 1);
        draw_text(x + 16, y + 40, "<", 0);
        draw_bevel_rect(x + 40, y + 38, 24, 20, (w->ns_history_pos < w->ns_history_count - 1 ? 3 : 1), 14, 1);
        draw_text(x + 46, y + 40, ">", 0);
        draw_bevel_rect(x + 75, y + 38, ww - 85, 20, 0, 15, 0);
        const char *bar_url = (w->ns_target_url[0] ? w->ns_target_url : w->ns_url);
        draw_text(x + 80, y + 40, bar_url, UI_C_TEXT_DIM);
        if (w->ns_input_active) {
            int cur_x = x + 80 + strlen(bar_url) * 8;
            if (cur_x < x + ww - 10) draw_rect_fill(cur_x, y + 40, 2, 14, 0);
        }
        netsurf_render_frame(w, x, y, ww, wh);
        return;
    }
    int rv = terminal_visible_rows(w);
    int line_h = terminal_line_h(w);
    int char_w = terminal_char_w(w);
    int char_h = terminal_char_h(w);
    int scale = terminal_font_scale(w);
    int ssr = 0, ssc = 0, ser = 0, sec = 0;
    int show_sel = (w->kind == WINDOW_KIND_TERMINAL && w->has_selection);
    if (show_sel) terminal_selection_normalized(w, &ssr, &ssc, &ser, &sec);
    for (int i = 0; i < rv; i++) {
        int idx = w->v_offset + i; if (idx < w->total_rows) {
            int tc = (w->lines[idx][0] == 'd') ? COL_DIR : COL_TEXT;
            char clipped[COLS];
            int max_cols = terminal_visible_cols(w);
            int j = 0;
            while (j < max_cols && w->lines[idx][j]) {
                clipped[j] = w->lines[idx][j];
                j++;
            }
            clipped[j] = '\0';
            if (show_sel && idx >= ssr && idx <= ser) {
                int c0 = (idx == ssr) ? ssc : 0;
                int c1 = (idx == ser) ? sec : line_text_len(w->lines[idx]);
                if (c0 < 0) c0 = 0;
                if (c1 < c0) c1 = c0;
                if (c0 > max_cols) c0 = max_cols;
                if (c1 > max_cols) c1 = max_cols;
                if (c1 > c0) {
                    draw_rect_fill(x + 10 + c0 * char_w, y + 40 + (i * line_h), (c1 - c0) * char_w, char_h, UI_C_SELECTION);
                }
            }
            draw_text_scaled(x + 10, y + 40 + (i * line_h), clipped, tc, scale);
        }
    }
    if (active_win_idx == w->id && (((sys_now() / 100U) & 1U) == 0U) && w->total_rows > 0) {
        int cx = x + 10 + (w->cur_col * char_w), cy = y + 40 + ((w->total_rows - 1 - w->v_offset) * line_h);
        if (cx < x + ww - 15 && cy < y + wh - 10 && cy > y + 30) draw_rect_fill(cx, cy, char_w, char_h, COL_TEXT);
    }
    if (w->total_rows > rv) {
        int sx = x + ww - 12, sy = y + 28, sh = wh - 30;
        draw_rect_fill(sx, sy, 8, sh, UI_C_BORDER); 
        int th = (rv * sh) / w->total_rows; if (th < 15) th = 15;
        int ms = terminal_scroll_max(w);
        int ty = sy;
        if (ms > 0 && sh > th) ty = sy + (w->v_offset * (sh - th)) / ms;
        draw_rect_fill(sx, ty, 8, th, UI_C_SCROLL_THUMB);
    }
}

void draw_taskbar() {
    for(int j=DESKTOP_H; j<HEIGHT; j++) for(int i=0; i<WIDTH; i++) sheet_map[j*WIDTH + i] = TASKBAR_ID;
    extern void draw_rect_fill(int,int,int,int,int);
    extern void draw_vertical_gradient(int,int,int,int,int,int);
    extern void draw_bevel_rect(int,int,int,int,int,int,int);
    int root_hover = (gui_mx >= 8 && gui_mx < 80 && gui_my >= DESKTOP_H + 4 && gui_my < DESKTOP_H + 26);
    draw_bevel_rect(0, DESKTOP_H, WIDTH, TASKBAR_H, UI_C_PANEL, UI_C_TEXT_MUTED, UI_C_PANEL_DEEP);
    draw_vertical_gradient(1, DESKTOP_H + 1, WIDTH - 2, TASKBAR_H - 2, UI_C_PANEL_LIGHT, UI_C_PANEL_DARK);
    draw_hline(0, DESKTOP_H + 1, WIDTH, UI_C_TEXT_MUTED);
    draw_hline(0, DESKTOP_H + TASKBAR_H - 2, WIDTH, UI_C_PANEL_DEEP);
    if (root_hover) {
        draw_bevel_rect(7, DESKTOP_H + 3, 74, 24, UI_C_PANEL_LIGHT, UI_C_TEXT_MUTED, UI_C_PANEL_DEEP);
        draw_hline(8, DESKTOP_H + 3, 72, UI_C_TEXT_DIM);
    } else {
        draw_bevel_rect(8, DESKTOP_H + 4, 72, 22, UI_C_PANEL, UI_C_TEXT_MUTED, UI_C_PANEL_DEEP);
    }
    draw_text(18, DESKTOP_H + 8, "[ROOT]", UI_C_TEXT);
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i].active) {
        int tx = taskbar_button_x(i), c = UI_C_TEXT;
        extern void draw_round_rect_wire(int,int,int,int,int,int);
        int hover = (gui_mx >= tx && gui_mx < tx + TASKBAR_BTN_W && gui_my >= DESKTOP_H + 5 && gui_my < DESKTOP_H + 25);
        int by = DESKTOP_H + 5;
        int fill = UI_C_PANEL, light = UI_C_TEXT_DIM, dark = UI_C_PANEL_DEEP;
        if (active_win_idx == i) {
            fill = UI_C_PANEL_ACTIVE;
            light = UI_C_TEXT;
            dark = UI_C_PANEL_DEEP;
        }
        if (hover) {
            by -= 1;
            fill = UI_C_PANEL_HOVER;
            light = UI_C_TEXT;
            dark = UI_C_PANEL_DEEP;
        }
        draw_bevel_rect(tx, by, TASKBAR_BTN_W, 20, fill, light, dark);
        draw_round_rect_wire(tx, DESKTOP_H + 5, TASKBAR_BTN_W, 20, 5, c);
        char label[10];
        int j = 0;
        while (j < 7 && wins[i].title[j]) {
            label[j] = wins[i].title[j];
            j++;
        }
        label[j] = '\0';
        draw_text(tx + 6, DESKTOP_H + 9, label, UI_C_SHADOW);
        draw_text(tx + 5, DESKTOP_H + 8, label, c);
    }
    {
        char status[24];
        status[0] = '\0';
        if (active_win_idx >= 0 && active_win_idx < MAX_WINDOWS &&
            wins[active_win_idx].active && wins[active_win_idx].kind == WINDOW_KIND_TERMINAL &&
            wins[active_win_idx].shell_status[0]) {
            lib_strcpy(status, wins[active_win_idx].shell_status);
        } else {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (wins[i].active && wins[i].kind == WINDOW_KIND_TERMINAL && wins[i].shell_status[0]) {
                    lib_strcpy(status, wins[i].shell_status);
                    break;
                }
            }
        }
        if (status[0]) {
            status[12] = '\0';
            draw_bevel_rect(WIDTH - 176, DESKTOP_H + 4, 68, 22, UI_C_PANEL_LIGHT, UI_C_TEXT, UI_C_PANEL_DEEP);
            draw_text(WIDTH - 169, DESKTOP_H + 8, status, UI_C_SHADOW);
            draw_text(WIDTH - 170, DESKTOP_H + 8, status, UI_C_TEXT);
        }
    }
    char clock[32];
    format_clock(clock);
    int clock_px = 19 * 8;
    int clock_x = WIDTH - clock_px - 12;
    if (clock_x < 0) clock_x = 0;
    draw_bevel_rect(clock_x - 7, DESKTOP_H + 4, clock_px + 14, 22, UI_C_PANEL_LIGHT, UI_C_TEXT, UI_C_PANEL_DEEP);
    draw_text(clock_x + 1, DESKTOP_H + 8, clock, UI_C_SHADOW);
    draw_text(clock_x, DESKTOP_H + 8, clock, UI_C_TEXT);
}

void gui_task(void) {
    extern void vga_update();
    extern void virtio_input_poll();
    for(int i=0; i<MAX_WINDOWS; i++) { reset_window(&wins[i], i); z_order[i] = i; }
    lib_printf("[BOOT] gui_task start active_win_idx=%d\n", active_win_idx);
    int last_mx = gui_mx, last_my = gui_my;
    int last_blink_phase = -1;
    int last_cursor_mode = -1;
    while (1) {
        int redraw_needed = 0;
        int has_demo3d = 0;
        release_finished_app_commands();
        gui_cursor_mode = CURSOR_NORMAL;
        virtio_input_poll();
        if (active_window_valid() && wins[active_win_idx].kind == WINDOW_KIND_TERMINAL) {
            struct Window *aw = &wins[active_win_idx];
            int injected = 0;
            for (int i = 0; i < 128; i++) {
                int ch = uart_input_pop();
                if (ch < 0) break;
                window_input_push(aw, (char)ch);
                injected = 1;
            }
            if (injected) {
                aw->mailbox = 1;
                wake_terminal_worker_for_window(active_win_idx);
                redraw_needed = 1;
            }
        }
        if (gui_shortcut_new_task) { gui_shortcut_new_task = 0; create_new_task(); redraw_needed = 1; }
        if (gui_shortcut_close_task) { gui_shortcut_close_task = 0; if (active_win_idx != -1) close_window(active_win_idx); redraw_needed = 1; }
        if (gui_shortcut_switch_task) { gui_shortcut_switch_task = 0; cycle_active_window(); redraw_needed = 1; }

        // --- NetSurf Input Dispatch START ---
        if (active_window_valid()) {
            struct Window *aw = &wins[active_win_idx];
            if (aw->kind == WINDOW_KIND_NETSURF) {
                extern void netsurf_handle_input(struct Window *w, int mx, int my, int clicked);
                netsurf_handle_input(aw, gui_mx, gui_my, gui_clicked);
                if (aw->ns_input_active) gui_key = 0; // Hijack keys for URL bar
            }
        }
        // --- NetSurf Input Dispatch END ---

        if (gui_click_pending) {
            gui_click_pending = 0; 
            redraw_needed = 1;
            int local_mx = gui_mx, local_my = gui_my;
            if (local_mx >= 0 && local_mx < WIDTH && local_my >= 0 && local_my < HEIGHT) {
                int tid = sheet_map[local_my * WIDTH + local_mx];
                if (tid == TASKBAR_ID) {
                    if (local_mx < 100) create_new_task();
                    else { for(int i=0; i<MAX_WINDOWS; i++) { int tx = taskbar_button_x(i); if(wins[i].active && local_mx >= tx && local_mx < tx + TASKBAR_BTN_W) { bring_to_front(i); wins[i].minimized = 0; break; } } }
                } else if (tid > 0 && tid <= MAX_WINDOWS) {
                    int idx = tid - 1; bring_to_front(idx);
                    struct Window *w = &wins[idx]; int ww = w->maximized ? WIDTH : w->w;
                    int wx = w->maximized ? 0 : w->x, wy = w->maximized ? 0 : w->y;
                    int resize_dir = hit_resize_zone(w, local_mx, local_my);
                    if (resize_dir != RESIZE_NONE) {
                        gui_cursor_mode = cursor_mode_for_resize_dir(resize_dir);
                        begin_resize(w, resize_dir, local_mx, local_my);
                    } else if (local_my < wy + 30) {
                        if (local_mx > wx + ww - 40) { close_window(idx); }
                        else if (local_mx > wx + ww - 75) {
                            if(!w->maximized) {
                                w->prev_x=w->x; w->prev_y=w->y; w->prev_w=w->w; w->prev_h=w->h; w->maximized=1;
                                w->dragging = 0; w->scroll_dragging = 0; w->resizing = 0; w->resize_dir = RESIZE_NONE;
                                if (w->kind == WINDOW_KIND_NETSURF) { w->v_offset = 0; w->ns_h_offset = 0; w->ns_input_active = 0; w->ns_resize_pending = 1; w->ns_resize_last_ms = sys_now(); extern void netsurf_invalidate_layout(int win_id); netsurf_invalidate_layout(idx); }
                            } else {
                                w->x=w->prev_x; w->y=w->prev_y; w->w=w->prev_w; w->h=w->prev_h; w->maximized=0;
                                w->dragging = 0; w->scroll_dragging = 0; w->resizing = 0; w->resize_dir = RESIZE_NONE;
                                if (w->kind == WINDOW_KIND_NETSURF) { w->v_offset = 0; w->ns_h_offset = 0; w->ns_input_active = 0; w->ns_resize_pending = 1; w->ns_resize_last_ms = sys_now(); extern void netsurf_invalidate_layout(int win_id); netsurf_invalidate_layout(idx); }
                            }
                        } else if (local_mx > wx + ww - 110) { w->minimized = 1; active_win_idx = -1; }
                        else {
                            static uint32_t last_click_ms = 0;
                            static int last_click_idx = -1;
                            uint32_t now = sys_now();
                            if (last_click_idx == idx && (now - last_click_ms) < 400) {
                                if (!w->maximized) {
                                    w->prev_x = w->x; w->prev_y = w->y; w->prev_w = w->w; w->prev_h = w->h; w->maximized = 1;
                                    w->dragging = 0; w->scroll_dragging = 0; w->resizing = 0; w->resize_dir = RESIZE_NONE;
                                    if (w->kind == WINDOW_KIND_NETSURF) { w->v_offset = 0; w->ns_h_offset = 0; w->ns_input_active = 0; w->ns_resize_pending = 1; w->ns_resize_last_ms = sys_now(); extern void netsurf_invalidate_layout(int win_id); netsurf_invalidate_layout(idx); }
                                } else {
                                    w->x = w->prev_x; w->y = w->prev_y; w->w = w->prev_w; w->h = w->prev_h; w->maximized = 0;
                                    w->dragging = 0; w->scroll_dragging = 0; w->resizing = 0; w->resize_dir = RESIZE_NONE;
                                    if (w->kind == WINDOW_KIND_NETSURF) { w->v_offset = 0; w->ns_h_offset = 0; w->ns_input_active = 0; w->ns_resize_pending = 1; w->ns_resize_last_ms = sys_now(); extern void netsurf_invalidate_layout(int win_id); netsurf_invalidate_layout(idx); }
                                }
                                last_click_ms = 0;
                                w->dragging = 0;
                            } else {
                                last_click_ms = now;
                                last_click_idx = idx;
                                if (!w->maximized) { w->dragging = 1; w->drag_off_x = local_mx - w->x; w->drag_off_y = local_my - w->y; }
                            }
                        }
                    } else if (local_mx > wx + ww - 15) { w->scroll_dragging = 1; }
                    else if (w->kind == WINDOW_KIND_DEMO3D) {
                        w->demo_dragging = 1;
                        w->demo_drag_last_mx = local_mx;
                        w->demo_drag_last_my = local_my;
                        w->demo_auto_spin = 0;
                    } else if (w->kind == WINDOW_KIND_TERMINAL) {
                        int row, col;
                        if (terminal_mouse_to_cell(w, local_mx, local_my, &row, &col)) {
                            w->selecting = 1;
                            w->has_selection = 1;
                            w->sel_start_row = w->sel_end_row = row;
                            w->sel_start_col = w->sel_end_col = col;
                        } else {
                            terminal_clear_selection(w);
                        }
                    }
                } else active_win_idx = -1;
            }
        }
        if (gui_right_click_pending) {
            gui_right_click_pending = 0;
            redraw_needed = 1;
            if (active_window_valid()) {
                struct Window *aw = &wins[active_win_idx];
                if (aw->kind == WINDOW_KIND_TERMINAL) {
                    terminal_paste_clipboard(aw);
                }
            }
        }
        if (!gui_clicked) {
            for(int i=0; i<MAX_WINDOWS; i++) {
                if (wins[i].kind == WINDOW_KIND_NETSURF && wins[i].ns_resize_pending) {
                    if ((uint32_t)(sys_now() - wins[i].ns_resize_last_ms) >= 120U) {
                        extern int netsurf_refresh_current_view(int win_id);
                        netsurf_refresh_current_view(i);
                        wins[i].ns_resize_pending = 0;
                    }
                }
                if (wins[i].kind == WINDOW_KIND_TERMINAL && wins[i].selecting && wins[i].has_selection) {
                    terminal_copy_selection(&wins[i]);
                }
                wins[i].dragging = 0;
                wins[i].scroll_dragging = 0;
                wins[i].demo_dragging = 0;
                wins[i].resizing = 0;
                wins[i].resize_dir = RESIZE_NONE;
                wins[i].selecting = 0;
            }
        }
        else if (active_window_valid()) {
            struct Window *aw = &wins[active_win_idx];
            if (aw->resizing) {
                gui_cursor_mode = cursor_mode_for_resize_dir(aw->resize_dir);
                apply_resize(aw, gui_mx, gui_my);
                redraw_needed = 1;
            } else if (aw->dragging) {
                aw->x = clamp_int(gui_mx - aw->drag_off_x, 0, WIDTH - aw->w);
                aw->y = clamp_int(gui_my - aw->drag_off_y, 0, DESKTOP_H - aw->h);
                redraw_needed = 1;
            }
            else if (aw->scroll_dragging) {
                int wy = aw->maximized ? 0 : aw->y, wh = aw->maximized ? DESKTOP_H : aw->h;
                int sy = wy + 28, sh = wh - 30;
                int ms = terminal_scroll_max(aw);
                if (ms > 0 && sh > 0) {
                    aw->v_offset = ((gui_my - sy) * ms) / sh;
                    terminal_clamp_v_offset(aw);
                }
                redraw_needed = 1;
            } else if (aw->kind == WINDOW_KIND_DEMO3D && aw->demo_dragging) {
                int dx = gui_mx - aw->demo_drag_last_mx;
                int dy = gui_my - aw->demo_drag_last_my;
                if (dx != 0 || dy != 0) {
                    aw->demo_angle = (aw->demo_angle - dx) % 360;
                    if (aw->demo_angle < 0) aw->demo_angle += 360;
                    aw->demo_pitch = (aw->demo_pitch + dy) % 360;
                    if (aw->demo_pitch < 0) aw->demo_pitch += 360;
                    aw->demo_drag_last_mx = gui_mx;
                    aw->demo_drag_last_my = gui_my;
                    redraw_needed = 1;
                }
            } else if (aw->kind == WINDOW_KIND_TERMINAL && aw->selecting) {
                int row, col;
                if (terminal_mouse_to_cell(aw, gui_mx, gui_my, &row, &col)) {
                    aw->sel_end_row = row;
                    aw->sel_end_col = col;
                    redraw_needed = 1;
                }
            }
        }
        if (!active_window_valid()) {
            active_win_idx = -1;
        }
        if (gui_cursor_mode == CURSOR_NORMAL && !gui_clicked) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                int idx = z_order[i];
                if (idx < 0 || idx >= MAX_WINDOWS) continue;
                if (!wins[idx].active || wins[idx].minimized) continue;
                int dir = hit_resize_zone(&wins[idx], gui_mx, gui_my);
                if (dir != RESIZE_NONE) {
                    gui_cursor_mode = cursor_mode_for_resize_dir(dir);
                    break;
                }
            }
        }
        if (gui_wheel != 0 && active_window_valid()) {
            struct Window *aw = &wins[active_win_idx];
            if (aw->kind == WINDOW_KIND_IMAGE) {
                if (gui_wheel > 0) resize_image_window(aw, aw->image_scale + gui_wheel);
                else if (gui_wheel < 0) resize_image_window(aw, aw->image_scale + gui_wheel);
            } else if (aw->kind == WINDOW_KIND_DEMO3D && !gui_ctrl_pressed) {
                if (gui_wheel > 0) aw->demo_dist -= 20;
                else if (gui_wheel < 0) aw->demo_dist += 20;
                if (aw->demo_dist < 120) aw->demo_dist = 120;
                if (aw->demo_dist > 2000) aw->demo_dist = 2000;
                gui_wheel = 0;
            } else if (aw->kind == WINDOW_KIND_TERMINAL && gui_ctrl_pressed) {
                if (gui_wheel > 0) resize_terminal_font(aw, aw->term_font_scale + 1);
                else if (gui_wheel < 0) resize_terminal_font(aw, aw->term_font_scale - 1);
            } else {
                aw->v_offset -= gui_wheel;
                terminal_clamp_v_offset(aw);
            }
            gui_wheel = 0;
            redraw_needed = 1;
        }
        if (gui_key != 0 && active_window_valid()) {
            if (wins[active_win_idx].kind == WINDOW_KIND_DEMO3D) {
                handle_demo3d_input(&wins[active_win_idx], &gui_key);
            } else if (wins[active_win_idx].kind == WINDOW_KIND_FPS_GAME) {
                handle_fps_input(&wins[active_win_idx], &gui_key, &redraw_needed);
            } else if (wins[active_win_idx].kind == WINDOW_KIND_GBEMU) {
                handle_gbemu_input(&wins[active_win_idx], &gui_key);
            } else if (wins[active_win_idx].kind == WINDOW_KIND_EDITOR) {
                window_input_push(&wins[active_win_idx], gui_key);
                wins[active_win_idx].mailbox = 1;
            }
            if (gui_key == 3 && wins[active_win_idx].kind == WINDOW_KIND_TERMINAL) {
                if (wins[active_win_idx].has_selection) {
                    terminal_copy_selection(&wins[active_win_idx]);
                } else if (wins[active_win_idx].executing_cmd || wins[active_win_idx].waiting_wget) {
                    broadcast_ctrl_c();
                }
                else {
                    wins[active_win_idx].submit_locked = 0;
                    wins[active_win_idx].cancel_requested = 0;
                    clear_window_input_queue(&wins[active_win_idx]);
                    clear_prompt_input(&wins[active_win_idx]);
                    if (wins[active_win_idx].total_rows > 0) redraw_prompt_line(&wins[active_win_idx], wins[active_win_idx].total_rows - 1);
                }
                gui_key = 0;
            } else if (wins[active_win_idx].kind == WINDOW_KIND_TERMINAL && gui_ctrl_pressed &&
                       (gui_key == 'v' || gui_key == 'V')) {
                terminal_paste_clipboard(&wins[active_win_idx]);
                gui_key = 0;
            } else if (wins[active_win_idx].kind == WINDOW_KIND_TERMINAL && gui_ctrl_pressed &&
                       (gui_key == '+' || gui_key == '=' || gui_key == '-' || gui_key == '0')) {
                if (gui_key == '+' || gui_key == '=') resize_terminal_font(&wins[active_win_idx], wins[active_win_idx].term_font_scale + 1);
                else if (gui_key == '-') resize_terminal_font(&wins[active_win_idx], wins[active_win_idx].term_font_scale - 1);
                else resize_terminal_font(&wins[active_win_idx], 1);
                gui_key = 0;
            } else {
                if (wins[active_win_idx].kind == WINDOW_KIND_TERMINAL) {
                    window_input_push(&wins[active_win_idx], gui_key);
                    wins[active_win_idx].mailbox = 1;
                    wake_terminal_worker_for_window(active_win_idx);
                } else if (wins[active_win_idx].kind == WINDOW_KIND_EDITOR) {
                    wake_terminal_worker_for_window(active_win_idx);
                }
                gui_key = 0;
            }
            redraw_needed = 1;
        }
        else if (gui_key != 0 && !active_window_valid()) { gui_key = 0; }
        if ((wget_job.active || wget_job.request_pending || (wget_job.done && wget_job.success && wget_job.save_pending)) &&
            wget_job.owner_win_id >= 0 && wget_job.owner_win_id < MAX_WINDOWS &&
            wins[wget_job.owner_win_id].active && wins[wget_job.owner_win_id].kind == WINDOW_KIND_TERMINAL &&
            wget_job.progress_row < ROWS && wget_progress_dirty()) {
            set_wget_progress_line(&wins[wget_job.owner_win_id], wget_job.progress_row);
            redraw_needed = 1;
        }
        if (gbemu_has_active_window()) {
            gbemu_sync_gamepad();
            if (gbemu_tick_active(50000)) redraw_needed = 1;
        }
        if (gui_mx != last_mx || gui_my != last_my) {
            redraw_needed = 1;
            last_mx = gui_mx;
            last_my = gui_my;
        }
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wins[i].active && !wins[i].minimized && wins[i].kind == WINDOW_KIND_DEMO3D) {
                has_demo3d = 1;
                break;
            }
        }
        if (has_demo3d) {
            uint32_t now = sys_now();
            if (demo3d_last_frame_ms == 0 || now - demo3d_last_frame_ms >= 16U) {
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (!wins[i].active || wins[i].minimized || wins[i].kind != WINDOW_KIND_DEMO3D) continue;
                    if (wins[i].demo_auto_spin) {
                        wins[i].demo_angle = (wins[i].demo_angle + 3) % 360;
                    }
                }
                demo3d_last_frame_ms = now;
                demo3d_fps_frames++;
                if (demo3d_fps_last_ms == 0) demo3d_fps_last_ms = now;
                if (now - demo3d_fps_last_ms >= 1000U) {
                    uint32_t elapsed = now - demo3d_fps_last_ms;
                    if (elapsed == 0) elapsed = 1;
                    demo3d_fps_value = (demo3d_fps_frames * 1000U) / elapsed;
                    demo3d_fps_frames = 0;
                    demo3d_fps_last_ms = now;
                }
                redraw_needed = 1;
            }
        }
        {
            uint32_t blink_now = sys_now();
            int blink_phase = (int)((blink_now / 100U) & 1U);
            if (blink_phase != last_blink_phase) {
                redraw_needed = 1;
                last_blink_phase = blink_phase;
            }
        }
        if (gui_cursor_mode != last_cursor_mode) {
            redraw_needed = 1;
            last_cursor_mode = gui_cursor_mode;
        }
        if (redraw_needed || (gui_redraw_needed && any_window_active())) {
            static int boot_redraw_logged = 0;
            if (!boot_redraw_logged) boot_redraw_logged = 1;
            gui_redraw_needed = 0;
            extern void draw_rect_fill(int,int,int,int,int);
            extern void draw_vertical_gradient(int,int,int,int,int,int);

            // 優化：如果 GameBoy 視窗在跑，不畫背景梯度，改用簡單的填色或直接跳過
            if (!gbemu_has_active_window()) {
                draw_vertical_gradient(0, 0, WIDTH, DESKTOP_H, UI_C_DESKTOP, UI_C_PANEL_DARK);
            } else {
                // 如果需要背景，只畫工作列以外的部分，或者乾脆不畫（保留上一幀內容）
                // 這裡我們只填色，不跑梯度計算
                draw_rect_fill(0, 0, WIDTH, DESKTOP_H, UI_C_DESKTOP);
            }

            draw_rect_fill(0, DESKTOP_H, WIDTH, TASKBAR_H, UI_C_DESKTOP);
            memset(sheet_map, 0, sizeof(sheet_map));
            for(int i=0; i<MAX_WINDOWS; i++) draw_window(&wins[z_order[i]]);
            draw_taskbar(); draw_cursor(gui_mx, gui_my, 14, gui_cursor_mode); vga_update();
        }
        lib_delay(3); if (need_resched) { need_resched = 0; task_os(); }
    }
}
void network_task(void) {
    extern void virtio_net_rx_loop2();
    extern int virtio_net_has_pending_irq();
    extern int virtio_net_has_rx_ready();
    extern int virtio_net_rx_pending_count();
    lib_printf("[BOOT] network_task start\n");
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
            lib_printf("[NET_TASK] request pending, kicking wget\n");
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
void user_init() {
    for (int i = 0; i < TERMINAL_WORKERS; i++) terminal_worker_task_ids[i] = -1;
    gui_task_id = task_create(&gui_task, 0, 1);
    for(int i=0; i<TERMINAL_WORKERS; i++) terminal_worker_task_ids[i] = task_create(&terminal_worker_task, 0, 1);
    network_task_id = task_create(&network_task, 1, 1);
    app_task_id = task_create(&app_bootstrap, 0, 1);
}
