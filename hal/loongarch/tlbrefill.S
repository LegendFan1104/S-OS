// LoongArch TLB refill entry.
// The sequence follows the known-good 2K1000LA reference implementation.

#include "la_asm.h"

.section .bss
.align 3
.globl tlbr_diag_stage
tlbr_diag_stage: .dword 0
.globl tlbr_diag_pgd
tlbr_diag_pgd: .dword 0
.globl tlbr_diag_badv
tlbr_diag_badv: .dword 0
.globl tlbr_diag_era
tlbr_diag_era: .dword 0
.globl tlbr_diag_val
tlbr_diag_val: .dword 0
.globl tlbr_diag_stack
tlbr_diag_stack: .space 4096

.section .text.tlbrentry
.globl handle_tlbr
.align 0x4
handle_tlbr:
	csrwr	$t0, LOONGARCH_CSR_TLBRSAVE
	csrrd	$t0, 0x5
	csrrd	$t0, LOONGARCH_CSR_PGD
	lddir	$t0, $t0, 3
	srli.d	$t0, $t0, 12
	slli.d	$t0, $t0, 12
	lddir	$t0, $t0, 2
	srli.d	$t0, $t0, 12
	slli.d	$t0, $t0, 12
	lddir	$t0, $t0, 1
	srli.d	$t0, $t0, 12
	slli.d	$t0, $t0, 12
	ldpte	$t0, 0
	ldpte	$t0, 1
	csrrd	$t0, 0x8c
	csrrd	$t0, 0x8d
	tlbfill
	csrrd	$t0, LOONGARCH_CSR_TLBRSAVE
	ertn
