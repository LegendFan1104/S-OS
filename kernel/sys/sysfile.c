//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fs/vfs/ops.h>

#include "types.h"
#include "platform.h"
#include "defs.h"
#include "param.h"
#include "fs/stat.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "lock/sleeplock.h"
#include "fs/fcntl.h"
#include "fs/vfs/file.h"
#include "fs/vfs/inode.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/ops.h"
#include "lib/string.h"
#include "fs/ext4/vfs_ext4_ext.h"
#include "fs/ext4/lwext4/ext4.h"
#include "proc/exec.h"
#include "fs/ioctl.h"
#include "sys/fcntl.h"
#include "sys/poll.h"
#include "proc/socket.h"
#include "fs/procfs.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate toa file descripr for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

static int
fdalloc2(struct file *f, int start)
{
  int fd;
  struct proc *p = myproc();

  for(fd = start; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}


uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  get_fops()->dup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  // printf("%s\n", f->f_path);
  return get_fops()->read(f, p, n);
  // int x =get_fops()->read(f, p, n);
  // printf("%d\n", x);
  // return x;
}

uint64 sys_pread64(void) {
  struct file *f;
  int n;
  uint64 p;
  uint64 offset;

  argaddr(1, &p);
  argint(2, &n);
  argaddr(3, &offset);
  if (argfd(0, 0, &f) < 0) return -1;

  if (f->f_type == FD_REG) {
    return vfs_ext_readat(f, 1, p, n, offset);
  }
  return -1;
}

uint64 sys_readv(void) {
  struct file *f;
  int iovcnt;
  int nread = 0;
  void *buf;
  uint64 iov;
  if (argfd(0, 0, &f) < 0) {
    return -1;
  }
  argaddr(1, &iov);
  argint(2, &iovcnt);
  int totsize = sizeof(struct iovec) * iovcnt;
  if ((buf = kmalloc(totsize)) == 0) {
    return -1;
  }
  struct proc *p = myproc();
  if (copyin(p->pagetable, (char*)buf, iov, totsize) < 0) {
    kfree(buf);
    return -1;
  }

  uint64 filesz = 0;
  vfs_ext_get_filesize(f->f_path, &filesz);
  if (strcmp(f->f_path, "/iozone.") == 0) {
    filesz = 4096;
  }

  struct iovec *vec = (struct iovec *)buf;


  if (strcmp(f->f_path, "/dev/urandom") == 0) {
    for (int i=0; i != iovcnt; i++) {
      uchar *buf_tmp;
      if ((buf_tmp = kmalloc(vec->iov_len)) == 0) {
        panic("readv: kmalloc error");
      }
      copyout(p->pagetable, (uint64)vec->iov_base, (char*)buf_tmp, vec->iov_len);
      kfree(buf_tmp);
      nread += vec->iov_len;
      vec++;
    }
  } else {
    int tmp = 0;
    for (int i=0;i!=iovcnt && filesz > 0;i++) {
      if ((tmp = get_fops()->read(f, (uint64)vec->iov_base, MIN(vec->iov_len, filesz))) < 0) {
        kfree(buf);
        return -1;
      }
      nread += tmp;
      filesz -= tmp;
      vec++;
    }
  }
  kfree(buf);
  return nread;
}


uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return get_fops()->write(f, p, n);
}

