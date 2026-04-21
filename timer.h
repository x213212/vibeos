#ifndef __TIMER_H__
#define __TIMER_H__

#include "riscv.h"
#include "sys.h"
#include "lib.h"
#include "task.h"

extern void timer_handler();
extern void timer_init();
extern unsigned int get_millisecond_timer(void);
extern unsigned int sys_now(void);
extern unsigned int get_wall_clock_seconds(void);

#endif
