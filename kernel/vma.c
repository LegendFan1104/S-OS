#include "vma.h"
#include "string.h"
#include "pmem.h"
#include "cpu.h"
#include "vmem.h"
#include "types.h"
#include "vfs_ext4.h"
#include "ext4_oflags.h"
#ifdef RISCV
#include "riscv.h"
#include "riscv_memlayout.h"
#else
#include "loongarch.h"
#endif

static uint64
mmap_lower_bound(proc_t *p)
{
    uint64 lower = PGROUNDUP(p->sz);

    if (lower < PGSIZE)
        lower = PGSIZE;
    return lower;
}

static int
mmap_choose_addr(proc_t *p, uint64 len, uint64 *addr_out)
{
    struct vma *vma;
    uint64 lower = mmap_lower_bound(p);
    uint64 upper = USER_MMAP_START;

    if (len == 0)
    {
        *addr_out = PGROUNDDOWN(USER_MMAP_START);
        return 0;
    }

    len = PGROUNDUP(len);
    for (vma = p->vma->prev; vma != p->vma; vma = vma->prev)
    {
        uint64 gap_low;
        uint64 candidate;

        if (vma->addr >= upper)
            continue;
        gap_low = vma->end;
        if (gap_low < lower)
            gap_low = lower;
        if (upper > gap_low)
        {
            candidate = PGROUNDDOWN(upper - len);
            if (candidate >= gap_low && candidate < upper)
            {
                *addr_out = candidate;
                return 0;
            }
        }
        upper = vma->addr;
    }

    if (upper > lower)
    {
        uint64 candidate = PGROUNDDOWN(upper - len);

        if (candidate >= lower && candidate < upper)
        {
            *addr_out = candidate;
            return 0;
        }
    }
    return -1;
}

struct vma *vma_init(struct proc *p)
{
    struct vma *vma = (struct vma *)pmem_alloc_pages(1);
    if (vma == NULL)
    {
        panic("vma_init: pmem_alloc_pages failed\n");
        return NULL;
    }
    memset(vma, 0, PGSIZE);
    vma->type = NONE;
    vma->prev = vma->next = vma;
    p->vma = vma;
    if (alloc_mmap_vma(p, 0, USER_MMAP_START, 0, 0, 0, 0) == NULL)
    {
        panic("init vma error!");
        return NULL;
    }
    return vma;
};

/**
 * @brief 将操作系统内存保护标志转换为硬件页表项权限标志
 *
 * @details 此函数根据传入的内存保护标志 `prot`，生成对应硬件架构的页表项权限位（PTE flags）。
 * 支持两种架构：
 * 1. RISC-V 架构：
 *    - `PROT_READ` → `PTE_R`
 *    - `PROT_WRITE` → `PTE_W`
 *    - `PROT_EXEC` → `PTE_X`
 *    - `PROT_NONE` 视为无效输入，返回 `-1`。
 * 2. 其他架构（如 LA64）：
 *    - `PROT_READ` → 清除不可读标志 `PTE_NR`
 *    - `PROT_WRITE` → 设置可写标志 `PTE_W`
 *    - `PROT_EXEC` → 清除不可执行标志 `PTE_NX` 并设置访问标志 `PTE_MAT | PTE_P`
 *
 * @param prot 内存保护标志，按位组合（`PROT_READ`、`PROT_WRITE`、`PROT_EXEC` 或 `PROT_NONE`）
 * @return 硬件页表项权限值（`uint64` 类型）
 *
 * @note 关键平台差异：
 * - RISC-V 显式设置权限位，其他架构通过修改默认权限位实现。
 * - 其他架构的默认权限包含 `PTE_PLV3 | PTE_MAT | PTE_D | PTE_NR | PTE_W | PTE_NX`。
 * @see PROT_READ, PROT_WRITE, PROT_EXEC, PROT_NONE
 */