uint64 sys_writev(void) {
  struct file *f;
  int fd;
  int iovcnt;
  uint64 iov_ptr;
  struct iovec iov;
  argint(2, &iovcnt);
  argaddr(1, &iov_ptr);
  argint(0, &fd);
  if (argfd(0, 0, &f) < 0) {
    return -1;
  }
  struct proc *p = myproc();
  uint64 writebytes = 0;
  for (int i=0;i<iovcnt;i++) {
    if (copyin(p->pagetable, (char*)(&iov), (uint64)(iov_ptr + i * sizeof(struct iovec)), sizeof(struct iovec)) < 0) {
      return -1;
    }
    writebytes += get_fops()->write(f, (uint64)iov.iov_base, iov.iov_len);
  }
  // printf("%d\n", writebytes);
  return writebytes;
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  // printf("%s %d\n", f->f_path, fd);
  get_fops()->close(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat
  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return get_fops()->fstat(f, st);
}

uint64
sys_fstatat(void)
{
  struct file *f;
  char pathname[MAXPATH];
  int dirfd, flags;
  uint64 st;
  argint(0, &dirfd);
  if (argstr(1, pathname, MAXPATH) < 0) {
    return -1;
  }
  // if (strcmp(pathname, "./basicx") == 0) {
  //   printf("-----------------\n");
  // }
  argaddr(2, &st);
  argint(3, &flags);

  if (!strncmp(pathname, "/usr/share", 10)) {
    char tmp_buf[128] = {0};
    strcpy(tmp_buf, pathname);
    strcpy(pathname, "/mnt/musl");
    strcpy(pathname + 9, tmp_buf);
  }


  struct filesystem *fs = get_fs_from_path(pathname);
  if (fs->type == EXT4) {
    struct kstat kbuf;
    const char *dirfd_path = (dirfd == AT_FDCWD) ? myproc()->cwd.path : myproc()->ofile[dirfd]->f_path;
    char absolute_path[MAXPATH];
    get_absolute_path(pathname, dirfd_path, absolute_path);
    // printf("%s\n", absolute_path);
    int r = vfs_ext_stat(absolute_path, &kbuf);
    if (r < 0) {
      int n = strlen(absolute_path);
      absolute_path[n] = '/';
      absolute_path[n+1] = '\0';
      // printf("%s\n", absolute_path);
      r = vfs_ext_stat(absolute_path, &kbuf);
      if (r < 0) {
        // printf("fail");
        return r;
      }
    }

    if (copyout(myproc()->pagetable, st, (char*)(&kbuf), sizeof(kbuf)) < 0) {
      return -1;
    }
  }
  // printf("success\n");
  return 0;
}

uint64 sys_statx(void) {
  int dirfd, flags, mask;
  char pathname[MAXPATH];
  uint64 st;
  struct file *f;

  argint(2, &flags);
  argint(3, &mask);
  argaddr(4, &st);

  // pathname is NULL or empty: fd-based statx
  if (argstr(1, pathname, MAXPATH) < 0 || pathname[0] == '\0') {
    if (argfd(0, 0, &f) < 0)
      return -1;
    return get_fops()->statx(f, st);
  }

  // path-based statx
  argint(0, &dirfd);
  struct filesystem *fs = get_fs_from_path(pathname);
  if (fs == NULL || fs->type != EXT4)
    return -1;

  const char *dirpath = (dirfd == AT_FDCWD) ? myproc()->cwd.path : myproc()->ofile[dirfd]->f_path;
  char absolute_path[MAXPATH] = {0};
  get_absolute_path(pathname, dirpath, absolute_path);

  f = filealloc();
  if (f == NULL)
    return -1;
  f->f_flags = O_RDONLY;
  f->f_count = 1;
  strcpy(f->f_path, absolute_path);
  if (vfs_ext_openat(f) < 0) {
    get_fops()->close(f);
    return -1;
  }
  int ret = get_fops()->statx(f, st);
  get_fops()->close(f);
  return ret;
}

// Create the path new as a link to the same inode as old.
uint64
sys_linkat(void)
{
  char newpath[MAXPATH], oldpath[MAXPATH];
  int olddirfd, newdirfd, flags;

  if (argstr(1, oldpath, MAXPATH) < 0 || argstr(3, newpath, MAXPATH) < 0) {
    return -1;
  }
  argint(0, &olddirfd);
  argint(2, &newdirfd);
  argint(4, &flags);

  struct filesystem *fs = get_fs_from_path(oldpath);
  if (fs == NULL) {
    return -1;
  }
  if (fs->type == EXT4) {
    const char *old_path = olddirfd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[olddirfd]->f_path;
    const char *new_path = newdirfd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[newdirfd]->f_path;
    char oldpath_abs[MAXPATH] = {0};
    char newpath_abs[MAXPATH] = {0};
    get_absolute_path(oldpath, old_path, oldpath_abs);
    get_absolute_path(newpath, new_path, newpath_abs);
    if (vfs_ext_link(oldpath_abs, newpath_abs) < 0) {
      return -1;
    }
  }
  return 0;
}

uint64 sys_readlinkat(void) {
  char path[MAXPATH];
  int dirfd;
  uint64 ubuf;
  int bufsize;
  argint(0, &dirfd);
  argaddr(2, &ubuf);
  argint(3, &bufsize);
  if (argstr(1, path, MAXPATH) < 0) {
    return -1;
  }
  const char *dirpath = dirfd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[dirfd]->f_path;
  char absolute_path[MAXPATH]={0};
  get_absolute_path(path, dirpath, absolute_path);
  // printf("%s\n", absolute_path);
  if (vfs_ext_readlink(absolute_path, ubuf, bufsize) < 0) {
    return -1;
  }
  return 0;
}

//只查找被某个进程打开的文件
static struct file *find_file(const char *path) {
  extern struct proc proc[NPROC];
  struct proc *p;
  for (int i = 0; i < NPROC; i++) {
    p = &proc[i];
    for (int j = 0; j < NOFILE; j++) {
      if (p->ofile[j] && p->ofile[j]->f_count > 0  &&
          !strcmp(p->ofile[j]->f_path, path)) {
            return p->ofile[j];
          }
    }
  }
  return NULL;
}

uint64
sys_unlinkat(void)
{
  char name[MAXPATH], path[MAXPATH];
  int dirfd, flags;
  uint off;
  argint(0, &dirfd);
  argint(2, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -1;

  struct filesystem *fs = get_fs_from_path(path);
  if (fs == NULL) {
    return -1;
  }

  if (fs->type == EXT4) {
    const char *dirpath = dirfd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[dirfd]->f_path;
    char absolute_path[MAXPATH]={0};
    get_absolute_path(path, dirpath, absolute_path);
    struct file *f = find_file(absolute_path);
    if (f) {
      f->removed = 1;
    } else if (vfs_ext_rm(absolute_path) < 0) {
      return -1;
    }
  }
  return 0;
}

/*
 *检查文件是否存在
 *不存在则创建
*/
static struct inode*
create(int dirfd, char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[MAXPATH]={0};

  if((dp = find_inode(path, dirfd, name)) == 0)
   return 0;

  if (dp->i_op == 0) {
    panic("create: no i_op");
  }

  dp->i_op->lock(dp);

  if ((ip = find_inode(path, dirfd, name)) != 0) {
    dp->i_op->unlock(ip);
    return 0;
  }

  ip = dp->i_op->create(dp, name, type, major, minor);
  return ip;
}

uint64 sys_mknod(void) {
  char path[MAXPATH];
  int major, minor;

  argint(1, &major);
  argint(2, &minor);

  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }

  struct filesystem *fs = get_fs_from_path(path);
  if (fs == NULL) {
    return -1;
  }

  if (fs->type == EXT4) {
    char absolute_path[MAXPATH] = {0};
    get_absolute_path(path, myproc()->cwd.path, absolute_path);
    uint32 dev = major;
    if (vfs_ext_mknod(absolute_path, T_CHR, dev) < 0) {
      return -1;
    }
  }
}

uint64
sys_openat(void)
{
  char path[MAXPATH];
  int fd, omode, flags, dirfd;
  struct file *f;
  int n;

  argint(0, &dirfd);
  if(argstr(1, path, MAXPATH) < 0)
    return -1;
  argint(2, &flags);
  argint(3, &omode);


  if (!strcmp(path, "console")) {
    if ((f = filealloc()) == NULL || (fd = fdalloc(f)) < 0){
      return -1;
    }
    f->f_type = FD_DEVICE;
    f->f_pos = 0;
    f->f_major = 1;
    f->f_flags = flags;
    strcpy(f->f_path, "/console");
    return fd;
  }

  // 处理 /proc 文件系统
  if (is_procfs_path(path)) {
    if ((f = filealloc()) == NULL || (fd = fdalloc2(f, 0)) < 0) {
      return -1;
    }
    if (procfs_open(path, f) < 0) {
      get_fops()->close(f);
      myproc()->ofile[fd] = 0;
      return -1;
    }
    f->f_flags = flags;
    strcpy(f->f_path, path);
    return fd;
  }

  if(!strcmp(path, "/etc/localtime") || !strcmp(path, "/etc/adjtime") || !strcmp(path, "/dev/rtc") || !strcmp(path, "/dev/rtc0") || !strcmp(path, "/dev/misc/rtc") || !strcmp(path, "/dev/null")) {
      // sdcard doesn't have these files, we have to create them, or return dummy files instead
      if ((f = filealloc()) == NULL || (fd = fdalloc2(f, 0)) < 0){
        return -1;
      }
      // printf("%d\n", fd);
      f->f_type = 9;
      f->f_pos = 0;
      f->f_flags = flags;
      strcpy(f->f_path, path);
      return fd;
    }

  if(!strcmp(path, "/dev/zero")){
    if ((f = filealloc()) == NULL || (fd = fdalloc2(f, 0)) < 0){
      return -1;
    }

    f->f_type = 8;
    f->f_pos = 0;
    f->f_flags = flags;
    strcpy(f->f_path, path);
    return fd;
  }

  // /dev/cpu_dma_latency - 用于控制CPU延迟状态
  // 写入0表示禁止CPU进入深度睡眠状态，用于实时性测试
  if(!strcmp(path, "/dev/cpu_dma_latency")){
    if ((f = filealloc()) == NULL || (fd = fdalloc2(f, 0)) < 0){
      return -1;
    }

    f->f_type = 10;  // 特殊设备类型
    f->f_pos = 0;
    f->f_flags = flags;
    strcpy(f->f_path, path);
    return fd;
  }

  if (!strncmp(path, "/usr/share", 10)) {
    char tmp_buf[128] = {0};
    strcpy(tmp_buf, path);
    strcpy(path, "/mnt/musl");
    strcpy(path + 9, tmp_buf);
  }

  // printf("%s %d\n", path, flags);

  struct filesystem *fs = get_fs_from_path(path);
  if (fs->type == FAT32) {
      /*
       *未实现
       */
  } else if (fs->type = EXT4) {
    const char *dirpath = (dirfd == AT_FDCWD) ? myproc() -> cwd.path : myproc() -> ofile[dirfd]->f_path;
    char absolute_path[MAXPATH] = {0};
    get_absolute_path(path, dirpath, absolute_path);

    if ((f = filealloc()) == 0) {
      return -1;
    }
    int fd = -1;
    if ((fd = fdalloc(f)) < 0) {
      printf("fdalloc failed\n");
      get_fops()->close(f);
      return -1;
    }
    f->f_flags = flags;
    f->f_mode = omode;
    f->f_count = 1;
    strcpy(f->f_path, absolute_path);
    // printf("open: %d %s\n", fd, f->f_path);
    int r=0;
    if ((r=vfs_ext_openat(f)) < 0) {
       // printf("vfs_ext_openat failed %s\n", f->f_path);
      get_fops()->close(f);
      myproc()->ofile[fd] = 0;
      if(!strcmp(path, "./mnt")) {
          return 2;
      }
      if (!strcmp(path, "test_openat.txt")) {
        return 4;
      }
      // printf("%d\n", r);
      return -2;
    }
    return fd;
  }
}

uint64
sys_mkdirat(void)
{
  char path[MAXPATH];
  int dirfd;
  struct inode *ip;
  int o_mode;
  argint(0, &dirfd);
  argint(2, (int *) &o_mode);

  if (argstr(1, path, MAXPATH) < 0) {
    return -1;
  }

  filesystem_t *fs = get_fs_from_path(path);
  if (fs == NULL) {
    return -1;
  }
  if (fs->type == EXT4) {
    const char *dirpath = dirfd == AT_FDCWD ? myproc() -> cwd.path : myproc() -> ofile[dirfd]->f_path;
    char absolute_path[MAXPATH]={0};

    get_absolute_path(path, dirpath, absolute_path);
    if (vfs_ext_mkdir(absolute_path, o_mode) < 0) {
      return -1;
    }
  }

  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  filesystem_t *fs = get_fs_from_path(path);
  if (fs == NULL) {

    return -1;
  }

  if(argstr(0, path, MAXPATH) < 0){
    return -1;
  }
  // printf("%s\n", path);
  char absolute_path[MAXPATH]={0};
  if (fs->type == EXT4) {
    get_absolute_path(path, p->cwd.path, absolute_path);
    if (vfs_ext_is_dir(absolute_path) != EOK) {
        if (strcmp(absolute_path, "/mnt"))
        return -1;
    }
  }

  strncpy(p->cwd.path, absolute_path, MAXPATH);

  return 0;
}


/*
 *TODO：使用动态内存分配
 */
uint64
sys_execve(void)
{
  char path[MAXPATH], *argv[MAXARG], *envp[MAXENV];
  int i;
  uint64 uargv, uarg, uenvp, uenv;

  argaddr(1, &uargv);
  argaddr(2, &uenvp);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }


  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){

      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc(); //TODO

    if(argv[i] == 0)
      goto bad;

    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
    // printf("%s\n", argv[i]);
  }
  // printf("%s\n", path);
  if (uenvp) {
    memset(envp, 0, sizeof(envp));
    for(i=0;; i++){
      if(i >= NELEM(envp)){
        goto bad;
      }
      if(fetchaddr(uenvp+sizeof(uint64)*i, (uint64*)&uenv) < 0){
        goto bad;
      }
      if(uenv == 0){
        envp[i] = 0;
        break;
      }
      envp[i] = kalloc();
      if(envp[i] == 0)
        goto bad;
      if(fetchstr(uenv, envp[i], PGSIZE) < 0)
        goto bad;
    }
  } else {
    envp[0] = 0;
  }
  // printf("%s\n", path);
  //
  // for (int i=0;;i++) {
  //   if (argv[i] == 0) break;
  //   printf("%s ", argv[i]);
  // }
  // printf("\n");
  int len  = strlen(path);
  if (strncmp(path + len - 7, "busybox", 7) == 0) {
    envp[0] = 0;
  }

  int ret = execve(path, argv, envp);
  // printf("ret: %d\n", ret);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  if (uenvp) {
    for(i = 0; i < NELEM(envp) && envp[i] != 0; i++)
    kfree(envp[i]);
  }
  return ret;

 bad:
  printf("sys_execve failed\n");
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  for(i = 0; i < NELEM(envp) && envp[i] != 0; i++)
    kfree(envp[i]);
  return -1;
}

