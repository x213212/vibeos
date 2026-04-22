#include "time.h"
#include "timer.h"
#include "os.h" // r_mhartid, CSR helpers

#ifndef HOST_BUILD_HOUR
#define HOST_BUILD_HOUR 0
#endif
#ifndef HOST_BUILD_MIN
#define HOST_BUILD_MIN 0
#endif
#ifndef HOST_BUILD_SEC
#define HOST_BUILD_SEC 0
#endif

// qemu virt CLINT mtime runs at 10MHz in the normal configuration used by the
// current Makefile (no -icount), so 1ms = 10000 ticks.
#define MTIME_TICKS_PER_MS 10000ULL
#define INTERVAL 100000ULL
static inline unsigned long long read_mtime_real(void) {
    volatile uint32_t *mtime = (volatile uint32_t *)(uintptr_t)CLINT_MTIME;
    uint32_t hi, lo;
    do {
        hi = mtime[1];
        lo = mtime[0];
    } while (mtime[1] != hi); // low wrap 改變 hi 的話 retry
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}


// ---------- safe 寫 mtimecmp（RV32） ----------
static void set_next_timer(int id, unsigned long long next) {
    volatile unsigned int *mtimecmp = (volatile unsigned int *)CLINT_MTIMECMP(id);
    unsigned int hi = (unsigned int)(next >> 32);
    unsigned int lo = (unsigned int)(next & 0xFFFFFFFFULL);

    // Optional: 如果在 interrupt context 可能重入，可外層先關 MTIE
    unsigned int old_mie = r_mie();
    w_mie(old_mie & ~MIE_MTIE); // 暫時關 timer interrupt

    // safe update sequence: upper large, lower, then target upper
    mtimecmp[1] = 0xFFFFFFFF;
    mtimecmp[0] = lo;
    mtimecmp[1] = hi;

    w_mie(old_mie); // restore original mie (可能重新 enable)
}

// ---------- state ----------
static int timer_count = 0;
static uint32_t lwip_rand_state = 0x6d2b79f5U;

void timer_init()
{
    int id = r_mhartid();

    // 讀當前時間並 schedule 下一次
    unsigned long long now = read_mtime_real();
    set_next_timer(id, now + INTERVAL);

    // 啟用 machine timer interrupt source（假設 global MIE 先開）
    w_mie(r_mie() | MIE_MTIE);
}

void timer_handler()
{
    int id = r_mhartid();

    // 先讀時間
    unsigned long long now = read_mtime_real();
    timer_count++;

    // debug 可以暫時開
    // lib_printf("timer_handler #%d now=0x%llx\n", timer_count, now);

    // 安排下一次
    set_next_timer(id, now + INTERVAL);
}

// 轉毫秒（10MHz tick => ms = ticks / 10000）
unsigned int get_millisecond_timer(void) {
    unsigned long long mtime = read_mtime_real();
    return (unsigned int)(mtime / MTIME_TICKS_PER_MS);
}

unsigned int sys_now(void) {
    return get_millisecond_timer();
}

unsigned int get_wall_clock_seconds(void) {
    unsigned int base = ((unsigned int)HOST_BUILD_HOUR * 3600U) +
                        ((unsigned int)HOST_BUILD_MIN * 60U) +
                        (unsigned int)HOST_BUILD_SEC;
    return (base + (get_millisecond_timer() / 1000U)) % 86400U;
}

time_t time(time_t *t) {
    time_t now = (time_t)get_wall_clock_seconds();
    if (t != 0) {
        *t = now;
    }
    return now;
}

long long difftime(time_t time1, time_t time0) {
    return (long long)(time1 - time0);
}

unsigned int lwip_rand(void) {
    uint32_t x = lwip_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    lwip_rand_state = x ^ sys_now() ^ ((uint32_t)r_mhartid() << 16) ^ (uint32_t)timer_count;
    return lwip_rand_state;
}
