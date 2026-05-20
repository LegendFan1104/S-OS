#pragma once

struct stat;
struct sigaction;

struct linux_dirent64 {
    uint64 d_ino;	// 索引结点号
    int64 d_off;	// 到下一个dirent的偏移
    unsigned short d_reclen;	// 当前dirent的长度
    unsigned char d_type;	// 文linux_dirent64件类型
    char d_name[];	//文件名
};


struct tms
{
    long tms_utime; //用户cpu时间
    long tms_stime;//系统cpu时间
    long tms_cutime;//已终止子进程的用户cpu时间
    long tms_cstime;//已终止子进程的系统cpu时间
};

struct sysinfo{
    uint64 uptime; //系统运行时间
    // uint64 loads[3]; //系统负载
    // uint64 totalram; //总内存
    uint64 freeram; //空闲内存
    // uint64 sharedram; //共享内存
    // uint64 bufferram; //缓存内存
    // uint64 totalswap; //交换区总大小
    // uint64 freeswap; //交换区剩余大小
    uint64 procs; //进程数
};

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe2(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int execve(const char*, char**, char **);
int exec(const char *, char**);
int openat(int, const char*, int, int);
int unlinkat(int, const char*, int);
int fstat(int fd, struct kstat*);
int linkat(int, const char*, int, const char*, int);
int mkdirat(int, const char*, int);
int mknod(const char*, short, short);
int chdir(const char*);
int dup(int);
int getpid(void);
char* brk(int);
char *sbrk(uint64);
int sleep(int);
int uptime(void);
int times(struct tms *);
int mmap(void *, uint64, int, int, int, uint64);
int munmap(void *, uint64);
int shutdown();


// ulib.c
int stat(const char*, struct kstat*);
char* strcpy(char*, const char*);
char *strcat(char *dest, const char *src);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...);
void printf(const char*, ...);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
void* malloc(uint);
void free(void*);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);

//add
int getcwd(char*, int);
int dup3(int, int, int);
int getdents64(int, struct linux_dirent64*, int);
int umount2(const char*, int);
int mount(const char*, const char*, const char*, uint64, const void *);
int wait4(int, int *, int);
int clone(int(*)(void*), void*, void*, int, int);

#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400
#define O_DIRECTORY 0x004
#define O_CLOEXEC 0x008

#define AT_FDCWD -100
#define DIRSIZ 260

int getppid(void);
int times(struct tms *);
int rt_sigaction(int, struct sigaction*, struct sigaction*);
int rt_sigprocmask(int, int, struct sigset*, struct sigset*);
int tkill(int, int);
int set_tid_address(int *tidptr);
int getuid(void);
int getgid(void);
int setgid(int);
int setuid(int);
int sysinfo(struct sysinfo *);
int clock_nanosleep(int , const struct timespecc *, struct timespecc *);
int futex(uint64 *uaddr, int op, int val, const struct timespecc *timeout, uint64 *uaddr2, int val3);
ssize_t copy_file_range(int fd_in, uint64 *off_in, int fd_out, uint64 *off_out, size_t len, unsigned int flags);
int ftruncate(int fd, uint length);