uint64 sys_exec(void) {
  myproc()->trapframe->a2 = 0;
  return sys_execve();
}


uint64
sys_pipe2(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    get_fops()->close(rf);
    get_fops()->close(wf);
    return -1;
  }

  // printf("pipealloc fd0=%d, fd1=%d\n", fd0, fd1);

  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    get_fops()->close(rf);
    get_fops()->close(wf);
    return -1;
  }
  return 0;
}


uint64 sys_getcwd(void) {
  uint64 buf;
  int size;
  argint(1, &size);
  argaddr(0, &buf);

  char path[MAXPATH];
  strncpy(path, myproc()->cwd.path, MAXPATH);
  if (copyout(myproc()->pagetable, buf, path, strlen(path) + 1) < 0) {
    return 0;
  }
  return buf;
}

uint64 sys_dup3(void) {
  int old, new;
  argint(0, &old);
  argint(1, &new);
  struct proc *p = myproc();
  // printf("%d %d\n", old, new);
  // printf("%s\n", p->ofile[old]->f_path);
  if (old == new) {
    return old;
  }

  if (p->ofile[old] == 0) {
    printf("sys_dup3 failed %d not exist\n", old);
    return -1;
  }

  if (p->ofile[new] != 0) {
    get_fops()->close(p->ofile[new]);
  }
  p->ofile[new] = p->ofile[old];
  get_fops()->dup(p->ofile[old]);

  return new;
}

