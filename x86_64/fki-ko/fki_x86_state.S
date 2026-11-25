/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level x86_64 state switch for fki_x86_enter().
 *
 * Runs in fki_x86 module text (which is mapped in both Linux and XNU page
 * tables) with no __fentry__, no pv_ops, no stack protector, and no
 * __x86_return_thunk, so it is safe after CR3 switches to XNU.
 */
	.text

	.equ FS_CR3,            0
	.equ FS_IDT_BASE,       8
	.equ FS_GDT_BASE,       16
	.equ FS_IDT_LIMIT,      24
	.equ FS_GDT_LIMIT,      32
	.equ FS_TR,             40
	.equ FS_GS_BASE,        48
	.equ FS_KERNEL_GS_BASE, 56
	.equ FS_STAR,           64
	.equ FS_LSTAR,          72
	.equ FS_CSTAR,          80
	.equ FS_SFMASK,         88

	.macro WRMSR_FIELD msr, off
	movl	$\msr, %ecx
	movl	\off(%rdi), %eax
	movl	(\off + 4)(%rdi), %edx
	wrmsr
	.endm

/*
 * void fki_x86_load_state_asm(const struct fki_x86_state *s);
 */
	.globl fki_x86_load_state_asm
	.type  fki_x86_load_state_asm, @function
fki_x86_load_state_asm:
	endbr64
	subq	$32, %rsp

	movw	FS_IDT_LIMIT(%rdi), %ax
	movw	%ax, 0(%rsp)
	movq	FS_IDT_BASE(%rdi), %rax
	movq	%rax, 2(%rsp)

	movw	FS_GDT_LIMIT(%rdi), %ax
	movw	%ax, 16(%rsp)
	movq	FS_GDT_BASE(%rdi), %rax
	movq	%rax, 18(%rsp)

	movq	FS_CR3(%rdi), %rax
	movq	$5, fki_x86_phase(%rip)
	movq	%rax, %cr3
	movq	$6, fki_x86_phase(%rip)
	movq	%rax, fki_x86_phase_arg(%rip)

	lgdt	16(%rsp)
	lidt	0(%rsp)

	movq	FS_TR(%rdi), %rax
	testq	%rax, %rax
	jz	1f
	str	%cx
	cmpw	%cx, %ax
	je	1f
	ltr	%ax
1:
	WRMSR_FIELD 0xc0000102, FS_KERNEL_GS_BASE
	WRMSR_FIELD 0xc0000101, FS_GS_BASE
	WRMSR_FIELD 0xc0000081, FS_STAR
	WRMSR_FIELD 0xc0000082, FS_LSTAR
	WRMSR_FIELD 0xc0000083, FS_CSTAR
	WRMSR_FIELD 0xc0000084, FS_SFMASK

	movq	$7, fki_x86_phase(%rip)
	addq	$32, %rsp
	ret
	.size	fki_x86_load_state_asm, . - fki_x86_load_state_asm

/*
 * unsigned long fki_x86_enter(const struct fki_x86_state *foreign,
 *                             struct fki_x86_state *saved);
 */
	.globl fki_x86_enter
	.type  fki_x86_enter, @function
fki_x86_enter:
	endbr64
	pushq	%rdi
	call	fki_x86_enter_prep
	popq	%rdi
	testq	%rdi, %rdi
	jz	1f
	pushq	%rax
	call	fki_x86_load_state_asm
	popq	%rax
1:
	ret
	.size	fki_x86_enter, . - fki_x86_enter

	.section .note.GNU-stack,"",@progbits
