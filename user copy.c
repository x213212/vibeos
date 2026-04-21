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
int app_owner_win_id = -1;
unsigned char file_io_buf[WGET_MAX_FILE_SIZE];
int gui_cursor_mode = CURSOR_NORMAL;


void lib_strcpy(char *dst, const char *src) { while ((*dst++ = *src++)); }
void lib_strcat(char *dst, const char *src) { while (*dst) dst++; while ((*dst++ = *src++)); }
void lib_strncat(char *dst, const char *src, int n) { while (*dst) dst++; for (int i=0; i<n && src[i]; i++) { if(src[i]>=32 && src[i]<=126) *dst++=src[i]; else break; } *dst = '\0'; }
int strcmp(const char *s1, const char *s2) { while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(unsigned char *)s1 - *(unsigned char *)s2; }
char* strchr(const char *s, int c) { while (*s) { if (*s == (char)c) return (char *)s; s++; } return 0; }
extern char* strstr(const char* haystack, const char* needle);
void lib_itoa(uint32_t n, char *s) { char tmp[12]; int i = 0; if (n == 0) tmp[i++] = '0'; while (n > 0) { tmp[i++] = (n % 10) + '0'; n /= 10; } int j = 0; while (i > 0) s[j++] = tmp[--i]; s[j] = '\0'; }

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

