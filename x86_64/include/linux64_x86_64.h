// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (c) 2026 xnulinux contributors. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file contains Original Code and/or Modifications of Original Code as defined in and that are subject to the Apple
 * Public Source License Version 2.0 (the 'License'). You may not use this file except in compliance with the License.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * XNU-side interface of the x86_64 LINUX64 machine layer.
 *
 * Copied with linux64_abi_x86_64.h into XNU's EXTERNAL_HEADERS/linux64. The
 * names shared with the ARM64 layer (linux64_hook, linux64_vint, ...) are kept
 * on purpose: the arch-neutral part of the patches can then be applied to both
 * trees without renaming. All architecture state here is x86_64: APIC ID
 * slots, a SysV `callq *hook` upcall, GS-based per-CPU data, CR2/RIP faults.
 *
 * Hosted or not is decided at run time: linux64_hook is 0 when this kernel
 * boots on real hardware, and every hosted path is behind `if (linux64_hook)`.
 */
#ifndef _LINUX64_X86_64_H_
#define _LINUX64_X86_64_H_

#include <linux64/linux64_abi_x86_64.h>

#ifndef LINUX64_DEBUG
#define LINUX64_DEBUG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned long linux64_hook;
extern unsigned long linux64_slide;

/* APIC ID of this CPU. Implemented by the x86_64 machine-layer patch; before
 * cpu_data_t exists it reads the local APIC ID register, afterwards it uses
 * cpu_number()/cpu_data. */
extern unsigned linux64_x86_apic_id(void);
static inline unsigned
linux64_cpu_slot(void)
{
	return LINUX64_X86_CPU_SLOT(linux64_x86_apic_id());
}

/* Virtual interrupt flag: real IF stays clear while XNU runs. */
extern unsigned int linux64_vint[LINUX64_X86_CPU_SLOTS];
#define LINUX64_VINT linux64_vint[linux64_cpu_slot()]
extern unsigned int linux64_ipi_pending[LINUX64_X86_CPU_SLOTS];
extern unsigned long linux64_dbg[LINUX64_NCOUNTERS];

/*
 * A plain indirect SysV call: rdi = call, rsi = a1, rdx = a2, rax = return.
 * Not a function-pointer call from C: the hook is host text in a different
 * page table/IDT world and must be entered with one explicit instruction.
 */
static inline unsigned long
linux64_call2(enum linux64_call call, unsigned long a1, unsigned long a2)
{
	register unsigned long rdi __asm__("rdi") = (unsigned long)call;
	register unsigned long rsi __asm__("rsi") = a1;
	register unsigned long rdx __asm__("rdx") = a2;
	register unsigned long hook __asm__("rax") = linux64_hook;

	linux64_dbg[LINUX64_CTR_LAST_CALL] = (unsigned long)call;
	if (linux64_hook) {
		__asm__ volatile ("callq *%3"
		    : "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(hook)
		    :
		    : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
	}
	return rdi;
}

static inline unsigned long
linux64_call(enum linux64_call call, const void *a1)
{
	return linux64_call2(call, (unsigned long)a1, 0);
}

static inline void
linux64_random(void *buf, unsigned long len)
{
	linux64_call2(LINUX64_CALL_RANDOM, (unsigned long)buf, len);
}

static inline __UINT64_TYPE__
linux64_random_uniform(__UINT64_TYPE__ bound)
{
	__UINT64_TYPE__ v, limit;

	if (bound == 0) {
		return 0;
	}
	limit = -bound % bound;
	do {
		linux64_random(&v, sizeof(v));
	} while (v < limit);
	return v % bound;
}

static inline void
linux64_count(enum linux64_counter c)
{
	linux64_dbg[c]++;
}

/* Shared with the ARM64 patches (arch-neutral call sites). */
void linux64_assert_report(const char *file, int line, const char *expr);
void linux64_ipi_take(void);
void linux64_cpu_signal(unsigned int cpu);
void linux64_trust_cache_late_init(void);
int linux64_amfi_off(void);
void linux64_dump_threads(void);
void linux64_spin_relax(void);

/* x86_64-only machine-layer entry points. */
void linux64_cpu_entry(void *cpu_data) __attribute__((noreturn));
void linux64_kernel_fault(struct linux64_x86_fault *fault);
void linux64_user_fault(struct linux64_x86_fault *fault);
void linux64_user_enter(unsigned long rip);
void linux64_user_run(void *saved_state, unsigned long arg) __attribute__((noreturn));
void linux64_x86_timer(void);
void linux64_x86_ipi(void);

#ifdef __cplusplus
}
#endif

#endif /* _LINUX64_X86_64_H_ */
