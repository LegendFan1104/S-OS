#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "platform.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "sys/syscall.h"
#include "defs.h"
#include "lib/string.h"
#include "proc/signal.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  // if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
  //   return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  if (walkaddr(myproc()->pagetable, *ip) == 0) {
    pagefault_handler(*ip, 0);
  }
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

uint64 unused() {
  return 0;
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_waitid(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_execve(void);
extern uint64 sys_fstat(void);
extern uint64 sys_fstatat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_openat(void);
extern uint64 sys_write(void);
extern uint64 sys_unlinkat(void);
extern uint64 sys_linkat(void);
extern uint64 sys_mkdirat(void);
extern uint64 sys_close(void);
//add
extern uint64 sys_clone(void);
extern uint64 sys_mknod(void);
extern uint64 sys_exec(void);
extern uint64 sys_pipe2(void);
extern uint64 sys_getcwd(void);
extern uint64 sys_dup3(void);
extern uint64 sys_getdents64(void);
extern uint64 sys_mount(void);
extern uint64 sys_umount2(void);
extern uint64 sys_wait4(void);
extern uint64 sys_brk(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_times(void);
extern uint64 sys_gettimeofday(void);
extern uint64 sys_sched_yield(void);
extern uint64 sys_uname(void);
extern uint64 sys_nanosleep(void);
extern uint64 sys_shutdown(void);
extern uint64 sys_getppid(void);
extern uint64 sys_statx(void);
extern uint64 sys_rt_sigaction(void);
extern uint64 sys_rt_sigprocmask(void);
extern uint64 sys_rt_sigtimedwait(void);
extern uint64 sys_rt_sigpending(void);
extern uint64 sys_kill_signal(void);
extern uint64 sys_tkill(void);
extern uint64 sys_tgkill(void);
extern uint64 sys_set_tid_address(void);
extern uint64 sys_getuid(void);
extern uint64 sys_getgid(void);
extern uint64 sys_setgid(void);
extern uint64 sys_setuid(void);
extern uint64 sys_exit_group(void);
extern uint64 sys_gettid(void);
extern uint64 sys_set_robust_list(void);
extern uint64 sys_get_robust_list(void);
extern uint64 sys_writev(void);
extern uint64 sys_prlimit64(void);
extern uint64 sys_readlinkat(void);
extern uint64 sys_clock_gettime(void);
extern uint64 sys_getrandom(void);
extern uint64 sys_ioctl(void);
extern uint64 sys_syslog(void);
extern uint64 sys_fcntl(void);
extern uint64 sys_sysinfo(void);
extern uint64 sys_faccessat(void);
extern uint64 sys_ppoll(void);
extern uint64 sys_madvise(void);
extern uint64 sys_mremap(void);
extern uint64 sys_sendfile(void);
extern uint64 sys_lseek(void);
extern uint64 sys_utimensat(void);
extern uint64 sys_renameat2(void);
extern uint64 sys_readv(void);
extern uint64 sys_clock_nanosleep(void);
extern uint64 sys_getpgid(void);
extern uint64 sys_setpgid(void);
extern uint64 sys_readv(void);
extern uint64 sys_writev(void);
extern uint64 sys_preadv(void);
extern uint64 sys_pwritev(void);
extern uint64 sys_mprotect(void);
extern uint64 sys_copy_file_range(void);
extern uint64 sys_geteuid(void);
extern uint64 sys_getegid(void);
extern uint64 sys_ftruncate(void);
extern uint64 sys_pread64(void);
extern uint64 sys_splice(void);
extern uint64 sys_fchmodat(void);
extern uint64 sys_symlinkat(void);
extern uint64 sys_futex(void);
extern uint64 sys_socket(void);
extern uint64 sys_memfd_create(void);
extern uint64 sys_inotify_init1(void);
extern uint64 sys_inotify_add_watch(void);
extern uint64 sys_inotify_rm_watch(void);
extern uint64 sys_eventfd2(void);
extern uint64 sys_timerfd_create(void);
extern uint64 sys_timerfd_settime(void);
extern uint64 sys_timerfd_gettime(void);
extern uint64 sys_epoll_create1(void);
extern uint64 sys_epoll_ctl(void);
extern uint64 sys_epoll_pwait(void);
extern uint64 sys_sigaltstack(void);
extern uint64 sys_sigsuspend(void);

// Scheduling system calls
extern uint64 sys_sched_setscheduler(void);
extern uint64 sys_sched_setparam(void);
extern uint64 sys_sched_getparam(void);
extern uint64 sys_sched_getscheduler(void);
extern uint64 sys_sched_get_priority_max(void);
extern uint64 sys_sched_get_priority_min(void);
extern uint64 sys_sched_setaffinity(void);
extern uint64 sys_sched_getaffinity(void);

// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe2]    sys_pipe2,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_execve]    sys_execve,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_brk]    sys_brk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_openat]    sys_openat,
[SYS_write]   sys_write,
[SYS_unlinkat]  sys_unlinkat,
[SYS_linkat]    sys_linkat,
[SYS_mkdirat]   sys_mkdirat,
[SYS_close]   sys_close,
//add
[SYS_clone]   sys_clone,
[SYS_mknod]   sys_mknod,
[SYS_exec]    sys_exec,
[SYS_getcwd]  sys_getcwd,
[SYS_dup3]    sys_dup3,
[SYS_getdents64] sys_getdents64,
[SYS_umount2] sys_umount2,
[SYS_mount]   sys_mount,
[SYS_mmap]    sys_mmap,
[SYS_munmap]  sys_munmap,
[SYS_wait4]   sys_wait4,
[SYS_waitid]  sys_waitid,
[SYS_times]   sys_times,
[SYS_gettimeofday] sys_gettimeofday,
[SYS_sched_yield] sys_sched_yield,
[SYS_uname]   sys_uname,
[SYS_nanosleep] sys_nanosleep,
[SYS_shutdown]  sys_shutdown,
[SYS_getppid]   sys_getppid,
[SYS_statx]  sys_statx,
[SYS_rt_sigaction] sys_rt_sigaction,
[SYS_rt_sigprocmask] sys_rt_sigprocmask,
[SYS_rt_sigtimedwait] sys_rt_sigtimedwait,
[SYS_rt_sigpending] sys_rt_sigpending,
[SYS_sigaltstack] sys_sigaltstack,
[SYS_sigsuspend] sys_sigsuspend,
[SYS_kill_signal] sys_kill_signal,
[SYS_tkill] sys_tkill,
[SYS_tgkill] sys_tgkill,
[SYS_set_tid_address] sys_set_tid_address,
[SYS_getuid] sys_getuid,
[SYS_getgid] sys_getgid,
[SYS_setgid] sys_setgid,
[SYS_setuid] sys_setuid,
[SYS_fstatat] sys_fstatat,
[SYS_exit_group] sys_exit_group,
[SYS_gettid] sys_gettid,
[SYS_set_robust_list] sys_set_robust_list,
[SYS_get_robust_list] sys_get_robust_list,
[SYS_writev] sys_writev,
[SYS_preadv] sys_preadv,
[SYS_pwritev] sys_pwritev,
[SYS_readlinkat] sys_readlinkat,
[SYS_prlimit64] sys_prlimit64,
[SYS_getrandom] sys_getrandom,
[SYS_clock_gettime] sys_clock_gettime,
[SYS_ioctl] sys_ioctl,
[SYS_syslog] sys_syslog,
[SYS_fcntl] sys_fcntl,
[SYS_sysinfo] sys_sysinfo,
[SYS_faceessat] sys_faccessat,
[SYS_ppoll] sys_ppoll,
[SYS_madvise] sys_madvise,
[SYS_mremap] sys_mremap,
[SYS_sendfile] sys_sendfile,
[SYS_lseek] sys_lseek,
[SYS_utimensat] sys_utimensat,
[SYS_renameat2] sys_renameat2,
[SYS_readv] sys_readv,
[SYS_writev] sys_writev,
[SYS_clock_nanosleep] sys_clock_nanosleep,
[SYS_getpgid] sys_getpgid,
[SYS_setpgid] sys_setpgid,
[SYS_geteuid] sys_geteuid,
[SYS_mprotect] sys_mprotect,
[SYS_getegid] sys_getegid,
[SYS_copy_file_range] sys_copy_file_range,
[SYS_ftruncate] sys_ftruncate,
[SYS_pread64] sys_pread64,
[SYS_splice] sys_splice,
[SYS_fchmodat] sys_fchmodat,
[SYS_symlinkat] sys_symlinkat,
[SYS_futex] sys_futex,
[SYS_socket] sys_socket,
[SYS_memfd_create] sys_memfd_create,
[SYS_inotify_init1] sys_inotify_init1,
[SYS_inotify_add_watch] sys_inotify_add_watch,
[SYS_inotify_rm_watch] sys_inotify_rm_watch,
[SYS_eventfd2] sys_eventfd2,
[SYS_timerfd_create] sys_timerfd_create,
[SYS_timerfd_settime] sys_timerfd_settime,
[SYS_timerfd_gettime] sys_timerfd_gettime,
[SYS_epoll_create1] sys_epoll_create1,
[SYS_epoll_ctl] sys_epoll_ctl,
[SYS_epoll_pwait] sys_epoll_pwait,
[SYS_sched_setscheduler] sys_sched_setscheduler,
[SYS_sched_setparam] sys_sched_setparam,
[SYS_sched_getparam] sys_sched_getparam,
[SYS_sched_getscheduler] sys_sched_getscheduler,
[SYS_sched_get_priority_max] sys_sched_get_priority_max,
[SYS_sched_get_priority_min] sys_sched_get_priority_min,
[SYS_sched_setaffinity] sys_sched_setaffinity,
[SYS_sched_getaffinity] sys_sched_getaffinity,
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;

  // if (p->pid > 1) {
  //   printf("%d : call %d\n", p->pid, num);
  // }

  if (num == SYS_rt_sigreturn) {
    // printf("11111\n");
    sig_return();
    return ;
  }
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
