.section .text
.global _start
_start:
    # DMW 与 U-Boot 完全一致：DMW0=0x8000 非缓存，DMW1=0x9000 缓存
    li.d    $t0, 0x800000000000000f
    csrwr   $t0, 0x180
    li.d    $t0, 0x900000000000001f
    csrwr   $t0, 0x181
    # CRMD: PLV0, IE=0, PG=0（与内核一致）
    li.w    $t0, 0xb0
    csrwr   $t0, 0x0
    # 栈
    la      $sp, _stack_top
    bl      main
spin:
    b       spin

.section .bss
.align 12
_stack_bottom:
.skip 8192
_stack_top:
