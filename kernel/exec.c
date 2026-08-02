#include "elf.h"
#include "defs.h"
#include "types.h"
#include "string.h"
#include "print.h"
#include "virt.h"
#include "vmem.h"
#include "defs.h"
#include "process.h"
#include "cpu.h"
#include "inode.h"
#include "ext4_oflags.h"
#include "file.h"
#include "vfs_ext4.h"
#include "fs_defs.h"
#include "vma.h"
#if defined RISCV
#include "riscv.h"
#include "riscv_memlayout.h"
#else
#include "loongarch.h"
#endif
// 重定向枚举类型
enum redir
{
    REDIR_OUT,
    REDIR_APPEND,
};
#ifndef S_IFMT
#define S_IFMT 00170000
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#define EXEC_MAX_SYMLINKS 8
static int flags_to_perm(int flags);
static int loadseg(pgtbl_t pt, uint64 va, struct inode *ip, uint offset, uint sz);
static int elf_load_segments(pgtbl_t pt, struct inode *ip,
                             const elf_header_t *ehdr, uint64 load_bias,
                             uint64 *low_vaddr, uint64 *high_vaddr);
void alloc_aux(uint64 *aux, uint64 atid, uint64 value);
int loadaux(pgtbl_t pt, uint64 sp, uint64 stackbase, uint64 *aux);
void debug_print_stack(pgtbl_t pagetable, uint64 sp, uint64 argc, uint64 envc, uint64 aux[]);
static uint64 load_interpreter(pgtbl_t pt, struct inode *ip, elf_header_t *interpreter);
static int has_suffix(const char *path, const char *suffix);
static void exec_parent_dir_from_path(const char *path, char *parent);
static int resolve_exec_path(const char *path, char *resolved);
static void exec_single_thread(proc_t *p);
static void exec_reset_user_regs(struct trapframe *trapframe);
static int exec_trace_once = 0;

/*
 * Establish a complete ELF image before returning to user mode.  PT_LOAD
 * records may share a page and PIE images do not start at virtual address
 * zero, so treating every record as an independent uvm_grow() interval is
 * not valid.  The loader maps each page once, copies exactly p_filesz bytes,
 * and only then applies the union of the segment permissions for that page.
 */
static int
elf_load_segments(pgtbl_t pt, struct inode *ip, const elf_header_t *ehdr,
                  uint64 load_bias, uint64 *low_vaddr, uint64 *high_vaddr)
{
    program_header_t ph;
    uint64 low = MAXVA;
    uint64 high = 0;
    int have_load = 0;

    for (int i = 0; i < ehdr->phnum; i++)
    {
        uint64 phoff = ehdr->phoff + (uint64)i * ehdr->phentsize;
        uint64 seg_start;
        uint64 seg_end;

        if (ip->i_op->read(ip, 0, (uint64)&ph, phoff, sizeof(ph)) != sizeof(ph))
            return -1;
        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr ||
            ph.off + ph.filesz < ph.off)
            return -1;

        seg_start = load_bias + ph.vaddr;
        seg_end = seg_start + ph.memsz;
        if (seg_end < seg_start || seg_end > MAXVA)
            return -1;

        for (uint64 va = PGROUNDDOWN(seg_start); va < PGROUNDUP(seg_end); va += PGSIZE)
        {
            pte_t *pte = walk(pt, va, 0);

            if (pte != NULL && (*pte & PTE_V))
                continue;
            {
                char *page = (char *)pmem_alloc_pages(1);
                if (page == NULL)
                    return -1;
                memset(page, 0, PGSIZE);
                if (mappages(pt, va, (uint64)page, PGSIZE,
                             PTE_R | PTE_W | PTE_U) != 1)
                {
                    pmem_free_pages(page, 1);
                    return -1;
                }
            }
        }

        /* Copy only the bytes belonging to this segment.  The surrounding
         * parts of a page are already zero-filled and may belong to another
         * PT_LOAD record. */
        for (uint64 copied = 0; copied < ph.filesz; )
        {
            uint64 va = seg_start + copied;
            uint64 page_off = va & (PGSIZE - 1);
            uint64 n = MIN(PGSIZE - page_off, ph.filesz - copied);
            uint64 pa = walkaddr(pt, PGROUNDDOWN(va));

            if (pa == 0 || ip->i_op->read(ip, 0, pa + page_off,
                                           ph.off + copied, n) != n)
                return -1;
            copied += n;
        }

        if (seg_start < low)
            low = seg_start;
        if (seg_end > high)
            high = seg_end;
        have_load = 1;
    }

    if (!have_load)
        return -1;

    /* First discard the temporary writable permissions. */
    for (int i = 0; i < ehdr->phnum; i++)
    {
        uint64 phoff = ehdr->phoff + (uint64)i * ehdr->phentsize;
        if (ip->i_op->read(ip, 0, (uint64)&ph, phoff, sizeof(ph)) != sizeof(ph))
            return -1;
        if (ph.type != ELF_PROG_LOAD)
            continue;
        for (uint64 va = PGROUNDDOWN(load_bias + ph.vaddr);
             va < PGROUNDUP(load_bias + ph.vaddr + ph.memsz); va += PGSIZE)
        {
            pte_t *pte = walk(pt, va, 0);
            if (pte == NULL || !(*pte & PTE_V))
                return -1;
            *pte &= ~(PTE_R | PTE_W | PTE_X);
        }
    }

    /* A writable RISC-V leaf must also be readable. */
    for (int i = 0; i < ehdr->phnum; i++)
    {
        uint64 phoff = ehdr->phoff + (uint64)i * ehdr->phentsize;
        int perm;
        if (ip->i_op->read(ip, 0, (uint64)&ph, phoff, sizeof(ph)) != sizeof(ph))
            return -1;
        if (ph.type != ELF_PROG_LOAD)
            continue;
        perm = flags_to_perm(ph.flags);
#if defined RISCV
        if (perm & PTE_W)
            perm |= PTE_R;
#endif
        for (uint64 va = PGROUNDDOWN(load_bias + ph.vaddr);
             va < PGROUNDUP(load_bias + ph.vaddr + ph.memsz); va += PGSIZE)
            *walk(pt, va, 0) |= perm;
    }
    sfence_vma();
    *low_vaddr = low;
    *high_vaddr = PGROUNDUP(high);
    return 0;
}

