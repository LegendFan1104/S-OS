#include "types.h"
#include "platform.h"
#include "defs.h"
#include "param.h"
#include "mem/memlayout.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "mem/mem.h"
#include "sys/syslog.h"
#include "lib/string.h"


const char SYSNAME[] = "SOS\0";
const char NODENAME[] = "None\0";
const char VERSION[] = "4.15.0\0";
const char RELEASE[] = "4.15.0\0";
const char MACHINE[] = "Riscv\0";
const char DOMAINNAME[] = "None\0";
uint ticks0;


char syslogbuffer[1024];
int bufferlength = 0;

void initlogbuffer() {
    bufferlength = 0;
    strncpy(syslogbuffer, "[log]init done\n", 1024);
    bufferlength += strlen(syslogbuffer);
}


uint64 sys_times(void) {
    uint64 addr; 
    struct tms tm;
    struct proc *p = myproc();
    argaddr(0, &addr);
    tm.tms_utime = p->proc_tms.tms_utime;
    tm.tms_stime = p->proc_tms.tms_stime;
    tm.tms_cutime = p->proc_tms.tms_cutime;
    tm.tms_cstime = p->proc_tms.tms_cstime; 
    if(copyout(p->pagetable, addr, (char *)&tm, sizeof(tm)) < 0)
        return -1;
    
    return ticks;
}

uint64 sys_gettimeofday(void) {
    uint64 addr;
    argaddr(0, &addr);
    uint64 time = rdtime();
    struct timeval tv = TIME2TIMEVAL(time);
    if (copyout(myproc()->pagetable, addr, (char *) &tv, sizeof(tv)) < 0) {
        return -1;
    }
    return 0;
}

uint64 sys_sched_yield(void) {
    yield();
    return 0;
}

uint64 sys_uname(void) {
    uint64 addr;
    argaddr(0, &addr);
    struct utsname *uts = (struct utsname *)addr;
    pagetable_t pg = myproc()->pagetable;
    if (copyout(pg, (uint64)&(uts->sysname), (char*)SYSNAME, sizeof(SYSNAME)) < 0) {
        return -1;
    }
    if (copyout(pg, (uint64)&(uts->nodename), (char*)NODENAME, sizeof(NODENAME)) < 0) {
        return -1;
    }
    if (copyout(pg, (uint64)&(uts->release), (char*)RELEASE, sizeof(RELEASE)) < 0) {
        return -1;
    }
    if (copyout(pg, (uint64)&(uts->version), (char*)VERSION, sizeof(VERSION)) < 0) {
        return -1;
    }
    if (copyout(pg, (uint64)&(uts->machine), (char*)MACHINE, sizeof(MACHINE)) < 0) {
        return -1;
    }
    if (copyout(pg, (uint64)&(uts->domainname), (char*)DOMAINNAME, sizeof(DOMAINNAME)) < 0) {
        return -1;
    }
    return 0;
}

uint64 sys_nanosleep(void) {
    uint64 req_addr, rem_addr;
    argaddr(0, &req_addr);
    argaddr(1, &rem_addr);
    struct timespec req, rem;
    if (copyin(myproc()->pagetable, (char*)(&req), req_addr, sizeof(req)) < 0) {
        return -1;
    }
    if (copyin(myproc()->pagetable, (char*)(&rem), rem_addr, sizeof(rem)) < 0) {
        return -1;
    }
    uint64 n = req.tv_sec;
    acquire(&tickslock);
    ticks0 = ticks;
    while(ticks - ticks0 < n){
        if(killed(myproc())){
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

//用户空间随机数，使用rdtime()的低字节当做随机数
uint64 sys_getrandom(void) {
    uint64 ubuf;
    int bufsize;
    argaddr(0, &ubuf);
    argint(1, &bufsize);
    // printf("%p %d\n", ubuf, bufsize);
    char *randombuf = kmalloc(bufsize);
    for (int i=0;i<bufsize;i++) {
        randombuf[i] = rdtime() & 0xFF;
    }
    if (copyout(myproc()->pagetable, ubuf, (char*)randombuf, bufsize) < 0) {
        return -1;
    }
    kfree(randombuf);

    return bufsize;
}

uint64 sys_clock_gettime(void) {
    uint64 addr;
    int clockid;
    argint(0, &clockid);
    argaddr(1, &addr);
    if(clockid == CLOCK_REALTIME){
        struct timespec ts;
        uint64 timestamp = rdtime();
        // printf("[sys_clock_gettime] timestamp: %p, ticks: %p\n", timestamp, ticks);
        ts.tv_sec = timestamp / FREQUENCY;
        ts.tv_nsec = (timestamp % FREQUENCY) * 1000000000 / FREQUENCY;
        // printf("[sys_clock_gettime] sec: %d, nsec: %d\n", ts.sec, ts.nsec);

        if(copyout(myproc()->pagetable, (uint64)addr, (char*)&ts, sizeof(ts)) < 0)
            return -1;
    }
    return 0;
}

uint64 sys_syslog(void) {
    int type, len, source;
    uint64 buf;
    argint(0, &type);
    argint(2, &len);
    argaddr(1, &buf);

    if (type == SYSLOG_ACTION_READ_ALL) {
        if (either_copyout(1, buf, syslogbuffer, bufferlength) < 0)
            return -1;
        return bufferlength;
    } else if (type == SYSLOG_ACTION_SIZE_BUFFER)
        return sizeof(syslogbuffer);

    return 0;
}