int get_mmapperms(int prot)
{
    uint64 perm = 0;
#if defined RISCV
    perm = PTE_U;
    if (prot == PROT_NONE)
        return 0; //< 这个地方先设成可读可写,似乎只可读和只可写也能通过. 不，设置成0也可以通过
    //< 如果return -1,会在glibc dynamic程序结束时报错panic:[pmem.c:103] pmem_free_pages: page_idx out of range
    if (prot & PROT_READ)
        perm |= PTE_R;
    if (prot & PROT_WRITE)
        perm |= PTE_W;
    if (prot & PROT_EXEC)
        perm |= PTE_X;
#else
    perm = PTE_PLV3 | PTE_MAT | PTE_D | PTE_NR | PTE_W | PTE_NX;
    if (prot & PROT_READ)
        // LA64  架构下，0表示可读
        perm &= ~PTE_NR;
    if (prot & PROT_WRITE)
        // 1 表示可写
        perm |= PTE_W;
    // 表示可以执行
    if (prot & PROT_EXEC)
    {
        perm |= PTE_MAT | PTE_P;
        perm &= ~PTE_NX;
    }
#endif
    return perm;
}
/**
 * @brief 修改页表项权限并返回对应的物理地址
 * @details 该函数在指定的页表中查找虚拟地址对应的页表项（PTE），进行安全校验后，添加指定的权限位，最后返回映射的物理地址。
 * 主要流程：
 * 1. 检查虚拟地址范围有效性（< MAXVA）
 * 2. 通过 `walk()` 获取页表项指针
 * 3. 验证页表项有效性（PTE_V）和用户态权限（PTE_U）
 * 4. 添加权限位（`*pte |= perm`）
 * 5. 转换页表项为物理地址（PTE2PA）
 *
 * @param pagetable [in] 页表根节点指针（pgtbl_t 类型）
 * @param va        [in] 目标虚拟地址（64位）
 * @param perm      [in] 待添加的权限标志位（如 PTE_W, PTE_X）
 * @return 物理地址（成功时）或 0（失败时）
 *
 * @retval >0  操作成功，返回虚拟地址对应的物理地址
 * @retval 0   操作失败（地址无效、PTE无效或权限不足）
 *
 * @note 关键安全约束：
 * - 仅允许修改用户态页表项（`PTE_U` 必须置位）
 * - 权限修改是叠加操作（`|=`），非覆盖（需先清除旧权限应额外处理）
 * - 调用方需确保 TLB 刷新（如 RISCV 的 `sfence_vma`）
 *
 * @warning 非原子操作！并发场景需加锁保护页表访问
 * @see walk(), PTE2PA(), PTE_V, PTE_U
 */
uint64 experm(pgtbl_t pagetable, uint64 va, uint64 perm)
{
    pte_t *pte;
    uint64 pa;
    if (va >= MAXVA)
        return 0;
    pte = walk(pagetable, va, 0);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    *pte |= perm;
    pa = PTE2PA(*pte);
    return pa;
}

int vm_protect(pgtbl_t pagetable, uint64 va, uint64 addr, uint64 perm)
{
    return experm(pagetable, va, perm);
}

