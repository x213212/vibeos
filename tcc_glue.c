#include "os.h"
#include "vga.h"
#include "timer.h"
#include "libtcc.h"
#include "setjmp.h"
#include "unistd.h"
#include "sys/time.h"
#include <stdarg.h>

// --- 使用核心現有的符號 (由 user_utils.c 提供) ---
extern void *realloc(void *ptr, size_t size);
extern void *calloc(uint32_t nmemb, uint32_t size);
extern int fflush(void *stream);
extern int puts(const char *s);
extern char *strcpy(char *dst, const char *src);
extern char *strcat(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, uint32_t n);
extern int strncmp(const char *s1, const char *s2, uint32_t n);
extern int atoi(const char *nptr);
extern int abs(int n);

// --- 符號與 Linker 修復 ---
void *stdout = (void*)1;
int fputs(const char *s, void *stream) { lib_printf("%s", s); return 0; }
char *getcwd(char *buf, size_t size) { if (buf) lib_strcpy(buf, "/root"); return buf; }

// 基礎字串/數字轉換
unsigned long strtoul(const char *nptr, char **endptr, int base) {
    unsigned long res = 0; const char *p = nptr;
    if (base == 0) { if (*p == '0') { p++; if (*p == 'x' || *p == 'X') { base = 16; p++; } else base = 8; } else base = 10; }
    while (*p) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        if (v < 0 || v >= base) break;
        res = res * base + v; p++;
    }
    if (endptr) *endptr = (char *)p; return res;
}
long strtol(const char *nptr, char **endptr, int base) {
    if (*nptr == '-') return -(long)strtoul(nptr + 1, endptr, base);
    return (long)strtoul(nptr, endptr, base);
}
unsigned long long strtoull(const char *nptr, char **endptr, int base) { return (unsigned long long)strtoul(nptr, endptr, base); }
long long strtoll(const char *nptr, char **endptr, int base) { return (long long)strtol(nptr, endptr, base); }

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {}
int sscanf(const char *str, const char *format, ...) { return 0; }
double strtod(const char *nptr, char **endptr) { return 0.0; }
float strtof(const char *nptr, char **endptr) { return 0.0f; }
long double strtold(const char *nptr, char **endptr) { return 0.0L; }
double ldexp(double x, int exp) { return x; }
void *localtime(const long *timer) { return NULL; }

int sem_init(int *sem, int pshared, unsigned int value) { return 0; }
int sem_wait(int *sem) { return 0; }
int sem_post(int *sem) { return 0; }

// --- 檔案系統 (對接到 appfs，確保符號可見) ---
void *fopen(const char *path, const char *mode) {
    int fd = appfs_open(path, 1); // 1 = READ
    if (fd < 0) return NULL;
    int *pfd = malloc(sizeof(int));
    *pfd = fd;
    return (void*)pfd;
}
int fclose(void *fp) {
    if (!fp) return -1;
    appfs_close(*(int*)fp);
    free(fp);
    return 0;
}
size_t fread(void *ptr, size_t size, size_t nmemb, void *fp) {
    if (!fp) return 0;
    int rc = appfs_read(*(int*)fp, ptr, size * nmemb);
    return (rc > 0) ? (size_t)rc / size : 0;
}

// 基礎系統調用
int open(const char *pathname, int flags, ...) { return appfs_open(pathname, flags); }
int close(int fd) { return appfs_close(fd); }
ssize_t read(int fd, void *buf, size_t count) { return appfs_read(fd, buf, count); }
ssize_t write(int fd, const void *buf, size_t count) { return appfs_write(fd, buf, count); }
off_t lseek(int fd, off_t offset, int whence) { return 0; }
int unlink(const char *pathname) { return -1; }
int getpagesize(void) { return 4096; }
void *mmap(void *addr, size_t length, int prot, int flags, int fd, size_t offset) { return malloc(length); }
int munmap(void *addr, size_t length) { free(addr); return 0; }
int mprotect(void *addr, size_t length, int prot) { return 0; }

int setjmp(jmp_buf env) {
    asm volatile ("sw ra, 0(a0); sw sp, 4(a0); sw s0, 8(a0); sw s1, 12(a0); sw s2, 16(a0); sw s3, 20(a0); sw s4, 24(a0); sw s5, 28(a0); sw s6, 32(a0); sw s7, 36(a0); sw s8, 40(a0); sw s9, 44(a0); sw s10, 48(a0); sw s11, 52(a0); li a0, 0; ret");
    return 0;
}
void longjmp(jmp_buf env, int val) {
    asm volatile ("lw ra, 0(a0); lw sp, 4(a0); lw s0, 8(a0); lw s1, 12(a0); lw s2, 16(a0); lw s3, 20(a0); lw s4, 24(a0); lw s5, 28(a0); lw s6, 32(a0); lw s7, 36(a0); lw s8, 40(a0); lw s9, 44(a0); lw s10, 48(a0); lw s11, 52(a0); mv a0, a1; ret");
}

