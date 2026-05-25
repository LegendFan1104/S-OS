#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "platform.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "lib/elf.h"
#include "fs/vfs/inode.h"
#include "fs/vfs/ops.h"
#include "lib/string.h"
#include "mem/kalloc.h"
#include "proc/exec.h"


struct commit {
  char *path;
  uint64 last_bss;
  uint64 elf_bss;
  uint64 size;
  uint64 interp;
  uint64 phoff;
  uint64 phentsize;
  uint64 phnum;

  uint64 entry; /* epc */
  uint64 a1;
  uint64 a2;
  uint64 sp;
};

#define ADD_AUXV(id, val) \
  do{                     \
    aux[index++] = id;    \
    aux[index++] = val;   \
  }                       \
  while(0);               \

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// Map pages at a specific virtual address range.
// Unlike uvmalloc, this does not require contiguity with existing mappings.
// Returns end VA on success, 0 on error.
static uint64
uvmmap_range(pagetable_t pagetable, uint64 va, uint64 size, int perm)
{
  char *mem;
  uint64 a;
  uint64 start = PGROUNDDOWN(va);
  uint64 end = PGROUNDUP(va + size);

  for(a = start; a < end; a += PGSIZE){
    if(walkaddr(pagetable, a) != 0)
      continue;
    mem = kalloc();
    if(mem == 0)
      return 0;
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, perm) != 0){
      kfree(mem);
      return 0;
    }
  }
  return end;
}

// Resolve dynamic linker (ld-linux / ld-musl) path.
// Tries: 1) exact PT_INTERP path  2) /mnt/glibc/lib/<basename>
//        3) /mnt/musl/lib/<basename>  4) /mnt/glibc/lib/libc.so (musl fallback)
//        5) /mnt/musl/lib/libc.so (musl fallback)
// Returns inode on success, 0 on failure.
static struct inode *
resolve_interp(const char *interp_path)
{
  struct inode *ip;
  char buf[256];
  const char *basename;
  int base_len;

  // 1. Try exact PT_INTERP path (e.g. /lib/ld-linux-riscv64-lp64d.so.1)
  ip = namei(interp_path);
  if (ip) return ip;

  // Extract basename
  basename = interp_path;
  for (const char *s = interp_path; *s; s++)
    if (*s == '/') basename = s + 1;
  base_len = strlen(basename);

  // 2. Try /mnt/glibc/lib/<basename>
  if (17 + base_len < 255) {
    memcpy(buf, "/mnt/glibc/lib/", 15);
    memcpy(buf + 15, basename, base_len + 1);
    ip = namei(buf);
    if (ip) return ip;
  }

  // 3. Try /mnt/musl/lib/<basename>
  if (15 + base_len < 255) {
    memcpy(buf, "/mnt/musl/lib/", 14);
    memcpy(buf + 14, basename, base_len + 1);
    ip = namei(buf);
    if (ip) return ip;
  }

  // 4. For musl, libc.so doubles as ldso - try glibc's libc.so first
  ip = namei("/mnt/glibc/lib/libc.so");
  if (ip) return ip;

  // 5. Try musl libc.so
  ip = namei("/mnt/musl/lib/libc.so");
  if (ip) return ip;

  return 0;
}

