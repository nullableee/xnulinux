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

/* Same as SITE_PROBE, but log the caller's %rdx as a2 instead of clearing it.
 * The value is pushed/popped like every other caller-saved register. */
	.macro SITE_PROBE_RDX id
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
	movl	$\id, %esi
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
	/* Snapshot the hook under the booter page tables into a dedicated
	 * global. The phase word below changes too fast for the logger to
	 * sample, so this value is printed alongside phase 72. */
	pushq	%r9
	movabsq	$0xffffff800106fa70, %r9
	movq	(%r9), %r9
	movq	%r9, xnu_site3_hook_pre(%rip)
	popq	%r9
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

/* vstart breadcrumbs after KERNEL_TABLES. Each patch covers exactly 13 bytes;
 * the replayed instructions are the original ones, so the only added effect
 * is one fsynced SITE_TAG upcall with the id in a1. */
	.globl xnu_world_site5_probe_x86
	.type  xnu_world_site5_probe_x86, @function
xnu_world_site5_probe_x86:
	endbr64
	SITE_PROBE 5
	movabsq	$0xffffff800106f378, %r11
	movl	(%r11), %eax
	movabsq	$0xffffff800106f428, %r11
	movq	%r11, %rcx
	movabsq	$0xffffff80004efda4, %r11
	jmp	*%r11
	.size	xnu_world_site5_probe_x86, . - xnu_world_site5_probe_x86

	.globl xnu_world_site6_probe_x86
	.type  xnu_world_site6_probe_x86, @function
xnu_world_site6_probe_x86:
	endbr64
	SITE_PROBE 6
	SITE_PROBE 63		/* upcall returned, replay starts */
	movq	%rax, (%rcx)
	movl	$1, %edi
	movabsq	$0xffffff80004e3c60, %r11	/* cpu_data_alloc */
	call	*%r11
	SITE_PROBE 64		/* cpu_data_alloc returned */
	movabsq	$0xffffff80004efdb1, %r11
	jmp	*%r11
	.size	xnu_world_site6_probe_x86, . - xnu_world_site6_probe_x86

	.globl xnu_world_site7_probe_x86
	.type  xnu_world_site7_probe_x86, @function
xnu_world_site7_probe_x86:
	endbr64
	SITE_PROBE 7
	movq	(%rbx), %rdi
	movabsq	$0xffffff80004e3080, %r11	/* cpu_desc_init */
	call	*%r11
	movq	(%rbx), %rdi
	movabsq	$0xffffff80004e3af0, %r11	/* cpu_desc_load */
	call	*%r11
	movabsq	$0xffffff80004efdc8, %r11
	jmp	*%r11
	.size	xnu_world_site7_probe_x86, . - xnu_world_site7_probe_x86

	.globl xnu_world_site8_probe_x86
	.type  xnu_world_site8_probe_x86, @function
xnu_world_site8_probe_x86:
	endbr64
	SITE_PROBE 8
	movabsq	$0xffffff80004e3af0, %r11	/* cpu_desc_load */
	call	*%r11
	movq	(%rbx), %rdi
	movabsq	$0xffffff80004e3b70, %r11	/* cpu_syscall_init */
	call	*%r11
	movabsq	$0xffffff80004efdd0, %r11
	jmp	*%r11
	.size	xnu_world_site8_probe_x86, . - xnu_world_site8_probe_x86

	.globl xnu_world_site9_probe_x86
	.type  xnu_world_site9_probe_x86, @function
xnu_world_site9_probe_x86:
	endbr64
	SITE_PROBE 9
	movq	(%rax,%rbx,8), %rax
	movq	0x40(%rax), %rsi
	movabsq	$0xffffff80004f8db0, %r11	/* x86_init_wrapper */
	call	*%r11
	movabsq	$0xffffff80004eeeb6, %r11
	jmp	*%r11
	.size	xnu_world_site9_probe_x86, . - xnu_world_site9_probe_x86

