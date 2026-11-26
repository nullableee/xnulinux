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
 * The interface between hosted XNU on x86_64 and its Linux host (x86_64/xnu-ko).
 *
 * This is deliberately NOT xnu-patches/include/linux64_abi.h. The ARM64 ABI is
 * keyed by MPIDR, carries PSTATE/ESR/FAR and PAC state, and is read through
 * TPIDR/SCTLR. The x86_64 ABI is keyed by the local APIC ID, carries CR2/RIP/
 * error-code fault frames, and is reached through a plain SysV function call.
 * Call numbers that keep the same semantic meaning keep the same value so the
 * arch-neutral patches can be shared textually; x86-only calls start at 850.
 *
 * Both sides share physical memory: the world page, boot_args, the kernel
 * collection and all request buffers are mapped in both page tables.
 * This header includes nothing and is copied into XNU's EXTERNAL_HEADERS.
 */
#ifndef _LINUX64_ABI_X86_64_H_
#define _LINUX64_ABI_X86_64_H_

#define LINUX64_X86_ABI_VERSION 1

enum linux64_call {
	LINUX64_CALL_KERNEL_TABLES  = 210,	/* a1 = XNU PML4 physical address; host installs shared kernel entries */
	LINUX64_CALL_IDLE           = 810,	/* a1 = ns to next XNU timer, a2 = pending IPI word for this CPU */
	LINUX64_CALL_PREEMPTED      = 811,	/* host interrupt pending: a1 = 1 from kernel, 0 from user */
	LINUX64_CALL_WALLCLOCK      = 812,	/* a1 = u64[2] {seconds, nanoseconds} UTC */
	LINUX64_CALL_NET_SEND       = 816,	/* a1 = struct linux64_buf_req: one Ethernet frame */
	LINUX64_CALL_NET_RECV       = 817,	/* a1 = struct linux64_buf_req: one Ethernet frame, result 0 if none */
	LINUX64_CALL_INPUT_READ     = 818,	/* a1 = struct linux64_buf_req: evdev records */
	LINUX64_CALL_KEYSTORE_RW    = 819,	/* a1 = struct linux64_rw_req */
	LINUX64_CALL_DISK_RW        = 820,	/* a1 = struct linux64_rw_req */
	LINUX64_CALL_DISK_SIZE      = 821,	/* a1 = u64 *: system disk size in bytes */
	LINUX64_CALL_RANDOM         = 822,	/* a1 = buffer, a2 = length */
	LINUX64_CALL_HOSTCALL       = 830,	/* a1 = struct linux64_hostcall_req */
	LINUX64_CALL_CPU_START      = 840,	/* a1 = APIC ID, a2 = cpu_data_t: host enters linux64_cpu_entry(a2) */
	LINUX64_CALL_IPI            = 841,	/* a1 = target APIC ID: set its pending-IPI word and wake it */
	LINUX64_CALL_CPU_HALT       = 842,	/* a1 = APIC ID: park a secondary CPU until CPU_START */
	LINUX64_CALL_FAULT          = 843,	/* a1 = struct linux64_x86_fault: unhandled kernel fault, ends the run */
	LINUX64_CALL_CONSOLE_READ   = 993,
	LINUX64_CALL_RAMDISK_RW     = 994,
	LINUX64_CALL_RAMDISK_SIZE   = 995,
	LINUX64_CALL_CONSOLE_WRITE  = 996,
	LINUX64_CALL_ASSERT         = 997,
	LINUX64_CALL_KPRINTF        = 998,
	LINUX64_CALL_PANIC          = 999,
	LINUX64_CALL_SITE_TAG       = 1001,	/* a1 = diagnostic site id (host-only probing) */
	LINUX64_CALL_USER_ENTER     = 9100,	/* a1 = first user RIP */
};

struct linux64_buf_req {
	void *buf;
	__UINT64_TYPE__ len;
	__INT64_TYPE__ result;
};

struct linux64_rw_req {
	__UINT64_TYPE__ write;
	__UINT64_TYPE__ off;
	__UINT64_TYPE__ len;
	void *buf;
	__INT64_TYPE__ result;
};

struct linux64_hostcall_req {
	const void *req;
	__UINT64_TYPE__ len;
	void *reply;
	__UINT64_TYPE__ max;
	__INT64_TYPE__ result;
};

struct linux64_assert_info {
	const char *file;
	long line;
	const char *expr;
};

struct linux64_input_event {
	unsigned short type;
	unsigned short code;
	int value;
};

/* x86_64 kernel fault handed to the host (replaces ESR/FAR/SPSR on ARM64). */
struct linux64_x86_fault {
	__UINT64_TYPE__ vector;		/* 14 (#PF), 13 (#GP), 6 (#UD), 10 (#TS) */
	__UINT64_TYPE__ error_code;
	__UINT64_TYPE__ cr2;
	__UINT64_TYPE__ rip;
	__UINT64_TYPE__ rsp;
	__UINT64_TYPE__ rflags;
};

/*
 * Slots are indexed by the x2APIC ID (8 bits is enough for the first port).
 * The host refuses CPUs whose APIC IDs collide in this table.
 */
#define LINUX64_X86_CPU_SLOTS 256
#define LINUX64_X86_CPU_SLOT(apic_id) ((unsigned)(apic_id) & 0xffu)
#define LINUX64_CPU_SLOTS LINUX64_X86_CPU_SLOTS

enum linux64_counter {
	LINUX64_CTR_IDLE_THREAD  = 0,
	LINUX64_CTR_MACHINE_IDLE = 1,
	LINUX64_CTR_CPU_IDLE     = 2,
	LINUX64_CTR_HOST_IDLE    = 3,
	LINUX64_CTR_SWITCHES     = 4,
	LINUX64_CTR_LAST_CALL    = 6,
	LINUX64_CTR_RTCLOCK      = 7,	/* host timer injections */
	LINUX64_CTR_IPI          = 8,	/* host IPI deliveries */
	LINUX64_CTR_THREAD       = 9,
	LINUX64_CTR_USER_EXC     = 10,	/* exceptions from ring 3 */
	LINUX64_CTR_USER_VECTOR  = 11,
	LINUX64_CTR_USER_RIP     = 12,
	LINUX64_CTR_BAD_VECTOR   = 14,
	LINUX64_CTR_DUMP_REQ     = 15,
	LINUX64_NCOUNTERS        = 16,
};

#endif /* _LINUX64_ABI_X86_64_H_ */