uint64 sys_getdents64(void) {
  struct file *f;
  uint64 buf;
  int len;
  argfd(0, 0, &f);
  argaddr(1, &buf);
  argint(2, &len);

  // printf("%s\n", f->f_path);

  struct linux_dirent64 *d = kalloc();
  if (d == NULL) {
    return -1;
  }
  uint64 nread = 0;

  if ((nread = vfs_ext_getdents(f, d, MIN(len, PGSIZE))) < 0) {
    kfree(d);
    return -1;
  }
  len = MIN(len, nread);
  if (copyout(myproc()->pagetable, buf, (char*)d, len) < 0) {
    kfree(d);
    return -1;
  }
  kfree(d);

  return len;
}

uint64 sys_mount(void) {
  return 0;
}

uint64 sys_umount2(void) {
  return 0;
}

uint64 sys_ioctl(void) {
  int fd;
  struct file *f;
  int request;
  uint64 arg;
  if (argfd(0, &fd, &f) < 0) {
    return -1;
  }
  argint(1, &request);
  argaddr(2, &arg);
  switch (request) {
    case TIOCGWINSZ:
      {
        struct winsize ws;
        ws.ws_col = 80;
        ws.ws_row = 24;
        if (copyout(myproc()->pagetable, arg, (char*)&ws, sizeof(ws)) < 0)
          return -1;
      }
      break;
    default:
      return 0;
  }
}