/* cpuid_info() slow path: 12 before PE_parse_boot_argn(), 13 between it and
 * cpuid_set_info(). The 13 patched bytes are mov $4,%edx plus both calls. */
	.globl xnu_world_cpuid_probe_x86
	.type  xnu_world_cpuid_probe_x86, @function
xnu_world_cpuid_probe_x86:
	endbr64
	SITE_PROBE 12
	movl	$4, %edx
	movabsq	$0xffffff8000b4cf10, %r11	/* PE_parse_boot_argn */
	call	*%r11
	SITE_PROBE 13
	movabsq	$0xffffff80004e5750, %r11	/* cpuid_set_info */
	call	*%r11
	.globl xnu_world_cpuid_probe_after_info_x86
xnu_world_cpuid_probe_after_info_x86:
	SITE_PROBE 58
	/* Replay cpuid_info()'s tail here and continue straight to the cpc2
	 * probe, bypassing the original pop rbp / ret path. */
	movabsq	$0xffffff80010eb100, %rax
	movabsq	$0xffffff80010eb368, %r11
	movq	%rax, (%r11)
	popq	%rbp
	addq	$8, %rsp		/* discard the cpc1 stub return address */
	movabsq	$0xffffff8000512fc3, %r11
	jmp	*%r11
	.size	xnu_world_cpuid_probe_x86, . - xnu_world_cpuid_probe_x86

/* cpuid_set_info() breadcrumbs. Every patched window is exactly 13 bytes and
 * ends on an instruction boundary, so each stub replays the original bytes
 * before resuming. %r11 is sacrificial after SITE_PROBE. */
	.macro JMP_XNU addr
	movabsq	$\addr, %r11
	jmp	*%r11
	.endm

	.globl xnu_world_cpuid_p14_x86
	.type  xnu_world_cpuid_p14_x86, @function
xnu_world_cpuid_p14_x86:
	endbr64
	SITE_PROBE 14
	movl	$3, %edi
	movabsq	$0xffffff80004e8780, %r11
	call	*%r11
	cmpl	$2, %eax
	JMP_XNU 0xffffff80004e5789
	.size	xnu_world_cpuid_p14_x86, . - xnu_world_cpuid_p14_x86

	.globl xnu_world_cpuid_p16_x86
	.type  xnu_world_cpuid_p16_x86, @function
xnu_world_cpuid_p16_x86:
	endbr64
	SITE_PROBE 16
	movabsq	$0xffffff80010eb158, %rax
	movq	%rax, %rdx
	notq	%rdx
	JMP_XNU 0xffffff80004e6a68
	.size	xnu_world_cpuid_p16_x86, . - xnu_world_cpuid_p16_x86

	.globl xnu_world_cpuid_p17_x86
	.type  xnu_world_cpuid_p17_x86, @function
xnu_world_cpuid_p17_x86:
	endbr64
	SITE_PROBE 17
	notl	%edx
	movl	$8, %ecx
	testl	$0x128, %edx
	JMP_XNU 0xffffff80004e6a9b
	.size	xnu_world_cpuid_p17_x86, . - xnu_world_cpuid_p17_x86

	.globl xnu_world_cpuid_p18_x86
	.type  xnu_world_cpuid_p18_x86, @function
xnu_world_cpuid_p18_x86:
	endbr64
	SITE_PROBE 18
	movabsq	$0xffffff80010eb360, %r11
	cmpl	$0, (%r11)
	jne	.Lp18_taken
	JMP_XNU 0xffffff80004e6bdc
.Lp18_taken:
	JMP_XNU 0xffffff80004e6b5a
	.size	xnu_world_cpuid_p18_x86, . - xnu_world_cpuid_p18_x86

	/* MSR_CORE_THREAD_COUNT (0x35) is not implemented on modern hosts.
	 * Use XNU's own zero-MSR fallback: 1 core / 1 thread. */
	.globl xnu_world_cpuid_p19_x86
	.type  xnu_world_cpuid_p19_x86, @function