uint64 mmap(uint64 start, int64 len, int prot, int flags, int fd, int offset)
{
    proc_t *p = myproc();
    int perm = get_mmapperms(prot);
    // assert(start == 0, "uvm_mmap: 0");
    //  assert(flags & MAP_PRIVATE, "uvm_mmap: 1");
    // len += PGSIZE;
    struct file *f = fd == -1 ? NULL : p->ofile[fd];

    if (fd != -1 && f == NULL)
        return -1;
    struct vma *vma = alloc_mmap_vma(p, flags, start, len, perm, fd, offset);
    if (vma == NULL)
        return -1;
    if (!(flags & MAP_FIXED))
        start = vma->addr;
    if (-1 == fd)
    {
        return start;
    }
    assert(len, "len is zero!");
    uint64 i;
    uint64 file_size = 0;

    if (f->f_type == FD_REG && f->f_data.f_vnode.data != NULL)
    {
        struct ext4_file *efile = (struct ext4_file *)f->f_data.f_vnode.data;
        file_size = efile->fsize;
    }

    for (i = 0; i < len; i += PGSIZE) //< 从offset开始读len字节  //< ?为什么la glibc一进来i就是0x8c000
    {
        // LOG_LEVEL(LOG_ERROR,"[mmap] i=%x",i);
        if ((flags & MAP_SHARED) && fd != -1)
        {
            pte_t *shared_pte = walk(p->pagetable, start + i, 0);
            if (shared_pte == NULL || (*shared_pte & PTE_V) == 0)
            {
                char *shared_mem = (char *)pmem_alloc_pages(1);
                if (shared_mem == NULL)
                    return -1;
                memset(shared_mem, 0, PGSIZE);
                if (mappages(p->pagetable, start + i, (uint64)shared_mem, PGSIZE, perm | PTE_U | PTE_D) != 1)
                {
                    pmem_free_pages(shared_mem, 1);
                    return -1;
                }
            }
        }
        uint64 pa = experm(p->pagetable, start + i, perm); //< 检查是否可以访问start + i，如果可以就返回start + i所在页的物理地址
        if (pa == 0)
            return -1;

        int remaining = len - i;
        int to_read = (remaining > PGSIZE) ? PGSIZE : remaining;

        // 读取文件内容（如果 to_read > 0）
        int bytes_read = 0;
        if (to_read > 0)
        {
            int available = 0;

            if ((uint64)offset + i < file_size)
            {
                uint64 file_remaining = file_size - ((uint64)offset + i);
                available = (file_remaining > (uint64)to_read) ? to_read : (int)file_remaining;
            }

            if (available > 0)
            {
            bytes_read = get_file_ops()->readat(
                f,
                (pa | dmwin_win0),
                available,
                offset + i);
                if (bytes_read < 0)
                {
                    return bytes_read;
                }
            }
        }

        // 文件内容不足时，填充零
        if (bytes_read < to_read)
        {
            memset((void *)((pa + bytes_read) | dmwin_win0), 0, to_read - bytes_read);
        }

        // 页面剩余部分清零
        if (to_read < PGSIZE)
        {
            memset((void *)((pa + to_read) | dmwin_win0), 0, PGSIZE - to_read);
        }
    }
    // if (aligned_len > len)
    // {
    //     size_t extra_len = aligned_len - len;
    //     uint64 prot_start = start + len;
    //     DEBUG_LOG_LEVEL(LOG_DEBUG, "Set PROT_NONE for extra pages: 0x%llx-0x%llx\n",
    //                     prot_start, prot_start + extra_len);
    //     vm_protect(p->pagetable, prot_start, extra_len, PROT_NONE);
    // }
    get_file_ops()->dup(f);
    return start;
}