void copy_name20(char *dst, const char *src) {
    int i = 0;
    for (; i < 19 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void clear_window_input_queue(struct Window *w) {
    if (!w) return;
    w->input_head = w->input_tail;
    w->mailbox = 0;
}

struct Window wins[MAX_WINDOWS];
static void clear_prompt_input(struct Window *w);
void redraw_prompt_line(struct Window *w, int row);
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

static int palette_index_from_rgb(unsigned char r, unsigned char g, unsigned char b) {
    int ri = (r * 5) / 255;
    int gi = (g * 5) / 255;
    int bi = (b * 5) / 255;
    return 16 + (ri * 36) + (gi * 6) + bi;
}

static void quantize_rgb_to_palette(int r, int g, int b, int *idx, int *qr, int *qg, int *qb) {
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

static uint16_t rgb565_from_rgb(unsigned char r, unsigned char g, unsigned char b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void rgb_from_rgb565(uint16_t c, int *r, int *g, int *b) {
    *r = ((c >> 11) & 0x1F) * 255 / 31;
    *g = ((c >> 5) & 0x3F) * 255 / 63;
    *b = (c & 0x1F) * 255 / 31;
}

static int dither_err_r0[WIDTH + 2], dither_err_g0[WIDTH + 2], dither_err_b0[WIDTH + 2];
static int dither_err_r1[WIDTH + 2], dither_err_g1[WIDTH + 2], dither_err_b1[WIDTH + 2];

void redraw_prompt_line(struct Window *w, int row) {
    if (row < 0 || row >= ROWS) return;
    lib_strcpy(w->lines[row], PROMPT);
    lib_strcat(w->lines[row], w->cmd_buf);
    w->cur_col = PROMPT_LEN + w->cursor_pos;
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

static void seed_terminal_history(struct Window *w) {
    if (!w || w->kind != WINDOW_KIND_TERMINAL) return;
    if (w->hist_count > 0) return;
    lib_strcpy(w->history[0], "wget https://192.168.123.100/test.bin test.bin");
    w->hist_count = 1;
    w->hist_idx = -1;
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
           strcmp(cmd, "cat") == 0 ||
           strcmp(cmd, "open") == 0 ||
           strcmp(cmd, "run") == 0 ||
           strcmp(cmd, "write") == 0 ||
           strcmp(cmd, "mkdir") == 0 ||
           strcmp(cmd, "rm") == 0 ||
           strcmp(cmd, "touch") == 0 ||
           strcmp(cmd, "vim") == 0;
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

static void tab_complete_command(struct Window *w, int row, int token_start, int token_end) {
    static const char *cmds[] = {
        "pwd", "format", "cd", "ls", "mkdir", "rm", "touch", "write", "cat", "wget", "open", "run", "demo3d", "help", "clear"
    };
    char prefix[COLS];
    int plen = token_end - token_start;
    if (plen < 0) plen = 0;
    if (plen >= COLS) plen = COLS - 1;
    for (int i = 0; i < plen; i++) prefix[i] = w->cmd_buf[token_start + i];
    prefix[plen] = '\0';

    int matches = 0;
    const char *best = 0;
    char common[COLS];
    common[0] = '\0';
    for (unsigned int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (!str_starts_with(cmds[i], prefix)) continue;
        if (matches == 0) {
            lib_strcpy(common, cmds[i]);
            best = cmds[i];
        } else {
            int j = 0;
            while (common[j] && cmds[i][j] && common[j] == cmds[i][j]) j++;
            common[j] = '\0';
        }
        matches++;
    }
    if (matches == 0) return;
    apply_completion(w, row, token_start, token_end, (matches == 1) ? best : common, matches == 1);
}

static void tab_complete_name(struct Window *w, int row, int token_start, int token_end, int type_filter) {
    struct dir_block *db = load_current_dir(w);
    if (!db) return;
    char prefix[24];
    int plen = token_end - token_start;
    if (plen < 0) plen = 0;
    if (plen > 19) plen = 19;
    for (int i = 0; i < plen; i++) prefix[i] = w->cmd_buf[token_start + i];
    prefix[plen] = '\0';
    if (strchr(prefix, '/')) return;

    int matches = 0;
    char common[20];
    char best[20];
    common[0] = '\0';
    best[0] = '\0';
    for (int i = 0; i < 13; i++) {
        if (!db->entries[i].name[0]) continue;
        if (type_filter != -1 && db->entries[i].type != type_filter) continue;
        if (!str_starts_with(db->entries[i].name, prefix)) continue;
        if (matches == 0) {
            lib_strcpy(common, db->entries[i].name);
            lib_strcpy(best, db->entries[i].name);
        } else {
            int j = 0;
            while (common[j] && db->entries[i].name[j] && common[j] == db->entries[i].name[j]) j++;
            common[j] = '\0';
        }
        matches++;
    }
    if (matches == 0) return;
    apply_completion(w, row, token_start, token_end, (matches == 1) ? best : common, matches == 1);
}

static void try_tab_complete(struct Window *w, int row) {
    if (w->cursor_pos < 0 || w->cursor_pos > w->edit_len) return;
    int token_start = w->cursor_pos;
    while (token_start > 0 && w->cmd_buf[token_start - 1] != ' ') token_start--;
    int token_end = w->cursor_pos;
    while (token_end < w->edit_len && w->cmd_buf[token_end] != ' ') token_end++;

    int first_end = 0;
    while (first_end < w->edit_len && w->cmd_buf[first_end] != ' ') first_end++;
    if (token_start < first_end) {
        tab_complete_command(w, row, token_start, token_end);
        return;
    }

    char cmd[16];
    int clen = first_end;
    if (clen > 15) clen = 15;
    for (int i = 0; i < clen; i++) cmd[i] = w->cmd_buf[i];
    cmd[clen] = '\0';
    if (!is_name_completion_cmd(cmd)) return;
    tab_complete_name(w, row, token_start, token_end, strcmp(cmd, "cd") == 0 ? 1 : -1);
}

int find_entry_index(struct dir_block *db, const char *name, int type_filter) {
    for (int i = 0; i < 13; i++) {
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
        lib_strcpy(out, "ERR: App Busy.");
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


void reset_window(struct Window *w, int idx) {
    memset(w, 0, sizeof(*w));
    w->id = idx;
    w->cwd_bno = 1;
    w->term_font_scale = 1;
    lib_strcpy(w->cwd, "/root");
}

void close_window(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
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

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void format_clock(char *out) {
    unsigned int total = get_wall_clock_seconds();
    unsigned int sec = total % 60U;
    unsigned int min = (total / 60U) % 60U;
    unsigned int hour = (total / 3600U) % 24U;
    out[0] = '0' + (hour / 10U);
    out[1] = '0' + (hour % 10U);
    out[2] = ':';
    out[3] = '0' + (min / 10U);
    out[4] = '0' + (min % 10U);
    out[5] = ':';
    out[6] = '0' + (sec / 10U);
    out[7] = '0' + (sec % 10U);
    out[8] = '\0';
}

uint32_t current_fs_time(void) {
    return get_wall_clock_seconds();
}

static void format_hms(uint32_t total, char *out) {
    total %= 86400U;
    unsigned int sec = total % 60U;
    unsigned int min = (total / 60U) % 60U;
    unsigned int hour = (total / 3600U) % 24U;
    out[0] = '0' + (hour / 10U);
    out[1] = '0' + (hour % 10U);
    out[2] = ':';
    out[3] = '0' + (min / 10U);
    out[4] = '0' + (min % 10U);
    out[5] = ':';
    out[6] = '0' + (sec / 10U);
    out[7] = '0' + (sec % 10U);
    out[8] = '\0';
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

static int decode_bmp_to_window(struct Window *w, const unsigned char *buf, uint32_t size) {
    if (size < sizeof(struct bmp_file_header) + sizeof(struct bmp_info_header)) return -1;
    const struct bmp_file_header *fh = (const struct bmp_file_header *)buf;
    const struct bmp_info_header *ih = (const struct bmp_info_header *)(buf + sizeof(struct bmp_file_header));
    if (fh->type != 0x4D42) return -1;
    if (ih->size < sizeof(struct bmp_info_header)) return -1;
    if (ih->planes != 1 || ih->bit_count != 24 || ih->compression != 0) return -1;
    if (ih->width <= 0 || ih->height == 0) return -1;

    int src_w = ih->width;
    int src_h = (ih->height > 0) ? ih->height : -ih->height;
    int draw_w = (src_w > IMG_MAX_W) ? IMG_MAX_W : src_w;
    int draw_h = (src_h > IMG_MAX_H) ? IMG_MAX_H : src_h;
    uint32_t row_stride = (uint32_t)(((src_w * 3) + 3) & ~3);
    if (fh->off_bits >= size) return -1;
    if (fh->off_bits + row_stride * (uint32_t)src_h > size) return -1;

    memset(w->image, 0, sizeof(w->image));
    for (int y = 0; y < draw_h; y++) {
        int src_y = (ih->height > 0) ? (src_h - 1 - y) : y;
        const unsigned char *row = buf + fh->off_bits + row_stride * (uint32_t)src_y;
        for (int x = 0; x < draw_w; x++) {
            const unsigned char *px = row + x * 3;
            w->image[y * IMG_MAX_W + x] = rgb565_from_rgb(px[2], px[1], px[0]);
        }
    }

    w->kind = WINDOW_KIND_IMAGE;
    w->image_w = draw_w;
    w->image_h = draw_h;
    w->image_scale = 1;
    w->w = draw_w + 20;
    if (w->w < 220) w->w = 220;
    w->h = draw_h + 46;
    if (w->h < 140) w->h = 140;
    return 0;
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
        if (decode_bmp_to_window(&wins[i], file_io_buf, size) != 0) {
            reset_window(&wins[i], i);
            return -2;
        }
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
    int rc = load_file_bytes(term, name, file_io_buf, sizeof(file_io_buf), &size);
    if (rc == -3) return -4;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].active) continue;
        reset_window(&wins[i], i);
        wins[i].active = 1;
        wins[i].minimized = 0;
        wins[i].kind = WINDOW_KIND_EDITOR;
        wins[i].maximized = 1;
        wins[i].x = 0;
        wins[i].y = 0;
        wins[i].w = WIDTH;
        wins[i].h = DESKTOP_H;
        copy_name20(wins[i].editor_name, name);
        lib_strcpy(wins[i].editor_cwd, term->cwd);
        wins[i].cwd_bno = term->cwd_bno;
        if (rc == 0) editor_load_bytes(&wins[i], file_io_buf, size);
        else editor_clear(&wins[i]);
        lib_strcpy(wins[i].title, "Vim: ");
        lib_strncat(wins[i].title, name, 15);
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

void exec_single_cmd(struct Window *w, char *cmd) {
    char *out = w->out_buf; out[0] = '\0';
    if (strncmp(cmd, "pwd", 3) == 0) { lib_strcpy(out, w->cwd); }
    else if (strncmp(cmd, "format", 6) == 0) {
        fs_format_root(w);
        fs_reset_window_cwd(w);
        lib_strcpy(out, ">> Matrix Rebuilt.");
    } else if (strncmp(cmd, "cd", 2) == 0) {
        char *target = cmd + 2;
        while (*target == ' ') target++;
        if (*target == '\0' || strcmp(target, "/root") == 0 || strcmp(target, "/") == 0) {
            w->cwd_bno = 1;
            lib_strcpy(w->cwd, "/root");
            return;
        }
        if (strcmp(target, "..") == 0) {
            struct dir_block *db = load_current_dir(w);
            if (!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
            w->cwd_bno = db->parent_bno ? db->parent_bno : 1;
            if (w->cwd_bno == 1) lib_strcpy(w->cwd, "/root");
            else path_set_parent(w->cwd);
            return;
        }
        struct dir_block *db = load_current_dir(w);
        if (!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
        for (int i = 0; i < 13; i++) {
            if (db->entries[i].name[0] && db->entries[i].type == 1 && strcmp(db->entries[i].name, target) == 0) {
                w->cwd_bno = db->entries[i].bno;
                if (strcmp(w->cwd, "/root") == 0) lib_strcpy(w->cwd, "/root");
                path_set_child(w->cwd, db->entries[i].name);
                return;
            }
        }
        lib_strcpy(out, "ERR: Dir Not Found.");
    } else if (strncmp(cmd, "ls", 2) == 0) {
        int all = (strstr(cmd, "-all") != 0);
        struct dir_block *db = load_current_dir(w);
        if(!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
        for(int i=0; i<13; i++) if(db->entries[i].name[0]) {
            if(all) {
                char ms[5];
                char sz[16];
                char ts[10];
                mode_to_str(db->entries[i].mode, db->entries[i].type, ms);
                format_size_human(db->entries[i].size, sz);
                format_hms(db->entries[i].mtime, ts);
                lib_strcat(out, ms);
                lib_strcat(out, " ");
                lib_strcat(out, sz);
                lib_strcat(out, " ");
                lib_strcat(out, ts);
                lib_strcat(out, " ");
            }
            else { lib_strcat(out, (db->entries[i].type==1)?"d ":"f "); }
            lib_strncat(out, db->entries[i].name, 19); lib_strcat(out, "\n");
        }
        if (out[0] == '\0') lib_strcpy(out, "(empty)");
    } else if (strncmp(cmd, "mkdir ", 6) == 0) {
        char *dn = cmd+6; struct dir_block *db = load_current_dir(w);
        if (!db) { lib_strcpy(out, "ERR: Run 'format'."); return; }
        while (*dn == ' ') dn++;
        if (*dn == '\0') { lib_strcpy(out, "ERR: Missing Dir Name."); return; }
        if (strlen(dn) > 19) { lib_strcpy(out, "ERR: Dir Name Too Long."); return; }
        if (find_entry_index(db, dn, -1) != -1) { lib_strcpy(out, "ERR: Already Exists."); return; }
        for(int i=0; i<13; i++) if(db->entries[i].name[0]==0) {
            uint32_t nb = balloc();
            memset(&db->entries[i], 0, sizeof(db->entries[i]));
            copy_name20(db->entries[i].name, dn);
            db->entries[i].bno = nb;
            db->entries[i].size = 0;
            db->entries[i].ctime = current_fs_time();
            db->entries[i].mtime = db->entries[i].ctime;
            db->entries[i].type = 1;
            db->entries[i].mode = 7;
            virtio_disk_rw(&w->local_b, 1);

            struct blk nb_blk;
            memset(&nb_blk, 0, sizeof(nb_blk));
            nb_blk.blockno = nb;
            struct dir_block *nd = (struct dir_block *)nb_blk.data;
            nd->magic = FS_MAGIC;
            nd->parent_bno = w->cwd_bno;
            virtio_disk_rw(&nb_blk, 1);
            lib_strcpy(out, ">> Node Created.");
            return;
        }
        lib_strcpy(out, "ERR: Dir Full.");
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        char *name = cmd + 3;
        while (*name == ' ') name++;
        if (*name == '\0') { lib_strcpy(out, "ERR: Missing Name."); return; }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, "/") == 0) {
            lib_strcpy(out, "ERR: Invalid Target.");
            return;
        }
        int rc = remove_entry_named(w, name);
        if (rc == 0) lib_strcpy(out, ">> Removed.");
        else if (rc == -3) lib_strcpy(out, "ERR: Dir Not Empty.");
        else if (rc == -2) lib_strcpy(out, "ERR: Not Found.");
        else lib_strcpy(out, "ERR: Remove Failed.");
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        char *fn = cmd + 6;
        while (*fn == ' ') fn++;
        if (*fn == '\0') { lib_strcpy(out, "ERR: Missing File Name."); return; }
        if (strlen(fn) > 19) { lib_strcpy(out, "ERR: File Name Too Long."); return; }
        int rc = store_file_bytes(w, fn, (unsigned char *)"", 0);
        if (rc == 0) lib_strcpy(out, ">> Touched.");
        else if (rc == -3) lib_strcpy(out, "ERR: Name Is Directory.");
        else lib_strcpy(out, "ERR: Touch Failed.");
    } else if (strncmp(cmd, "write ", 6) == 0) {
        char *fn = cmd + 6, *sp = strchr(fn,' '); if(!sp) return; *sp='\0'; char *txt=sp+1;
        if (*fn == '\0') { lib_strcpy(out, "ERR: Missing File Name."); return; }
        int txt_len = strlen(txt);
        int rc = store_file_bytes(w, fn, (unsigned char *)txt, txt_len);
        if (rc == 0) lib_strcpy(out, ">> Stored.");
        else if (rc == -3) lib_strcpy(out, "ERR: Name Is Directory.");
        else lib_strcpy(out, "ERR: Store Failed.");
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        char *fn = cmd + 4;
        while (*fn == ' ') fn++;
        if (*fn == '\0') { lib_strcpy(out, "ERR: Missing File."); return; }
        uint32_t size = 0;
        if (load_file_bytes(w, fn, file_io_buf, sizeof(file_io_buf), &size) == 0) {
            if (is_mostly_text(file_io_buf, size)) {
                uint32_t n = size;
                if (n > OUT_BUF_SIZE - 1) n = OUT_BUF_SIZE - 1;
                memcpy(out, file_io_buf, n);
                out[n] = '\0';
            } else {
                render_hex_dump(out, file_io_buf, size);
            }
        } else {
            lib_strcpy(out, "ERR: File Not Found.");
        }
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
    } else if (strncmp(cmd, "vim", 3) == 0) {
        char *fn = cmd + 3;
        while (*fn == ' ') fn++;
        if (*fn == '\0') fn = "untitled.txt";
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
    } else if (strncmp(cmd, "help", 4) == 0) {
        lib_strcpy(out, "Matrix v1.1\nls (-all), mkdir, rm, touch, cd, pwd, write, cat, wget, wget status, open, run, vim, asm, demo3d, frankenstein, format, clear\nwget <ip> <path> <file>\nwget http(s)://host[:port]/path [file]\nasm <file.s>: li/mv/addi/add/sub/lui/beq/bne/j/ecall\nECALL: a7=1 print a0, a7=2 print hex a0, a7=10 exit\ndemo3d cube | demo3d x,y,z x,y,z ...\nrun <file> [args...]: load ELF app from FS and execute\nCTRL+T/Q/C, SHIFT+TAB");
    } else { lib_strcpy(out, "Invalid signal."); }
}

void run_command(struct Window *w) {
    char *full_cmd = w->cmd_buf;
    w->shell_status[0] = '\0';
    if (w->total_rows >= ROWS - 2) { w->total_rows = 1; w->v_offset = 0; }
    if (strlen(full_cmd) > 0) {
        w->executing_cmd = 1;
        w->cancel_requested = 0;
        if (w->hist_count == 0 || strcmp(w->history[0], full_cmd) != 0) {
            for(int i=MAX_HIST-1; i>0; i--) lib_strcpy(w->history[i], w->history[i-1]);
            lib_strcpy(w->history[0], full_cmd); if(w->hist_count < MAX_HIST) w->hist_count++;
        }
    }
    w->hist_idx = -1;
    if (strncmp(full_cmd, "clear", 5) == 0) {
        w->total_rows = 1; w->v_offset = 0; lib_strcpy(w->lines[0], "matrix:~$ > "); w->cur_col = 12;
        w->executing_cmd = 0; w->cancel_requested = 0;
        return;
    }
    int follow_bottom = terminal_is_at_bottom(w);
    exec_single_cmd(w, full_cmd);
    char *res = w->out_buf; int len = strlen(res), pos = 0, mc = w->maximized ? 65 : (w->w/8-3);
    while(pos < len && w->total_rows < ROWS) {
        int t = 0; while(pos+t < len && res[pos+t] != '\n' && t < mc) t++;
        for(int i=0; i<t; i++) if(w->total_rows < ROWS) w->lines[w->total_rows][i] = res[pos+i];
        if(w->total_rows < ROWS) { w->lines[w->total_rows][t] = '\0'; w->total_rows++; }
        if (res[pos+t] == '\n') pos += (t + 1); else pos += t;
    }
    if (follow_bottom) terminal_scroll_to_bottom(w);
    if (!(strncmp(full_cmd, "run ", 4) == 0 && app_running)) {
        w->executing_cmd = 0;
        w->cancel_requested = 0;
    }
}

static void handle_window_mailbox(struct Window *w) {
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

void create_new_task() {
    for(int i=0; i<MAX_WINDOWS; i++) if(!wins[i].active) {
        reset_window(&wins[i], i);
        wins[i].active=1; wins[i].maximized=0; wins[i].minimized=0;
        wins[i].kind = WINDOW_KIND_TERMINAL;
        wins[i].x=120+i*40; wins[i].y=70+i*30; wins[i].w=520; wins[i].h=320;
        wins[i].total_rows=1; wins[i].cur_col=12;
        lib_strcpy(wins[i].lines[0], "matrix:~$ > ");
        set_window_title(&wins[i], i);
        seed_terminal_history(&wins[i]);
        bring_to_front(i); break;
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
    if (title_max > 22) title_max = 22;
    if (w->kind == WINDOW_KIND_TERMINAL) {
        char title[32];
        lib_strcpy(title, w->title);
        lib_strcat(title, " [x");
        char num[4];
        lib_itoa((uint32_t)terminal_font_scale(w), num);
        lib_strcat(title, num);
        lib_strcat(title, "]");
        title[title_max] = '\0';
        draw_text(x + 10, y + 6, title, UI_C_TEXT);
    } else if (w->kind == WINDOW_KIND_DEMO3D) {
        char title[40];
        char num[12];
        lib_strcpy(title, w->title);
        lib_strcat(title, " [");
        lib_itoa(demo3d_fps_value, num);
        lib_strcat(title, num);
        lib_strcat(title, " FPS]");
        title[title_max] = '\0';
        draw_text(x + 10, y + 6, title, UI_C_TEXT);
    } else {
        char title[32];
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
    char clock[9];
    format_clock(clock);
    draw_bevel_rect(WIDTH - 84, DESKTOP_H + 4, 72, 22, UI_C_PANEL_LIGHT, UI_C_TEXT, UI_C_PANEL_DEEP);
    draw_text(WIDTH - 77, DESKTOP_H + 8, clock, UI_C_SHADOW);
    draw_text(WIDTH - 78, DESKTOP_H + 8, clock, UI_C_TEXT);
}

void gui_task(void) {
    extern void vga_update();
    extern void virtio_input_poll();
    for(int i=0; i<MAX_WINDOWS; i++) { reset_window(&wins[i], i); z_order[i] = i; }
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
                            if(!w->maximized) { w->prev_x=w->x; w->prev_y=w->y; w->prev_w=w->w; w->prev_h=w->h; w->maximized=1; }
                            else { w->x=w->prev_x; w->y=w->prev_y; w->w=w->prev_w; w->h=w->prev_h; w->maximized=0; }
                        } else if (local_mx > wx + ww - 110) { w->minimized = 1; active_win_idx = -1; }
                        else {
                            static uint32_t last_click_ms = 0;
                            static int last_click_idx = -1;
                            uint32_t now = sys_now();
                            if (last_click_idx == idx && (now - last_click_ms) < 400) {
                                if (!w->maximized) { w->prev_x = w->x; w->prev_y = w->y; w->prev_w = w->w; w->prev_h = w->h; w->maximized = 1; }
                                else { w->x = w->prev_x; w->y = w->prev_y; w->w = w->prev_w; w->h = w->prev_h; w->maximized = 0; }
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
                window_input_push(&wins[active_win_idx], gui_key);
                wins[active_win_idx].mailbox = 1;
                wake_terminal_worker_for_window(active_win_idx);
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
        if (redraw_needed) {
            extern void draw_rect_fill(int,int,int,int,int);
            extern void draw_vertical_gradient(int,int,int,int,int,int);
            draw_vertical_gradient(0, 0, WIDTH, DESKTOP_H, UI_C_DESKTOP, UI_C_PANEL_DARK);
            draw_rect_fill(0, DESKTOP_H, WIDTH, TASKBAR_H, UI_C_DESKTOP);
            memset(sheet_map, 0, sizeof(sheet_map));
            for(int i=0; i<MAX_WINDOWS; i++) draw_window(&wins[z_order[i]]);
            draw_taskbar(); draw_cursor(gui_mx, gui_my, 14, gui_cursor_mode); vga_update();
        }
        lib_delay(5); if (need_resched) { need_resched = 0; task_os(); }
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
                        if (ow->total_rows < ROWS) {
                            ow->total_rows++;
                            lib_strcpy(ow->lines[ow->total_rows - 1], PROMPT);
                            ow->cur_col = PROMPT_LEN;
                        }
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
                if (ow->total_rows < ROWS) {
                    ow->total_rows++;
                    lib_strcpy(ow->lines[ow->total_rows - 1], PROMPT);
                    ow->cur_col = PROMPT_LEN;
                }
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
            if (ow->total_rows < ROWS) {
                ow->total_rows++;
                lib_strcpy(ow->lines[ow->total_rows - 1], PROMPT);
                ow->cur_col = PROMPT_LEN;
            }
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
    task_create(&gui_task, 0, 1);
    for(int i=0; i<TERMINAL_WORKERS; i++) terminal_worker_task_ids[i] = task_create(&terminal_worker_task, 0, 1);
    network_task_id = task_create(&network_task, 1, 1);
    app_task_id = task_create(&app_bootstrap, 0, 1);
}