int is_sh_script(char *path);
int exec(char *path, char **argv, char **env)
{
    // load_elf_from_disk(0);
    struct inode *ip;
    char *original_path = path;
    char resolved_path[MAXPATH];
    uint64 ustack[NARG + 2] = {0};
    uint64 estack[NENV + 2] = {0};
    char *modified_argv[MAXARG] = {0};
    uint64 aux[MAXARG * 2 + 3] = {0};
    uint64 execfn_sp = 0;

    if (resolve_exec_path(path, resolved_path) == 0)
        path = resolved_path;

    if (FINAL_DEV_DIAG)
        printf("[diag][exec] requested=%s resolved=%s\n", original_path, path);

    /* 脚本处理，如果是shell脚本，替换为busybox执行 */
    int is_shell_script = is_sh_script(path); ///< 判断路径是否为shell脚本
    if (is_shell_script)
    {
        original_path = "/musl/busybox"; ///< 若为脚本，替换为busybox执行脚本
        modified_argv[0] = "busybox";
        modified_argv[1] = "sh";
        modified_argv[2] = path;
        int i;
        for (i = 3; i < MAXARG - 1 && argv[i - 2] != NULL; i++) ///< 跳过原argv[0]，传入解析后的脚本路径
        {
            modified_argv[i] = argv[i - 1];
        }
        modified_argv[i] = NULL;
        argv = modified_argv;
        path = original_path;
    }
    strcpy(myproc()->exe_path, path);
    /* 打开目标文件 */
    if ((ip = namei(path)) == NULL)
    {
        printf("exec: fail to find file %s\n", path);
        return -1;
    }
    elf_header_t ehdr;
    program_header_t ph;
    program_header_t interp; //< 保存interp程序头地址，用来读取所需解释器的name
    int ret;
    int is_dynamic = 0;
    const char *bad_stage = "unknown";
    /// @todo : 对ip上锁
    /* 读取ELF头部信息并进行验证 */
    if (ip->i_op->read(ip, 0, (uint64)&ehdr, 0, sizeof(ehdr)) != sizeof(ehdr)) ///< 读取Elf头部信息
    {
        bad_stage = "read-ehdr";
        goto bad;
    }
    if (ehdr.magic != ELF_MAGIC) ///< 判断是否为ELF文件
    {
        printf("错误:不是有效的ELF文件\n");
        return -1;
    }

    /* 准备新进程环境 */
    proc_t *p = myproc();
    pgtbl_t old_pt = p->pagetable;
    uint64 old_virt_addr = p->virt_addr;
    uint64 oldsz = p->sz;
    exec_single_thread(p);
    p->sz = 0;
    free_vma_list(p);                      ///< 清除进程原来映射的VMA空间
    vma_init(p);                           ///< 初始化VMA列表
    pgtbl_t new_pt = proc_pagetable(p);    ///< 给进程分配新的页表
    uint64 low_vaddr = 0;
    uint64 sz = 0;
    uint64 load_bias = 0;
    uint64 at_phdr = 0;
    int off;
    if (new_pt == NULL)
        panic("alloc new_pt\n");
    if (ehdr.type == ELF_TYPE_DYN)
    {
        // LTP test binaries are PIE. Map them at a low, non-zero base so
        // the loader sees relocated AT_* values and page zero stays unmapped.
        load_bias = 0x10000UL;
    }
    int i;
    /* Find interpreter and auxiliary-vector locations.  PT_LOAD records are
     * established by elf_load_segments below as one coherent image. */
    for (i = 0, off = ehdr.phoff; i < ehdr.phnum; i++, off += sizeof(ph))
    {
        if (ip->i_op->read(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
        {
            bad_stage = "read-phdr";
            goto bad;
        }
        if (ph.type == ELF_PROG_INTERP)
        {
            is_dynamic = 1;
            memmove((void *)&interp, (const void *)&ph, sizeof(ph)); //< 拷贝到interp，不然ph下一轮就被覆写了
        }
        if (ph.type == ELF_PROG_PHDR)
        {
            at_phdr = load_bias + ph.vaddr;
        }
        // if(ph.type == ELF_PROG_PHDR)
        // {
        //     //< 本来想加载PHDR的，但是发现没有作用
        // }
    }
    if (elf_load_segments(new_pt, ip, &ehdr, load_bias,
                          &low_vaddr, &sz) < 0)
    {
        bad_stage = "load-program-segments";
        goto bad;
    }
    if (at_phdr == 0)
        at_phdr = load_bias + ehdr.phoff;
    /* 设置进程内存，页表，虚拟地址，为动态映射mmap做准备 */
    p->virt_addr = low_vaddr;
    p->sz = sz;
    p->pagetable = new_pt; ///< 便于mmap映射

    /*----------------------------处理动态链接--------------------------*/
    uint64 interp_start_addr = 0;
    elf_header_t interpreter;
    if (is_dynamic && strcmp(myproc()->cwd.path, "/glibc/basic") && strcmp(myproc()->cwd.path, "/musl/basic"))
    {
        /* 从INTERP段读取所需的解释器 */
        char interp_name[256];
        if (interp.filesz > 256) //< 应该不会大于64吧
        {
            panic("interp段长度大于256,缓冲区不够读了!\n");
        }
        // interp.off表示interp段在elf文件中的偏移量。interp.filesz表示其长度。
        // interp段是一个字符串，例如/lib/ld-linux-riscv64-lp64d.so.1加上结尾的\0是0x21长
        ip->i_op->read(ip, 0, (uint64)interp_name, interp.off, interp.filesz); //< 读取字符串到interp_name
        DEBUG_LOG_LEVEL(LOG_INFO, "elf文件%s所需的解释器: %s\n", path, interp_name);
        free_inode(ip);
        if (!strcmp((const char *)interp_name, "/lib/ld-linux-riscv64-lp64d.so.1")) //< rv glibc dynamic
        {
            if ((ip = namei("/usr/lib/riscv64-linux-gnu/ld-linux-riscv64-lp64d.so.1")) == NULL)
            {
                LOG_LEVEL(LOG_ERROR, "exec: fail to find interpreter: %s\n", interp_name);
                return -1;
            }
        }
        else if (!strcmp((const char *)interp_name, "/lib/ld-musl-riscv64-sf.so.1") ||
                 !strcmp((const char *)interp_name, "/lib/ld-musl-riscv64.so.1")) //< rv musl dynamic
        {
            if ((ip = namei("lib/libc.so")) == NULL) ///< musl加载libc.so就行了
            {
                LOG_LEVEL(LOG_ERROR, "exec: fail to find libc.so for riscv musl\n");
                return -1;
            }
        }
        else if (!strcmp((const char *)interp_name, "/lib64/ld-musl-loongarch-lp64d.so.1")) //< la musl dynamic
        {
            if ((ip = namei("lib/libc.so")) == NULL) ///< musl加载libc.so就行了
            {
                LOG_LEVEL(LOG_ERROR, "exec: fail to find libc.so for loongarch musl\n");
                return -1;
            }
        }
        else if (!strcmp((const char *)interp_name, "/lib64/ld-linux-loongarch-lp64d.so.1")) //< la glibc dynamic
        {
            if ((ip = namei("lib/ld-linux-loongarch-lp64d.so.1")) == NULL) ///< 现在这个解释器加载动态库的时候有问题
            {
                LOG_LEVEL(LOG_ERROR, "exec: fail to find libc.so for loongarch musl\n");
                return -1;
            }
        }
        else
        {
            LOG_LEVEL(LOG_ERROR, "unknown interpreter: %s\n", interp_name);
        }

        // program_header_t  interpreter_ph; ld-linux-riscv64-lp64d.so.1 libc.so.6 ld-linux-loongarch-lp64d.so.1

        if (ip->i_op->read(ip, 0, (uint64)&interpreter, 0, sizeof(interpreter)) != sizeof(interpreter)) ///< 读取Elf头部信息
        {
            bad_stage = "read-interpreter-ehdr";
            goto bad;
        }
        if (interpreter.magic != ELF_MAGIC) ///< 判断是否为ELF文件
        {
            printf("错误：不是有效的ELF文件\n");
            return -1;
        }
        interp_start_addr = load_interpreter(new_pt, ip, &interpreter); ///< 加载解释器
    }
    free_inode(ip);

    /*----------------------------结束动态链接--------------------------*/
// 5. 打印入口点信息
#if DEBUG
    printf("ELF加载完成，入口点: 0x%lx\n", ehdr.entry);
#endif
    /* 设置程序入口点 */
    uint64 program_entry = 0;
    if (interp_start_addr)
        program_entry = interp_start_addr + interpreter.entry; ///< 动态链接地址
    else
        program_entry = load_bias + ehdr.entry; ///< 设置程序的entry地址
    alloc_vma_stack(p);             ///< 给进程分配栈空间
    uint64 sp = get_proc_sp(p);     ///< 获取栈指针
    uint64 stackbase = sp - USER_STACK_SIZE;

    /*-------------------------------   开始处理glibc环境    -----------------------------*/
    int redirection = -1;
    char *redir_file = NULL;
    int argc;
    int redirend = -1;
    int first = -1;
    // 处理重定向符号 > 或 >>
    for (argc = 0; argv[argc]; argc++)
    {
        if (strlen(argv[argc]) == 1 && strncmp(argv[argc], ">", 1) == 0)
        {
            redirection = REDIR_OUT;
        }
        else if (strlen(argv[argc]) == 2 && strncmp(argv[argc], ">>", 2) == 0)
        {
            redirection = REDIR_APPEND;
        }
        if (redirection != -1 && first == -1)
        {
            redir_file = argv[argc + 1];
            first = 1;
            redirend = argc; ///< 标记重定向结束位置
            continue;
        }
    }

    sp -= strlen(path) + 1;
    sp -= sp % 16;
    assert(sp > stackbase, "sp out of range!");
    ret = copyout(new_pt, sp, path, strlen(path) + 1);
    if (ret < 0)
    {
        bad_stage = "copy-execfn";
        goto bad;
    }
    execfn_sp = sp;

    /// 遍历环境变量数组 env，将每个环境变量字符串复制到用户栈 environment ASCIIZ str
    int envc = 0;
    int startup_aux_words = 0;
    estack[0] = 0;
    sp -= sp % 16;
    if (env)
    {
        for (envc = 0; env[envc]; envc++)
        {
            uint64 index = ++estack[0];
            assert(envc < NENV, "envc out of range!");
            sp -= strlen(env[envc]) + 1;
            sp -= sp % 16;
            assert(sp > stackbase, "sp out of range!");
            ret = copyout(new_pt, sp, (char *)env[envc], strlen(env[envc]) + 1);
            if (ret < 0)
            {
                bad_stage = "copy-env-str";
                goto bad;
            }
            estack[index] = sp;
            estack[index + 1] = 0;
        }
        estack[estack[0] + 1] = 0;
    }

    /// 填充ustack
    ustack[0] = 0;
    if (argv)
    {
        for (argc = 0; argv[argc] && argc != redirend; argc++)
        {
            uint64 index = ++ustack[0];
            assert(argc < NARG, "argc out of range!");
            sp -= strlen(argv[argc]) + 1;
            sp -= sp % 16;
            assert(sp > stackbase, "sp out of range!");
            ret = copyout(new_pt, sp, (char *)argv[argc], strlen(argv[argc]) + 1);
            if (ret < 0)
            {
                bad_stage = "copy-argv-str";
                goto bad;
            }
            ustack[index] = sp;
            ustack[index + 1] = 0;
        }
    }
    ustack[ustack[0] + 1] = 0; // 添加终止符 NULL
    // 随机数
    sp -= 16;
    uint64 random[2] = {0x7be6f23c6eb43a7e, 0xb78b3ea1f7c8db96}; /// AT_RANDOM值
    if (sp < stackbase || copyout(new_pt, sp, (char *)random, 16) < 0)
    {
        bad_stage = "copy-random";
        goto bad;
    }
    /// auxv 填充辅助变量

    alloc_aux(aux, AT_HWCAP, 0);
    alloc_aux(aux, AT_PAGESZ, PGSIZE);
    alloc_aux(aux, AT_PHDR, at_phdr);                   // 程序头表地址
    // LOG_LEVEL(LOG_ERROR,"ehdr.phoff + p->virt_addr: %x\n",ehdr.phoff + p->virt_addr); //< 红字显示信息，更醒目 :) .本来是想看ehdr头的地址，但是好像没有影响
    alloc_aux(aux, AT_PHENT, ehdr.phentsize); // 程序头大小
    alloc_aux(aux, AT_PHNUM, ehdr.phnum);
    alloc_aux(aux, AT_BASE, interp_start_addr); // 解释器基址
    alloc_aux(aux, AT_ENTRY, load_bias + ehdr.entry);  // 程序入口
    alloc_aux(aux, AT_UID, 0);                  // 用户ID
    alloc_aux(aux, AT_EUID, 0);                 // 有效用户ID
    alloc_aux(aux, AT_GID, 0);                  // 组ID
    alloc_aux(aux, AT_EGID, 0);                 // 有效组ID
    alloc_aux(aux, AT_SECURE, 0);               // 安全模式
    alloc_aux(aux, AT_RANDOM, sp);              // 随机数地址
    alloc_aux(aux, AT_EXECFN, execfn_sp);       // 实际执行文件路径
    alloc_aux(aux, AT_FLAGS, 0);                // 标志位
    alloc_aux(aux, AT_NULL, 0);                 // 结束标志

    startup_aux_words = 2 * aux[0] + 2;

    /* Load Aux */
    if ((sp = loadaux(new_pt, sp, stackbase, aux)) == -1)
    {
        printf("loadaux failed\n");
        bad_stage = "loadaux";
        goto bad;
    }

    /// 复制环境变量指针数组（即使没有环境变量也要复制NULL指针）
    argc = estack[0];
    if (argc)
    {
        sp -= (estack[0] + 1) * sizeof(uint64); // +1 为终止NULL
        sp -= sp % 16;                          // 确保栈指针对齐
                                                // 复制从 estack[1] 开始的所有指针（包含终止NULL）
        if (copyout(new_pt, sp, (char *)(estack + 1),
                    (argc + 1) * sizeof(uint64)) < 0)
        {
            bad_stage = "copy-envp";
            goto bad;
        }
    }

    argc = ustack[0];
    sp -= (argc + 2) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase)
    {
        bad_stage = "argv-stack-range";
        goto bad;
    }
    if (copyout(new_pt, sp, (char *)ustack, (argc + 2) * sizeof(uint64)) < 0)
    {
        bad_stage = "copy-argvp";
        goto bad;
    }

    {
        int env_words = estack[0];
        int arg_words = ustack[0];
        int startup_words = 0;
        uint64 startup[1 + (NARG + 1) + (NENV + 1) + (MAXARG * 2 + 2)];

        startup[startup_words++] = arg_words;
        for (i = 1; i <= arg_words; i++)
            startup[startup_words++] = ustack[i];
        startup[startup_words++] = 0;
        for (i = 1; i <= env_words; i++)
            startup[startup_words++] = estack[i];
        startup[startup_words++] = 0;
        for (i = 1; i <= startup_aux_words; i++)
            startup[startup_words++] = aux[i];

        sp -= startup_words * sizeof(uint64);
        sp -= sp % 16;
        if (sp < stackbase)
        {
            bad_stage = "startup-stack-range";
            goto bad;
        }
        if (copyout(new_pt, sp, (char *)startup,
                    startup_words * sizeof(uint64)) < 0)
        {
            bad_stage = "copy-startup";
            goto bad;
        }
    }

    /*
     * exec starts a new user ABI context.  Keeping gp, ra, or saved
     * registers from the old image is invalid and breaks position-independent
     * glibc startup after an exec from BusyBox.
     */
    exec_reset_user_regs(p->trapframe);
    p->trapframe->a0 = ustack[0];
    p->trapframe->a1 = sp + sizeof(uint64);
    p->trapframe->a2 = sp + sizeof(uint64) * (ustack[0] + 2);
    /*
     * A freshly exec'd image must not inherit the previous program's TLS
     * thread pointer. Static glibc binaries may touch stack-protector or TLS
     * state very early during startup, so carrying over a stale tp can make
     * them abort immediately with stack smashing detection.
     */
    p->trapframe->tp = 0;
#if defined RISCV
    p->trapframe->epc = program_entry;
#else
    p->trapframe->era = program_entry;
#endif
    p->trapframe->sp = sp;
    if (FINAL_DEV_DIAG && has_suffix(path, "rustup"))
        debug_print_stack(new_pt, sp, ustack[0], estack[0], aux);
    if (!exec_trace_once && has_suffix(path, "abort01"))
    {
        exec_trace_once = 1;
        debug_print_stack(new_pt, sp, ustack[0], estack[0], aux);
    }
#if DEBUG
    printf("Jump to entry: 0x%lx (interp base: 0x%lx)\n",
           program_entry, interp_start_addr);
    debug_print_stack(new_pt, sp, ustack[0], estack[0], aux);
#endif
    /// 处理重定向
    if (redirection != -1)
    {
        get_file_ops()->close(p->ofile[1]); ///< 标准输出
        myproc()->ofile[1] = 0;
        const char *dirpath = myproc()->cwd.path;
        if (redirection == REDIR_OUT)
        {
            vfs_ext4_open(redir_file, dirpath, O_WRONLY);
        }
        else if (redirection == REDIR_APPEND)
        {
            vfs_ext4_open(redir_file, dirpath, O_WRONLY | O_APPEND);
        }
    }
    /// 清理旧进程资源
    if (old_pt)
    {
        vmunmap(old_pt, TRAMPOLINE, 1, 0);
        vmunmap(old_pt, TRAPFRAME, 1, 0);
        uvmfree(old_pt, old_virt_addr, oldsz - old_virt_addr);
    }

    return 0;
    //< FUCK GLIBC!!!

bad:
    if (FINAL_DEV_DIAG)
        printf("[diag][exec-bad] stage=%s path=%s original=%s argc=%d envc=%d sp=0x%lx oldsz=0x%lx\n",
               bad_stage, path, original_path, (int)ustack[0], (int)estack[0], sp, oldsz);
    panic("exec error!\n");
    return -1;
}

static void exec_single_thread(proc_t *p)
{
    thread_t *current = p->main_thread;
    acquire(&p->lock);
    for (struct list_elem *e = list_begin(&p->thread_queue);
         e != list_end(&p->thread_queue); e = list_next(e))
    {
        thread_t *t = list_entry(e, thread_t, elem);
        if (t != current)
            t->state = t_ZOMBIE;
    }
    p->thread_num = 1;
    release(&p->lock);
}

static void exec_reset_user_regs(struct trapframe *trapframe)
{
#if defined RISCV
    trapframe->ra = 0;
    trapframe->sp = 0;
    trapframe->gp = 0;
    trapframe->tp = 0;
    trapframe->t0 = trapframe->t1 = trapframe->t2 = 0;
    trapframe->s0 = trapframe->s1 = 0;
    trapframe->a0 = trapframe->a1 = trapframe->a2 = trapframe->a3 = 0;
    trapframe->a4 = trapframe->a5 = trapframe->a6 = trapframe->a7 = 0;
    trapframe->s2 = trapframe->s3 = trapframe->s4 = trapframe->s5 = 0;
    trapframe->s6 = trapframe->s7 = trapframe->s8 = trapframe->s9 = 0;
    trapframe->s10 = trapframe->s11 = 0;
    trapframe->t3 = trapframe->t4 = trapframe->t5 = trapframe->t6 = 0;
#else
    trapframe->ra = trapframe->tp = trapframe->sp = 0;
    trapframe->a0 = trapframe->a1 = trapframe->a2 = trapframe->a3 = 0;
    trapframe->a4 = trapframe->a5 = trapframe->a6 = trapframe->a7 = 0;
    trapframe->t0 = trapframe->t1 = trapframe->t2 = trapframe->t3 = 0;
    trapframe->t4 = trapframe->t5 = trapframe->t6 = trapframe->t7 = 0;
    trapframe->t8 = trapframe->r21 = trapframe->fp = 0;
    trapframe->s0 = trapframe->s1 = trapframe->s2 = trapframe->s3 = 0;
    trapframe->s4 = trapframe->s5 = trapframe->s6 = trapframe->s7 = 0;
    trapframe->s8 = 0;
#endif
}

static int has_suffix(const char *path, const char *suffix)
{
    int path_len = strlen(path);
    int suffix_len = strlen(suffix);

    if (path_len < suffix_len)
        return 0;
    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static void exec_parent_dir_from_path(const char *path, char *parent)
{
    const char *slash = strrchr(path, '/');

    if (slash == path)
    {
        strcpy(parent, "/");
        return;
    }
    if (slash == 0)
    {
        strcpy(parent, ".");
        return;
    }

    memmove(parent, path, slash - path);
    parent[slash - path] = '\0';
}

static int resolve_exec_path(const char *path, char *resolved)
{
    char current[MAXPATH];
    char linkpath[MAXPATH];
    char parent[MAXPATH];
    struct kstat st;
    size_t readbytes = 0;
    int depth;

    memset(current, 0, sizeof(current));
    memset(linkpath, 0, sizeof(linkpath));
    memset(parent, 0, sizeof(parent));
    get_absolute_path(path, myproc()->cwd.path, current);

    for (depth = 0; depth < EXEC_MAX_SYMLINKS; depth++)
    {
        if (vfs_ext4_stat(current, &st) < 0)
            return -1;
        if ((st.st_mode & S_IFMT) != S_IFLNK)
        {
            strncpy(resolved, current, MAXPATH - 1);
            resolved[MAXPATH - 1] = '\0';
            return 0;
        }

        memset(linkpath, 0, sizeof(linkpath));
        readbytes = 0;
        if (vfs_ext4_readlink(current, linkpath, MAXPATH - 1, &readbytes) < 0)
            return -1;
        if (readbytes >= MAXPATH)
            readbytes = MAXPATH - 1;
        linkpath[readbytes] = '\0';

        if (linkpath[0] == '/')
        {
            strncpy(current, linkpath, MAXPATH - 1);
            current[MAXPATH - 1] = '\0';
        }
        else
        {
            memset(parent, 0, sizeof(parent));
            exec_parent_dir_from_path(current, parent);
            memset(current, 0, sizeof(current));
            get_absolute_path(linkpath, parent, current);
        }
    }

    return -1;
}

int is_sh_script(char *path)
{
    char magic[2] = {0};
    struct inode *ip;
    int len = strlen(path);

    if (len < 3)
    {
        goto detect_by_magic;
    }
    if (path[len - 1] == 'h' && path[len - 2] == 's' && path[len - 3] == '.')
    {
        return 1;
    }

detect_by_magic:
    ip = namei(path);
    if (ip == NULL)
        return 0;
    if (ip->i_op->read(ip, 0, (uint64)magic, 0, sizeof(magic)) != sizeof(magic))
    {
        free_inode(ip);
        return 0;
    }
    free_inode(ip);
    return magic[0] == '#' && magic[1] == '!';
}

void alloc_aux(uint64 *aux, uint64 atid, uint64 value)
{
    // printf("aux[%d] = %p\n",atid,value);
    uint64 argc = aux[0];
    aux[argc * 2 + 1] = atid;
    aux[argc * 2 + 2] = value;
    aux[argc * 2 + 3] = 0;
    aux[argc * 2 + 4] = 0;
    aux[0]++;
}

int loadaux(pgtbl_t pt, uint64 sp, uint64 stackbase, uint64 *aux)
{
    int argc = aux[0];
    if (!argc)
        return sp;
    sp -= (2 * argc + 2) * sizeof(uint64);
    if (sp < stackbase)
    {
        return -1;
    }
    aux[0] = 0;
    if (copyout(pt, sp, (char *)(aux + 1), (2 * argc + 2) * sizeof(uint64)) < 0)
        return -1;
    return sp;
}

static int flags_to_perm(int flags)
{
    int perm = 0;
#if defined RISCV
    if (flags & 0x01)
        perm |= PTE_X;
    if (flags & 0x02)
        perm |= PTE_W;
    if (flags & 0x04)
        perm |= PTE_R;
#else

    if (!(flags & 1))
        perm |= PTE_NX;
    if (flags & 2)
        perm |= PTE_W;
    if (!(flags & 4))
        perm |= PTE_NR;

#endif
    return perm;
}

/// @brief 计算解释器内存映射大小
/// @param interpreter
/// @param ip
/// @return
uint64 get_mmap_size(elf_header_t *interpreter, struct inode *ip)
{
    int i, off;
    uint64 minaddr = -1;
    uint64 maxaddr = 0;
    int flag = 0;
    program_header_t ph;
    for (i = 0, off = interpreter->phoff; i < interpreter->phnum; i++, off += sizeof(ph))
    {
        if (ip->i_op->read(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
        {
            panic("read ph error!\n");
        }
        if (ph.type == ELF_PROG_LOAD)
        {
            flag = 1;
            minaddr = MIN(minaddr, PGROUNDDOWN(ph.vaddr));
            maxaddr = MAX(maxaddr, ph.vaddr + ph.memsz);
        }
    }
    if (flag)
        return maxaddr - minaddr;
    return 0;
}
/**
 * @brief  加载段到内存
 *
 * @param pt
 * @param va
 * @param ip
 * @param offset
 * @param sz
 * @return int
 */
static int loadseg(pgtbl_t pt, uint64 va, struct inode *ip, uint offset, uint sz)
{
    uint64 pa;
    int i, n;
#if DEBUG
    LOG_LEVEL(LOG_DEBUG, "[loadseg] : va:%p,end:%p,sz:%p\n", va, va + sz, sz);
#endif
    assert(va % PGSIZE == 0, "va need be aligned!\n");
    for (i = 0; i < sz; i += PGSIZE)
    {
        pa = walkaddr(pt, va + i);
        assert(pa != 0, "pa is null!,virt_addr:%p not map!\n", va + i);
        n = MIN(sz - i, PGSIZE);
        if (ip->i_op->read(ip, 0, (uint64)pa, offset + i, n) != n)
            return -1;
    }
    return 0;
}
/**
 * @brief 加载动态链接器
 *
 * @param pt
 * @param ip
 * @param interpreter
 * @return uint64
 */
static uint64 load_interpreter(pgtbl_t pt, struct inode *ip, elf_header_t *interpreter)
{
    uint64 sz, startaddr;
    program_header_t ph;
    if ((sz = get_mmap_size(interpreter, ip)) == 0)
        panic("mmap size is zero!\n");
    /// 分配内存空间
    startaddr = mmap(0, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ALLOC |MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (startaddr == -1)
        panic("mmap error!\n");
    /// 加载解释器的每个段
    int i, off;
    for (i = 0, off = interpreter->phoff; i < interpreter->phnum; i++, off += sizeof(ph))
    {
        if (ip->i_op->read(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
        {
            panic("read ph error!\n");
        }
        if (ph.type == ELF_PROG_LOAD)
        {
            assert(ph.memsz >= ph.filesz, "ph.memsz:%d < ph.filesz:%d!");
            assert(ph.vaddr + ph.memsz >= ph.vaddr, "ph.vaddr + ph.memsz < ph.vaddr");

            uint32 margin_size = 0;
            if (ph.vaddr % PGSIZE != 0)
                margin_size = ph.vaddr % PGSIZE;

            if (loadseg(pt, PGROUNDDOWN(startaddr + ph.vaddr), ip, PGROUNDDOWN(ph.off), ph.filesz + margin_size) < 0)
                panic("loadseg error!\n");
        }
    }

    return startaddr;
}

void debug_print_stack(pgtbl_t pagetable, uint64 sp, uint64 argc, uint64 envc, uint64 aux[])
{
    PRINT_COLOR(YELLOW_COLOR_PRINT, "User stack layout at exec:\n");
    PRINT_COLOR(YELLOW_COLOR_PRINT, "position            content                     size (bytes) + comment\n");
    PRINT_COLOR(YELLOW_COLOR_PRINT, "------------------------------------------------------------------------\n");

    // 1. 打印 argc
    uint64 argc_val;
    if (copyin(pagetable, (char *)&argc_val, sp, sizeof(uint64)))
    {
        PRINT_COLOR(YELLOW_COLOR_PRINT, "  [sp]             [Failed to read argc]\n");
        return;
    }
    PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ argc = %d ]            8\n", sp, (int)argc_val);
    sp += sizeof(uint64);

    // 2. 打印 argv 数组
    for (int i = 0; i <= argc; i++)
    {
        uint64 argv_ptr;
        if (copyin(pagetable, (char *)&argv_ptr, sp + i * sizeof(uint64), sizeof(uint64)))
        {
            PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ Failed to read argv[%d] ]\n", sp + i * sizeof(uint64), i);
            continue;
        }

        if (i < argc)
        {
            char arg_str[256];
            if (copyinstr(pagetable, arg_str, argv_ptr, sizeof(arg_str)) > 0)
            {
                PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ argv[%d] = %s ]      8\n",
                            sp + i * sizeof(uint64), i, arg_str);
            }
            else
            {
                PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ argv[%d] = ? ]      8\n",
                            sp + i * sizeof(uint64), i);
            }
        }
        else
        {
            PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ argv[%d] = NULL ]      8\n",
                        sp + i * sizeof(uint64), i);
        }
    }
    sp += (argc + 1) * sizeof(uint64);
    sp += sp % 16;

    // 3. 打印 envp 数组
    for (int i = 0; i <= envc; i++)
    {
        uint64 envp_ptr;
        if (copyin(pagetable, (char *)&envp_ptr, sp + i * sizeof(uint64), sizeof(uint64)))
        {
            PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ Failed to read envp[%d] ]\n", sp + i * sizeof(uint64), i);
            continue;
        }

        if (i < envc)
        {
            char env_str[256];
            if (copyinstr(pagetable, env_str, envp_ptr, sizeof(env_str)) > 0)
            {
                PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ envp[%d] = %s ]      8\n",
                            sp + i * sizeof(uint64), i, env_str);
            }
            else
            {
                PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ envp[%d] = ? ]      8\n",
                            sp + i * sizeof(uint64), i);
            }
        }
        else
        {
            PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ envp[%d] = NULL ]      8\n",
                        sp + i * sizeof(uint64), i);
        }
    }
    sp += (envc + 1) * sizeof(uint64);

    // 4. 打印 auxv 数组
    int aux_index = 0;
    while (1)
    {
        uint64 type, val;
        if (copyin(pagetable, (char *)&type, sp, sizeof(uint64)) ||
            copyin(pagetable, (char *)&val, sp + 8, sizeof(uint64)))
        {
            break;
        }

        const char *type_str = "UNKNOWN";
        if (type == AT_ENTRY)
            type_str = "AT_ENTRY";
        else if (type == AT_PHNUM)
            type_str = "AT_PHNUM";
        else if (type == AT_PHDR)
            type_str = "AT_PHDR";
        else if (type == AT_RANDOM)
            type_str = "AT_RANDOM";
        else if (type == AT_HWCAP)
            type_str = "AT_HWCAP";
        else if (type == AT_PAGESZ)
            type_str = "AT_PAGESZ";
        else if (type == AT_PHENT)
            type_str = "AT_PHENT";
        else if (type == AT_ENTRY)
            type_str = "AT_ENTRY";
        else if (type == AT_BASE)
            type_str = "AT_BASE";
        else if (type == AT_NULL)
            type_str = "AT_NULL";

        PRINT_COLOR(YELLOW_COLOR_PRINT, "  %lx  [ %s ]      16  (type=0x%lx, val=0x%lx)\n",
                    sp, type_str, type, val);

        if (type == AT_NULL)
            break;

        sp += 16;
        aux_index++;
    }
    PRINT_COLOR(YELLOW_COLOR_PRINT, "------------------------------------------------------------------------\n");
}
