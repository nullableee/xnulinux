/* SPDX-License-Identifier: APSL-2.0 */
/* Host-side world table. world_x86.S finds its per-CPU world by APIC ID. */
#include "world_x86.h"

#include "../include/linux64_abi_x86_64.h"

#include <linux/module.h>

/* Linux's taint table has no APSL entry; this keeps modpost happy without
 * misrepresenting the licence. */
MODULE_LICENSE("Proprietary");
MODULE_DESCRIPTION("XNU x86_64 world switch (APSL-2.0)");

struct xnu_world_x86 xnu_W_x86;
struct xnu_world_x86 *xnu_worlds_x86[LINUX64_X86_CPU_SLOTS];