// Try to handle shebang (#!) line.
// On success, returns interpreter inode pointer and sets *out_argv to new argv.
// On failure, returns 0.
static struct inode *
try_shebang(struct inode *ip, char *path, char **argv,
            char *interp_buf, int interp_size, char **new_argv)
{
  char shebang_buf[256];
  struct inode *interp_ip = 0;

  int n = ip->i_op->read(ip, 0, (uint64)shebang_buf, 0, sizeof(shebang_buf));

  // Try to parse shebang line
  if (n >= 2 && shebang_buf[0] == '#' && shebang_buf[1] == '!') {
    // Find end of first line
    int i = 2;
    while (i < n && shebang_buf[i] != '\n')
      i++;

    // Skip whitespace after #!
    int start = 2;
    while (start < i && (shebang_buf[start] == ' ' || shebang_buf[start] == '\t'))
      start++;

    // Find interpreter path
    int end = start;
    while (end < i && shebang_buf[end] != ' ' && shebang_buf[end] != '\t')
      end++;

    if (end > start && end - start < interp_size) {
      memcpy(interp_buf, shebang_buf + start, end - start);
      interp_buf[end - start] = '\0';
      interp_ip = namei(interp_buf);
    }
  }

  // Fallback: if no shebang or interpreter not found, try architecture-specific default interpreter
  if (!interp_ip) {
#ifdef RISCV
    interp_ip = namei("/riscv/sh");
    if (interp_ip) {
      safestrcpy(interp_buf, "/riscv/sh", interp_size);
    }
#else
    interp_ip = namei("/sh");
    if (interp_ip) {
      safestrcpy(interp_buf, "/sh", interp_size);
    }
#endif
  }

  if (!interp_ip)
    return 0;

  // Build new argv: [interpreter, original_script, original_args...]
  new_argv[0] = interp_buf;
  new_argv[1] = path;
  int j = 2;
  for (int k = 1; argv[k] && j < MAXARG - 1; k++, j++)
    new_argv[j] = argv[k];
  new_argv[j] = 0;

  return interp_ip;
}

int flags2perm(int flags)
{
#ifdef RISCV
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
#elif defined(LOONGARCH)
    int perm = PTE_P | PTE_PLV | PTE_MAT | PTE_D;
    if((flags & 0x1) == 0) {
      perm |= PTE_NX;
    }
    if(flags & 0x2) {
      perm |= PTE_W;
    }
    return perm;
#endif
}

#ifdef LOONGARCH
//For loognarch

uint64 user_stack_push_str(pagetable_t pt, uint64 *ustack, char *str, uint64 sp
                        ) {
  uint64 argc = ++ustack[0];
  if (argc > MAXARG + 1) {
    return -1;
  }
  sp -= strlen(str) + 1;
  sp -= sp % 16; // riscv sp must be 16-byte aligned

  if (copyout(pt, sp, str, strlen(str) + 1) < 0) {
    printf("copyout failed\n");
    return -1;
  }
  ustack[argc] = sp;

  ustack[argc + 1] = 0;
  return sp;
}

void alloc_aux(uint64 *aux, uint64 atid, uint64 value) {
  // printf("aux[%d] = %p\n",atid,value);
  uint64 argc = aux[0];
  aux[argc * 2 + 1] = atid;
  aux[argc * 2 + 2] = value;
  aux[argc * 2 + 3] = 0;
  aux[argc * 2 + 4] = 0;
  aux[0]++;
}

uint64 loadaux(pagetable_t pagetable, uint64 sp, uint64 *aux) {
  int argc = aux[0];
  if (!argc)
    return sp;
  /*
  printf("aux argc:%d\n",argc);
  for(int i=1;i<=2*argc+2;i++){
    printf("final raw aux[%d] = %p\n",i,aux[i]);
  }
  */
  sp -= (2 * argc + 2) * sizeof(uint64);
  aux[0] = 0;
  if (copyout(pagetable, sp, (char *)(aux + 1),
              (2 * argc + 2) * sizeof(uint64)) < 0) {
    return -1;
              }
  return sp;
}

enum redir {
  REDIR_OUT,
  REDIR_APPEND,
};