int munmap(uint64 start, int len)
{
    proc_t *p = myproc();
    struct vma *vma = p->vma->next; // 从链表头部开始遍历
    uint64 end;
    uint64 orig_start = start;
    int found = 0;

    // 参数合法性检查（需页对齐）
    if (len <= 0)
    {
        return -1; // EINVAL
    }
    start = PGROUNDDOWN(start);
    end = PGROUNDUP(orig_start + len);
    if (end <= start)
        return -1;

    // 遍历所有VMA
    while (vma != p->vma)
    {
        struct vma *next_vma = vma->next; // 在修改链表之前就保存下一个节点
        uint64 vma_start = vma->addr;
        uint64 vma_end = vma->end;

        // 判断是否与当前VMA重叠
        if (vma_end > start && vma_start <= end)
        {
            found = 1;
            // 情况1：当前VMA完全在解除范围内
            if (vma_start >= start && vma_end <= end)
            {
                // 释放物理内存和页表项
                vmunmap(p->pagetable, vma_start, (vma_end - vma_start) / PGSIZE, 1);
                // 从链表中移除VMA
                vma->prev->next = vma->next;
                vma->next->prev = vma->prev;
                pmem_free_pages(vma, 1); // 释放VMA结构体
            }
            // 情况2：仅部分重叠（需分割VMA）
            else if (vma_start < start || vma_end > end)
            {
                // 先移除原VMA，避免在创建新VMA时干扰链表结构
                vma->prev->next = vma->next;
                vma->next->prev = vma->prev;
                
                // 分割为前段和后段，中间部分解除映射
                if (vma_start < start)
                {
                    // 创建前段VMA（保留start之前的区域）
                    struct vma *new_front = (struct vma *)pmem_alloc_pages(1);
                    if (new_front == NULL) {
                        panic("munmap: failed to allocate front VMA");
                        return -1;
                    }
                    
                    // 复制原VMA的属性
                    *new_front = *vma;
                    new_front->addr = vma_start;
                    new_front->end = start;
                    new_front->size = start - vma_start;
                    
                    // 插入到原VMA的位置
                    new_front->prev = vma->prev;
                    new_front->next = vma->next;
                    vma->prev->next = new_front;
                    vma->next->prev = new_front;
                }
                
                if (vma_end > end)
                {
                    // 创建后段VMA（保留end之后的区域）
                    struct vma *new_back = (struct vma *)pmem_alloc_pages(1);
                    if (new_back == NULL) {
                        panic("munmap: failed to allocate back VMA");
                        return -1;
                    }
                    
                    // 复制原VMA的属性
                    *new_back = *vma;
                    new_back->addr = end;
                    new_back->end = vma_end;
                    new_back->size = vma_end - end;
                    new_back->f_off = vma->f_off + (end - vma_start);
                    
                    // 插入到链表中
                    if (vma_start < start) {
                        // 如果前段存在，插入到前段之后
                        struct vma *front = vma->prev->next; // 新创建的前段
                        new_back->prev = front;
                        new_back->next = front->next;
                        front->next->prev = new_back;
                        front->next = new_back;
                    } else {
                        // 如果前段不存在，插入到原位置
                        new_back->prev = vma->prev;
                        new_back->next = vma->next;
                        vma->prev->next = new_back;
                        vma->next->prev = new_back;
                    }
                }
                
                // 解除重叠部分的映射
                uint64 unmap_start = (vma_start > start) ? vma_start : start;
                uint64 unmap_end = (vma_end < end) ? vma_end : end;
                if (unmap_end > unmap_start) {
                    vmunmap(p->pagetable, unmap_start, (unmap_end - unmap_start) / PGSIZE, 1);
                }
                
                // 释放原VMA
                pmem_free_pages(vma, 1);
            }
        }
        
        // 移动到下一个VMA (使用之前保存的next_vma)
        vma = next_vma;
    }

    return found ? 0 : -1; // 返回成功或失败
}

struct vma *alloc_mmap_vma(struct proc *p, int flags, uint64 start, int64 len, int perm, int fd, int offset)
{
    struct vma *vma = NULL;
    uint64 mapped_len = 0;

    if (len < 0)
        return NULL;
    if ((flags & MAP_FIXED) && (start % PGSIZE) != 0)
        return NULL;

    mapped_len = PGROUNDUP((uint64)len);
    if ((flags & MAP_FIXED) == 0)
    {
        if (mmap_choose_addr(p, mapped_len, &start) < 0)
        {
            if (FINAL_DEV_DIAG)
                printf("[diag][mmap-alloc-fail] pid=%d len=0x%lx sz=0x%lx flags=0x%x fd=%d\n",
                       p->pid, mapped_len, p->sz, flags, fd);
            return NULL;
        }
    }
    else
    {
        start = PGROUNDDOWN(start);
    }