xnu_world_cpuid_p19_x86:
	endbr64
	SITE_PROBE 19
	movl	$0x10001, %ecx
	movzwl	%cx, %eax
	SITE_PROBE 46
	JMP_XNU 0xffffff80004e6b1d
	.size	xnu_world_cpuid_p19_x86, . - xnu_world_cpuid_p19_x86

	.globl xnu_world_cpuid_p20_x86
	.type  xnu_world_cpuid_p20_x86, @function
xnu_world_cpuid_p20_x86:
	endbr64
	SITE_PROBE 20
	movl	$0x10001, %ecx
	movzwl	%cx, %eax
	JMP_XNU 0xffffff80004e6b39
	.size	xnu_world_cpuid_p20_x86, . - xnu_world_cpuid_p20_x86

	.globl xnu_world_cpuid_p21_x86
	.type  xnu_world_cpuid_p21_x86, @function
xnu_world_cpuid_p21_x86:
	endbr64
	SITE_PROBE 21
	movabsq	$0xffffff80010eb360, %r11
	cmpl	$0, (%r11)
	jne	.Lp21_taken
	JMP_XNU 0xffffff80004e6bdc
.Lp21_taken:
	JMP_XNU 0xffffff80004e6b5a
	.size	xnu_world_cpuid_p21_x86, . - xnu_world_cpuid_p21_x86

	.globl xnu_world_cpuid_p22_x86
	.type  xnu_world_cpuid_p22_x86, @function
xnu_world_cpuid_p22_x86:
	endbr64
	SITE_PROBE 22
	movabsq	$0xffffff80010eb100, %rbx
	movq	%rbx, %rdi
	movl	$1, %esi
	JMP_XNU 0xffffff80004e6bf9
	.size	xnu_world_cpuid_p22_x86, . - xnu_world_cpuid_p22_x86

	.globl xnu_world_cpuid_p23_x86
	.type  xnu_world_cpuid_p23_x86, @function
xnu_world_cpuid_p23_x86:
	endbr64
	SITE_PROBE 23
	movabsq	$0xffffff80010b3218, %r11
	movq	(%r11), %rax
	cmpq	-0x30(%rbp), %rax
	jne	.Lp23_fail
	addq	$0x98, %rsp
	JMP_XNU 0xffffff80004e6c1f
.Lp23_fail:
	JMP_XNU 0xffffff80004e6c2f
	.size	xnu_world_cpuid_p23_x86, . - xnu_world_cpuid_p23_x86

/* Same bypass as the cache-info epilogue: replay the rest of
 * cpuid_set_info()'s epilogue here and continue straight to the cpuid probe's
 * post-call label, skipping the original pop rbp / ret. */
	.globl xnu_world_cpuid_p59_x86
	.type  xnu_world_cpuid_p59_x86, @function
xnu_world_cpuid_p59_x86:
	endbr64
	SITE_PROBE 23
	movabsq	$0xffffff80010b3218, %r11
	movq	(%r11), %rax
	cmpq	-0x30(%rbp), %rax
	jne	.Lp59_fail
	addq	$0x98, %rsp
	jmp	.Lp59_continue
.Lp59_fail:
	/* Log both sides of the failed comparison: id60 = saved canary,
	 * id61 = the current ___stack_chk_guard value. */
	movq	-0x30(%rbp), %rdx
	SITE_PROBE_RDX 60
	movabsq	$0xffffff80010b3218, %r11
	movq	(%r11), %rdx
	SITE_PROBE_RDX 61
	addq	$0x98, %rsp
.Lp59_continue:
	popq	%rbx
	popq	%r12
	popq	%r13
	popq	%r14
	popq	%r15
	popq	%rbp
	addq	$8, %rsp		/* discard the cpuid probe's return address */
	movabsq	$xnu_world_cpuid_probe_after_info_x86, %r11
	jmp	*%r11
	.size	xnu_world_cpuid_p59_x86, . - xnu_world_cpuid_p59_x86

	.globl xnu_world_cache_p24_x86
	.type  xnu_world_cache_p24_x86, @function
xnu_world_cache_p24_x86:
	endbr64
	SITE_PROBE 24
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%r15
	pushq	%r14
	pushq	%r13
	pushq	%r12
	pushq	%rbx
	JMP_XNU 0xffffff80004e6e7d
	.size	xnu_world_cache_p24_x86, . - xnu_world_cache_p24_x86

	.globl xnu_world_cache_p25_x86
	.type  xnu_world_cache_p25_x86, @function
xnu_world_cache_p25_x86:
	endbr64
	SITE_PROBE 25
	movl	$2, %eax
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	cpuid
	JMP_XNU 0xffffff80004e6ed1
	.size	xnu_world_cache_p25_x86, . - xnu_world_cache_p25_x86

/* Intel core/thread store block just before the second cache-info call.
 * The 13-byte window eats the first byte of the call, so replay it here. */
	.globl xnu_world_ci_p44_x86
	.type  xnu_world_ci_p44_x86, @function
xnu_world_ci_p44_x86:
	endbr64
	SITE_PROBE 44
	movabsq	$0xffffff80010eb290, %r11
	movl	%ecx, (%r11)
	movabsq	$0xffffff80010eb294, %r11
	movl	%eax, (%r11)
	movabsq	$0xffffff80004e6e70, %r11
	call	*%r11
	SITE_PROBE 45
	SITE_PROBE 62
	JMP_XNU 0xffffff80004e6b4d
	.size	xnu_world_ci_p44_x86, . - xnu_world_ci_p44_x86

/* After leaf-2 descriptor loop, right before the cpuid(0) deterministic
 * check. Replay the five original instructions and resume at 0x70f0. */
	.globl xnu_world_cache_p47_x86
	.type  xnu_world_cache_p47_x86, @function
xnu_world_cache_p47_x86:
	endbr64
	SITE_PROBE 47
	xorl	%eax, %eax
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	cpuid
	movl	%eax, %r13d
	JMP_XNU 0xffffff80004e70f0
	.size	xnu_world_cache_p47_x86, . - xnu_world_cache_p47_x86

/* Common path after the deterministic cache loop. */
	.globl xnu_world_cache_p49_x86
	.type  xnu_world_cache_p49_x86, @function
xnu_world_cache_p49_x86:
	endbr64
	SITE_PROBE 49
	movabsq	$0xffffff80010eb184, %r11
	movl	%ebx, (%r11)
	movabsq	$0xffffff80010eb360, %r11
	cmpl	$0, (%r11)
	JMP_XNU 0xffffff80004e7171
	.size	xnu_world_cache_p49_x86, . - xnu_world_cache_p49_x86

/* Inside the deterministic cache loop, just after cpuid(4,%ecx). */
	.globl xnu_world_cache_p48_x86
	.type  xnu_world_cache_p48_x86, @function
xnu_world_cache_p48_x86:
	endbr64
	SITE_PROBE 48
	movl	%eax, %r13d
	movl	%ecx, %r15d
	movabsq	$0xffffff80010eb360, %r11
	cmpl	$0, (%r11)
	JMP_XNU 0xffffff80004e71dd
	.size	xnu_world_cache_p48_x86, . - xnu_world_cache_p48_x86

/* Entry to leaf-2 TLB descriptor extraction (startup_debug == 0 path). */
	.globl xnu_world_cache_p52_x86
	.type  xnu_world_cache_p52_x86, @function
xnu_world_cache_p52_x86:
	endbr64
	SITE_PROBE 52
	movl	$1, %r15d
	movabsq	$0xffffff8000dbcdf0, %r12
	JMP_XNU 0xffffff80004e7479
	.size	xnu_world_cache_p52_x86, . - xnu_world_cache_p52_x86

