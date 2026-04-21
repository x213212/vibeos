#ifndef MINI_RISCV_OS_SYS_TIME_H
#define MINI_RISCV_OS_SYS_TIME_H

#include "../time.h"

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tzp);

#endif
