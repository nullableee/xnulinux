/* SPDX-License-Identifier: APSL-2.0 */
/*
 * The x86 boot_args passed to vstart() (pexpert/pexpert/i386/boot.h).
 * Kept here as a plain C description so the host-side loader can synthesize it
 * without including any XNU header. The layout must stay exactly 4096 bytes.
 */
#ifndef _LINUX64_BOOT_ARGS_X86_64_H_
#define _LINUX64_BOOT_ARGS_X86_64_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define X86_BOOT_LINE_LENGTH      1024
#define X86_BOOT_ARGS_VERSION     2
#define X86_BOOT_ARGS_REVISION    1

enum x86_efi_memory_type {
	X86_EFI_RESERVED_MEMORY_TYPE       = 0,
	X86_EFI_LOADER_CODE                = 1,
	X86_EFI_LOADER_DATA                = 2,
	X86_EFI_BOOT_SERVICES_CODE         = 3,
	X86_EFI_BOOT_SERVICES_DATA         = 4,
	X86_EFI_RUNTIME_SERVICES_CODE      = 5,
	X86_EFI_RUNTIME_SERVICES_DATA      = 6,
	X86_EFI_CONVENTIONAL_MEMORY        = 7,
	X86_EFI_UNUSABLE_MEMORY            = 8,
	X86_EFI_ACPI_RECLAIM_MEMORY        = 9,
	X86_EFI_ACPI_MEMORY_NVS            = 10,
	X86_EFI_MEMORY_MAPPED_IO           = 11,
	X86_EFI_MEMORY_MAPPED_IO_PORT_SPACE= 12,
	X86_EFI_PAL_CODE                   = 13,
	X86_EFI_MAX_MEMORY_TYPE            = 14,
};

struct x86_efi_memory_range {
	uint32_t type;
	uint32_t pad;
	uint64_t physical_start;
	uint64_t virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
};

struct x86_boot_video_v1 {
	uint32_t v_base_addr;
	uint32_t v_display;
	uint32_t v_row_bytes;
	uint32_t v_width;
	uint32_t v_height;
	uint32_t v_depth;
};

struct x86_boot_video {
	uint32_t v_display;
	uint32_t v_row_bytes;
	uint32_t v_width;
	uint32_t v_height;
	uint32_t v_depth;
	uint8_t v_rotate;
	uint8_t v_resv_byte[3];
	uint32_t v_resv[6];
	uint64_t v_base_addr;
};

/*
 * Host pages that must survive XNU's switch from the booter page tables to
 * IdlePML4. Stored in the boot_args reserved area; the same layout is declared
 * in pexpert/pexpert/i386/boot.h by patch 0011.
 */
#define LINUX64_HOST_MAP_MAX 128
struct linux64_host_map {
	uint32_t count;
	uint32_t pad;
	uint64_t va[LINUX64_HOST_MAP_MAX];
	uint64_t pa[LINUX64_HOST_MAP_MAX];
};

struct x86_boot_args {
	uint16_t revision;
	uint16_t version;
	uint8_t efi_mode;
	uint8_t debug_mode;
	uint16_t flags;
	char command_line[X86_BOOT_LINE_LENGTH];

	uint32_t memory_map;              /* physical address, must be < 4 GiB */
	uint32_t memory_map_size;
	uint32_t memory_map_descriptor_size;
	uint32_t memory_map_descriptor_version;

	struct x86_boot_video_v1 video_v1;

	uint32_t device_tree_p;
	uint32_t device_tree_length;

	uint32_t kaddr;                   /* physical address of kernel text */
	uint32_t ksize;                   /* kernel text+data+efi */

	uint32_t efi_runtime_services_page_start;
	uint32_t efi_runtime_services_page_count;
	uint64_t efi_runtime_services_virtual_page_start;

	uint32_t efi_system_table;
	uint32_t kslide;

	uint32_t performance_data_start;
	uint32_t performance_data_size;

	uint32_t key_store_data_start;
	uint32_t key_store_data_size;
	uint64_t boot_mem_start;
	uint64_t boot_mem_size;
	uint64_t physical_memory_size;
	uint64_t fsb_frequency;
	uint64_t pci_config_space_base_address;
	uint32_t pci_config_space_start_bus_number;
	uint32_t pci_config_space_end_bus_number;
	uint32_t csr_active_config;
	uint32_t csr_capabilities;
	uint32_t boot_smc_plimit;
	uint16_t boot_progress_meter_start;
	uint16_t boot_progress_meter_end;
	struct x86_boot_video video;

	uint32_t apfs_data_start;
	uint32_t apfs_data_size;

	/* Version 2, Revision 1 */
	uint64_t kc_hdrs_vaddr;

	uint64_t arv_root_hash_start;
	uint64_t arv_root_hash_size;
	uint64_t arv_manifest_start;
	uint64_t arv_manifest_size;
	uint64_t bs_arv_root_hash_start;
	uint64_t bs_arv_root_hash_size;
	uint64_t bs_arv_manifest_start;
	uint64_t bs_arv_manifest_size;

	union {
		uint32_t reserved4[692];
		struct linux64_host_map host_map;
	};
};

_Static_assert(sizeof(struct x86_boot_args) == 4096, "x86 boot_args must be 4096 bytes");
_Static_assert(sizeof(struct linux64_host_map) <= 692 * sizeof(uint32_t),
    "host map must fit in boot_args reserved area");

int linux64_boot_args_init(struct x86_boot_args *ba,
    uint64_t kernel_pa, uint32_t kernel_size, uint64_t kc_hdrs_vaddr,
    const char *command_line,
    uint32_t memory_map_pa, const struct x86_efi_memory_range *map,
    uint32_t map_count, uint32_t descriptor_size,
    uint64_t physical_memory_size, uint64_t apfs_key_pa, uint32_t apfs_key_size);

#endif /* _LINUX64_BOOT_ARGS_X86_64_H_ */
