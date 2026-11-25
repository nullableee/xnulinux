/* SPDX-License-Identifier: APSL-2.0 */
/*
 * x86_64 LINUX64 world switch.
 *
 * Linux and hosted XNU are both ring-0 kernels; CR3, IDT, GDT, TR, GS/KernelGS
 * and the syscall MSRs can only belong to one of them. fki_x86_enter/leave()
 * own that state; this file owns the general-purpose registers and the two
 * stacks. Interrupts are off across a downcall. An upcall turns them back on
 * while Linux runs the host hook, then turns them off again before returning.
 *
 * No register is reserved across XNU execution: XNU's trap/return assembly uses
 * %r15 as the saved-state pointer. Each upcall rediscovers its world from CPUID
 * leaf 1 (initial APIC ID -> xnu_worlds_x86), exactly as the ARM64 port
 * rediscovers its world from MPIDR_EL1.
 */
	.text

	.equ XW_LINUX_RBX,    0
	.equ XW_LINUX_RBP,    8
	.equ XW_LINUX_R12,   16
	.equ XW_LINUX_R13,   24
	.equ XW_LINUX_R14,   32
	.equ XW_LINUX_R15,   40
	.equ XW_LINUX_RSP,   48
	.equ XW_XNU_RBX,     56
	.equ XW_XNU_RBP,     64
	.equ XW_XNU_R12,     72
	.equ XW_XNU_R13,     80
	.equ XW_XNU_R14,     88
	.equ XW_XNU_R15,     96
	.equ XW_XNU_RSP,    104
	.equ XW_LINUX_STATE,112
	.equ XW_XNU_STATE,  224
	.equ XW_XNU_STACK_TOP, 336
	.equ XW_ENTRY_RIP,  344
	.equ XW_ENTRY_ARG,  352
	.equ XW_HOOK_COOKIE,360
	.equ XW_FAULT_RIP,  368
	.equ XW_RUNNING,    376
	.equ XW_HOOK_CALL,  384
	.equ XW_HOOK_A1,    392
	.equ XW_HOOK_A2,    400
	.equ XW_RUN_RESULT, 408

/*
 * Find this CPU's world. CPUID leaf 1 returns the initial APIC ID in EBX[31:24];
 * LINUX64_X86_CPU_SLOT() masks it to 8 bits. Clobbers %rax/%rbx/%rcx/%rdx.
 */
	.macro W reg
	movl	$1, %eax
	xorl	%ecx, %ecx
	cpuid
	shrl	$24, %ebx
	movzbl	%bl, %ebx
	leaq	xnu_worlds_x86(%rip), \reg
	movq	(\reg,%rbx,8), \reg
	.endm

/*
 * long xnu_world_call_x86(struct xnu_world_x86 *w, unsigned long fn,
 *                         unsigned long arg);
 */
	.globl xnu_world_call_x86
	.type  xnu_world_call_x86, @function
xnu_world_call_x86:
	endbr64
	pushfq
	/* fki_x86_enter() masks IRQs and saves the caller's real IF state. */
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%rdi, %r15		/* r15 = w for this downcall/upcall pair */
	movq	%rsp, XW_LINUX_RSP(%r15)
	movq	%rbx, XW_LINUX_RBX(%r15)
	movq	%rbp, XW_LINUX_RBP(%r15)
	movq	%r12, XW_LINUX_R12(%r15)
	movq	%r13, XW_LINUX_R13(%r15)
	movq	%r14, XW_LINUX_R14(%r15)
	movq	(%rsp), %rax		/* original r15 was pushed last */
	movq	%rax, XW_LINUX_R15(%r15)

	movq	%rsi, XW_ENTRY_RIP(%r15)
	movq	%rdx, XW_ENTRY_ARG(%r15)
	movq	$0,   XW_RUNNING(%r15)

	/* Save Linux's system state and load XNU's. */
	leaq	XW_XNU_STATE(%r15), %rdi
	leaq	XW_LINUX_STATE(%r15), %rsi
	call	fki_x86_enter
	movq	%rax, XW_HOOK_COOKIE(%r15)
	movq	$10, fki_x86_phase(%rip)
	movq	%rax, fki_x86_phase_arg(%rip)

	movq	XW_XNU_RSP(%r15), %rsp
	movq	XW_ENTRY_ARG(%r15), %rdi
	movq	XW_ENTRY_RIP(%r15), %rax
	movq	%rax, fki_x86_phase_arg(%rip)
	movq	$11, fki_x86_phase(%rip)
	call	*%rax
	movq	$12, fki_x86_phase(%rip)

	xorl	%eax, %eax
	jmp	.Lxnu_world_exit

/* Reached from the upcall when the host ended the run (still in XNU world). */
	.globl xnu_world_abort_x86
	.type  xnu_world_abort_x86, @function
xnu_world_abort_x86:
	movq	$1, XW_RUNNING(%r15)
	movl	$1, %eax
.Lxnu_world_exit:
	movq	%rax, XW_RUN_RESULT(%r15)
	movq	XW_LINUX_RSP(%r15), %rsp
	movq	XW_HOOK_COOKIE(%r15), %rdi
	leaq	XW_LINUX_STATE(%r15), %rsi
	leaq	XW_XNU_STATE(%r15), %rdx
	call	fki_x86_leave

	movq	XW_RUNNING(%r15), %rax
	movq	XW_LINUX_RBX(%r15), %rbx
	movq	XW_LINUX_RBP(%r15), %rbp
	movq	XW_LINUX_R12(%r15), %r12
	movq	XW_LINUX_R13(%r15), %r13
	movq	XW_LINUX_R14(%r15), %r14
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	popfq
	ret
	.size	xnu_world_call_x86, . - xnu_world_call_x86