/* Final cpuid_info pointer check before the epilogue. id53 means the saved
 * pointer no longer matches, which would send XNU to __stack_chk_fail. */
	.globl xnu_world_cache_p50_x86
	.type  xnu_world_cache_p50_x86, @function
xnu_world_cache_p50_x86:
	endbr64
	SITE_PROBE 50
	movabsq	$0xffffff80010b3218, %r11
	movq	(%r11), %rax
	cmpq	-0x30(%rbp), %rax
	jne	.Lp50_bad
	testl	%r14d, %r14d
	JMP_XNU 0xffffff80004e75b7
.Lp50_bad:
	SITE_PROBE 53
	testl	%r14d, %r14d
	JMP_XNU 0xffffff80004e75b7
	.size	xnu_world_cache_p50_x86, . - xnu_world_cache_p50_x86

/* Canary check / epilogue, immediately after the id50 window. Replay the
 * whole epilogue here and continue straight to cpuid_set_info()+0x143d,
 * bypassing the SITE_PROBE 45 upcall on the p44 stub's return path. */
	.globl xnu_world_cache_p54_x86
	.type  xnu_world_cache_p54_x86, @function
xnu_world_cache_p54_x86:
	endbr64
	SITE_PROBE 54
	cmpq	-0x30(%rbp), %rax
	jne	.Lp54_bad
	addq	$0x48, %rsp
	popq	%rbx
	popq	%r12
	popq	%r13
	popq	%r14
	popq	%r15
	popq	%rbp
	addq	$8, %rsp		/* discard p44 stub's return address */
	SITE_PROBE 56
	JMP_XNU 0xffffff80004e6b4d
.Lp54_bad:
	JMP_XNU 0xffffff80004e75f0
	.size	xnu_world_cache_p54_x86, . - xnu_world_cache_p54_x86

/* cpuid_wa_required() breadcrumbs. The two PE_parse_boot_argn() windows are
 * 16 bytes long; write_abs_jmp() replaces the first 13 and the stub replays
 * all 16 before resuming. */
	.globl xnu_world_wa_p26_x86
	.type  xnu_world_wa_p26_x86, @function
xnu_world_wa_p26_x86:
	endbr64
	movabsq	$0xffffff80010eb3c0, %r11
	cmpb	$0, (%r11)
	jne	.Lp26_taken
	SITE_PROBE 26
	movabsq	$0xffffff8000cb3ece, %rdi
	JMP_XNU 0xffffff80004e8799
.Lp26_taken:
	SITE_PROBE 30
	JMP_XNU 0xffffff80004e87e7
	.size	xnu_world_wa_p26_x86, . - xnu_world_wa_p26_x86

	.globl xnu_world_wa_p27_x86
	.type  xnu_world_wa_p27_x86, @function
xnu_world_wa_p27_x86:
	endbr64
	SITE_PROBE 27
	movabsq	$0xffffff8000b4cf10, %r11
	call	*%r11
	testl	%eax, %eax
	jne	.Lp27_done
	movabsq	$0xffffff80010eb3b0, %r11
	movq	$0, (%r11)
.Lp27_done:
	JMP_XNU 0xffffff80004e87b9
	.size	xnu_world_wa_p27_x86, . - xnu_world_wa_p27_x86

	.globl xnu_world_wa_p28_x86
	.type  xnu_world_wa_p28_x86, @function
xnu_world_wa_p28_x86:
	endbr64
	SITE_PROBE 28
	movabsq	$0xffffff8000b4cf10, %r11
	call	*%r11
	testl	%eax, %eax
	jne	.Lp28_done
	movabsq	$0xffffff80010eb3b8, %r11
	movq	$0, (%r11)
