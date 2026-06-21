#ifdef RISCV
#include "riscv.h"
#else
#include "loongarch.h"
#endif

#include "fs_defs.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"

#include "ext4_oflags.h"
#include "errno-base.h"

#include "process.h"
#include "vfs_ext4.h"
#include "string.h"

#include "cpu.h"
#include "vmem.h"
#include "print.h"

struct devsw devsw[NDEV];
char zeros[ZERO_BYTES];

static int
busybox_virtual_oom_path_kind(const char *path)
{
    const char *prefix = "/proc/";
    const char *p;

    if (!strcmp(path, "/proc/self/oom_score_adj") ||
        !strcmp(path, "/proc/thread-self/oom_score_adj"))
        return 1;
    if (!strcmp(path, "/proc/self/oom_adj") ||
        !strcmp(path, "/proc/thread-self/oom_adj"))
        return 2;
    if (strncmp(path, prefix, strlen(prefix)) != 0)
        return 0;
    p = path + strlen(prefix);
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    if (!strcmp(p, "/oom_score_adj"))
        return 1;
    if (!strcmp(p, "/oom_adj"))
        return 2;
    return 0;
}

static int
busybox_virtual_oom_score_adj_pid(const char *path)
{
    const char *prefix = "/proc/";
    const char *p;
    int pid = 0;

    if (!strcmp(path, "/proc/self/oom_score_adj") ||
        !strcmp(path, "/proc/thread-self/oom_score_adj"))
    {
        proc_t *current = myproc();
        return current ? current->pid : -1;
    }
    if (strncmp(path, prefix, strlen(prefix)) != 0)
        return -1;
    p = path + strlen(prefix);
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9')
    {
        pid = pid * 10 + (*p - '0');
        p++;
    }
    return busybox_virtual_oom_path_kind(path) ? pid : -1;
}

static int
busybox_virtual_is_pid_oom_score_adj(const char *path)
{
    return busybox_virtual_oom_path_kind(path) != 0 &&
           busybox_virtual_oom_score_adj_pid(path) >= 0;
}

static int
oom_adj_from_score_adj(int score_adj)
{
    if (score_adj <= -1000)
        return -17;
    if (score_adj >= 1000)
        return 15;
    if (score_adj < 0)
        return -(((-score_adj) * 17 + 999) / 1000);
    return (score_adj * 15) / 1000;
}

static int
score_adj_from_oom_adj(int oom_adj)
{
    if (oom_adj <= -17)
        return -1000;
    if (oom_adj >= 15)
        return 1000;
    if (oom_adj < 0)
        return -(((-oom_adj) * 1000) / 17);
    return (oom_adj * 1000) / 15;
}

static int
busybox_virtual_is_dir(const char *path)
{
    const char *prefix = "/proc/";
    const char *p;

    if (!strcmp(path, "/proc") ||
        !strcmp(path, "/proc/self") ||
        !strcmp(path, "/proc/sys") ||
        !strcmp(path, "/proc/sys/kernel") ||
        !strcmp(path, "/proc/sys/fs") ||
        !strcmp(path, "/etc"))
        return 1;
    if (strncmp(path, prefix, strlen(prefix)) != 0)
        return 0;
    p = path + strlen(prefix);
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    return *p == '\0';
}

static void
busybox_virtual_append_decimal(char *buf, int *pos, int value);

static int
busybox_virtual_proc_pid_from_path(const char *path, const char *suffix)
{
    const char *prefix = "/proc/";
    const char *p;
    int pid = 0;

    if (!strcmp(path, "/proc/self/stat") && !strcmp(suffix, "/stat"))
    {
        proc_t *current = myproc();
        return current ? current->pid : -1;
    }
    if (strncmp(path, prefix, strlen(prefix)) != 0)
        return -1;
    p = path + strlen(prefix);
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9')
    {
        pid = pid * 10 + (*p - '0');
        p++;
    }
    return !strcmp(p, suffix) ? pid : -1;
}

