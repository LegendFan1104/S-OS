# qemu -kernel loads the kernel at 0x80000000
        # and causes each CPU to jump there.
        # ld_script causes the following code to
        # be placed at 0x80000000.
.global start
.global sos_start_kernel
.section .text

.global _entry
_entry:
	# set up a stack for C.
        # with a 4096-byte stack per CPU.
        la sp, entry_stack
#中文注释
# OpenSBI 启动协议下 a0 已经是 hartid；非 SBI 直接读 mhartid。
# 为每个 hart 预留独立启动栈，避免多核早期共享同一栈。
#if defined SBI
        mv t0, a0
#else
        csrr a0, mhartid
        mv t0, a0
#endif
        li t1, 1024*4
        addi t0, t0, 1
        mul t0, t0, t1
        add sp, sp, t0
	# jump to sos_start_kernel() in kernel/sos_start_kernel.c
        call start
spin:
        j spin