.Lp28_done:
	JMP_XNU 0xffffff80004e87e0
	.size	xnu_world_wa_p28_x86, . - xnu_world_wa_p28_x86

	.globl xnu_world_wa_p29_x86
	.type  xnu_world_wa_p29_x86, @function
xnu_world_wa_p29_x86:
	endbr64
	SITE_PROBE 29
	movl	$0x10a, %ecx
	rdmsr
	movl	%eax, %ecx
	movl	$4, %eax
	JMP_XNU 0xffffff80004e884f
	.size	xnu_world_wa_p29_x86, . - xnu_world_wa_p29_x86

	.globl xnu_world_wa_p33_x86
	.type  xnu_world_wa_p33_x86, @function
xnu_world_wa_p33_x86:
	endbr64
	SITE_PROBE 33
	movl	$7, %eax
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	cpuid
	JMP_XNU 0xffffff80004e8882
	.size	xnu_world_wa_p33_x86, . - xnu_world_wa_p33_x86

	.globl xnu_world_wa_p34_x86
	.type  xnu_world_wa_p34_x86, @function
xnu_world_wa_p34_x86:
	endbr64
	SITE_PROBE 34
	movl	$0x10a, %ecx
	rdmsr
	testb	$0x10, %bl
	je	.Lp34_exit
	movl	%eax, %ecx
	JMP_XNU 0xffffff80004e88c2
.Lp34_exit:
	JMP_XNU 0xffffff80004e8923
	.size	xnu_world_wa_p34_x86, . - xnu_world_wa_p34_x86

/* Host CPUs here do not implement the workaround MSRs (0x10a and friends), so
 * report CWA_OFF and let XNU skip every workaround write. */
	.globl xnu_world_wa_off_x86
	.type  xnu_world_wa_off_x86, @function
xnu_world_wa_off_x86:
	endbr64
	SITE_PROBE 34
	movl	$4, %eax
	ret
	.size	xnu_world_wa_off_x86, . - xnu_world_wa_off_x86

/* cpuid_set_info() between the workaround call and the bootarg checks. */
	.globl xnu_world_ci_p40_x86
	.type  xnu_world_ci_p40_x86, @function
xnu_world_ci_p40_x86:
	endbr64
	SITE_PROBE 40
	movl	$0x80000000, %eax
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	cpuid
	JMP_XNU 0xffffff80004e5827
	.size	xnu_world_ci_p40_x86, . - xnu_world_ci_p40_x86

	.globl xnu_world_ci_p35_x86
	.type  xnu_world_ci_p35_x86, @function
xnu_world_ci_p35_x86:
	endbr64
	SITE_PROBE 35
	movabsq	$0xffffff80010eb110, %rbx
	movl	$0x30, %r14d
	movl	$0x30, %edx
	JMP_XNU 0xffffff80004e5a2c
	.size	xnu_world_ci_p35_x86, . - xnu_world_ci_p35_x86

	.globl xnu_world_ci_p36_x86
	.type  xnu_world_ci_p36_x86, @function
xnu_world_ci_p36_x86:
	endbr64
	SITE_PROBE 36
	movabsq	$0xffffff8000cb3d35, %r15
	movq	%r15, %rdi
	movabsq	$0xffffff80004b3ec0, %r11
	call	*%r11
	JMP_XNU 0xffffff80004e5a43
	.size	xnu_world_ci_p36_x86, . - xnu_world_ci_p36_x86

	.globl xnu_world_ci_p37_x86
	.type  xnu_world_ci_p37_x86, @function
xnu_world_ci_p37_x86:
	endbr64
	SITE_PROBE 37
	movq	%rbx, %rdi
	movq	%r15, %rsi
	movq	%r14, %rdx
	movabsq	$0xffffff80004b3f50, %r11
	call	*%r11
	JMP_XNU 0xffffff80004e5a5c
	.size	xnu_world_ci_p37_x86, . - xnu_world_ci_p37_x86

	.globl xnu_world_ci_p38_x86
	.type  xnu_world_ci_p38_x86, @function