static proc_t *
busybox_find_proc_by_pid_local(int pid)
{
    extern struct proc pool[NPROC];
    proc_t *p;

    for (p = pool; p < &pool[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->state != UNUSED && p->pid == pid)
        {
            release(&p->lock);
            return p;
        }
        release(&p->lock);
    }
    return 0;
}

static const char *
busybox_virtual_proc_pid_stat(uint64 *len, int pid)
{
    static char statbuf[256];
    proc_t *p = busybox_find_proc_by_pid_local(pid);
    int pos = 0;
    int ppid = 0;
    int pgid = pid;
    int sid = pid;
    int i;
    const char *prefix1 = "";
    const char *prefix2 = " (busybox) R ";
    const char *suffix =
        " 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

    if (p)
    {
        ppid = (p->parent != 0) ? p->parent->pid : 0;
        pgid = p->pgid;
        sid = p->sid;
    }

    for (i = 0; prefix1[i]; i++)
        statbuf[pos++] = prefix1[i];
    busybox_virtual_append_decimal(statbuf, &pos, pid);
    for (i = 0; prefix2[i]; i++)
        statbuf[pos++] = prefix2[i];
    busybox_virtual_append_decimal(statbuf, &pos, ppid);
    statbuf[pos++] = ' ';
    busybox_virtual_append_decimal(statbuf, &pos, pgid);
    statbuf[pos++] = ' ';
    busybox_virtual_append_decimal(statbuf, &pos, sid);
    for (i = 0; suffix[i]; i++)
        statbuf[pos++] = suffix[i];
    statbuf[pos] = '\0';
    *len = pos;
    return statbuf;
}

static void
busybox_virtual_append_decimal(char *buf, int *pos, int value)
{
    char digits[16];
    int n = 0;
    int tmp = value;

    if (tmp == 0)
    {
        buf[(*pos)++] = '0';
        return;
    }
    if (tmp < 0)
    {
        buf[(*pos)++] = '-';
        tmp = -tmp;
    }
    while (tmp > 0 && n < (int)sizeof(digits))
    {
        digits[n++] = (char)('0' + (tmp % 10));
        tmp /= 10;
    }
    while (n > 0)
        buf[(*pos)++] = digits[--n];
}

static const char *
busybox_virtual_proc_self_status(uint64 *len)
{
    static char status[256];
    proc_t *current = myproc();
    int pid = current ? current->pid : 1;
    int ppid = (current && current->parent) ? current->parent->pid : 0;
    int pos = 0;
    const char *prefix1 = "Name:\tbusybox\nState:\tR (running)\nTgid:\t";
    const char *prefix2 = "\nPid:\t";
    const char *prefix3 = "\nPPid:\t";
    const char *suffix =
        "\nUid:\t0\t0\t0\t0\n"
        "Gid:\t0\t0\t0\t0\n"
        "Groups:\t0\n";
    int i;

    for (i = 0; prefix1[i]; i++)
        status[pos++] = prefix1[i];
    busybox_virtual_append_decimal(status, &pos, pid);
    for (i = 0; prefix2[i]; i++)
        status[pos++] = prefix2[i];
    busybox_virtual_append_decimal(status, &pos, pid);
    for (i = 0; prefix3[i]; i++)
        status[pos++] = prefix3[i];
    busybox_virtual_append_decimal(status, &pos, ppid);
    for (i = 0; suffix[i]; i++)
        status[pos++] = suffix[i];
    status[pos] = '\0';
    *len = pos;
    return status;
}

