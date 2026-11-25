/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Compatibility include for arch-neutral xnulinux patch hunks.
 *
 * The original ARM64 series includes <linux64/linux64.h>. The x86_64 port
 * keeps its own ABI header, so this shim lets the same arch-neutral hunks be
 * applied to both trees without editing them.
 */
#ifndef _LINUX64_COMPAT_H_
#define _LINUX64_COMPAT_H_

#include <linux64/linux64_abi_x86_64.h>
#include <linux64/linux64_x86_64.h>

#endif /* _LINUX64_COMPAT_H_ */