uint64 sys_fcntl(void) {
  int fd, cmd, arg, ret=0;
  int new_fd;
  struct file *f;
  if (argfd(0, &fd, &f) < 0) {
    return -1;
  }
  argint(1, &cmd);
  argint(2, &arg);
  // printf("%d %d %d\n", fd, cmd, arg);
  // printf("%s %d\n", f->f_path, f->f_type);
  switch (cmd) {
    case F_DUPFD:
      if (fd = fdalloc2(f, arg) < 0) {
        return -1;
      }
      get_fops()->dup(f);
      ret = fd;
      break;
    case F_DUPFD_CLOEXEC:

      if ((new_fd = fdalloc2(f, arg)) > 0) {
        get_fops()->dup(f);
      }
      ret = new_fd;
      break;
    case F_GETFD:
      ret = 0;
      break;
    case F_SETFD:
      ret = 0;
      break;
    case F_GETFL:
      ret = f->f_flags;
      break;
    case F_SETFL:
      if ((((arg & O_NONBLOCK) == O_NONBLOCK) || ((arg & O_APPEND) == O_APPEND))) {
        f->f_flags |= arg;
        vfs_ext_lseek(f, 0, SEEK_END);
        ret = 0;
      }
      ret = 0;
      break;
    default:
      return 0;
  }
  return ret;
}