int
execve(char *path, char **argv, char **envp)
{

  char *s, *last;
  int i, off;
  uint64 envc, argc, sz = 0, sp, ustack[MAXARG], estack[MAXENV], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  if ((ip = namei(path)) == 0) {
    // printf("11\n");
    return -1;
  }
  ip->i_op->lock(ip);

#ifdef LOONGARCH
  //if(strcmp(path, "/mnt/musl/busybox") == 0 || strcmp(path, "/mnt/glibc/busybox") == 0) {
  //  return -1;
  //}
#endif

  // Check ELF header
  if(ip->i_op->read(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC) {
    char interp_path[128];
    char *new_argv[MAXARG];
    struct inode *interp_ip = try_shebang(ip, path, argv, interp_path, sizeof(interp_path), new_argv);
    if (interp_ip) {
      ip->i_op->unlock(ip);
      ip = interp_ip;
      ip->i_op->lock(ip);
      argv = new_argv;
      if(ip->i_op->read(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;
      if(elf.magic != ELF_MAGIC)
        goto bad;
    } else {
      goto bad;
    }
  }

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;
  uint64 elf_bss = 0, last_bss = 0;
  struct commit com;
  char interp_path[128];
  int has_interp = 0;
  uint64 interp_entry = 0, interp_base = 0;
  int getphdr = 0;
  struct proghdr phdr;
  uint64 low_vaddr = 0xffffffffffffffff;
  uint64 start_vaddr = 0;


  // printf("alloc: %p\n", pagetable);
  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(ip->i_op->read(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type == ELF_PROG_INTERP) {
      int ilen = ph.filesz < 127 ? (int)ph.filesz : 127;
      if(ip->i_op->read(ip, 0, (uint64)interp_path, (uint)ph.off, (uint)ilen) != ilen)
        goto bad;
      interp_path[ilen] = '\0';
      has_interp = 1;
      continue;
    }
    if (ph.type == ELF_PROG_LOAD) {
      if (ph.memsz < ph.filesz) {
        printf("ph.memsz < ph.filesz\n");
        return -1;
      }
      if (ph.vaddr + ph.memsz < ph.vaddr) {
        printf("ph.vaddr + ph.memsz < ph.vaddr\n");
        return -1;
      }
      if (!getphdr && ph.off == 0) {
        phdr.vaddr = elf.phoff + ph.vaddr;
      }
      if (ph.vaddr < low_vaddr){
        low_vaddr = ph.vaddr;
      }
      if  (start_vaddr == 0){
        start_vaddr = ph.vaddr;
      }
      uint64 sz1;
      // printf("%p\n", ph.vaddr);
      if ((sz1 = uvmalloc(pagetable,  PGROUNDDOWN(start_vaddr), PGROUNDUP(ph.vaddr + ph.memsz), PTE_P|PTE_W|PTE_PLV|PTE_MAT|PTE_D)) == 0) {
        goto bad;
      }
      start_vaddr = PGROUNDUP(ph.vaddr + ph.memsz);
      sz = sz1;
      uint margin_size = 0;
      if((ph.vaddr % PGSIZE) != 0){
        margin_size = ph.vaddr % PGSIZE;
      }

      if(loadseg(pagetable, PGROUNDDOWN(ph.vaddr), ip, PGROUNDDOWN(ph.off), ph.filesz + margin_size) < 0)
        goto bad;

      uint64 tmp = ph.vaddr + ph.filesz;
      if (tmp > elf_bss) elf_bss = tmp;
      tmp = ph.vaddr + ph.memsz;
      if (tmp > last_bss) last_bss = tmp;
    } else if (ph.type == ELF_PROG_PHDR) {
      getphdr = 1;
      phdr = ph;
    }

  }
  com.elf_bss = elf_bss;
  com.last_bss = last_bss;
  com.size = sz;
  com.entry = elf.entry;
  com.phentsize = elf.phentsize;
  com.phnum = elf.phnum;
  com.path = path;

  sz = PGROUNDUP(sz);

  // Load dynamic linker (ld-linux / ld-musl) if the executable needs one
  if (has_interp) {
    struct inode *interp_ip = resolve_interp(interp_path);
    if (interp_ip) {
      interp_ip->i_op->lock(interp_ip);
      struct elfhdr iehdr;
      if (interp_ip->i_op->read(interp_ip, 0, (uint64)&iehdr, 0, sizeof(iehdr)) == sizeof(iehdr)
          && iehdr.magic == ELF_MAGIC) {
        interp_base = PGROUNDUP(sz);
        struct proghdr iph;
        int ok = 1;
        for(int j=0, ioff=iehdr.phoff; j<iehdr.phnum && ok; j++, ioff+=sizeof(iph)){
          if(interp_ip->i_op->read(interp_ip, 0, (uint64)&iph, ioff, sizeof(iph)) != sizeof(iph))
            break;
          if(iph.type != ELF_PROG_LOAD) continue;
          if(iph.memsz < iph.filesz) break;

          uint64 iva = interp_base + iph.vaddr;
          int xperm = flags2perm(iph.flags);
          if(uvmmap_range(pagetable, iva, iph.memsz, PTE_P|PTE_PLV|PTE_MAT|PTE_D|xperm) == 0) break;

          uint64 margin = iph.vaddr % PGSIZE;
          if(loadseg(pagetable, PGROUNDDOWN(iva), interp_ip,
                    PGROUNDDOWN(iph.off), iph.filesz + margin) < 0) break;

          // Zero BSS
          uint64 bss_start = iva + iph.filesz;
          uint64 bss_end = iva + iph.memsz;
          for(uint64 b = bss_start; b < bss_end; b += PGSIZE){
            uint64 pa = walkaddr(pagetable, b);
            if(pa){
              uint n = (bss_end - b < PGSIZE) ? (uint)(bss_end - b) : PGSIZE;
              memset((void*)pa, 0, n);
            }
          }

          uint64 seg_end = PGROUNDUP(iva + iph.memsz);
          if(seg_end > sz) sz = seg_end;
        }
        if(ok) interp_entry = interp_base + iehdr.entry;
        else { interp_entry = 0; interp_base = 0; }
      }
      interp_ip->i_op->unlock(interp_ip);
    } else {
      has_interp = 0;
    }
  }

  // printf("%p %p %p", elf_bss, last_bss, sz);

  // printf("%p\n", sz);

  ip->i_op->unlock(ip);
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate 32 pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the second as the user stack.

  sp = USTACK_TOP;
  stackbase = USTACK;
  uint64 sz1;


  if((sz1 = uvmalloc(pagetable, USTACK, USTACK + USTACK_PAGE * PGSIZE, PTE_P|PTE_W|PTE_PLV|PTE_MAT|PTE_D)) == 0)
    goto bad;

  uvmclear(pagetable, USTACK);

  // printf("%p\n", sp);
  uint64 envp2[33];
  envp2[0] = 0;
  sp = user_stack_push_str(pagetable, envp2, "UB_BINDIR=.", sp);
  // printf("%p\n", sp);


  uint64 random[2] = {0xcde142a16cb93072, 0x128a39c127d8bbf2};
  sp -= 16;
  if (copyout(pagetable, sp, (char *)random, 16) < 0) {
    printf("[exec] random copy bad\n");
    goto bad;
  }


  uint64 rd_pos = sp;

  int jump = 0;
  int redirection = -1;
  char *redir_file = 0;
  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    // if (strlen(argv[argc]) == 1 && strncmp(argv[argc], ">", 1) == 0) {
    //   // printf("redirection 1 %s\n", argv[argc]);
    //   redirection = REDIR_OUT;
    //   continue;
    // } else if (strlen(argv[argc]) == 2 && strncmp(argv[argc], ">>", 2) == 0) {
    //   // printf("redirection 2\n");
    //   redirection = REDIR_APPEND;
    //   continue;
    //   // 管道
    //   // wty
    // }
    // else if (strlen(argv[argc]) == 1 && strncmp(argv[argc], "|", 1) == 0){
    //   redirection = REDIR_ ;
    //   continue;
    // }
    if (redirection != -1 && jump == 0) {
      redir_file = argv[argc];
      jump = 1;
      continue;
    }
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    // printf("%s\n", argv[argc]);
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // printf("%p\n", phdr);
  sp -= sp % 16;

  uint64 aux[MAXARG * 2 + 3] = {0, 0, 0};
  uint64 phdr_addr = getphdr ? phdr.vaddr : (low_vaddr + elf.phoff);
  alloc_aux(aux, AT_HWCAP, 0);
  alloc_aux(aux, AT_PAGESZ, PGSIZE);
  alloc_aux(aux, AT_PHDR, phdr_addr);
  alloc_aux(aux, AT_PHENT, elf.phentsize);
  alloc_aux(aux, AT_PHNUM, elf.phnum);
  alloc_aux(aux, AT_BASE, interp_base);
  alloc_aux(aux, AT_ENTRY, elf.entry);
  alloc_aux(aux, AT_UID, 0);
  alloc_aux(aux, AT_EUID, 0);
  alloc_aux(aux, AT_GID, 0);
  alloc_aux(aux, AT_EGID, 0);
  alloc_aux(aux, AT_SECURE, 0);
  alloc_aux(aux, AT_RANDOM, sp);
  sp = loadaux(pagetable, sp, aux);



  int tmp = envp2[0];
  sp -= (tmp + 1) * sizeof(uint64);
  sp -= sp % 16;

  if (copyout(pagetable, sp, (char *)(envp2 + 1),
              (tmp + 1) * sizeof(uint64)) < 0) {
    printf("copyout failed\n");
    goto bad;
              }

  com.a2 = sp;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  com.a1 = sp;

  sp -= sizeof(uint64);
  if(copyout(pagetable, sp, (char*)&argc, sizeof(uint64)) < 0)
    goto bad;

  com.sp = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  memset(p->trapframe, 0, sizeof(*(p->trapframe)));
  p->trapframe->era = has_interp ? interp_entry : elf.entry;
  p->trapframe->sp = sp; // initial stack pointer
  p->trapframe->a1 = com.a1;
  p->trapframe->a2 = com.a2;

  // if (redirection != -1) {
  //   get_fops()->close(p->ofile[1]);
  //   p->ofile[1] = 0;
  //   int fd = 0;
  //   if (redirection == REDIR_OUT) {
  //     fd = open(redir_file, O_WRONLY | O_CREATE);
  //   } else if (redirection == REDIR_APPEND) {
  //     fd = open(redir_file, O_WRONLY | O_CREATE | O_APPEND);
  //   }
  //   if (fd != 1) {
  //     printf("[exec]:fd != 1\n");
  //     goto bad;
  //   }
  // }


  proc_freepagetable(oldpagetable, oldsz);

  return 0; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  printf("execve: Failed\n");
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  return -1;
}

#endif


#ifdef RISCV

int
execve(char *path, char **argv, char **envp)
{

  char *s, *last;
  int i, off;
  uint64 envc, argc, sz = 0, sp, ustack[MAXARG], estack[MAXENV], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  // printf("%s\n", path);
  if ((ip = namei(path)) == 0) {
    // printf("11\n");
    return -1;
  }
  ip->i_op->lock(ip);


  // Check ELF header
  if(ip->i_op->read(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC) {
    char interp_path[128];
    char *new_argv[MAXARG];
    struct inode *interp_ip = try_shebang(ip, path, argv, interp_path, sizeof(interp_path), new_argv);
    if (interp_ip) {
      ip->i_op->unlock(ip);
      ip = interp_ip;
      ip->i_op->lock(ip);
      argv = new_argv;
      if(ip->i_op->read(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;
      if(elf.magic != ELF_MAGIC)
        goto bad;
    } else {
      goto bad;
    }
  }

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  uint64 elf_bss = 0, last_bss = 0;
  struct commit com;
  char interp_path[128];
  int has_interp = 0;
  uint64 interp_entry = 0, interp_base = 0;

  // printf("alloc: %p\n", pagetable);
  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(ip->i_op->read(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type == ELF_PROG_INTERP) {
      int ilen = ph.filesz < 127 ? (int)ph.filesz : 127;
      if(ip->i_op->read(ip, 0, (uint64)interp_path, (uint)ph.off, (uint)ilen) != ilen)
        goto bad;
      interp_path[ilen] = '\0';
      has_interp = 1;
      continue;
    }
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    uint64 sz1;
    uint64 pva = PGROUNDDOWN(ph.vaddr);


    // printf("%p %p\n", pva, PGROUNDUP(ph.vaddr + ph.memsz));
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, PTE_W|PTE_X|PTE_R|PTE_U)) == 0)
      goto bad;
    sz = sz1;
    // printf("%p\n", sz);
    uint margin_size = 0;
    if((ph.vaddr % PGSIZE) != 0){
      margin_size = ph.vaddr % PGSIZE;
    }

    if(loadseg(pagetable, PGROUNDDOWN(ph.vaddr), ip, PGROUNDDOWN(ph.off), ph.filesz + margin_size) < 0)
      goto bad;



    uint64 tmp = ph.vaddr + ph.filesz;
    if (tmp > elf_bss) elf_bss = tmp;
    tmp = ph.vaddr + ph.memsz;
    if (tmp > last_bss) last_bss = tmp;
  }
  com.elf_bss = elf_bss;
  com.last_bss = last_bss;
  com.size = sz;
  com.entry = elf.entry;
  com.phentsize = elf.phentsize;
  com.phnum = elf.phnum;
  com.path = path;

  sz = PGROUNDUP(sz);

  // Load dynamic linker (ld-linux / ld-musl) if the executable needs one
  if (has_interp) {
    struct inode *interp_ip = resolve_interp(interp_path);
    if (interp_ip) {
      interp_ip->i_op->lock(interp_ip);
      struct elfhdr iehdr;
      if (interp_ip->i_op->read(interp_ip, 0, (uint64)&iehdr, 0, sizeof(iehdr)) == sizeof(iehdr)
          && iehdr.magic == ELF_MAGIC) {
        interp_base = PGROUNDUP(sz);
        struct proghdr iph;
        int ok = 1;
        for(int j=0, ioff=iehdr.phoff; j<iehdr.phnum && ok; j++, ioff+=sizeof(iph)){
          if(interp_ip->i_op->read(interp_ip, 0, (uint64)&iph, ioff, sizeof(iph)) != sizeof(iph))
            break;
          if(iph.type != ELF_PROG_LOAD) continue;
          if(iph.memsz < iph.filesz) break;

          uint64 iva = interp_base + iph.vaddr;
          int xperm = flags2perm(iph.flags);
#ifdef RISCV
          if(uvmmap_range(pagetable, iva, iph.memsz, PTE_R|PTE_U|xperm) == 0) break;
#else
          if(uvmmap_range(pagetable, iva, iph.memsz, PTE_P|PTE_PLV|PTE_MAT|PTE_D|xperm) == 0) break;
#endif

          uint64 margin = iph.vaddr % PGSIZE;
          if(loadseg(pagetable, PGROUNDDOWN(iva), interp_ip,
                    PGROUNDDOWN(iph.off), iph.filesz + margin) < 0) break;

          // Zero BSS
          uint64 bss_start = iva + iph.filesz;
          uint64 bss_end = iva + iph.memsz;
          for(uint64 b = bss_start; b < bss_end; b += PGSIZE){
            uint64 pa = walkaddr(pagetable, b);
            if(pa){
              uint n = (bss_end - b < PGSIZE) ? (uint)(bss_end - b) : PGSIZE;
              memset((void*)pa, 0, n);
            }
          }

          uint64 seg_end = PGROUNDUP(iva + iph.memsz);
          if(seg_end > sz) sz = seg_end;
        }
        if(ok) interp_entry = interp_base + iehdr.entry;
        else { interp_entry = 0; interp_base = 0; }
      }
      interp_ip->i_op->unlock(interp_ip);
    } else {
      has_interp = 0;
    }
  }

  // printf("%p %p %p", elf_bss, last_bss, sz);

  // printf("%p\n", sz);

  ip->i_op->unlock(ip);
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate 32 pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the second as the user stack.

  sp = USTACK_TOP;
  uint64 sz1;


  if((sz1 = uvmalloc(pagetable, USTACK, USTACK + USTACK_PAGE * PGSIZE, PTE_W|PTE_X|PTE_R|PTE_U)) == 0)
    goto bad;

  uvmclear(pagetable, USTACK);

  // printf("%p\n", sp);

  // printf("%p\n", sp);

  sp -= 32;
  uint64_t random[4] = {0x0, -0x114514FF114514UL, 0x2UL << 60, 0x3UL << 60};
  if(sp < stackbase || copyout(pagetable, sp, (char*)random, 32) < 0)
    goto bad;
  uint64 rd_pos = sp;



  if (envp != 0) {
      for (envc = 0;envp[envc];envc++) {
      if (envc >= MAXENV) {
        goto bad;
      }
      sp -= strlen(envp[envc]) + 1;
      sp -= sp % 16;
      if (sp < stackbase) {
        goto bad;
      }
      if (copyout(pagetable, sp, envp[envc], strlen(envp[envc]) + 1) < 0) {
        goto bad;
      }
      estack[envc] = sp;
    }
  }


  estack[envc] = 0;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    // printf("%s\n", argv[argc]);
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // printf("%p\n", phdr);
  sp -= sp % 16;

  uint64 aux[MAX_AT * 2];
  memset(aux, 0, sizeof(aux));

  for (int i=0;i<MAX_AT;i++) {
    if (i + 1 > AT_RANDOM) {
      break;
    }
    aux[i * 2] = i + 1;
  }

  aux[AT_PHDR * 2 - 1] = elf.phoff;
  aux[AT_PHENT * 2 - 1] = elf.phentsize;
  aux[AT_PHNUM * 2 - 1] = elf.phnum;
  aux[AT_PAGESZ * 2 - 1] = PGSIZE;
  aux[AT_HWCAP * 2 - 1] = 0;

  aux[AT_BASE * 2 - 1] = interp_base;
  aux[AT_ENTRY * 2 - 1] = elf.entry;
  aux[AT_UID * 2 - 1] = 0;
  aux[AT_EUID * 2 - 1] = 0;
  aux[AT_GID * 2 - 1] = 0;
  aux[AT_EGID * 2 - 1] = 0;
  aux[AT_SECURE * 2 - 1] = 0;
  aux[AT_RANDOM * 2 - 1] = rd_pos;
  aux[AT_NULL * 2 - 1] = 0;


  sp -= sizeof(aux);
  if(copyout(pagetable, sp, (char*)aux, sizeof(aux)) < 0) {
    goto bad;
  }

  if (envp[0]) {
    sp -= (envc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase) {
      goto bad;
    }
    if(copyout(pagetable, sp, (char*)estack, (envc + 1) * sizeof(uint64)) < 0)
      goto bad;
  }

  com.a2 = sp;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  com.a1 = sp;

  sp -= sizeof(uint64);
  if(copyout(pagetable, sp, (char*)&argc, sizeof(uint64)) < 0)
    goto bad;

  com.sp = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  memset(p->trapframe, 0, sizeof(*(p->trapframe)));
  p->trapframe->epc = has_interp ? interp_entry : elf.entry;
  p->trapframe->sp = sp; // initial stack pointer
  p->trapframe->a1 = com.a1;
  p->trapframe->a2 = com.a2;
  proc_freepagetable(oldpagetable, oldsz);

  return 0; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  printf("execve: Failed\n");
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  return -1;
}


#endif


// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i=0, n=0;
  uint64 pa=0;

  for(; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    // printf("%p\n", pa);
    if(ip->i_op->read(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  return 0;
}