char *empty_environ[] = { NULL };
char **environ = empty_environ;
void tcc_error_report(void *opaque, const char *msg) { lib_printf("TCC Error: %s\n", msg); }

#define JIT_UHEAP_SIZE (16u * 1024u * 1024u)
#define JIT_MAX_JOBS 4
#define JIT_UHEAP_MAGIC 0x55484d30u /* "UHM0" */
#define JIT_SHARED_SIZE (1024u * 1024u)
#define JIT_MAX_KPTRS 32
#define JIT_MAX_FDS 32

enum {
    JIT_JOB_EMPTY = 0,
    JIT_JOB_COMPILING = 1,
    JIT_JOB_READY = 2,
    JIT_JOB_RUNNING = 3,
    JIT_JOB_DONE = 4,
    JIT_JOB_FAILED = 5
};

struct jit_ublock {
    uint32_t magic;
    uint32_t size;
    uint32_t free;
    struct jit_ublock *next;
};

struct jit_job {
    int id;
    int state;
    int bg;
    int owner_win_id;
    int task_id;
    int waiter_task_id;
    int cancel_requested;
    int exit_code;
    TCCState *tcc;
    int (*main_func)(void);
    uint8_t *uheap;
    struct jit_ublock *uheap_head;
    void *kptrs[JIT_MAX_KPTRS];
    int fds[JIT_MAX_FDS];
};

static uint8_t jit_user_heaps[JIT_MAX_JOBS][JIT_UHEAP_SIZE];
static uint8_t jit_shared_area[JIT_SHARED_SIZE];
static struct jit_job jit_jobs[JIT_MAX_JOBS];
static int jit_next_id = 1;
static int jit_initialized = 0;

void os_jit_init(void);

static struct jit_job *jit_current_job(void) {
    int tid = task_current();
    for (int i = 0; i < JIT_MAX_JOBS; i++) {
        if (jit_jobs[i].state != JIT_JOB_EMPTY && jit_jobs[i].task_id == tid) return &jit_jobs[i];
    }
    return NULL;
}

static int jit_check_cancel(void) {
    struct jit_job *job = jit_current_job();
    if (!job) return 0;
    return job->cancel_requested != 0;
}

static void jit_yield(void) {
    struct jit_job *job = jit_current_job();
    if (job && job->cancel_requested) {
        job->exit_code = -2;
        job->state = JIT_JOB_FAILED;
        lib_printf("[JITDBG] job=%d cancelled at yield\n", job->id);
        if (job->bg) task_sleep_current();
        return;
    }
    if (task_current() >= 0) task_os();
}

void os_delay(unsigned int ms) {
    unsigned int start = get_millisecond_timer();
    while (get_millisecond_timer() - start < ms) {
        if (jit_check_cancel()) {
            jit_yield();
            return;
        }
        for (volatile int i = 0; i < 2000; i++) asm volatile("nop");
        jit_yield();
    }
}

static struct jit_job *jit_alloc_job(int bg, int owner_win_id) {
    if (!jit_initialized) os_jit_init();
    for (int i = 0; i < JIT_MAX_JOBS; i++) {
        if (jit_jobs[i].state == JIT_JOB_EMPTY || jit_jobs[i].state == JIT_JOB_DONE || jit_jobs[i].state == JIT_JOB_FAILED) {
            int old_task_id = jit_jobs[i].task_id;
            if (jit_jobs[i].tcc) {
                tcc_delete(jit_jobs[i].tcc);
                jit_jobs[i].tcc = NULL;
            }
            memset(&jit_jobs[i], 0, sizeof(jit_jobs[i]));
            jit_jobs[i].id = jit_next_id++;
            jit_jobs[i].state = JIT_JOB_COMPILING;
            jit_jobs[i].bg = bg;
            jit_jobs[i].owner_win_id = owner_win_id;
            jit_jobs[i].task_id = old_task_id;
            jit_jobs[i].waiter_task_id = -1;
            jit_jobs[i].uheap = jit_user_heaps[i];
            for (int j = 0; j < JIT_MAX_FDS; j++) jit_jobs[i].fds[j] = -1;
            return &jit_jobs[i];
        }
    }
    return NULL;
}