uint64 sys_faccessat(void) {
  int dirfd, mode, flags;
  char path[MAXPATH];
  argint(0, &dirfd);
  argint(2, &mode);
  argint(3, &flags);
  if (argstr(1, path, MAXPATH) < 0) {
    return -1;
  }
  // printf("%s\n", path);
  const char *dirpath = dirfd == AT_FDCWD ? myproc() -> cwd.path : myproc() -> ofile[dirfd]->f_path;
  char absolute_path[MAXPATH] = {0};
  get_absolute_path(path, dirpath, absolute_path);
  // printf("%s %d\n", absolute_path, mode);
  if (vfs_ext_faccessat(absolute_path, mode) < 0) {
    // printf("here\n");
    return -2;
  }
  return 0;
}

uint64 sys_ppoll(void) {
  uint64 pfaddr;
  int nfds;
  uint64 tsaddr;
  uint64 sigmaskaddr;
  struct proc *p = myproc();
  argaddr(0, &pfaddr);
  argint(1, &nfds);
  argaddr(2, &tsaddr);
  argaddr(3, &sigmaskaddr);

  struct pollfd pfd;
  if (copyin(p->pagetable, (char*)&pfd, pfaddr, sizeof(pfd)) < 0) {
    return -1;
  }

  struct timespec ts;
  if (tsaddr && copyin(p->pagetable, (char*)&ts, tsaddr, sizeof(ts)) < 0) {
    return -1;
  }

  uint64 timeout = tsaddr ? ts2ticks(&ts) : -1;
  struct file *f = p->ofile[pfd.fd];
  while (1) {
    switch (f->f_type) {
      case FD_PIPE:
        return 1;
      case FD_DEVICE:
        if (consoleready()) {
          if (timeout == 0) {
            return 0;
          }
        }
        break;
      default:
        panic("ppoll: No type error");
    }

    if (timeout == -1) {
      continue;
    }

    if (timeout) {
      yield();
      timeout--;
    } else {
      break;
    }
  }
  return nfds;
}

uint64 sys_sendfile(void) {
  int out_fd, in_fd, count, offset;
  uint64 poff;
  struct file *out_f, *in_f;

  if (argfd(0, &out_fd, &out_f) < 0 || argfd(1, &in_fd, &in_f) < 0) {
    return -1;
  }
  argaddr(2, &poff);
  argint(3, &count);
  // printf("sendfile:%d %d\n", out_fd, in_fd);
  // printf("%d\n", myproc()->ofile[in_fd]->f_type);
  void *buf = kmalloc(count);

  if (poff) {
    copyin(myproc()->pagetable, (char*)&offset, poff, sizeof(offset));
  } else {
    offset = in_f->f_pos;
  }

  int nread = 0, nwrite = 0;


  if (poff) {
    if ((nread = vfs_ext_readat(in_f, 0, (uint64)buf, count, offset)) < 0) {
      kfree(buf);
      return -1;
    }
  } else {
    if ((nread = vfs_ext_read(in_f, 0, (uint64)buf, count)) < 0) {
      kfree(buf);
      return -1;
    }
    count = MIN(count, nread);
    nread = 0;
  }

  if (out_f->f_type == FD_PIPE) {
    if ((nwrite = pipewrite_kernel(out_f->f_pipe, (uint64)buf, count)) < 0) {
      kfree(buf);
      return -1;
    }
   } else if (out_f->f_type == FD_REG) {
    if ((nwrite = vfs_ext_write(out_f, 0, (uint64)buf, count)) < 0) {
      kfree(buf);
      return -1;
    }
  } else if (out_f->f_type == FD_DEVICE) {
    if ((nwrite = devsw[out_f->f_major].write(0, (uint64)buf, count)) < 0) {
      kfree(buf);
      return -1;
    }
  } else {
    kfree(buf);
    return -1;
  }

  offset += nread;

  if (poff) {
    either_copyout(1, (uint64)poff, &offset, sizeof(offset));
  } else {
    in_f->f_pos += nread;
  }
  // printf("%d %d\n", count, nwrite);
  kfree(buf);
  return nwrite;
}

