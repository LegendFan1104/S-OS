#pragma once

#include "types.h"
#include "fs/vfs/file.h"
#include "fs/ext4/vfs_ext4_ext.h"

// procfs 文件类型
enum procfs_type {
    PROCFS_ROOT,        // /proc
    PROCFS_PID_DIR,     // /proc/[pid]
    PROCFS_PID_STAT,    // /proc/[pid]/stat
    PROCFS_PID_CMDLINE, // /proc/[pid]/cmdline
    PROCFS_PID_EXE,     // /proc/[pid]/exe
    PROCFS_SELF,        // /proc/self
    PROCFS_UPTIME,      // /proc/uptime
    PROCFS_MEMINFO,     // /proc/meminfo
    PROCFS_STAT,        // /proc/stat
    PROCFS_LOADAVG,     // /proc/loadavg
    PROCFS_VERSION,     // /proc/version
    PROCFS_MOUNTS,      // /proc/mounts
    PROCFS_INTERRUPTS,  // /proc/interrupts
    PROCFS_DUMMY,       // 其他占位文件
};

// procfs 文件状态
struct procfs_file {
    enum procfs_type type;
    int pid;            // 对于 PID 相关的文件
    uint64 offset;      // 读取偏移
    char buf[4096];     // 文件内容缓冲区
    int buflen;         // 缓冲区内容长度
};

// 判断路径是否是 procfs 路径
int is_procfs_path(const char *path);

// 获取 procfs 文件类型
enum procfs_type get_procfs_type(const char *path, int *pid);

// 打开 procfs 文件
int procfs_open(const char *path, struct file *f);

// 读取 procfs 文件
int procfs_read(struct file *f, uint64 addr, int n);

// 读取目录
int procfs_getdents(struct file *f, struct linux_dirent64 *dirp, int count);

// 生成 procfs 文件内容
int procfs_generate_content(struct procfs_file *pf);