static uint32_t jit_align8(uint32_t n) {
    return (n + 7u) & ~7u;
}

static void jit_cleanup_resources(struct jit_job *job) {
    if (!job) return;
    for (int i = 0; i < JIT_MAX_FDS; i++) {
        if (job->fds[i] >= 0) {
            appfs_close(job->fds[i]);
            job->fds[i] = -1;
        }
    }
    for (int i = 0; i < JIT_MAX_KPTRS; i++) {
        if (job->kptrs[i]) {
            free(job->kptrs[i]);
            job->kptrs[i] = NULL;
        }
    }
}

static int jit_track_kptr(struct jit_job *job, void *p) {
    if (!job || !p) return -1;
    for (int i = 0; i < JIT_MAX_KPTRS; i++) {
        if (!job->kptrs[i]) {
            job->kptrs[i] = p;
            return 0;
        }
    }
    return -1;
}

static int jit_untrack_kptr(struct jit_job *job, void *p) {
    if (!job || !p) return -1;
    for (int i = 0; i < JIT_MAX_KPTRS; i++) {
        if (job->kptrs[i] == p) {
            job->kptrs[i] = NULL;
            return 0;
        }
    }
    return -1;
}

static int jit_track_fd(struct jit_job *job, int fd) {
    if (!job || fd < 0) return -1;
    for (int i = 0; i < JIT_MAX_FDS; i++) {
        if (job->fds[i] < 0) {
            job->fds[i] = fd;
            return 0;
        }
    }
    return -1;
}

static int jit_untrack_fd(struct jit_job *job, int fd) {
    if (!job || fd < 0) return -1;
    for (int i = 0; i < JIT_MAX_FDS; i++) {
        if (job->fds[i] == fd) {
            job->fds[i] = -1;
            return 0;
        }
    }
    return -1;
}

static void *jit_kmalloc(size_t size) {
    struct jit_job *job = jit_current_job();
    if (!job || job->cancel_requested) return NULL;
    void *p = malloc(size);
    if (!p) return NULL;
    if (jit_track_kptr(job, p) != 0) {
        free(p);
        return NULL;
    }
    return p;
}

static void jit_kfree(void *p) {
    struct jit_job *job = jit_current_job();
    if (!p) return;
    if (job && jit_untrack_kptr(job, p) == 0) {
        free(p);
    }
}

static int jit_appfs_open(const char *path, int flags) {
    struct jit_job *job = jit_current_job();
    if (!job || job->cancel_requested) return -1;
    int fd = appfs_open(path, flags);
    if (fd < 0) return fd;
    if (jit_track_fd(job, fd) != 0) {
        appfs_close(fd);
        return -1;
    }
    return fd;
}

static int jit_appfs_close(int fd) {
    struct jit_job *job = jit_current_job();
    if (job) jit_untrack_fd(job, fd);
    return appfs_close(fd);
}

static void jit_uheap_reset(struct jit_job *job) {
    if (!job || !job->uheap) return;
    job->uheap_head = (struct jit_ublock *)job->uheap;
    job->uheap_head->magic = JIT_UHEAP_MAGIC;
    job->uheap_head->size = JIT_UHEAP_SIZE - sizeof(struct jit_ublock);
    job->uheap_head->free = 1;
    job->uheap_head->next = NULL;
}