    int isalloc = 0;
    if (flags & MAP_ALLOC)
        isalloc = 1;
    else if (fd != -1 && !(flags & MAP_SHARED))
        isalloc = 1;

    vma = alloc_vma(p, MMAP, start, mapped_len, perm, isalloc, 0);
    if (vma == NULL)
    {
        if (FINAL_DEV_DIAG)
            printf("[diag][mmap-vma-null] pid=%d start=0x%lx len=0x%lx sz=0x%lx flags=0x%x fd=%d\n",
                   p->pid, start, mapped_len, p->sz, flags, fd);
        return NULL;
    }
    vma->flags = flags;
    vma->fd = fd;
    vma->f_off = offset;
    return vma;
}

struct vma *alloc_vma(struct proc *p, enum segtype type, uint64 addr, int64 sz, int perm, int alloc, uint64 pa)
{
    // 添加空指针检查
    if (p == NULL || p->vma == NULL) {
        panic("alloc_vma: invalid process or VMA list");
        return NULL;
    }
    
    uint64 start = PGROUNDDOWN(addr);
    uint64 end = PGROUNDUP(addr + sz);

    // uint64 start = PGROUNDUP(p->sz);
    // uint64 end = PGROUNDUP(p->sz + sz);
#if DEBUG
    LOG_LEVEL(LOG_DEBUG, "[allocvma] : start:%p,end:%p,sz:%p\n", start, start + sz, sz);
#endif
    // p->sz += sz;
    // p->sz = PGROUNDUP(p->sz);
    struct vma *find_vma = p->vma->next;
    while (find_vma != p->vma)
    {
        if (end <= find_vma->addr)
            break;
        else if (start >= find_vma->end)
            find_vma = find_vma->next;
        else if (start >= find_vma->addr && end <= find_vma->end)
        {
            return find_vma;
        }
        else
        {
            panic("vma address overflow\n");
            return NULL;
        }
    }
    struct vma *vma = (struct vma *)pmem_alloc_pages(1);
    if (vma == NULL)
    {
        panic("vma alloc failed\n");
        return NULL;
    }
    if (sz != 0)
    {
        if (alloc)
        {

            if (!uvmalloc1(p->pagetable, start, end, perm))
            {
                panic("uvmalloc1 failed\n");
                return NULL;
            }
        }
        else if (pa != 0)
        {
            if (mappages(p->pagetable, start, end, pa, perm) == -1)
            {
                panic("mappages failed\n");
                return NULL;
            }
        }
    }
    vma->addr = start;
    vma->size = sz;
    vma->perm = perm;
    vma->end = end;
    vma->flags = 0;
    vma->fd = -1;
    vma->f_off = 0;
    vma->type = type;
    vma->prev = find_vma->prev;
    vma->next = find_vma;
    find_vma->prev->next = vma;
    find_vma->prev = vma;
    return vma;
}

struct vma *find_mmap_vma(struct vma *head)
{
    struct vma *vma = head->next;
    while (vma != head)
    {
        // vma 映射类型是： MMAP
        if (MMAP == vma->type)
            return vma;
        vma = vma->next;
    }
    return NULL;
}

uint64 alloc_vma_stack(struct proc *p)
{
    // assert(len == PGSIZE, "user stack size must be PGSIZE");
    uint64 end = USER_STACK_TOP;
    uint64 start = end - USER_STACK_SIZE;
    struct vma *find_vma = p->vma->next;
    // stack 放到链表的最后端
    while (find_vma != p->vma && find_vma->next != p->vma)
    {
        find_vma = find_vma->next;
    }
    struct vma *vma = (struct vma *)pmem_alloc_pages(1);
    if (NULL == vma)
    {
        panic("vma kalloc failed\n");
        return -1;
    }
    if (uvmalloc1(p->pagetable, start, end, PTE_STACK) != 1)
    {
        panic("user stack vma alloc failed\n");
        return -1;
    }
    vma->type = STACK;
    vma->perm = PTE_R | PTE_W;
    vma->addr = start;
    vma->end = end;
    vma->size = USER_STACK_SIZE;
    vma->flags = 0;
    vma->fd = -1;
    vma->f_off = -1;
    vma->prev = find_vma;
    vma->next = find_vma->next;
    find_vma->next->prev = vma;
    find_vma->next = vma;
    return 0;
}

