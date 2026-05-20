#pragma once


/* Event types always implicitly polled for.  These bits need not be set in
   `events', but they will appear in `revents' to indicate the status of
   the file descriptor.  */
#define POLLERR 0x008 /* Error condition.  */
#define POLLHUP 0x010 /* Hung up.  */
#define POLLNVAL 0x020 /* Invalid polling request.  */

struct pollfd {
    int fd; /* file descriptor */
    short events; /* requested events */
    short revents; /* returned events */
};

#define TICK_GRANULARITY 10L // ms
#define SEC2TICK(sec) ((sec) * 1000 / TICK_GRANULARITY)
#define MS2TICK(ms) ((ms) / TICK_GRANULARITY)
#define US2TICK(us) ((us) * TICK_GRANULARITY / 1000)
#define NS2TICK(ns) ((ns) / 1000 / 1000 / TICK_GRANULARITY)
static inline uint64 ts2ticks(struct timespec *ts) { return !ts ? 0 : NS2TICK(ts->tv_nsec) + SEC2TICK(ts->tv_sec); }