/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared between FKI-X86's own files (not part of fki.h). */
#ifndef FKI_X86_INTERNAL_H
#define FKI_X86_INTERNAL_H
/* Run fn(arg) where Linux may use its own stack, IRQs and preemption. */
void fki_linux_runs_ext(void (*fn)(void *), void *arg);
int fki_hostcall_init(void);
void fki_hostcall_exit(void);
typedef void *(*xnu_hook_translate_fn)(unsigned long va);
int xnu_hook_set_translate(xnu_hook_translate_fn fn);
#endif
