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

void os_delay(unsigned int ms) {
    unsigned int start = get_millisecond_timer();
    while (get_millisecond_timer() - start < ms) asm volatile("nop");
}

char *empty_environ[] = { NULL };
char **environ = empty_environ;
void tcc_error_report(void *opaque, const char *msg) { lib_printf("TCC Error: %s\n", msg); }

#define JIT_UHEAP_SIZE (16u * 1024u * 1024u)
#define JIT_UHEAP_MAGIC 0x55484d30u /* "UHM0" */

struct jit_ublock {
    uint32_t magic;
    uint32_t size;
    uint32_t free;
    struct jit_ublock *next;
};

static uint8_t jit_user_heap[JIT_UHEAP_SIZE];
static struct jit_ublock *jit_uheap_head;

static uint32_t jit_align8(uint32_t n) {
    return (n + 7u) & ~7u;
}

static void jit_uheap_reset(void) {
    jit_uheap_head = (struct jit_ublock *)jit_user_heap;
    jit_uheap_head->magic = JIT_UHEAP_MAGIC;
    jit_uheap_head->size = JIT_UHEAP_SIZE - sizeof(struct jit_ublock);
    jit_uheap_head->free = 1;
    jit_uheap_head->next = NULL;
}

static void jit_uheap_coalesce(void) {
    struct jit_ublock *b = jit_uheap_head;
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
    if (!jit_uheap_head) jit_uheap_reset();
    uint32_t need = jit_align8(size ? (uint32_t)size : 1u);
    struct jit_ublock *b = jit_uheap_head;
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
    uint8_t *addr = (uint8_t *)p;
    if (addr < jit_user_heap + sizeof(struct jit_ublock) ||
        addr >= jit_user_heap + JIT_UHEAP_SIZE) {
        lib_printf("[JITDBG] ufree reject ptr=%lx\n", (unsigned long)(uintptr_t)p);
        return;
    }
    struct jit_ublock *b = (struct jit_ublock *)(addr - sizeof(struct jit_ublock));
    if (b->magic != JIT_UHEAP_MAGIC || b->free) {
        lib_printf("[JITDBG] ufree invalid ptr=%lx\n", (unsigned long)(uintptr_t)p);
        return;
    }
    b->free = 1;
    jit_uheap_coalesce();
}

static void *ucalloc(uint32_t nmemb, uint32_t size) {
    uint32_t total = nmemb * size;
    if (size && total / size != nmemb) return NULL;
    void *p = umalloc(total ? total : 1u);
    if (p) memset(p, 0, total);
    return p;
}

static void *urealloc(void *ptr, size_t size) {
    if (!ptr) return umalloc(size);
    if (size == 0) {
        ufree(ptr);
        return NULL;
    }
    uint8_t *addr = (uint8_t *)ptr;
    if (addr < jit_user_heap + sizeof(struct jit_ublock) ||
        addr >= jit_user_heap + JIT_UHEAP_SIZE) {
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
    if (base) *base = (uint32_t)(uintptr_t)jit_user_heap;
    if (size) *size = JIT_UHEAP_SIZE;
    if (jit_uheap_head) {
        struct jit_ublock *b = jit_uheap_head;
        while (b) {
            if (b->magic != JIT_UHEAP_MAGIC) break;
            if (b->free) f += b->size;
            else u += b->size;
            n++;
            b = b->next;
        }
    } else {
        f = JIT_UHEAP_SIZE - sizeof(struct jit_ublock);
    }
    if (used) *used = u;
    if (free_bytes) *free_bytes = f;
    if (blocks) *blocks = n;
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
    jit_add_symbol_logged(s, "kmalloc", (void*)malloc);
    jit_add_symbol_logged(s, "kfree", (void*)free);
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

    jit_add_symbol_logged(s, "appfs_open", (void*)appfs_open);
    jit_add_symbol_logged(s, "appfs_read", (void*)appfs_read);
    jit_add_symbol_logged(s, "appfs_write", (void*)appfs_write);
    jit_add_symbol_logged(s, "appfs_close", (void*)appfs_close);
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

void os_jit_run(const char *source) {
    lib_printf("[JITDBG] os_jit_run source=%lx first='%c'\n",
               (unsigned long)(uintptr_t)source,
               source ? source[0] : '?');
    TCCState *s = tcc_new();
    if (!s) { lib_printf("JIT: Init Fail\n"); return; }
    jit_uheap_reset();
    lib_printf("[JITDBG] uheap=%lx size=%u\n",
               (unsigned long)(uintptr_t)jit_user_heap, JIT_UHEAP_SIZE);
    lib_printf("[JITDBG] tcc_new s=%lx\n", (unsigned long)(uintptr_t)s);
    tcc_set_error_func(s, NULL, tcc_error_report);
    tcc_set_options(s, "-nostdlib");
    lib_printf("[JITDBG] options=-nostdlib\n");
    lib_printf("[JITDBG] set_output_type rc=%d\n", tcc_set_output_type(s, TCC_OUTPUT_MEMORY));
    lib_printf("[JITDBG] sym printf=%lx print=%lx get_ticks=%lx\n",
               (unsigned long)(uintptr_t)lib_printf,
               (unsigned long)(uintptr_t)lib_printf,
               (unsigned long)(uintptr_t)get_millisecond_timer);
    jit_add_os_symbols(s);
    char *compile_source = jit_make_source_with_prelude(source);
    if (!compile_source) {
        lib_printf("JIT: source alloc failed\n");
        tcc_delete(s);
        return;
    }
    lib_printf("[JITDBG] compile start\n");
    int compile_rc = tcc_compile_string(s, compile_source);
    free(compile_source);
    lib_printf("[JITDBG] compile rc=%d\n", compile_rc);
    if (compile_rc != -1) {
        lib_printf("[JITDBG] relocate start\n");
        int reloc_rc = tcc_relocate(s, TCC_RELOCATE_AUTO);
        lib_printf("[JITDBG] relocate rc=%d\n", reloc_rc);
        if (reloc_rc == 0) {
            int (*main_func)(void) = (int (*)(void))tcc_get_symbol(s, "main");
            lib_printf("[JITDBG] main=%lx\n", (unsigned long)(uintptr_t)main_func);
            if (main_func) {
                asm volatile("fence.i");
                lib_printf("[JITDBG] call main\n");
                main_func();
                lib_printf("[JITDBG] main returned\n");
            } else {
                lib_printf("JIT: main not found\n");
            }
        } else {
            lib_printf("JIT: relocate failed\n");
        }
    } else {
        lib_printf("JIT: compile failed\n");
    }
    tcc_delete(s);
}