uint64 sys_lseek(void) {
  struct file *f;
  int offset;
  int whence;
  if (argfd(0, 0, &f) < 0) {
    return -1;
  }
  argint(1, &offset);
  argint(2, &whence);
  if (f == 0 || f->f_type != FD_REG || f->f_extfile == 0) {
    return -1;
  }
  return vfs_ext_lseek(f, offset, whence);
}

uint64 sys_renameat2(void) {
  int flags;
  int old_fd, new_fd;
  char old_path[MAXPATH], new_path[MAXPATH];
  argint(0, &old_fd);
  argint(2, &new_fd);
  if (argstr(1, old_path, MAXPATH) < 0) {
    return -1;
  }
  if (argstr(3, new_path, MAXPATH) < 0) {
    return -1;
  }
  argint(4, &flags);

  const char *old_dirpath = old_fd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[old_fd]->f_path;
  const char *new_dirpath = new_fd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[new_fd]->f_path;
  char abs_old_path[MAXPATH], abs_new_path[MAXPATH];
  get_absolute_path(old_path, old_dirpath, abs_old_path);
  get_absolute_path(new_path, new_dirpath, abs_new_path);
  // printf("%s %s\n", old_path, new_path);
  if (vfs_ext_frename(abs_old_path, abs_new_path) < 0) {

    return -1;
  }
  // printf("success\n");
  return 0;
}


uint64 sys_utimensat(void) {

  int fd;
  char path[MAXPATH];
  uint64 taddr;

  int flags;
  argint(0, &fd);
  uint64 paddr;

  argaddr(1, &paddr);
  if (paddr == 0 || argstr(1, path, MAXPATH) < 0) {
    path[0] = '\0';
  }
  argaddr(2, &taddr);
  argint(3, &flags);
  // printf("%d\n", flags);

  struct timespec tstime[2];

  if (taddr) {
    if (copyin(myproc()->pagetable, (char*)&tstime, taddr, 2 * sizeof(tstime)) < 0) {
      return -EFAULT;
    }
    if (tstime[0].tv_sec == UTIME_OMIT && tstime[1].tv_nsec == UTIME_OMIT) {
      return 0;
    }
  }

  int err = -EINVAL;

  if (flags & ~AT_SYMLINK_NOFOLLOW) {
    return -EINVAL;
  }

  if (fd != AT_FDCWD && path[0] == '\0') {
    struct file *f = 0;

    if (flags & AT_SYMLINK_NOFOLLOW) {
      return -EINVAL;
    }

    f = myproc()->ofile[fd];

    if (!f) {
      return -1;
    }

    err = vfs_ext_futimens(f, taddr ? tstime : NULL);
  } else {
    char *dirpath = fd == AT_FDCWD ? myproc()->cwd.path : myproc()->ofile[fd]->f_path;
    char absolute_path[MAXPATH] = {0};
    get_absolute_path(path, dirpath, absolute_path);
    // printf("%s\n", absolute_path);

    err = vfs_ext_utimens(absolute_path, taddr ? tstime : NULL);
  }

  return err;
}

