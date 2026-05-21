#include "types.h"
#include "param.h"
#include "platform.h"
#include "defs.h"
#include "proc/proc.h"
#include "mem/mem.h"
#include "mem/memlayout.h"
#include "lib/string.h"
#include "fs/procfs.h"
#include "fs/vfs/file.h"
#include "fs/stat.h"

#ifndef T_LNK
#define T_LNK     3   // Symbolic link
#endif

extern struct proc proc[];
extern int nproc;
extern uint ticks;
extern struct spinlock tickslock;

// 简单的整数转字符串函数
static int itoa(int n, char *buf) {
    int i = 0, sign = n;
    char tmp[16];
    
    if (n < 0) n = -n;
    
    do {
        tmp[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);
    
    if (sign < 0) tmp[i++] = '-';
    
    int j;
    for (j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    buf[j] = '\0';
    return j;
}

// 简单的无符号长整型转字符串函数
static int ultoa(uint64 n, char *buf) {
    int i = 0;
    char tmp[32];
    
    do {
        tmp[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);
    
    int j;
    for (j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    buf[j] = '\0';
    return j;
}

// 判断是否是 procfs 路径
int is_procfs_path(const char *path) {
    return strncmp(path, "/proc", 5) == 0;
}

// 获取 procfs 文件类型
enum procfs_type get_procfs_type(const char *path, int *pid) {
    *pid = -1;
    
    if (strcmp(path, "/proc") == 0) {
        return PROCFS_ROOT;
    }
    if (strcmp(path, "/proc/self") == 0) {
        return PROCFS_SELF;
    }
    if (strcmp(path, "/proc/uptime") == 0) {
        return PROCFS_UPTIME;
    }
    if (strcmp(path, "/proc/meminfo") == 0) {
        return PROCFS_MEMINFO;
    }
    if (strcmp(path, "/proc/stat") == 0) {
        return PROCFS_STAT;
    }
    if (strcmp(path, "/proc/loadavg") == 0) {
        return PROCFS_LOADAVG;
    }
    if (strcmp(path, "/proc/version") == 0) {
        return PROCFS_VERSION;
    }
    if (strcmp(path, "/proc/mounts") == 0) {
        return PROCFS_MOUNTS;
    }
    if (strcmp(path, "/proc/interrupts") == 0) {
        return PROCFS_INTERRUPTS;
    }
    
    // 检查 /proc/[pid] 格式
    if (strncmp(path, "/proc/", 6) == 0) {
        const char *p = path + 6;
        if (*p >= '0' && *p <= '9') {
            int id = 0;
            while (*p >= '0' && *p <= '9') {
                id = id * 10 + (*p - '0');
                p++;
            }
            
            if (*p == '\0') {
                *pid = id;
                return PROCFS_PID_DIR;
            }
            if (strcmp(p, "/stat") == 0) {
                *pid = id;
                return PROCFS_PID_STAT;
            }
            if (strcmp(p, "/cmdline") == 0) {
                *pid = id;
                return PROCFS_PID_CMDLINE;
            }
            if (strcmp(p, "/exe") == 0) {
                *pid = id;
                return PROCFS_PID_EXE;
            }
        }
    }
    
    return PROCFS_DUMMY;
}

// 生成 /proc/uptime 内容
static int gen_uptime(char *buf, int maxlen) {
    uint uptime = 0;
    acquire(&tickslock);
    uptime = ticks;
    release(&tickslock);
    
    // 转换为秒（假设 ticks 是 10ms 间隔）
    uint seconds = uptime / 100;
    uint remainder = uptime % 100;
    
    int n = 0;
    n += itoa(seconds, buf + n);
    buf[n++] = '.';
    if (remainder < 10) buf[n++] = '0';
    n += itoa(remainder, buf + n);
    buf[n++] = ' ';
    buf[n++] = '0';
    buf[n++] = '.';
    buf[n++] = '0';
    buf[n++] = '0';
    buf[n++] = '\n';
    buf[n] = '\0';
    
    return n;
}

// 生成 /proc/meminfo 内容
static int gen_meminfo(char *buf, int maxlen) {
    uint64 total_pages = PHYSTOP / PGSIZE;
    uint64 free_pages = 0;
    
    // 获取内存信息 - 这里简化处理
    free_pages = total_pages / 2;  // 假设一半空闲
    
    int n = 0;
    // MemTotal
    strncpy(buf + n, "MemTotal:     ", maxlen - n);
    n += strlen("MemTotal:     ");
    n += ultoa(total_pages * 4, buf + n);
    strncpy(buf + n, " kB\n", maxlen - n);
    n += strlen(" kB\n");
    
    // MemFree
    strncpy(buf + n, "MemFree:      ", maxlen - n);
    n += strlen("MemFree:      ");
    n += ultoa(free_pages * 4, buf + n);
    strncpy(buf + n, " kB\n", maxlen - n);
    n += strlen(" kB\n");
    
    // MemAvailable
    strncpy(buf + n, "MemAvailable: ", maxlen - n);
    n += strlen("MemAvailable: ");
    n += ultoa(free_pages * 4, buf + n);
    strncpy(buf + n, " kB\n", maxlen - n);
    n += strlen(" kB\n");
    
    buf[n] = '\0';
    return n;
}

// 生成 /proc/stat 内容
static int gen_stat(char *buf, int maxlen) {
    uint uptime = 0;
    acquire(&tickslock);
    uptime = ticks;
    release(&tickslock);
    
    int n = 0;
    
    // CPU 统计
    strncpy(buf + n, "cpu  0 0 0 0 0 0 0 ", maxlen - n);
    n += strlen("cpu  0 0 0 0 0 0 0 ");
    n += itoa(uptime, buf + n);
    strncpy(buf + n, " 0 0\n", maxlen - n);
    n += strlen(" 0 0\n");
    
    // 进程统计
    int proc_count = 0;
    for (int i = 0; i < NPROC; i++) {
        if (proc[i].state != UNUSED) {
            proc_count++;
        }
    }
    strncpy(buf + n, "procs_running ", maxlen - n);
    n += strlen("procs_running ");
    n += itoa(proc_count, buf + n);
    buf[n++] = '\n';
    strncpy(buf + n, "procs_blocked 0\n", maxlen - n);
    n += strlen("procs_blocked 0\n");
    
    buf[n] = '\0';
    return n;
}

// 生成 /proc/loadavg 内容
static int gen_loadavg(char *buf, int maxlen) {
    const char *s = "0.00 0.00 0.00 1/1 1\n";
    int len = strlen(s);
    if (len >= maxlen) len = maxlen - 1;
    strncpy(buf, s, len);
    buf[len] = '\0';
    return len;
}

// 生成 /proc/version 内容
static int gen_version(char *buf, int maxlen) {
    const char *s = "Linux version 4.15.0 (SOS)\n";
    int len = strlen(s);
    if (len >= maxlen) len = maxlen - 1;
    strncpy(buf, s, len);
    buf[len] = '\0';
    return len;
}

// 生成 /proc/mounts 内容
static int gen_mounts(char *buf, int maxlen) {
    const char *s = "rootfs / rootfs rw 0 0\n/dev/root / ext4 rw,relatime 0 0\nproc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n";
    int len = strlen(s);
    if (len >= maxlen) len = maxlen - 1;
    strncpy(buf, s, len);
    buf[len] = '\0';
    return len;
}

// 生成 /proc/[pid]/stat 内容
static int gen_pid_stat(int pid, char *buf, int maxlen) {
    struct proc *p = 0;
    
    // 查找进程
    for (int i = 0; i < NPROC; i++) {
        if (proc[i].pid == pid && proc[i].state != UNUSED) {
            p = &proc[i];
            break;
        }
    }
    
    if (!p) {
        return -1;
    }
    
    acquire(&p->lock);
    
    // 获取进程状态字符
    char state_char = 'R';
    switch (p->state) {
        case SLEEPING:  state_char = 'S'; break;
        case RUNNABLE:  state_char = 'R'; break;
        case RUNNING:   state_char = 'R'; break;
        case ZOMBIE:    state_char = 'Z'; break;
        default:        state_char = 'S'; break;
    }
    
    int n = 0;
    // pid
    n += itoa(p->pid, buf + n);
    buf[n++] = ' ';
    buf[n++] = '(';
    // comm
    int namelen = strlen(p->name);
    if (namelen > 15) namelen = 15;
    strncpy(buf + n, p->name, namelen);
    n += namelen;
    buf[n++] = ')';
    buf[n++] = ' ';
    // state
    buf[n++] = state_char;
    buf[n++] = ' ';
    // ppid
    n += itoa(p->parent ? p->parent->pid : 0, buf + n);
    buf[n++] = ' ';
    // pgrp
    n += itoa(p->pid, buf + n);
    buf[n++] = ' ';
    // session
    n += itoa(p->pid, buf + n);
    buf[n++] = ' ';
    // tty_nr
    buf[n++] = '-';
    buf[n++] = '1';
    buf[n++] = ' ';
    // tpgid
    buf[n++] = '0';
    buf[n++] = ' ';
    // flags
    buf[n++] = '0';
    buf[n++] = ' ';
    // minflt
    buf[n++] = '0';
    buf[n++] = ' ';
    // cminflt
    buf[n++] = '0';
    buf[n++] = ' ';
    // majflt
    buf[n++] = '0';
    buf[n++] = ' ';
    // cmajflt
    buf[n++] = '0';
    buf[n++] = ' ';
    // utime
    n += itoa(p->proc_tms.tms_utime, buf + n);
    buf[n++] = ' ';
    // stime
    n += itoa(p->proc_tms.tms_stime, buf + n);
    buf[n++] = ' ';
    // cutime
    buf[n++] = '0';
    buf[n++] = ' ';
    // cstime
    buf[n++] = '0';
    buf[n++] = ' ';
    // priority
    buf[n++] = '0';
    buf[n++] = ' ';
    // nice
    buf[n++] = '0';
    buf[n++] = ' ';
    // num_threads
    buf[n++] = '1';
    buf[n++] = ' ';
    // itrealvalue
    buf[n++] = '0';
    buf[n++] = ' ';
    // starttime
    buf[n++] = '0';
    buf[n++] = ' ';
    // vsize
    buf[n++] = '0';
    buf[n++] = ' ';
    // rss
    buf[n++] = '0';
    buf[n++] = ' ';
    // rsslim
    buf[n++] = '0';
    buf[n++] = ' ';
    // startcode
    buf[n++] = '0';
    buf[n++] = ' ';
    // endcode
    buf[n++] = '0';
    buf[n++] = ' ';
    // startstack
    buf[n++] = '0';
    buf[n++] = ' ';
    // kstkesp
    buf[n++] = '0';
    buf[n++] = ' ';
    // kstkeip
    buf[n++] = '0';
    buf[n++] = ' ';
    // signal
    buf[n++] = '0';
    buf[n++] = ' ';
    // blocked
    buf[n++] = '0';
    buf[n++] = ' ';
    // sigignore
    buf[n++] = '0';
    buf[n++] = ' ';
    // sigcatch
    buf[n++] = '0';
    buf[n++] = ' ';
    // wchan
    buf[n++] = '0';
    buf[n++] = ' ';
    // nswap
    buf[n++] = '0';
    buf[n++] = ' ';
    // cnswap
    buf[n++] = '0';
    buf[n++] = ' ';
    // exit_signal
    buf[n++] = '0';
    buf[n++] = ' ';
    // processor
    buf[n++] = '0';
    buf[n++] = ' ';
    // rt_priority
    buf[n++] = '0';
    buf[n++] = ' ';
    // policy
    buf[n++] = '0';
    buf[n++] = ' ';
    // delayacct_blkio_ticks
    buf[n++] = '0';
    buf[n++] = ' ';
    // guest_time
    buf[n++] = '0';
    buf[n++] = ' ';
    // cguest_time
    buf[n++] = '0';
    buf[n++] = '\n';
    buf[n] = '\0';
    
    release(&p->lock);
    return n;
}

// 生成 /proc/[pid]/cmdline 内容
static int gen_pid_cmdline(int pid, char *buf, int maxlen) {
    struct proc *p = 0;
    
    for (int i = 0; i < NPROC; i++) {
        if (proc[i].pid == pid && proc[i].state != UNUSED) {
            p = &proc[i];
            break;
        }
    }
    
    if (!p) {
        return -1;
    }
    
    acquire(&p->lock);
    int len = strlen(p->name);
    if (len >= maxlen) {
        len = maxlen - 1;
    }
    memcpy(buf, p->name, len);
    buf[len] = '\0';
    release(&p->lock);
    
    return len + 1;  // +1 for null terminator
}

// 生成 procfs 文件内容
int procfs_generate_content(struct procfs_file *pf) {
    switch (pf->type) {
        case PROCFS_UPTIME:
            pf->buflen = gen_uptime(pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_MEMINFO:
            pf->buflen = gen_meminfo(pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_STAT:
            pf->buflen = gen_stat(pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_LOADAVG:
            pf->buflen = gen_loadavg(pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_VERSION:
            pf->buflen = gen_version(pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_MOUNTS:
            pf->buflen = gen_mounts(pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_PID_STAT:
            pf->buflen = gen_pid_stat(pf->pid, pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_PID_CMDLINE:
            pf->buflen = gen_pid_cmdline(pf->pid, pf->buf, sizeof(pf->buf));
            break;
        case PROCFS_INTERRUPTS:
            pf->buf[0] = '\0';
            pf->buflen = 0;
            break;
        default:
            pf->buf[0] = '\0';
            pf->buflen = 0;
            break;
    }
    
    return pf->buflen;
}

// 打开 procfs 文件
int procfs_open(const char *path, struct file *f) {
    struct procfs_file *pf = kmalloc(sizeof(struct procfs_file));
    if (!pf) {
        return -1;
    }
    
    pf->type = get_procfs_type(path, &pf->pid);
    pf->offset = 0;
    
    // 处理 /proc/self
    if (pf->type == PROCFS_SELF) {
        pf->pid = myproc()->pid;
        pf->type = PROCFS_PID_DIR;
    }
    
    // 生成文件内容
    if (procfs_generate_content(pf) < 0 && pf->type != PROCFS_ROOT && pf->type != PROCFS_PID_DIR) {
        kfree(pf);
        return -1;
    }
    
    f->f_type = FD_PROCFS;
    f->private_data = pf;
    f->f_pos = 0;
    
    return 0;
}

// 读取 procfs 文件
int procfs_read(struct file *f, uint64 addr, int n) {
    struct procfs_file *pf = (struct procfs_file *)f->private_data;
    if (!pf) {
        return -1;
    }
    
    // 目录不能读取
    if (pf->type == PROCFS_ROOT || pf->type == PROCFS_PID_DIR) {
        return -1;
    }
    
    // 计算可读取的字节数
    int available = pf->buflen - f->f_pos;
    if (available <= 0) {
        return 0;
    }
    
    if (n > available) {
        n = available;
    }
    
    // 复制数据到用户空间
    if (copyout(myproc()->pagetable, addr, pf->buf + f->f_pos, n) < 0) {
        return -1;
    }
    
    f->f_pos += n;
    return n;
}

// 读取 procfs 目录
int procfs_getdents(struct file *f, struct linux_dirent64 *dirp, int count) {
    struct procfs_file *pf = (struct procfs_file *)f->private_data;
    if (!pf) {
        return -1;
    }
    
    if (pf->type != PROCFS_ROOT && pf->type != PROCFS_PID_DIR) {
        return -1;
    }
    
    struct linux_dirent64 *d = dirp;
    int totlen = 0;
    int index = 0;
    
    // 添加 . 和 ..
    const char *std_entries[] = { ".", ".." };
    for (int i = 0; i < 2; i++) {
        int namelen = strlen(std_entries[i]);
        int reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
        if (reclen < sizeof(struct linux_dirent64)) {
            reclen = sizeof(struct linux_dirent64);
        }
        
        if (totlen + reclen > count) {
            break;
        }
        
        strncpy(d->d_name, std_entries[i], MAXPATH);
        d->d_type = T_DIR;
        d->d_ino = 1;
        d->d_off = ++index;
        d->d_reclen = reclen;
        
        totlen += reclen;
        d = (struct linux_dirent64 *)((char *)d + reclen);
    }
    
    if (pf->type == PROCFS_ROOT) {
        // 添加 /proc/self
        {
            int namelen = strlen("self");
            int reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
            if (reclen < sizeof(struct linux_dirent64)) {
                reclen = sizeof(struct linux_dirent64);
            }
            
            if (totlen + reclen <= count) {
                strncpy(d->d_name, "self", MAXPATH);
                d->d_type = T_LNK;
                d->d_ino = 2;
                d->d_off = ++index;
                d->d_reclen = reclen;
                
                totlen += reclen;
                d = (struct linux_dirent64 *)((char *)d + reclen);
            }
        }
        
        // 添加系统文件
        const char *sys_files[] = { "uptime", "meminfo", "stat", "loadavg", "version", "mounts", "interrupts" };
        for (int i = 0; i < 7; i++) {
            int namelen = strlen(sys_files[i]);
            int reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
            if (reclen < sizeof(struct linux_dirent64)) {
                reclen = sizeof(struct linux_dirent64);
            }
            
            if (totlen + reclen > count) {
                break;
            }
            
            strncpy(d->d_name, sys_files[i], MAXPATH);
            d->d_type = T_FILE;
            d->d_ino = 100 + i;
            d->d_off = ++index;
            d->d_reclen = reclen;
            
            totlen += reclen;
            d = (struct linux_dirent64 *)((char *)d + reclen);
        }
        
        // 添加进程目录
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state == UNUSED) {
                continue;
            }
            
            char pid_str[16];
            itoa(proc[i].pid, pid_str);
            int namelen = strlen(pid_str);
            int reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
            if (reclen < sizeof(struct linux_dirent64)) {
                reclen = sizeof(struct linux_dirent64);
            }
            
            if (totlen + reclen > count) {
                break;
            }
            
            strncpy(d->d_name, pid_str, MAXPATH);
            d->d_type = T_DIR;
            d->d_ino = 1000 + proc[i].pid;
            d->d_off = ++index;
            d->d_reclen = reclen;
            
            totlen += reclen;
            d = (struct linux_dirent64 *)((char *)d + reclen);
        }
    } else if (pf->type == PROCFS_PID_DIR) {
        // 添加 /proc/[pid] 下的文件
        const char *pid_files[] = { "stat", "cmdline", "exe" };
        for (int i = 0; i < 3; i++) {
            int namelen = strlen(pid_files[i]);
            int reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
            if (reclen < sizeof(struct linux_dirent64)) {
                reclen = sizeof(struct linux_dirent64);
            }
            
            if (totlen + reclen > count) {
                break;
            }
            
            strncpy(d->d_name, pid_files[i], MAXPATH);
            d->d_type = T_FILE;
            d->d_ino = 200 + i;
            d->d_off = ++index;
            d->d_reclen = reclen;
            
            totlen += reclen;
            d = (struct linux_dirent64 *)((char *)d + reclen);
        }
    }
    
    return totlen;
}