xnu_world_ci_p38_x86:
	endbr64
	SITE_PROBE 38
	movabsq	$0xffffff8000b4cf10, %r11
	call	*%r11
	testl	%eax, %eax
	jne	.Lp38_taken
	movl	$0, -0xb0(%rbp)
	JMP_XNU 0xffffff80004e6a2b
.Lp38_taken:
	JMP_XNU 0xffffff80004e6a51
	.size	xnu_world_ci_p38_x86, . - xnu_world_ci_p38_x86

	.globl xnu_world_ci_p39_x86
	.type  xnu_world_ci_p39_x86, @function
xnu_world_ci_p39_x86:
	endbr64
	SITE_PROBE 39
	movabsq	$0xffffff8000b4cf10, %r11
	call	*%r11
	testl	%eax, %eax
	je	.Lp39_done
	movl	$0, -0xb4(%rbp)
.Lp39_done:
	JMP_XNU 0xffffff80004e6a51
	.size	xnu_world_ci_p39_x86, . - xnu_world_ci_p39_x86

/* Bounded strlcpy(). XNU's copy scans for NUL without a limit; the host CPUID
 * brand string is 48 non-NUL bytes, so that scan ran off the stack. This
 * version copies at most size-1 bytes, always NUL-terminates, and returns the
 * copied length (callers here only test the return for nonzero). */
	.globl xnu_world_strlcpy_p41_x86
	.type  xnu_world_strlcpy_p41_x86, @function
xnu_world_strlcpy_p41_x86:
	endbr64
	SITE_PROBE 41
	testq	%rdx, %rdx
	je	.Lstrlcpy_done
	leaq	-1(%rdx), %rcx
	movq	%rdi, %r8
	xorl	%eax, %eax
.Lstrlcpy_scan:
	testq	%rcx, %rcx
	je	.Lstrlcpy_copy
	cmpb	$0, (%rsi,%rax,1)
	je	.Lstrlcpy_copy
	incq	%rax
	decq	%rcx
	jmp	.Lstrlcpy_scan
.Lstrlcpy_copy:
	movq	%rax, %rcx
	movq	%r8, %rdi
	rep movsb
	movb	$0, (%rdi)
.Lstrlcpy_done:
	ret
	.size	xnu_world_strlcpy_p41_x86, . - xnu_world_strlcpy_p41_x86

/* cpc_early_init() breadcrumbs: id10 after PE_parse_boot_argn(), id11 after
 * cpuid_info(). Both replay the original compare/branch so the early-exit
 * path still lands at cpc_early_init+0xe7. */
	.globl xnu_world_cpc1_probe_x86
	.type  xnu_world_cpc1_probe_x86, @function
xnu_world_cpc1_probe_x86:
	endbr64
	SITE_PROBE 10
	testl	%eax, %eax
	jne	.Lcpc1_exit
	movabsq	$0xffffff80004e5710, %r11	/* cpuid_info */
	call	*%r11
	movabsq	$0xffffff8000512fc3, %r11
	jmp	*%r11
.Lcpc1_exit:
	movabsq	$0xffffff8000513087, %r11
	jmp	*%r11
	.size	xnu_world_cpc1_probe_x86, . - xnu_world_cpc1_probe_x86

	.globl xnu_world_cpc2_probe_x86
	.type  xnu_world_cpc2_probe_x86, @function
xnu_world_cpc2_probe_x86:
	endbr64
	SITE_PROBE 11
	cmpb	$0x2, 0x138(%rax)
	jne	.Lcpc2_exit
	movq	%rax, %rbx
	movabsq	$0xffffff8000512fd0, %r11
	jmp	*%r11
.Lcpc2_exit:
	movabsq	$0xffffff8000513087, %r11
	jmp	*%r11
	.size	xnu_world_cpc2_probe_x86, . - xnu_world_cpc2_probe_x86

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