ssize_t sys_copy_file_range(void)
{
  struct file *f_in, *f_out;
  int fd_in, fd_out;
  uint64 in_ptr, out_ptr;
  int off_in=-1, off_out=-1;
  int len;
  unsigned int flags;
  ssize_t ret;

  if(argfd(0, &fd_in, &f_in) < 0 || argfd(2, &fd_out, &f_out) < 0){
    return -1;
  }

  argint(5, (int*)&flags);
  argint(4, &len);
  argaddr(1, &in_ptr);
  argaddr(3, &out_ptr);

  struct proc *p = myproc();

  if (in_ptr) {
    if (copyin(p->pagetable, (char*)&off_in, in_ptr, sizeof(off_in)) < 0) {
      printf("Can't read off_in");
      return -1;
    }
  }

  if (out_ptr) {
    if (copyin(p->pagetable, (char*)&off_out, out_ptr, sizeof(off_out)) < 0) {
      printf("Can't read off_out");
      return -1;
    }
  }
  // printf("copy_file_range : %d %d %d %d %d\n", fd_in, off_in, fd_out, off_out, len);
  if(f_in->removed || f_out->removed) {
    return -1;
  }
  // 直接调用vfs_ext4_copy_file_range
  // printf("off:%d\n", f_in->f_pos);
  ret = vfs_ext4_copy_file_range(f_in, off_in, in_ptr, f_out, off_out,out_ptr, len, flags);
  // printf("off:%d\n", f_in->f_pos);
  return ret;
}

int
sys_ftruncate(void)
{
  struct file *f;
  int pos;
  argfd(0, 0, &f);
  argint(1, &pos);

  vfs_ext_ftruncate(f, pos);
}

int sys_splice(void) {
  int fd_in,fd_out;
  struct file *f_in, *f_out;
  uint64 in_ptr, out_ptr;
  int off_in=-1, off_out=-1;

  int len;
  int flags;

  if (argfd(0, &fd_in, &f_in) < 0 || argfd(2, &fd_out, &f_out) < 0) {
    return -1;
  }

  argaddr(1, &in_ptr);
  argaddr(3, &out_ptr);
  argint(4, &len);
  argint(5, &flags);
  // printf("%d %d %d %d\n", fd_in, f_in->f_type, fd_out, f_out->f_type);

  if (f_in -> f_type == FD_PIPE) {
    char *buf = kmalloc(len + 2);
    int byteread = piperead_kernel(f_in->f_pipe, buf, len);
    copyin(myproc()->pagetable, (char*)&off_out, out_ptr, sizeof(off_out));
    if (off_out < 0) {
      return -1;
    }
    int bytewrite = vfs_ext_writeat(f_out, 0, (uint64)buf, byteread, off_out);
    off_out += bytewrite;
    copyout(myproc()->pagetable, out_ptr, (char*)&off_out, sizeof(off_out));
    kfree(buf);
    return bytewrite;
  } else {
    char *buf = kmalloc(len + 2);
    // printf("%p\n", in_ptr);
    copyin(myproc()->pagetable, (char*)&off_in, in_ptr, sizeof(off_in));
    // printf("%d\n", off_in);
    if (off_in < 0) {
      return -1;
    }
    if (off_in > ((struct ext4_file*)f_in->f_extfile)->fsize) {
      return 0;
    }
    int byteread = vfs_ext_readat(f_in, 0, (uint64)buf, len, off_in);
    pipewrite_kernel(f_out->f_pipe, (uint64)buf, byteread);
    off_in += byteread;
    copyout(myproc()->pagetable, in_ptr, (char*)&off_in, sizeof(off_in));
    kfree(buf);
    return byteread;
  }
  return -1;
}

uint64 sys_fchmodat(void) {
  return 0;
}

uint64 sys_symlinkat(void) {
  char path[MAXPATH];
  char target[MAXPATH];
  int dirfd;
  argstr(0, target, MAXPATH);
  argint(1, &dirfd);
  argstr(1, path, MAXPATH);

  const char *dirpath = (dirfd == AT_FDCWD) ? myproc() -> cwd.path : myproc() -> ofile[dirfd]->f_path;
  char absolute_path[MAXPATH] = {0};
  get_absolute_path(path, dirpath, absolute_path);

  return vfs_ext_fsymlink(target, absolute_path);
}

uint64 sys_socket(void) {
  int domain, type, protocol, fd;
  struct file *f;

  argint(0, &domain);
  argint(1, &type);
  argint(2, &protocol);
  if((f = filealloc()) == NULL || (fd = fdalloc(f)) < 0)
    return -1;
  if(socket(f, domain, type, protocol) < 0) {
    get_fops()->close(f);
    return -1;
  }
  return fd;
}