static void jit_uheap_coalesce(struct jit_job *job) {
    struct jit_ublock *b = job ? job->uheap_head : NULL;
    while (b && b->next) {
        if (b->free && b->next->free) {
            b->size += sizeof(struct jit_ublock) + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
}

static void *umalloc(size_t size) {
    struct jit_job *job = jit_current_job();
    if (!job) return NULL;
    if (job->cancel_requested) return NULL;
    if (!job->uheap_head) jit_uheap_reset(job);
    uint32_t need = jit_align8(size ? (uint32_t)size : 1u);
    struct jit_ublock *b = job->uheap_head;
    while (b) {
        if (b->magic != JIT_UHEAP_MAGIC) return NULL;
        if (b->free && b->size >= need) {
            uint32_t remain = b->size - need;
            if (remain > sizeof(struct jit_ublock) + 8u) {
                struct jit_ublock *n = (struct jit_ublock *)((uint8_t *)b + sizeof(struct jit_ublock) + need);
                n->magic = JIT_UHEAP_MAGIC;
                n->size = remain - sizeof(struct jit_ublock);
                n->free = 1;
                n->next = b->next;
                b->next = n;
                b->size = need;
            }
            b->free = 0;
            return (uint8_t *)b + sizeof(struct jit_ublock);
        }
        b = b->next;
    }
    return NULL;
}

static void ufree(void *p) {
    if (!p) return;
    struct jit_job *job = jit_current_job();
    if (!job) return;
    if (job->cancel_requested) return;
    uint8_t *addr = (uint8_t *)p;
    if (addr < job->uheap + sizeof(struct jit_ublock) ||
        addr >= job->uheap + JIT_UHEAP_SIZE) {
        lib_printf("[JITDBG] ufree reject ptr=%lx\n", (unsigned long)(uintptr_t)p);
        return;
    }
    struct jit_ublock *b = (struct jit_ublock *)(addr - sizeof(struct jit_ublock));
    if (b->magic != JIT_UHEAP_MAGIC || b->free) {
        lib_printf("[JITDBG] ufree invalid ptr=%lx\n", (unsigned long)(uintptr_t)p);
        return;
    }
    b->free = 1;
    jit_uheap_coalesce(job);
}

static void *ucalloc(uint32_t nmemb, uint32_t size) {
    if (jit_check_cancel()) return NULL;
    uint32_t total = nmemb * size;
    if (size && total / size != nmemb) return NULL;
    void *p = umalloc(total ? total : 1u);
    if (p) memset(p, 0, total);
    return p;
}

static void *urealloc(void *ptr, size_t size) {
    if (jit_check_cancel()) return NULL;
    if (!ptr) return umalloc(size);
    if (size == 0) {
        ufree(ptr);
        return NULL;
    }
    struct jit_job *job = jit_current_job();
    if (!job) return NULL;
    uint8_t *addr = (uint8_t *)ptr;
    if (addr < job->uheap + sizeof(struct jit_ublock) ||
        addr >= job->uheap + JIT_UHEAP_SIZE) {
        return NULL;
    }
    struct jit_ublock *b = (struct jit_ublock *)(addr - sizeof(struct jit_ublock));
    if (b->magic != JIT_UHEAP_MAGIC || b->free) return NULL;
    if (b->size >= size) return ptr;
    void *np = umalloc(size);
    if (np) {
        memcpy(np, ptr, b->size);
        ufree(ptr);
    }
    return np;
}

void jit_uheap_info(uint32_t *base, uint32_t *size, uint32_t *used, uint32_t *free_bytes, uint32_t *blocks) {
    uint32_t u = 0;
    uint32_t f = 0;
    uint32_t n = 0;
    if (base) *base = (uint32_t)(uintptr_t)jit_user_heaps[0];
    if (size) *size = JIT_UHEAP_SIZE * JIT_MAX_JOBS;
    for (int i = 0; i < JIT_MAX_JOBS; i++) {
        if (jit_jobs[i].uheap_head) {
            struct jit_ublock *b = jit_jobs[i].uheap_head;
            while (b) {
                if (b->magic != JIT_UHEAP_MAGIC) break;
                if (b->free) f += b->size;
                else u += b->size;
                n++;
                b = b->next;
            }
        }
    }
    if (used) *used = u;
    if (free_bytes) *free_bytes = f;
    if (blocks) *blocks = n;
}

static void *jit_shared_mem(void) {
    return jit_shared_area;
}

static uint32_t jit_shared_size(void) {
    return JIT_SHARED_SIZE;
}

void os_jit_shared_reset(void) {
    memset(jit_shared_area, 0, sizeof(jit_shared_area));
}

static const char jit_prelude[] =
    "typedef unsigned int size_t;\n"
    "typedef unsigned int uint32_t;\n"
    "typedef unsigned char uint8_t;\n"
    "int printf(const char *fmt, ...);\n"
    "int print(const char *fmt, ...);\n"
    "int puts(const char *s);\n"
    "int putchar(int c);\n"
    "int snprintf(char *out, size_t n, const char *fmt, ...);\n"
    "void *umalloc(size_t size);\n"
    "void ufree(void *p);\n"
    "void *ucalloc(uint32_t nmemb, uint32_t size);\n"
    "void *urealloc(void *ptr, size_t size);\n"
    "void *kmalloc(size_t size);\n"
    "void kfree(void *p);\n"
    "void yield(void);\n"
    "int cancelled(void);\n"
    "void *shared_mem(void);\n"
    "uint32_t shared_size(void);\n"
    "#define malloc umalloc\n"
    "#define free ufree\n"
    "#define calloc ucalloc\n"
    "#define realloc urealloc\n"
    "void *memset(void *dst, int c, size_t n);\n"
    "void *memcpy(void *dst, const void *src, size_t n);\n"
    "void *memmove(void *dst, const void *src, size_t n);\n"
    "size_t strlen(const char *s);\n"
    "int strcmp(const char *a, const char *b);\n"
    "int strncmp(const char *a, const char *b, uint32_t n);\n"
    "char *strcpy(char *dst, const char *src);\n"
    "char *strcat(char *dst, const char *src);\n"
    "char *strncpy(char *dst, const char *src, uint32_t n);\n"
    "char *strchr(const char *s, int c);\n"
    "int atoi(const char *s);\n"
    "int abs(int n);\n"
    "unsigned int get_ticks(void);\n"
    "unsigned int millis(void);\n"
    "unsigned int sys_now(void);\n"
    "unsigned int get_wall_clock_seconds(void);\n"
    "void delay(unsigned int ms);\n"
    "void putpixel(int x, int y, int color);\n"
    "void vga_update(void);\n"
    "void draw_char(int x, int y, char c, int color);\n"
    "void draw_text(int x, int y, const char *s, int color);\n"
    "void draw_text_scaled(int x, int y, const char *s, int color, int scale);\n"
    "void draw_rect_fill(int x, int y, int w, int h, int color);\n"
    "void draw_hline(int x, int y, int w, int color);\n"
    "void draw_vline(int x, int y, int h, int color);\n"
    "void draw_vertical_gradient(int x, int y, int w, int h, int top_color, int bottom_color);\n"
    "void draw_bevel_rect(int x, int y, int w, int h, int fill, int light, int dark);\n"
    "void draw_round_rect_fill(int x, int y, int w, int h, int r, int color);\n"
    "void draw_round_rect_wire(int x, int y, int w, int h, int r, int color);\n"
    "int appfs_open(const char *path, int flags);\n"
    "int appfs_read(int fd, void *buf, size_t size);\n"
    "int appfs_write(int fd, const void *buf, size_t size);\n"
    "int appfs_close(int fd);\n"
    "enum { WIDTH = 1024, HEIGHT = 768 };\n";

static void jit_add_symbol_logged(TCCState *s, const char *name, const void *val) {
    tcc_add_symbol(s, name, val);
}

static void jit_add_os_symbols(TCCState *s) {
    jit_add_symbol_logged(s, "printf", (void*)lib_printf);
    jit_add_symbol_logged(s, "print", (void*)lib_printf);
    jit_add_symbol_logged(s, "puts", (void*)puts);
    jit_add_symbol_logged(s, "putchar", (void*)uart_putc);
    jit_add_symbol_logged(s, "snprintf", (void*)snprintf);

    jit_add_symbol_logged(s, "umalloc", (void*)umalloc);
    jit_add_symbol_logged(s, "ufree", (void*)ufree);
    jit_add_symbol_logged(s, "ucalloc", (void*)ucalloc);
    jit_add_symbol_logged(s, "urealloc", (void*)urealloc);
    jit_add_symbol_logged(s, "kmalloc", (void*)jit_kmalloc);
    jit_add_symbol_logged(s, "kfree", (void*)jit_kfree);
    jit_add_symbol_logged(s, "yield", (void*)jit_yield);
    jit_add_symbol_logged(s, "cancelled", (void*)jit_check_cancel);
    jit_add_symbol_logged(s, "shared_mem", (void*)jit_shared_mem);
    jit_add_symbol_logged(s, "shared_size", (void*)jit_shared_size);
    jit_add_symbol_logged(s, "memset", (void*)memset);
    jit_add_symbol_logged(s, "memcpy", (void*)memcpy);
    jit_add_symbol_logged(s, "memmove", (void*)memmove);
    jit_add_symbol_logged(s, "strlen", (void*)strlen);
    jit_add_symbol_logged(s, "strcmp", (void*)strcmp);
    jit_add_symbol_logged(s, "strncmp", (void*)strncmp);
    jit_add_symbol_logged(s, "strcpy", (void*)strcpy);
    jit_add_symbol_logged(s, "strcat", (void*)strcat);
    jit_add_symbol_logged(s, "strncpy", (void*)strncpy);
    jit_add_symbol_logged(s, "strchr", (void*)strchr);
    jit_add_symbol_logged(s, "atoi", (void*)atoi);
    jit_add_symbol_logged(s, "abs", (void*)abs);

    jit_add_symbol_logged(s, "get_ticks", (void*)get_millisecond_timer);
    jit_add_symbol_logged(s, "millis", (void*)get_millisecond_timer);
    jit_add_symbol_logged(s, "sys_now", (void*)sys_now);
    jit_add_symbol_logged(s, "get_wall_clock_seconds", (void*)get_wall_clock_seconds);
    jit_add_symbol_logged(s, "delay", (void*)os_delay);

    jit_add_symbol_logged(s, "vga_update", (void*)vga_update);
    jit_add_symbol_logged(s, "putpixel", (void*)putpixel);
    jit_add_symbol_logged(s, "draw_char", (void*)draw_char);
    jit_add_symbol_logged(s, "draw_text", (void*)draw_text);
    jit_add_symbol_logged(s, "draw_text_scaled", (void*)draw_text_scaled);
    jit_add_symbol_logged(s, "draw_rect_fill", (void*)draw_rect_fill);
    jit_add_symbol_logged(s, "draw_hline", (void*)draw_hline);
    jit_add_symbol_logged(s, "draw_vline", (void*)draw_vline);
    jit_add_symbol_logged(s, "draw_vertical_gradient", (void*)draw_vertical_gradient);
    jit_add_symbol_logged(s, "draw_bevel_rect", (void*)draw_bevel_rect);
    jit_add_symbol_logged(s, "draw_round_rect_fill", (void*)draw_round_rect_fill);
    jit_add_symbol_logged(s, "draw_round_rect_wire", (void*)draw_round_rect_wire);

    jit_add_symbol_logged(s, "appfs_open", (void*)jit_appfs_open);
    jit_add_symbol_logged(s, "appfs_read", (void*)appfs_read);
    jit_add_symbol_logged(s, "appfs_write", (void*)appfs_write);
    jit_add_symbol_logged(s, "appfs_close", (void*)jit_appfs_close);
}

static char *jit_make_source_with_prelude(const char *source) {
    size_t prelude_len = strlen(jit_prelude);
    size_t source_len = source ? strlen(source) : 0;
    char *combined = (char *)malloc(prelude_len + source_len + 2);
    if (!combined) return NULL;
    memcpy(combined, jit_prelude, prelude_len);
    combined[prelude_len] = '\n';
    if (source_len) memcpy(combined + prelude_len + 1, source, source_len);
    combined[prelude_len + 1 + source_len] = '\0';
    return combined;
}

static const char *jit_state_name(int state) {
    switch (state) {
    case JIT_JOB_EMPTY: return "empty";
    case JIT_JOB_COMPILING: return "compiling";
    case JIT_JOB_READY: return "ready";
    case JIT_JOB_RUNNING: return "running";
    case JIT_JOB_DONE: return "done";
    case JIT_JOB_FAILED: return "failed";
    default: return "?";
    }
}

static int jit_compile_job(struct jit_job *job, const char *source, char *msg, size_t msg_size) {
    if (msg && msg_size) msg[0] = '\0';
    if (!job) return -1;
    lib_printf("[JITDBG] job=%d source=%lx first='%c'\n",
               job->id, (unsigned long)(uintptr_t)source, source ? source[0] : '?');
    job->tcc = tcc_new();
    if (!job->tcc) {
        job->state = JIT_JOB_FAILED;
        if (msg && msg_size) snprintf(msg, msg_size, "JIT: init failed");
        return -1;
    }
    jit_uheap_reset(job);
    lib_printf("[JITDBG] job=%d uheap=%lx size=%u\n",
               job->id, (unsigned long)(uintptr_t)job->uheap, JIT_UHEAP_SIZE);
    lib_printf("[JITDBG] tcc_new s=%lx\n", (unsigned long)(uintptr_t)job->tcc);
    tcc_set_error_func(job->tcc, NULL, tcc_error_report);
    tcc_set_options(job->tcc, "-nostdlib");
    lib_printf("[JITDBG] options=-nostdlib\n");
    lib_printf("[JITDBG] set_output_type rc=%d\n", tcc_set_output_type(job->tcc, TCC_OUTPUT_MEMORY));
    lib_printf("[JITDBG] sym printf=%lx print=%lx get_ticks=%lx\n",
               (unsigned long)(uintptr_t)lib_printf,
               (unsigned long)(uintptr_t)lib_printf,
               (unsigned long)(uintptr_t)get_millisecond_timer);
    jit_add_os_symbols(job->tcc);
    char *compile_source = jit_make_source_with_prelude(source);
    if (!compile_source) {
        job->state = JIT_JOB_FAILED;
        if (msg && msg_size) snprintf(msg, msg_size, "JIT: source alloc failed");
        return -1;
    }
    lib_printf("[JITDBG] compile start\n");
    int compile_rc = tcc_compile_string(job->tcc, compile_source);
    free(compile_source);
    lib_printf("[JITDBG] compile rc=%d\n", compile_rc);
    if (compile_rc == -1) {
        job->state = JIT_JOB_FAILED;
        if (msg && msg_size) snprintf(msg, msg_size, "JIT: compile failed");
        return -1;
    }
    lib_printf("[JITDBG] relocate start\n");
    int reloc_rc = tcc_relocate(job->tcc, TCC_RELOCATE_AUTO);
    lib_printf("[JITDBG] relocate rc=%d\n", reloc_rc);
    if (reloc_rc != 0) {
        job->state = JIT_JOB_FAILED;
        if (msg && msg_size) snprintf(msg, msg_size, "JIT: relocate failed");
        return -1;
    }
    job->main_func = (int (*)(void))tcc_get_symbol(job->tcc, "main");
    lib_printf("[JITDBG] main=%lx\n", (unsigned long)(uintptr_t)job->main_func);
    if (!job->main_func) {
        job->state = JIT_JOB_FAILED;
        if (msg && msg_size) snprintf(msg, msg_size, "JIT: main not found");
        return -1;
    }
    job->state = JIT_JOB_READY;
    if (msg && msg_size) snprintf(msg, msg_size, "JIT job %d ready.", job->id);
    return 0;
}

static void jit_job_cleanup(struct jit_job *job) {
    if (!job) return;
    jit_cleanup_resources(job);
    if (job->tcc) {
        tcc_delete(job->tcc);
        job->tcc = NULL;
    }
}

static void jit_job_execute(struct jit_job *job) {
    if (!job || !job->main_func) return;
    job->task_id = task_current();
    job->state = JIT_JOB_RUNNING;
    job->cancel_requested = 0;
    asm volatile("fence.i");
    lib_printf("[JITDBG] job=%d call main\n", job->id);
    job->exit_code = job->main_func();
    lib_printf("[JITDBG] job=%d main returned rc=%d\n", job->id, job->exit_code);
    if (job->cancel_requested || job->state == JIT_JOB_FAILED) {
        job->exit_code = -2;
        job->state = JIT_JOB_FAILED;
        lib_printf("[JITDBG] job=%d cancelled\n", job->id);
    } else {
        job->state = JIT_JOB_DONE;
    }
    jit_job_cleanup(job);
    if (!job->bg && job->waiter_task_id >= 0) {
        task_wake(job->waiter_task_id);
    }
}

static void jit_task_entry_idx(int idx) {
    while (1) {
        if (idx >= 0 && idx < JIT_MAX_JOBS && jit_jobs[idx].state == JIT_JOB_READY) {
            jit_job_execute(&jit_jobs[idx]);
        }
        task_sleep_current();
    }
}

static void jit_task0(void) { jit_task_entry_idx(0); }
static void jit_task1(void) { jit_task_entry_idx(1); }
static void jit_task2(void) { jit_task_entry_idx(2); }
static void jit_task3(void) { jit_task_entry_idx(3); }

static void (*jit_task_entries[JIT_MAX_JOBS])(void) = {
    jit_task0, jit_task1, jit_task2, jit_task3
};

void os_jit_init(void) {
    if (jit_initialized) return;
    for (int i = 0; i < JIT_MAX_JOBS; i++) {
        memset(&jit_jobs[i], 0, sizeof(jit_jobs[i]));
        jit_jobs[i].task_id = -1;
        jit_jobs[i].waiter_task_id = -1;
        jit_jobs[i].uheap = jit_user_heaps[i];
        for (int j = 0; j < JIT_MAX_FDS; j++) jit_jobs[i].fds[j] = -1;
    }
    memset(jit_shared_area, 0, sizeof(jit_shared_area));
    jit_initialized = 1;
}

void os_jit_run(const char *source) {
    struct jit_job *job = jit_alloc_job(0, -1);
    char msg[96];
    if (!job) {
        lib_printf("JIT: no free job slot\n");
        return;
    }
    int slot = (int)(job - jit_jobs);
    if (jit_compile_job(job, source, msg, sizeof(msg)) != 0) {
        lib_printf("%s\n", msg[0] ? msg : "JIT: failed");
        jit_job_cleanup(job);
        return;
    }
    job->bg = 0;
    job->owner_win_id = -1;
    job->waiter_task_id = task_current();
    if (job->task_id < 0 || job->task_id == job->waiter_task_id) {
        job->task_id = task_create(jit_task_entries[slot], 0, 1);
    } else {
        task_reset(job->task_id, jit_task_entries[slot], 0, 1);
    }
    task_wake(job->task_id);
    while (job->state == JIT_JOB_READY || job->state == JIT_JOB_RUNNING) {
        task_sleep_current();
    }
}

int os_jit_run_bg(const char *source, int owner_win_id, char *msg, size_t msg_size) {
    struct jit_job *job = jit_alloc_job(1, owner_win_id);
    if (!job) {
        if (msg && msg_size) snprintf(msg, msg_size, "ERR: no free JIT job slot.");
        return -1;
    }
    int slot = (int)(job - jit_jobs);
    if (jit_compile_job(job, source, msg, msg_size) != 0) {
        jit_job_cleanup(job);
        return -1;
    }
    if (job->task_id < 0) {
        job->task_id = task_create(jit_task_entries[slot], 0, 1);
    } else {
        task_reset(job->task_id, jit_task_entries[slot], 0, 1);
    }
    task_wake(job->task_id);
    if (msg && msg_size) snprintf(msg, msg_size, "JIT job %d started in background.", job->id);
    return job->id;
}

void os_jit_ps(char *out, size_t out_size) {
    int pos = 0;
    if (!out || out_size == 0) return;
    out[0] = '\0';
    pos += snprintf(out + pos, out_size - pos, "JIT jobs:\n");
    for (int i = 0; i < JIT_MAX_JOBS && pos < (int)out_size - 1; i++) {
        if (jit_jobs[i].state == JIT_JOB_EMPTY) continue;
        uint32_t used = 0, freeb = 0, blocks = 0;
        if (jit_jobs[i].uheap_head) {
            struct jit_ublock *b = jit_jobs[i].uheap_head;
            while (b) {
                if (b->magic != JIT_UHEAP_MAGIC) break;
                if (b->free) freeb += b->size;
                else used += b->size;
                blocks++;
                b = b->next;
            }
        }
        pos += snprintf(out + pos, out_size - pos,
                        "#%d slot=%d %s task=%d u=%uKB f=%uKB blocks=%u rc=%d\n",
                        jit_jobs[i].id, i, jit_state_name(jit_jobs[i].state),
                        jit_jobs[i].task_id, used / 1024, freeb / 1024,
                        blocks, jit_jobs[i].exit_code);
    }
}

int os_jit_kill(int id, char *msg, size_t msg_size) {
    for (int i = 0; i < JIT_MAX_JOBS; i++) {
        if (jit_jobs[i].state != JIT_JOB_EMPTY && jit_jobs[i].id == id) {
            jit_jobs[i].cancel_requested = 1;
            if (jit_jobs[i].task_id >= 0) {
                task_reset(jit_jobs[i].task_id, jit_task_entries[i], 0, 1);
                task_sleep(jit_jobs[i].task_id);
            }
            jit_jobs[i].state = JIT_JOB_FAILED;
            jit_jobs[i].exit_code = -1;
            jit_job_cleanup(&jit_jobs[i]);
            if (!jit_jobs[i].bg && jit_jobs[i].waiter_task_id >= 0) {
                task_wake(jit_jobs[i].waiter_task_id);
            }
            if (msg && msg_size) snprintf(msg, msg_size, "JIT job %d killed.", id);
            return 0;
        }
    }
    if (msg && msg_size) snprintf(msg, msg_size, "ERR: JIT job %d not found.", id);
    return -1;
}

int os_jit_cancel_task(int task_id) {
    for (int i = 0; i < JIT_MAX_JOBS; i++) {
        if (jit_jobs[i].state != JIT_JOB_EMPTY &&
            (jit_jobs[i].task_id == task_id || jit_jobs[i].waiter_task_id == task_id)) {
            jit_jobs[i].cancel_requested = 1;
            if (jit_jobs[i].task_id >= 0) {
                task_reset(jit_jobs[i].task_id, jit_task_entries[i], 0, 1);
                task_sleep(jit_jobs[i].task_id);
            }
            jit_jobs[i].state = JIT_JOB_FAILED;
            jit_jobs[i].exit_code = -2;
            jit_job_cleanup(&jit_jobs[i]);
            if (!jit_jobs[i].bg && jit_jobs[i].waiter_task_id >= 0) {
                task_wake(jit_jobs[i].waiter_task_id);
            }
            return jit_jobs[i].id;
        }
    }
    return -1;
}
