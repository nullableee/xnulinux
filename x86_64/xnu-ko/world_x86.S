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

	/* XNU's call site: the return address pushed by the original call.
	 * The prologue pushed 56 bytes, so it sits at 56(%rsp). */
	movq	56(%rsp), %rax
	movq	%rax, xnu_upcall_ret(%rip)

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
	.size	xnu_world_hook_trampoline_x86, . - xnu_world_hook_trampoline_x86

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

/*
 * Diagnostic site probes. A patched XNU site jumps here with the original
 * caller's return address still on the stack. We save flags and every
 * caller-saved register, make one SITE_TAG upcall through the normal hook
 * gate (so it is fsynced with the rest of the trace), restore, and then run
 * the original instructions. %rbx is preserved by the upcall itself, so it
 * can hold the unaligned stack pointer across the alignment adjustment.
 * %r11 is deliberately not restored: write_abs_jmp() already clobbers it to
 * enter every probe, and site1 reuses it for the final absolute jump.
 */
	.macro SITE_PROBE id
	pushfq
	pushq	%rbx
	movq	%rsp, %rbx
	andq	$-16, %rsp
	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10
	movl	$1001, %edi		/* LINUX64_CALL_SITE_TAG */
	movl	$\id, %esi		/* diagnostic site id */
	xorl	%edx, %edx
	call	xnu_world_hook_trampoline_x86
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rax
	movq	%rbx, %rsp
	popq	%rbx
	popfq
	.endm

/* vstart 0x9c4, right after doublemap_init() returns. The original leaq/nop
 * is replaced by the 13-byte absolute jump, which also consumes the first byte
 * of the loop's movq at 0x9d0. Replay that movq here and resume at 0x9d4. */
	.globl xnu_world_site1_probe_x86
	.type  xnu_world_site1_probe_x86, @function
xnu_world_site1_probe_x86:
	endbr64
	SITE_PROBE 1
	movabsq	$0xffffff800010c000, %r9	/* _master_idt64 */
	movq	(%r9,%rdi), %rsi		/* replay _vstart+0x9d0 */
	movabsq	$0xffffff80004ef7d4, %r11	/* resume _vstart+0x9d4 */
	jmp	*%r11
	.size	xnu_world_site1_probe_x86, . - xnu_world_site1_probe_x86

/* ml_static_ptovirt() entry: replay its original body, including the return
 * to the real caller that is still on the stack. */
	.globl xnu_world_ptovirt_probe_x86
	.type  xnu_world_ptovirt_probe_x86, @function
xnu_world_ptovirt_probe_x86:
	endbr64
	SITE_PROBE 2
	pushq	%rbp
	movq	%rsp, %rbp
	movabsq	$0xffffff8000000000, %rax
	orq	%rdi, %rax
	popq	%rbp
	retq
	.size	xnu_world_ptovirt_probe_x86, . - xnu_world_ptovirt_probe_x86

/* vstart+0xf35, right after the host-map loop has finished. The 13 patched
 * bytes are movq %rbx,physmap_max(%rip) and movl IdlePML4(%rip),%eax; replay
 * them with absolute addresses because module text is too far for rel32.
 * The following instruction is the CR3 switch we are about to make. */
	.globl xnu_world_site3_probe_x86
	.type  xnu_world_site3_probe_x86, @function
xnu_world_site3_probe_x86:
	endbr64
	SITE_PROBE 3
	movabsq	$0xffffff8000e8e118, %r11
	movq	%rbx, (%r11)
	movabsq	$0xffffff800106f360, %r11
	movl	(%r11), %eax
	/* Carry IdlePML4 into the saved XNU state here: site4 is deliberately
	 * not installed in this experiment. W finds this CPU's world but
	 * clobbers the CPUID registers, so save every live caller-saved value
	 * first. %r11 is already sacrificial after SITE_PROBE. */
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%r8
	pushq	%rax
	W	%r8
	movq	%r8, %r11		/* world pointer, safe in sacrificed %r11 */
	popq	%rax			/* IdlePML4 physical address */
	popq	%r8
	popq	%rdx
	popq	%rcx
	popq	%rbx
	movq	%rax, XW_XNU_STATE(%r11)
	/* Phase breadcrumbs are plain writes to module data mapped by both
	 * page tables, so they need no post-switch upcall. 70 means the CR3
	 * write was about to execute; 71 means it completed. */
	movq	%rax, fki_x86_phase_arg(%rip)
	movq	$70, fki_x86_phase(%rip)
	movq	%rax, %cr3
	movq	%rax, fki_x86_phase_arg(%rip)
	movq	$71, fki_x86_phase(%rip)

	/* Replay vstart+0xf45..0xf6f ourselves. 72 proves the hook pointer
	 * was read; 73 proves the debug/arg setup finished and the indirect
	 * call at 0xf71 is next. The je condition is preserved by doing the
	 * test here and branching before the phase writes (mov to memory does
	 * not touch flags). */
	movabsq	$0xffffff800106fa70, %r14
	movq	(%r14), %rax
	testq	%rax, %rax
	movq	%rax, fki_x86_phase_arg(%rip)
	movq	$72, fki_x86_phase(%rip)
	jz	.Lsite3_no_hook
	movabsq	$0xffffff800106fe80, %rbx
	movq	$0x3e6, 0x30(%rbx)
	movabsq	$0xffffff8000cb5fb0, %rsi
	movl	$0x3e6, %edi
	xorl	%edx, %edx
	movq	%rax, fki_x86_phase_arg(%rip)
	movq	$73, fki_x86_phase(%rip)
	movabsq	$0xffffff80004efd71, %r11
	jmp	*%r11
.Lsite3_no_hook:
	movabsq	$0xffffff80004efd92, %r11
	jmp	*%r11
	.size	xnu_world_site3_probe_x86, . - xnu_world_site3_probe_x86

/* vstart+0xf45, immediately after set_cr3_raw(IdlePML4). Record the new CR3
 * in this CPU's saved world state, replay leaq/mov/test, then resume at the
 * je so the following KPRINTF/KERNEL_TABLES upcalls still see the real
 * _linux64_hook value in %r14. */
	.globl xnu_world_site4_probe_x86
	.type  xnu_world_site4_probe_x86, @function
xnu_world_site4_probe_x86:
	endbr64
	/* %rax still holds the IdlePML4 physical address just loaded into CR3.
	 * %r11 is deliberately not restored by SITE_PROBE, so it carries the
	 * value across the upcall without clobbering a callee-saved register. */
	movq	%rax, %r11
	SITE_PROBE 4
	/* The trampoline switched to Linux CR3 and fki_x86_leave() restored the
	 * stale boot CR3 in this world's saved state. Replace it with IdlePML4
	 * now so every later upcall returns to the switched address space. */
	W	%rdi
	movq	%r11, XW_XNU_STATE(%rdi)
	movabsq	$0xffffff800106fa70, %r14
	movq	(%r14), %rax
	testq	%rax, %rax
	movabsq	$0xffffff80004efd52, %r11
	jmp	*%r11
	.size	xnu_world_site4_probe_x86, . - xnu_world_site4_probe_x86

	.section .note.GNU-stack,"",@progbits