static const char *
busybox_virtual_content(const char *path, uint64 *len)
{
#ifdef RISCV
    static const char meminfo[] =
        "MemTotal:       1024000 kB\n"
        "MemFree:           1024 kB\n"
        "MemAvailable:      1024 kB\n"
        "Buffers:              0 kB\n"
        "Cached:               0 kB\n"
        "SwapCached:           0 kB\n"
        "Active:               0 kB\n"
        "Inactive:             0 kB\n"
        "SwapTotal:            0 kB\n"
        "SwapFree:             0 kB\n";
    static const char cpuinfo[] =
        "processor\t: 0\n"
        "hart\t\t: 0\n"
        "isa\t\t: rv64imafdch\n";
#else
    static const char meminfo[] =
        "MemTotal:        409600 kB\n"
        "MemFree:           1024 kB\n"
        "MemAvailable:      1024 kB\n"
        "Buffers:              0 kB\n"
        "Cached:               0 kB\n"
        "SwapCached:           0 kB\n"
        "Active:               0 kB\n"
        "Inactive:             0 kB\n"
        "SwapTotal:            0 kB\n"
        "SwapFree:             0 kB\n";
    static const char cpuinfo[] =
        "processor\t: 0\n"
        "cpu family\t: loongarch64\n";
#endif
    static const char mounts[] =
        "rootfs / ext4 rw,relatime 0 0\n"
        "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n";
    static const char config_gz[] = "";
    static const char self_maps[] =
        "00400000-00401000 r-xp 00000000 00:00 0 /busybox\n"
        "7fff0000-80000000 rw-p 00000000 00:00 0 [stack]\n";
    static const char self_exe[] = "/busybox";
    static const char pid_max[] = "32768\n";
    static const char pipe_user_pages_soft[] = "16384\n";
    static const char rtc[] = "";
    static const char passwd[] =
        "root:x:0:0:root:/root:/bin/sh\n"
        "nobody:x:65534:65534:nobody:/:/sbin/nologin\n";
    static const char group[] =
        "root:x:0:\n"
        "nogroup:x:65534:\n";
    const char *content = "";

    if (!strcmp(path, "/proc/meminfo"))
        content = meminfo;
    else if (!strcmp(path, "/proc/mounts"))
        content = mounts;
    else if (!strcmp(path, "/proc/cpuinfo"))
        content = cpuinfo;
    else if (!strcmp(path, "/proc/config.gz"))
        content = config_gz;
    else if (!strcmp(path, "/proc/self/maps"))
        content = self_maps;
    else if (!strcmp(path, "/proc/self/exe"))
        content = self_exe;
    else if (!strcmp(path, "/proc/self/status"))
        return busybox_virtual_proc_self_status(len);
    else if (!strcmp(path, "/proc/self/stat"))
        return busybox_virtual_proc_pid_stat(len, myproc() ? myproc()->pid : 1);
    else if (!strcmp(path, "/proc/sys/kernel/pid_max"))
        content = pid_max;
    else if (!strcmp(path, "/proc/sys/fs/pipe-user-pages-soft"))
        content = pipe_user_pages_soft;
    else if (!strcmp(path, "/dev/misc/rtc"))
        content = rtc;
    else if (!strcmp(path, "/etc/passwd"))
        content = passwd;
    else if (!strcmp(path, "/etc/group"))
        content = group;
    else if (busybox_virtual_proc_pid_from_path(path, "/stat") >= 0)
        return busybox_virtual_proc_pid_stat(len, busybox_virtual_proc_pid_from_path(path, "/stat"));

    if (busybox_virtual_is_pid_oom_score_adj(path))
    {
        static char oom_score_adj_buf[16];
        int value = 0;
        int kind = busybox_virtual_oom_path_kind(path);
        int pid = busybox_virtual_oom_score_adj_pid(path);
        int pos = 0;
        int tmp;
        char digits[12];
        int n = 0;

        if (proc_get_oom_score_adj(pid, &value) < 0)
            value = 0;
        if (kind == 2)
            value = oom_adj_from_score_adj(value);
        if (value < 0)
        {
            oom_score_adj_buf[pos++] = '-';
            tmp = -value;
        }
        else
            tmp = value;
        do
        {
            digits[n++] = (char)('0' + (tmp % 10));
            tmp /= 10;
        } while (tmp > 0 && n < (int)sizeof(digits));
        while (n > 0 && pos < (int)sizeof(oom_score_adj_buf) - 2)
            oom_score_adj_buf[pos++] = digits[--n];
        oom_score_adj_buf[pos++] = '\n';
        oom_score_adj_buf[pos] = '\0';
        content = oom_score_adj_buf;
    }

    *len = strlen(content);
    return content;
}