/*
 * void xnu_world_upcall_x86(unsigned long call, unsigned long a1,
 *                           unsigned long a2);
 * Called by XNU as linux64_hook. The world is found from the APIC ID, so XNU
 * may freely use %r15 for its own saved-state pointer.
 */
	.globl xnu_world_upcall_x86
	.type  xnu_world_upcall_x86, @function
xnu_world_upcall_x86:
	movq	%rdi, fki_x86_phase_arg(%rip)
	movq	$21, fki_x86_phase(%rip)
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15		/* XNU uses r15 as a saved-state pointer. */
	subq	$8, %rsp		/* Keep the SysV stack 16-byte aligned at calls. */
	/* Keep the three hook arguments across CPUID (which clobbers %rdx). */
	movq	%rdi, %r10
	movq	%rsi, %r11
	movq	%rdx, %r12
	W	%r15
	movq	%r15, fki_x86_phase_arg(%rip)
	movq	$22, fki_x86_phase(%rip)

	movq	%r10, XW_HOOK_CALL(%r15)
	movq	%r11, XW_HOOK_A1(%r15)
	movq	%r12, XW_HOOK_A2(%r15)

	/* Linux world: interrupts and preemption may be on during the hook. */
	movq	XW_HOOK_COOKIE(%r15), %rdi
	leaq	XW_LINUX_STATE(%r15), %rsi
	leaq	XW_XNU_STATE(%r15), %rdx
	call	fki_x86_leave
	movq	$23, fki_x86_phase(%rip)

	/* Run the host hook on Linux's stack, not on an XNU kernel stack. */
	movq	%rsp, XW_XNU_RSP(%r15)
	movq	XW_LINUX_RSP(%r15), %rsp
	movq	XW_HOOK_CALL(%r15), %rdi
	movq	XW_HOOK_A1(%r15), %rsi
	movq	XW_HOOK_A2(%r15), %rdx
	call	xnu_hook_x86
	testq	%rax, %rax
	jne	.Lxnu_upcall_abort

	/* XNU world again, back on the XNU frame we saved above. */
	movq	XW_XNU_RSP(%r15), %rsp
	leaq	XW_XNU_STATE(%r15), %rdi
	leaq	XW_LINUX_STATE(%r15), %rsi
	call	fki_x86_enter
	movq	%rax, XW_HOOK_COOKIE(%r15)
	movq	$24, fki_x86_phase(%rip)

	addq	$8, %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	ret

/* Already in the Linux world: abandon XNU's frames and return from the
 * original xnu_world_call_x86() invocation. */
.Lxnu_upcall_abort:
	movq	$25, fki_x86_phase(%rip)
	movq	$1, XW_RUNNING(%r15)
	movq	$1, XW_RUN_RESULT(%r15)
	movq	XW_LINUX_RSP(%r15), %rsp
	movq	$1, %rax
	movq	XW_LINUX_RBX(%r15), %rbx
	movq	XW_LINUX_RBP(%r15), %rbp
	movq	XW_LINUX_R12(%r15), %r12
	movq	XW_LINUX_R13(%r15), %r13
	movq	XW_LINUX_R14(%r15), %r14
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	popfq
	ret
	.size	xnu_world_upcall_x86, . - xnu_world_upcall_x86

/*
 * Address XNU sees as linux64_hook. It must live in module text: the host
 * loader maps these pages into the XNU page tables for the downcall, and
 * Linux already maps them executable for the instructions after the CR3
 * switch. Only the host CR3 variable is read before the switch.
 */
	.globl xnu_world_hook_trampoline_x86
	.type  xnu_world_hook_trampoline_x86, @function
xnu_world_hook_trampoline_x86:
	endbr64
	movq	xnu_host_cr3_x86(%rip), %rax
	movq	%rax, fki_x86_phase_arg(%rip)
	movq	$20, fki_x86_phase(%rip)
	movq	%rax, %cr3
	jmp	xnu_world_upcall_x86

/*
 * Diagnostic replacement for _kprintf's first bytes. XNU calls kprintf with
 * rdi = format string (varargs in the remaining argument registers). Turn it
 * into a normal LINUX64_CALL_KPRINTF(998, format, 0) upcall so the host can
 * fsync the format string before returning to the caller. The stack still
 * holds the original return address, so the upcall's `ret` resumes XNU.
 */
	.globl xnu_world_kprintf_probe_x86
	.type  xnu_world_kprintf_probe_x86, @function
xnu_world_kprintf_probe_x86:
	endbr64
	movq	%rdi, %rsi		/* format string -> a1 */
	movl	$998, %edi		/* LINUX64_CALL_KPRINTF */
	xorl	%edx, %edx		/* a2 */
	jmp	xnu_world_hook_trampoline_x86
	.size	xnu_world_kprintf_probe_x86, . - xnu_world_kprintf_probe_x86
	.size	xnu_world_hook_trampoline_x86, . - xnu_world_hook_trampoline_x86

	.section .note.GNU-stack,"",@progbits
