#ifndef __SYSCALL_TIME_H__
#define __SYSCALL_TIME_H__

#include "types.h"
#include "timer.h"

uint64 sys_gettimeofday(uint64 tv_addr);
int sys_getitimer(int which, uint64 old_value);
int sys_clock_gettime(uint64 tid, uint64 uaddr);
int sys_clock_getres(int clk_id, uint64 uaddr);
int sleep(timespec_t *req, timespec_t *rem);
int sys_settimer(int which, uint64 new_value, uint64 old_value);

#endif