static int
busybox_virtual_read(struct file *f, uint64 addr, int n)
{
    uint64 len = 0;
    const char *content = busybox_virtual_content(f->f_path, &len);
    int remain;
    int to_copy;

    if (busybox_virtual_is_dir(f->f_path))
        return 0;
    if (f->f_pos >= len)
        return 0;

    remain = (int)(len - f->f_pos);
    to_copy = min(n, remain);
    if (to_copy <= 0)
        return 0;
    if (copyout(myproc()->pagetable, addr, (char *)(content + f->f_pos), to_copy) < 0)
        return -EFAULT;
    f->f_pos += to_copy;
    return to_copy;
}

static int
busybox_virtual_statx(struct file *f, struct statx *st)
{
    uint64 len = 0;

    memset(st, 0, sizeof(*st));
    st->stx_blksize = 4096;
    st->stx_nlink = busybox_virtual_is_dir(f->f_path) ? 2 : 1;
    st->stx_uid = 0;
    st->stx_gid = 0;
    st->stx_mode = busybox_virtual_is_dir(f->f_path) ? 0040777 : 0100644;
    st->stx_size = 0;
    st->stx_blocks = 0;
    st->stx_ino = 1;

    if (!busybox_virtual_is_dir(f->f_path))
        busybox_virtual_content(f->f_path, &len);
    st->stx_size = len;
    return 0;
}

/**
 * @brief 文件表
 * 
 * 该结构体用于管理系统中打开的文件描述符。
 * 包含一个锁和一个文件数组，锁用于保护对文件数组的访问。
 */
struct {
    struct spinlock lock;
    struct file file[NFILE];
} ftable;

/**
 * @brief vnode表
 * 
 * 该结构体用于管理文件系统中的vnode(包括对应文件系统的目录和文件)。
 * 包含一个锁和一个目录数组，锁用于保护对数组的访问。
 */
struct {
    struct spinlock lock;
    file_vnode_t vnodes[NFILE];
    int valid[NFILE];
    bool isdir[NFILE];
} file_vnode_table;

/**
 * @brief 分配文件结构体
 * 
 * Allocate a file structure.
 * 
 * @return struct file* 
 */
struct file*
filealloc(void)
{
    struct file *f;

    acquire(&ftable.lock);
    for(f = ftable.file; f < ftable.file + NFILE; f++)
    {
        if(f->f_count == 0){
            f->f_count = 1;
            f->f_time_update_sec = 0;
            release(&ftable.lock);
            return f;
        }
    }
    release(&ftable.lock);
    return 0;
}

/**
 * @brief 分配进程文件描述符
 * 
 * @param f 文件
 * @return int 文件描述符，失败返回-1 
 */
