/*
 * LINUX64 (xnulinux): synthesize the x86_64 boot_args that vstart() expects.
 *
 * This is host-loader code (APSL side), not part of the XNU tree: it copies no
 * Linux structure and only needs plain integers and physical addresses. The
 * host module provides the memory map and kernel image placement through FKI.
 */
#include "boot_args_x86_64.h"

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

int
linux64_boot_args_init(struct x86_boot_args *ba,
    uint64_t kernel_pa, uint32_t kernel_size, uint64_t kc_hdrs_vaddr,
    const char *command_line,
    uint32_t memory_map_pa, const struct x86_efi_memory_range *map,
    uint32_t map_count, uint32_t descriptor_size,
    uint64_t physical_memory_size, uint64_t apfs_key_pa, uint32_t apfs_key_size)
{
	if (ba == NULL || kernel_size == 0 || map == NULL || map_count == 0) {
		return -1;
	}
	/* boot_args::MemoryMap and kaddr/ksize are 32-bit physical addresses. */
	if (kernel_pa > UINT32_MAX || memory_map_pa > UINT32_MAX ||
	    apfs_key_pa > UINT32_MAX || apfs_key_size > UINT32_MAX) {
		return -2;
	}

	memset(ba, 0, sizeof(*ba));
	ba->revision = X86_BOOT_ARGS_REVISION;
	ba->version = X86_BOOT_ARGS_VERSION;
	ba->efi_mode = 64;
	ba->memory_map = memory_map_pa;
	ba->memory_map_size = map_count * descriptor_size;
	ba->memory_map_descriptor_size = descriptor_size;
	ba->memory_map_descriptor_version = 1;
	ba->kaddr = (uint32_t)kernel_pa;
	ba->ksize = kernel_size;
	ba->physical_memory_size = physical_memory_size;
	ba->boot_mem_start = kernel_pa + kernel_size;
	ba->boot_mem_size = physical_memory_size > ba->boot_mem_start ?
	    physical_memory_size - ba->boot_mem_start : 0;
	ba->kc_hdrs_vaddr = kc_hdrs_vaddr;
	ba->apfs_data_start = (uint32_t)apfs_key_pa;
	ba->apfs_data_size = apfs_key_size;

	/* A hosted boot has no firmware services, SMC or PCI config space. */
	ba->efi_system_table = 0;
	ba->pci_config_space_base_address = 0;
	ba->pci_config_space_start_bus_number = 0;
	ba->pci_config_space_end_bus_number = 0;

	/* CSR_ALLOW_UNAUTHENTICATED_ROOT | CSR_ALLOW_TASK_FOR_PID ... are passed
	 * as boot-args by the userland image; leave the bitmask to the command
	 * line parser in PE_init_platform. */
	if (command_line != NULL) {
		size_t n = strlen(command_line);

		if (n >= sizeof(ba->command_line)) {
			n = sizeof(ba->command_line) - 1;
		}
		memcpy(ba->command_line, command_line, n);
	}
	return 0;
}