uint64 get_proc_sp(struct proc *p)
{
    struct vma *vma = p->vma->next;
    while (vma != p->vma)
    {
        if (vma->type == STACK)
        {
            break;
        }
        vma = vma->next;
    }
    assert(vma->type == STACK, "proc don't have stack!");
    return vma->end;
}

struct vma *vma_copy(struct proc *np, struct vma *head)
{
    struct vma *new_vma = (struct vma *)pmem_alloc_pages(1);
    if (new_vma == NULL)
    {
        goto bad;
    }
    new_vma->next = new_vma->prev = new_vma;
    new_vma->type = NONE;
    np->vma = new_vma;
    struct vma *pre = head->next;
    struct vma *nvma = NULL;
    while (pre != head)
    {
        nvma = (struct vma *)pmem_alloc_pages(1);
        if (nvma == NULL)
            goto bad;
        memmove(nvma, pre, sizeof(struct vma));
        if (nvma->type == MMAP && nvma->fd != -1) {
            struct file *f = np->ofile[nvma->fd];
            if (f) {
                get_file_ops()->dup(f);
            }
        }
        nvma->next = nvma->prev = NULL;
        nvma->prev = new_vma->prev;
        nvma->next = new_vma;
        new_vma->prev->next = nvma;
        new_vma->prev = nvma;
        pre = pre->next;
    }
    return new_vma;
bad:
    np->vma = NULL;
    pmem_free_pages(new_vma, 1);
    panic("vma alloc failed");
    return NULL;
}

static int vma_is_shared_mapping(struct vma *vma)
{
    return vma->type == MMAP && (vma->flags & MAP_SHARED);
}

int vma_map(pgtbl_t old, pgtbl_t new, struct vma *vma)
{
    uint64 start = vma->addr;
    pte_t *pte, *new_pte;
    uint64 pa;
    char *mem;
    long flags;
    while (start < vma->end)
    {
        pte = walk(old, start, 0);
        if (vma_is_shared_mapping(vma) && vma->fd == -1 && (pte == NULL || (*pte & PTE_V) == 0))
        {
            mem = (char *)pmem_alloc_pages(1);
            if (mem == NULL)
                goto bad;
            memset(mem, 0, PGSIZE);
            if (mappages(old, start, (uint64)mem, PGSIZE, vma->perm | PTE_U | PTE_D) != 1)
            {
                pmem_free_pages(mem, 1);
                goto bad;
            }
            pte = walk(old, start, 0);
        }
        if (pte == NULL)
        {
            // LazyLoad VMA: 页面可能还没有分配，跳过
            start += PGSIZE;
            continue;
        }
        if ((*pte & PTE_V) == 0)
        {
            // LazyLoad VMA: 页面无效，跳过
            start += PGSIZE;
            continue;
        }
        
        // 检查目标页表项是否已经存在（避免重复映射）
        new_pte = walk(new, start, 0);
        if (new_pte != NULL && (*new_pte & PTE_V))
        {
            // 页面已经映射过了，跳过（可能是uvmcopy已经复制过）
            start += PGSIZE;
            continue;
        }
        
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        if (vma_is_shared_mapping(vma))
        {
            pmem_inc_ref((void *)(pa | dmwin_win0));
            if (mappages(new, start, pa | dmwin_win0, PGSIZE, flags) != 1)
            {
                pmem_free_pages((void *)(pa | dmwin_win0), 1);
                goto bad;
            }
            start += PGSIZE;
            continue;
        }
        pa |= dmwin_win0;
        mem = (char *)pmem_alloc_pages(1);
        if (mem == NULL)
            goto bad;
        memmove(mem, (char *)pa, PGSIZE);
        if (mappages(new, start, (uint64)mem, PGSIZE, flags) != 1)
        {
            pmem_free_pages(mem, 1);
            goto bad;
        }
        start += PGSIZE;
    }
    pa = walkaddr(new, vma->addr);
    return 0;
bad:
    vmunmap(new, vma->addr, (start - vma->addr) / PGSIZE, 1);
    return -1;
}

