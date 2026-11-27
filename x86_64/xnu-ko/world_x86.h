/* SPDX-License-Identifier: APSL-2.0 */
/* Per-CPU world switch state for hosted XNU on x86_64. Layout is shared with
 * world_x86.S; the offsets below are part of the ABI between host C and asm.
 *
 * Unlike ARM64 there are no PAC keys or TTBR0/TCR to swap. The conflicting
 * resources are ring-0 ones: CR3, IDT, GDT, TR, GS/KernelGS and the syscall
 * MSRs. Linux and XNU cannot own them at the same time.
 *
 * No XNU register is reserved: upcalls rediscover the world from the APIC ID
 * and index xnu_worlds_x86[], so XNU's own use of %r15 is safe. */
#pragma once
#include "../fki-ko/fki.h"

/* Offsets used by world_x86.S. Keep in sync with the struct below. */
#define XW_LINUX_RBX        0
#define XW_LINUX_RBP        8
#define XW_LINUX_R12       16
#define XW_LINUX_R13       24
#define XW_LINUX_R14       32
#define XW_LINUX_R15       40
#define XW_LINUX_RSP       48
#define XW_XNU_RBX         56
#define XW_XNU_RBP         64
#define XW_XNU_R12         72
#define XW_XNU_R13         80
#define XW_XNU_R14         88
#define XW_XNU_R15         96
#define XW_XNU_RSP        104
#define XW_LINUX_STATE    112
#define XW_XNU_STATE      (XW_LINUX_STATE + sizeof(struct fki_x86_state))
#define XW_XNU_STACK_TOP  (XW_XNU_STATE + sizeof(struct fki_x86_state))
#define XW_ENTRY_RIP      (XW_XNU_STACK_TOP + 8)
#define XW_ENTRY_ARG      (XW_ENTRY_RIP + 8)
#define XW_HOOK_COOKIE    (XW_ENTRY_ARG + 8)
#define XW_FAULT_RIP      (XW_HOOK_COOKIE + 8)
#define XW_RUNNING        (XW_FAULT_RIP + 8)
#define XW_HOOK_CALL      (XW_RUNNING + 8)
#define XW_HOOK_A1        (XW_HOOK_CALL + 8)
#define XW_HOOK_A2        (XW_HOOK_A1 + 8)
#define XW_RUN_RESULT     (XW_HOOK_A2 + 8)

struct xnu_world_x86 {
	unsigned long long linux_rbx, linux_rbp, linux_r12, linux_r13, linux_r14, linux_r15, linux_rsp;
	unsigned long long xnu_rbx, xnu_rbp, xnu_r12, xnu_r13, xnu_r14, xnu_r15, xnu_rsp;
	struct fki_x86_state linux_state;
	struct fki_x86_state xnu_state;
	unsigned long long xnu_stack_top;
	unsigned long long entry_rip;
	unsigned long long entry_arg;
	unsigned long long hook_cookie;
	unsigned long long fault_rip;
	unsigned long long running;
	unsigned long long hook_call, hook_a1, hook_a2;
	unsigned long long run_result;
};

_Static_assert(__builtin_offsetof(struct xnu_world_x86, linux_rbx) == XW_LINUX_RBX, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, xnu_rsp) == XW_XNU_RSP, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, xnu_rbx) == XW_XNU_RBX, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, linux_state) == XW_LINUX_STATE, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, xnu_state) == XW_XNU_STATE, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, xnu_stack_top) == XW_XNU_STACK_TOP, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, running) == XW_RUNNING, "xnu_world_x86 layout");
_Static_assert(__builtin_offsetof(struct xnu_world_x86, run_result) == XW_RUN_RESULT, "xnu_world_x86 layout");

extern struct xnu_world_x86 xnu_W_x86;
extern struct xnu_world_x86 *xnu_worlds_x86[];

/* Run fn(arg) in the XNU world. 0: fn returned, 1: the hook ended the run. */
long xnu_world_call_x86(struct xnu_world_x86 *w, unsigned long fn, unsigned long arg);
/* Linux CR3, kept in module data so the shared hook can switch worlds. */
extern unsigned long xnu_host_cr3_x86;
/* Address XNU calls as linux64_hook. Enters the Linux world and calls xnu_hook_x86(). */
void xnu_world_upcall_x86(void);
/* Module-text hook gate: restore Linux CR3, then jump to the upcall. */
void xnu_world_hook_trampoline_x86(void);
/* Diagnostic patch over _kprintf: log its format string as a KPRINTF upcall. */
void xnu_world_kprintf_probe_x86(void);
/* Diagnostic patches: emit a SITE_TAG upcall, then resume the original code. */
void xnu_world_site1_probe_x86(void);
void xnu_world_ptovirt_probe_x86(void);
void xnu_world_site3_probe_x86(void);
void xnu_world_site4_probe_x86(void);
void xnu_world_site5_probe_x86(void);
void xnu_world_site6_probe_x86(void);
void xnu_world_site7_probe_x86(void);
void xnu_world_site8_probe_x86(void);
void xnu_world_site9_probe_x86(void);
void xnu_world_cpc1_probe_x86(void);
void xnu_world_cpc2_probe_x86(void);
void xnu_world_cpuid_probe_x86(void);
void xnu_world_cpuid_p14_x86(void);
void xnu_world_cpuid_p16_x86(void);
void xnu_world_cpuid_p17_x86(void);
void xnu_world_cpuid_p18_x86(void);
void xnu_world_cpuid_p19_x86(void);
void xnu_world_cpuid_p20_x86(void);
void xnu_world_cpuid_p21_x86(void);
void xnu_world_cpuid_p22_x86(void);
void xnu_world_cpuid_p23_x86(void);
void xnu_world_cache_p24_x86(void);
void xnu_world_cache_p25_x86(void);
void xnu_world_wa_p26_x86(void);
void xnu_world_wa_p27_x86(void);
void xnu_world_wa_p28_x86(void);
void xnu_world_wa_p29_x86(void);
void xnu_world_wa_p33_x86(void);
void xnu_world_wa_p34_x86(void);
void xnu_world_wa_off_x86(void);
void xnu_world_ci_p35_x86(void);
void xnu_world_ci_p36_x86(void);
void xnu_world_ci_p37_x86(void);
void xnu_world_ci_p38_x86(void);
void xnu_world_ci_p39_x86(void);
void xnu_world_ci_p40_x86(void);
void xnu_world_strlcpy_p41_x86(void);
void xnu_world_ci_p44_x86(void);
void xnu_world_cache_p47_x86(void);
void xnu_world_cache_p48_x86(void);
void xnu_world_cache_p49_x86(void);
void xnu_world_cache_p50_x86(void);
void xnu_world_cache_p52_x86(void);
void xnu_world_cache_p54_x86(void);
/* Linux-world handler: 0 resumes XNU, nonzero ends the run. */
long xnu_hook_x86(unsigned long call, unsigned long a1, unsigned long a2);
/* Translate XNU virtual addresses in hook arguments to host aliases. */
typedef void *(*xnu_hook_translate_fn)(unsigned long va);
int xnu_hook_set_translate(xnu_hook_translate_fn fn);