int 
fdalloc(struct file *f){
    int fd;
    proc_t *p = myproc();
    for(fd = 0 ; fd < NOFILE && fd < myproc()->ofn.rlim_cur; fd++)
    {
        if(p->ofile[fd] == 0){
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

int fdalloc2(struct file *f,int begin)
{
    int fd;
    proc_t *p = myproc();
    for(fd = begin; fd < NOFILE; fd++){
        if(p->ofile[fd] == 0){
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
};

/**
 * @brief 增加文件描述符的引用计数
 * 
 * Increment ref count for file f.
 * 
 * @param f 
 * @return struct file* 
 */
struct file*
filedup(struct file *f)
{
    acquire(&ftable.lock);
    if(f->f_count < 1)
        panic("filedup");
    f->f_count++;
    release(&ftable.lock);
    return f;
}

/**
 * @brief 减少文件描述符引用计数，如果引用计数为0，彻底关闭
 * 
 * Close file f.  (Decrement ref count, close when reaches 0.)
 * 
 * @param f 
 */

int fileclose(struct file *f)
{
    struct file ff;

    acquire(&ftable.lock);
    if(f->f_count < 1){
        panic("fileclose");
        return -1;
    }
    if(--f->f_count > 0){
        release(&ftable.lock);
        return 0;
    }
    ff = *f;
    f->f_count = 0;
    f->f_type = FD_NONE;
    f->removed = 0;
    release(&ftable.lock);

    if(ff.f_type == FD_PIPE)
    {
        pipeclose(ff.f_data.f_pipe, get_file_ops()->writable(&ff));
    } 
    else if(ff.f_type == FD_REG || ff.f_type == FD_DEVICE)
    {
        if (ff.f_data.f_vnode.fs->type == EXT4) 
        {       
            if (vfs_ext4_is_dir(ff.f_path) == 0) 
                vfs_ext4_dirclose(&ff);
            else 
                vfs_ext4_fclose(&ff);
            if (ff.removed) 
            {
                vfs_ext4_rm(ff.f_path);
                ff.removed = 0;
            }
        }
        else if (ff.f_data.f_vnode.fs->type == VFAT) 
        {
            // panic("我还没写(๑>؂<๑)\n");
            /* @todo 测例好像哪怕挂载了vfat，也是用ext4来读写的 */
            if (vfs_ext4_is_dir(ff.f_path) == 0) 
                vfs_ext4_dirclose(&ff);
            else 
                vfs_ext4_fclose(&ff);
            if (ff.removed) 
            {
                vfs_ext4_rm(ff.f_path);
                ff.removed = 0;
            }
        } 
        else 
            panic("fileclose: %s unknown filesystem type!", ff.f_path);
    }
    else if (ff.f_type == FD_BUSYBOX)
    {
#if DEBUG
        LOG_LEVEL(LOG_DEBUG, "close file or dir %s for busybox\n", ff.f_path);
#endif
    }else if(ff.f_type == FD_SOCKET){
        DEBUG_LOG_LEVEL(LOG_WARNING,"[todo] 释放socket资源");
    } else if (ff.f_type == FD_EPOLL) {
        /* Minimal epoll stub has no backing resources yet. */
    } else if (ff.f_type == FD_EVENTFD) {
        /* eventfd counter is stored in f_pos only. */
    } else if (ff.f_type == FD_SIGNALFD) {
        /* signalfd stub has no backing resources yet. */
    } else if (ff.f_type == FD_TIMERFD) {
        /* timerfd stub has no backing resources yet. */
    } else if (ff.f_type == FD_PIDFD) {
        /* pidfd stub has no backing resources yet. */
    }
    else
        panic("fileclose: %s unknown file type!", ff.f_path);
    return 0;
}

/**
 * @brief 得到文件描述符f的metadata到addr(用户地址)
 * 
 * Get metadata about file f.
 * addr is a user virtual address, pointing to a struct stat.
 * 
 * @param f 
 * @param addr 
 * @return int 标准错误码负数
 */
int
filestat(struct file *f, uint64 addr)
{
    struct proc *p = myproc();
    struct kstat st;
    if(f->f_type == FD_REG || f->f_type == FD_DEVICE)
    {
        int ret = vfs_ext4_fstat(f, &st);
        if (ret < 0) return ret;
        if (copyout(p->pagetable, addr, (char *)(&st), sizeof(st)) < 0)
            return -EFAULT;
        return 0;
    }
    return -1;
}

/**
 * @brief 得到文件描述符f的拓展的metadata到addr(用户地址)
 * 
 * @param f 
 * @param addr 
 * @return int 状态码，0成功，-1失败
 */
int 
filestatx(struct file *f, uint64 addr) 
{
    struct proc *p = myproc();
    struct statx st;
    if( f->f_type == FD_REG || f->f_type == FD_DEVICE 
        || f->f_type == FD_BUSYBOX)
    {
        if (f->f_type == FD_BUSYBOX)
            busybox_virtual_statx(f, &st);
        else
            vfs_ext4_statx(f->f_path, &st);
        if(copyout(p->pagetable, addr, (char *)(&st), sizeof(st)) < 0)
            return -1;
        return 0;
    }
    return -1;
}

/**
 * @brief 从文件f中读取n字节数据到addr(用户地址)
 * 
 * Read from file f.
 * addr is a user virtual address.
 * 
 * @param f 
 * @param addr 
 * @param n 
 * @return int 状态码，0成功，-1失败
 */
int
fileread(struct file *f, uint64 addr, int n)
{
    int r = 0;

    if(get_file_ops()->readable(f) == 0)
        return -1;

    if(f->f_type == FD_PIPE)
    {
        r = piperead(f->f_data.f_pipe, addr, n);
    } 
    else if(f->f_type == FD_DEVICE)
    {
        if(f->f_major < 0 || f->f_major >= NDEV || !devsw[f->f_major].read)
            return -1;
        r = devsw[f->f_major].read(1, addr, n);
    } 
    else if(f->f_type == FD_REG)
    {
        if (f->f_data.f_vnode.fs->type == EXT4) 
        {
            r = vfs_ext4_read(f, 1, addr, n);
        } 
        else if (f->f_data.f_vnode.fs->type == VFAT) 
        {
            /* @todo 测例好像哪怕挂载了vfat，也是用ext4来读写的 */
            // panic("我还没写(๑>؂<๑)\n");
            r = vfs_ext4_read(f, 1, addr, n);
        } 
        else 
        {
            panic("fileread: unknown file type");
        }
    } 
    else if (f->f_type == FD_BUSYBOX)
    {
        return busybox_virtual_read(f, addr, n);
    }
    else if (f->f_type == FD_EVENTFD)
    {
        uint64 value = f->f_pos;

        if (n < (int)sizeof(value))
            return -EINVAL;
        if (value == 0)
            return -EAGAIN;
        if (copyout(myproc()->pagetable, addr, (char *)&value, sizeof(value)) < 0)
            return -EFAULT;
        f->f_pos = 0;
        return sizeof(value);
    }
    else if (f->f_type == FD_SIGNALFD)
    {
        return -EAGAIN;
    }
    else if (f->f_type == FD_TIMERFD)
    {
        return -EAGAIN;
    }
    else if (f->f_type == FD_PIDFD)
    {
        return -EINVAL;
    }
    else
    {
        panic("fileread");
    }

    return r;
}

/**
 * @brief 从指定偏移读文件f的n个字节到addr(内核地址)
 * 
 * @param f 
 * @param addr 
 * @param n 
 * @param offset 
 * @return int 状态码，0成功，-1失败
 */
int filereadat(struct file *f, uint64 addr, int n, uint64 offset) {
    int r = 0;

    if(get_file_ops()->readable(f) == 0)
        return -1;
    if (f->f_type == FD_REG) 
    {
        if (f->f_data.f_vnode.fs->type == EXT4) 
        {
            r = vfs_ext4_readat(f, 0, addr, n, offset);
        } 
        else if (f->f_data.f_vnode.fs->type == VFAT) 
        {
            /* @todo 测例好像哪怕挂载了vfat，也是用ext4来读写的 */
            r = vfs_ext4_readat(f, 0, addr, n, offset);
            // panic("我还没写(๑>؂<๑)\n");
        } 
        else 
        {
            panic("filereadat: unknown file type");
        }
    }
    return r;
}

/**
 * @brief 从addr(用户地址)写n字节数据到文件f
 * 
 * Write to file f.
 * addr is a user virtual address.
 * 
 * @param f 
 * @param addr 
 * @param n 
 * @return int 状态码，0成功，-1失败
 */
int
filewrite(struct file *f, uint64 addr, int n)
{
    int r, ret = 0;

    if(get_file_ops()->writable(f) == 0)
        return -1;

    if(f->f_type == FD_PIPE)
    {
        ret = pipewrite(f->f_data.f_pipe, addr, n);
    } 
    else if(f->f_type == FD_DEVICE)
    {
        if(f->f_major < 0 || f->f_major >= NDEV || !devsw[f->f_major].write)
            return -1;
        ret = devsw[f->f_major].write(1, addr, n);
    } 
    else if(f->f_type == FD_REG)
    {
        // write a few blocks at a time to avoid exceeding
        // the maximum log transaction size, including
        // i-node, indirect block, allocation blocks,
        // and 2 blocks of slop for non-aligned writes.
        // this really belongs lower down, since writei()
        // might be writing a device like the console.
        int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
        int i = 0;
        while(i < n)
        {
            int n1 = n - i;
            if (n1 > max)
                n1 = max;
            if (f->f_data.f_vnode.fs->type == EXT4) 
            {
                r = vfs_ext4_write(f, 1, addr + i, n1);
            } 
            else if (f->f_data.f_vnode.fs->type == VFAT) 
            {
                /* @todo 测例好像哪怕挂载了vfat，也是用ext4来读写的 */
                r = vfs_ext4_write(f, 1, addr + i, n1);
                // panic("我还没写(๑>؂<๑)\n");
            } 
            else 
            {
                panic("filewrite: unknown file type");
            }
            if(r != n1)
            {
                // error from writei
                break;
            }
            i += r;
        }
        ret = (i == n ? n : -1);
    } 
    else 
    {
        if (f->f_type == FD_BUSYBOX)
        {
            if (busybox_virtual_is_dir(f->f_path))
                return -EISDIR;
            if (busybox_virtual_is_pid_oom_score_adj(f->f_path))
            {
                char buf[16];
                int len = min(n, (int)sizeof(buf) - 1);
                int kind = busybox_virtual_oom_path_kind(f->f_path);
                int pid = busybox_virtual_oom_score_adj_pid(f->f_path);
                int sign = 1;
                int value = 0;
                int i = 0;
                int saw_digit = 0;

                if (n <= 0)
                    return 0;

                if (copyin(myproc()->pagetable, buf, addr, len) < 0)
                    return -EFAULT;
                buf[len] = '\0';
                while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')
                    i++;
                if (buf[i] == '-')
                {
                    sign = -1;
                    i++;
                }
                for (; buf[i] >= '0' && buf[i] <= '9'; i++)
                {
                    saw_digit = 1;
                    value = value * 10 + (buf[i] - '0');
                }
                if (!saw_digit)
                    return n;
                value *= sign;
                if (kind == 2)
                    value = score_adj_from_oom_adj(value);
                if (proc_set_oom_score_adj(pid, value) < 0)
                    return -ENOENT;
                return n;
            }
            return n;
        }
        if (f->f_type == FD_EVENTFD)
        {
            uint64 value = 0;

            if (n < (int)sizeof(value))
                return -EINVAL;
            if (copyin(myproc()->pagetable, (char *)&value, addr, sizeof(value)) < 0)
                return -EFAULT;
            f->f_pos += value;
            return sizeof(value);
        }
        if (f->f_type == FD_SIGNALFD)
            return -EINVAL;
        if (f->f_type == FD_TIMERFD)
            return -EINVAL;
        if (f->f_type == FD_PIDFD)
            return -EINVAL;
        panic("filewrite");
    }

    return ret;
}

/**
 * @brief 文件是否可读
 * 
 * @param f 
 * @return char 
 */
char 
filereadable(struct file *f) 
{
    char readable = !(f->f_flags & O_WRONLY);
    return readable;
}

/**
 * @brief 文件是否可写
 * 
 * @param f 
 * @return char 
 */
char 
filewriteable(struct file *f) 
{
    char writeable = (f->f_flags & O_WRONLY) || (f->f_flags & O_RDWR);
    return writeable;
}

struct file_operations FILE_OPS = 
{
    .dup = &filedup,
    .close = &fileclose,
    .read = &fileread,
    .readat = &filereadat,
    .write = &filewrite,
    .fstat = &filestat,
    .statx = &filestatx,
    .writable = &filewriteable,
    .readable = &filereadable,
};

struct file_operations *
get_file_ops(void) 
{
    return &FILE_OPS;
}

/**
 * @brief 初始化文件描述符表和ext4目录、文件表
 * 
 * Initialize the file descriptor table and ext4 directory and file tables.
 */
void 
fileinit(void) 
{
    initlock(&ftable.lock, "ftable");
    initlock(&file_vnode_table.lock, "file_vnode_table");
	memset(ftable.file, 0, sizeof(ftable.file));
    memset(file_vnode_table.vnodes, 0, sizeof(file_vnode_table.vnodes));
}

/**
 * @brief 分配目录结构体
 * 
 * @return file_vnode_t* 对应VFS的vnode
 */
file_vnode_t *
vfs_alloc_dir(void) 
{
    int i;
    acquire(&file_vnode_table.lock);
    for (i = 0;i < NFILE;i++) 
    {
        if (file_vnode_table.valid[i] == 0) 
        {
            file_vnode_table.valid[i] = 1;
            file_vnode_table.isdir[i] = 1;
            file_vnode_table.vnodes[i].fs = get_fs_by_type(EXT4);
            file_vnode_table.vnodes[i].data = kalloc();
            break;
        }
    }
    release(&file_vnode_table.lock);
    if (i == NFILE)
        return NULL;
    return &file_vnode_table.vnodes[i];
}

/**
 * @brief 分配文件结构体
 * 
 * @return file_vnode_t* 对应VFS的vnode
 */
file_vnode_t *
vfs_alloc_file(void) 
{
    int i;
    acquire(&file_vnode_table.lock);
    for (i = 0;i < NFILE;i++) 
    {
        if (file_vnode_table.valid[i] == 0) 
        {
            file_vnode_table.valid[i] = 1;
            file_vnode_table.isdir[i] = 0;
            file_vnode_table.vnodes[i].fs = get_fs_by_type(EXT4);
            file_vnode_table.vnodes[i].data = kalloc();            
            break;
        }
    }
    release(&file_vnode_table.lock);
    if (i == NFILE) 
        return NULL;
    return &file_vnode_table.vnodes[i];
}

/**
 * @brief 释放目录结构体
 * 
 * @param dir 
 */
void 
vfs_free_dir(void *dir) 
{
    int i;
    acquire(&file_vnode_table.lock);
    for (i = 0; i < NFILE; i++) 
    {
        if (file_vnode_table.isdir[i] && 
            (dir == file_vnode_table.vnodes[i].data))
        {
            file_vnode_table.valid[i] = 0;
            kfree(file_vnode_table.vnodes[i].data);
            file_vnode_table.vnodes[i].data = NULL;
            release(&file_vnode_table.lock);
            return;
        }
    }
    release(&file_vnode_table.lock);
}

/**
 * @brief 释放文件结构体
 * 
 * @param file 
 */
void 
vfs_free_file(void *file) 
{
    int i;
    acquire(&file_vnode_table.lock);
    for (i = 0; i < NFILE; i++) 
    {
        if ((file_vnode_table.isdir[i]==0) && 
            (file == file_vnode_table.vnodes[i].data)) 
        {
            file_vnode_table.valid[i] = 0;
            kfree(file_vnode_table.vnodes[i].data);
            file_vnode_table.vnodes[i].data = NULL;
            release(&file_vnode_table.lock);
            return;
        }
    }
    release(&file_vnode_table.lock);
}