int free_vma_list(struct proc *p)
{
    struct vma *vma_head = p->vma;
    if (vma_head == NULL)
    {
        return 1;
    }
    struct vma *vma = vma_head->next;
    while (vma && vma != vma_head)
    {
        struct vma *next = vma->next;
        uint64 a;
        pte_t *pte;
        for (a = vma->addr; a < vma->end; a += PGSIZE)
        {
            if ((pte = walk(p->pagetable, a, 0)) == NULL)
                continue;
            if ((*pte & PTE_V) == 0)
                continue;
            if (PTE_FLAGS(*pte) == PTE_V)
                continue;
            uint64 pa = PTE2PA(*pte) | dmwin_win0;
            pmem_free_pages((void *)pa, 1);
            *pte = 0;
        }
        pmem_free_pages(vma, 1);
        vma = next;
    }
    if (vma == vma_head)
        pmem_free_pages(vma_head, 1);
    else
        if (FINAL_DEV_DIAG)
            printf("free_vma_list: broken VMA list for pid=%d head=%p next=%p\n",
                   p->pid, vma_head, vma);
    p->vma = NULL;
    return 1;
}

int free_vma(struct proc *p, uint64 start, uint64 end)
{
    struct vma *vma_head = p->vma;
    if (!vma_head || !vma_head->next)
        return -1;

    struct vma *vma = vma_head->next;
    while (vma != vma_head)
    {
        struct vma *next_vma = vma->next;

        // 检查是否有重叠
        if (vma->end > start && vma->addr < end)
        {
            // 情况1：完全在释放范围内
            if (vma->addr >= start && vma->end <= end)
            {
                // 从链表移除
                vma->prev->next = vma->next;
                vma->next->prev = vma->prev;
                pmem_free_pages(vma, 1);
            }
            // 情况2：部分重叠（需要拆分）
            else
            {
                // 左侧非重叠部分
                if (vma->addr < start)
                {
                    // 创建新的左侧 VMA
                    struct vma *left = (struct vma *)pmem_alloc_pages(1);
                    if (!left)
                        panic("free_vma: pmem_alloc_pages failed");

                    memcpy(left, vma, sizeof(struct vma));
                    left->size = start - vma->addr;
                    left->end = start;

                    // 插入链表
                    left->prev = vma->prev;
                    left->next = vma;
                    vma->prev->next = left;
                    vma->prev = left;
                }

                // 右侧非重叠部分
                if (vma->end > end)
                {
                    // 创建新的右侧 VMA
                    struct vma *right = (struct vma *)pmem_alloc_pages(1);
                    if (!right)
                        panic("free_vma: pmem_alloc_pages failed");

                    memcpy(right, vma, sizeof(struct vma));
                    right->addr = end;
                    right->size = vma->end - end;
                    right->f_off = vma->f_off + (end - vma->addr); // 调整文件偏移

                    // 插入链表
                    right->prev = vma;
                    right->next = vma->next;
                    vma->next->prev = right;
                    vma->next = right;
                }

                // 移除当前 VMA（重叠部分）
                vma->prev->next = vma->next;
                vma->next->prev = vma->prev;
                pmem_free_pages(vma, 1);
            }
        }
        vma = next_vma;
    }
    return 1;
}
