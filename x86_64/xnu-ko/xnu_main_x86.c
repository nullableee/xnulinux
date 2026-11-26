/* SPDX-License-Identifier: APSL-2.0 */
/*
 * x86_64 host loader for the separate LINUX64 x86 path.
 *
 * Loads a macOS 26 x86_64 kernel collection, synthesises the x86 boot_args,
 * builds the minimum initial page tables XNU's vstart() needs, and enters it
 * through the x86 world switch. The first gate is a kprintf on the host console.
 */
#include "macho_x86.h"
#include "world_x86.h"

#include "../include/linux64_abi_x86_64.h"
#include "../include/boot_args_x86_64.h"
#include "../fki-ko/fki.h"

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/random.h>

MODULE_LICENSE("Proprietary");
MODULE_DESCRIPTION("XNU x86_64 host loader (APSL-2.0)");

/* ---- tunables ------------------------------------------------------------- */

static char *image = "xnu26.kernel";
module_param(image, charp, 0444);
static char *boot_line = "-v debug=0x8 startup_debug=1 amfi_get_out_of_my_way=1";
module_param(boot_line, charp, 0444);
static int ram_mb = 512;
module_param(ram_mb, int, 0444);
static int nr_cpus = 1;
module_param(nr_cpus, int, 0444);
static int allow_unhosted;
module_param(allow_unhosted, int, 0444);
/* Diagnostic: turn XNU's kprintf() into fsynced KPRINTF upcalls. */
static int kprintf_probe;
module_param(kprintf_probe, int, 0444);

#define MAX_XNU_CPUS 16
static struct xnu_world_x86 worlds[MAX_XNU_CPUS];
static struct xnu_region {
	struct fki_region kc;
	struct fki_region ba;
	struct fki_region dt;
	struct fki_region pt;
	struct fki_region xsave;
	struct fki_region linux_xsave;
} mem;

/*
 * boot_args, the EFI map and the device tree must live at physical addresses
 * above the kernel image's unslid link range. XNU converts their PAs with
 * ml_static_ptovirt(); below that range the static-alias PTEs are overwritten
 * by the kernel image overlay in build_init_pt(), so XNU would read kernel
 * data instead of the structures we prepared. Each of these is carved out of
 * a larger contiguous allocation; the backing region owns the pages.
 */
static struct fki_region ba_backing, efi_backing, dt_backing;

static struct xnu_image_x86 img;
static struct linux64_host_map host_map_store;
static u64_t a_vstart, a_hook, a_slide, a_cpu_entry, a_vint, a_ipi;
static u64_t kc_phys, kc_virt, xnu_slide;
/* XNU-visible address of the module-text upcall gate. */
static u64_t xnu_upcall_va;
/* Read by xnu_world_hook_trampoline_x86() before it leaves XNU's CR3. */
unsigned long xnu_host_cr3_x86;
static char *cmdline_copy;
static int stop_run;
module_param(stop_run, int, 0644);

/* Host-side window into XNU's stack, used by the watchdog below. */
static void *xnu_stack_host;
static u64_t xnu_stack_top;
static volatile int xnu_run_active;
static volatile int xnu_stack_dump_seq;
static struct task_struct *xnu_stack_thread;

static void logs(const char *s)
{
	fki_log(s, strlen(s));
	fki_debug_log(s, strlen(s));
}

static void debugf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		fki_debug_log(buf, min_t(int, n, sizeof(buf) - 1));
}

/*
 * XNU runs with interrupts and preemption disabled on its CPU, so the normal
 * phase breadcrumbs go quiet if it wedges. This watchdog runs on another CPU
 * and takes a few snapshots of XNU's stack. The addresses can be mapped back
 * to symbols offline from the same firmware image.
 *
 * xnu_rsp is stale after the last upcall: XNU keeps executing on the same
 * stack and reuses those slots.  The live frame can be BELOW the saved RSP
 * (a nested call such as early_random_init/doublemap_init) or above it (the
 * saved registers and return address).  Dump a window on both sides and
 * include an index with each non-zero word so the frames can be decoded.
 */
static int xnu_stack_watchdog(void *unused)
{
	static char buf[8192];

	while (!kthread_should_stop()) {
		if (READ_ONCE(xnu_run_active) && xnu_stack_host &&
		    xnu_stack_top && READ_ONCE(xnu_stack_dump_seq) < 3) {
			u64_t rsp = READ_ONCE(worlds[0].xnu_rsp);
			u64_t stack_base = (u64_t)(uintptr_t)xnu_stack_host;
			u64_t base = rsp > stack_base + 2048 ?
			    rsp - 2048 : stack_base;
			u64_t *words = (u64_t *)(uintptr_t)base;
			int nwords = (int)((xnu_stack_top - base) >> 3) + 1;
			int seq = READ_ONCE(xnu_stack_dump_seq) + 1;
			int n, i;

			if (nwords > 320)
				nwords = 320;
			n = snprintf(buf, sizeof(buf),
			    "xnu stack #%d top=0x%llx rsp=0x%llx base=0x%llx words=%d:",
			    seq, (unsigned long long)xnu_stack_top,
			    (unsigned long long)rsp,
			    (unsigned long long)base, nwords);
			for (i = 0; i < nwords && n < (int)sizeof(buf) - 32; i++) {
				u64_t v = READ_ONCE(words[i]);

				if (v)
					n += snprintf(buf + n, sizeof(buf) - n,
					    " [%d]=%016llx", i,
					    (unsigned long long)v);
			}
			if (n > 0)
				logs(buf);
			WRITE_ONCE(xnu_stack_dump_seq, seq);
		}
		msleep_interruptible(1000);
	}
	return 0;
}

/* ---- initial page tables -------------------------------------------------- */

/*
 * XNU boots at its unslid link addresses (0xffffff8000...). Map the copied
 * kernel image there with 4 KiB pages while keeping a normal identity map of
 * low physical memory for vstart()'s early code. Linux keeps its own CR3; this
 * one is installed only in XNU's world state.
 */
#define XNU_VM_MIN_KERNEL 0xffffff8000000000ULL
#define PML4_KERNEL_INDEX 511
#define PT_E_PRESENT      0x001
#define PT_E_WRITABLE     0x002
#define PT_E_LARGE        0x080

/*
 * The world switch and the hook gate execute from module text.  Map the
 * handful of host pages they touch into the XNU page tables at their native
 * Linux virtual addresses: module text/data, the current Linux stack (used
 * until fki_x86_enter() switches stacks) and the XNU stack itself.  The XNU
 * stack is vmalloc-backed so upcalls can also push on it after switching back
 * to Linux's CR3.
 */
#define HOST_PDPT_PAGES 8
#define HOST_PDE_PAGES  8
#define HOST_PTE_PAGES  8

#define PT_PML4       0
#define PT_PDPT_ID    1
#define PT_PDE_ID     2
#define PT_PDPT_K     3
#define PT_PDE_K      4
#define PT_HOST_PDPT  (PT_PDE_K + 1)
#define PT_HOST_PDE   (PT_HOST_PDPT + HOST_PDPT_PAGES)
#define PT_HOST_PTE   (PT_HOST_PDE + HOST_PDE_PAGES)
#define PT_PTE_K      (PT_HOST_PTE + HOST_PTE_PAGES)

struct init_pt {
	u64_t *pml4;
	u64_t *pdpt_id;
	u64_t *pde_id;
	u64_t *pdpt_k;
	u64_t *pde_k;
	u64_t **pte_k;
	unsigned long n_pte_pages;
	u64_t *host_pdpt[HOST_PDPT_PAGES];
	u64_t *host_pde[HOST_PDE_PAGES];
	u64_t *host_pte[HOST_PTE_PAGES];
	unsigned int n_host_pdpt, n_host_pde, n_host_pte;
	struct fki_region region;
};

static u64_t *pt_page_va(struct init_pt *pt, u64_t pt_pa, unsigned int idx)
{
	return (u64_t *)((char *)pt->region.va + ((u64_t)idx << PAGE_SHIFT));
}

static u64_t pt_page_pa(u64_t pt_pa, unsigned int idx)
{
	return pt_pa + ((u64_t)idx << PAGE_SHIFT);
}

/* Lazily allocate a child table, reusing the one already installed by the
 * kernel-half mapping (PML4[511] -> pdpt_k) when possible. */
static u64_t *pt_child_table(struct init_pt *pt, u64_t pt_pa, u64_t *parent,
    unsigned long index, u64_t **pool, unsigned int *used, unsigned int max,
    unsigned int base_idx)
{
	u64_t entry = parent[index];
	u64_t pa;
	unsigned int n;

	if (!entry) {
		if (*used >= max)
			return NULL;
		n = (*used)++;
		pool[n] = pt_page_va(pt, pt_pa, base_idx + n);
		entry = pt_page_pa(pt_pa, base_idx + n) |
		    PT_E_PRESENT | PT_E_WRITABLE;
		parent[index] = entry;
	}
	pa = entry & PAGE_MASK;
	return (u64_t *)((char *)pt->region.va + (pa - pt_pa));
}

static void record_host_map(u64_t va, u64_t pa)
{
	unsigned int i;

	if (host_map_store.count >= LINUX64_HOST_MAP_MAX)
		return;
	for (i = 0; i < host_map_store.count; i++) {
		if (host_map_store.va[i] == va) {
			host_map_store.pa[i] = pa;
			return;
		}
	}
	host_map_store.va[host_map_store.count] = va;
	host_map_store.pa[host_map_store.count] = pa;
	host_map_store.count++;
}

static void map_host_page(struct init_pt *pt, u64_t pt_pa, unsigned long va)
{
	struct page *page = vmalloc_to_page((void *)va);
	u64_t *pdpt, *pde, *pte;
	unsigned long pml4_i, pdpt_i, pde_i, pte_i;
	u64_t pa;

	if (!page)
		return;
	pa = page_to_phys(page);
	if (!pa)
		return;

	pml4_i = (va >> 39) & 0x1ffUL;
	pdpt_i = (va >> 30) & 0x1ffUL;
	pde_i = (va >> 21) & 0x1ffUL;
	pte_i = (va >> 12) & 0x1ffUL;

	pdpt = pt_child_table(pt, pt_pa, pt->pml4, pml4_i,
	    pt->host_pdpt, &pt->n_host_pdpt, HOST_PDPT_PAGES, PT_HOST_PDPT);
	if (!pdpt)
		return;
	pde = pt_child_table(pt, pt_pa, pdpt, pdpt_i,
	    pt->host_pde, &pt->n_host_pde, HOST_PDE_PAGES, PT_HOST_PDE);
	if (!pde)
		return;
	pte = pt_child_table(pt, pt_pa, pde, pde_i,
	    pt->host_pte, &pt->n_host_pte, HOST_PTE_PAGES, PT_HOST_PTE);
	if (!pte)
		return;
	pte[pte_i] = pa | PT_E_PRESENT | PT_E_WRITABLE;
	record_host_map(va, pa);
}

static void map_host_range(struct init_pt *pt, u64_t pt_pa,
    unsigned long start, unsigned long end)
{
	unsigned long va;

	for (va = start & PAGE_MASK; va < end; va += PAGE_SIZE)
		map_host_page(pt, pt_pa, va);
}

static void map_host_code(struct init_pt *pt, u64_t pt_pa,
    u64_t xnu_stack_va, u64_t stack_size)
{
	unsigned long sp = (unsigned long)&sp;
	unsigned long xnu = (unsigned long)xnu_world_call_x86;
	unsigned long fki = (unsigned long)fki_x86_enter;
	unsigned long worlds_va = (unsigned long)worlds;
	unsigned long cr3_va = (unsigned long)&xnu_host_cr3_x86;
	unsigned long phase_va = (unsigned long)&fki_x86_phase;
	unsigned long phase_arg_va = (unsigned long)&fki_x86_phase_arg;

	map_host_range(pt, pt_pa, xnu - 4 * PAGE_SIZE, xnu + 4 * PAGE_SIZE);
	map_host_range(pt, pt_pa, fki - 8 * PAGE_SIZE, fki + 8 * PAGE_SIZE);
	map_host_range(pt, pt_pa, worlds_va - PAGE_SIZE,
	    worlds_va + 4 * PAGE_SIZE);
	map_host_range(pt, pt_pa, cr3_va - PAGE_SIZE, cr3_va + PAGE_SIZE);
	/* The trampoline and load_state_asm store breadcrumbs after CR3. */
	map_host_range(pt, pt_pa, phase_va - PAGE_SIZE, phase_va + PAGE_SIZE);
	map_host_range(pt, pt_pa, phase_arg_va - PAGE_SIZE,
	    phase_arg_va + PAGE_SIZE);
	map_host_range(pt, pt_pa, sp - 16 * PAGE_SIZE, sp + 16 * PAGE_SIZE);
	map_host_range(pt, pt_pa, xnu_stack_va, xnu_stack_va + stack_size);
	debugf("pt: host sp=0x%lx xnu=0x%lx fki=0x%lx worlds=0x%lx "
	    "cr3var=0x%lx stack=0x%llx pdpt/pde/pte=%u/%u/%u map=%u\n",
	    sp, xnu, fki, worlds_va, cr3_va,
	    (unsigned long long)xnu_stack_va, pt->n_host_pdpt,
	    pt->n_host_pde, pt->n_host_pte, host_map_store.count);
}

static int build_init_pt(struct init_pt *pt, u64_t kc_pa, u64_t vmlow,
    u64_t vmhigh, u64_t map_top, u64_t xnu_stack_va, u64_t stack_size,
    u64_t *out_phys)
{
	u64_t alias_top = map_top;
	const unsigned long id_n_pde = DIV_ROUND_UP(map_top, 2UL << 20);
	const unsigned long k_pages = DIV_ROUND_UP(vmhigh - vmlow, PAGE_SIZE);
	const unsigned long k_first_pt = (vmlow >> PAGE_SHIFT) & 0x1ffUL;
	/*
	 * XNU expects the booter to have installed the "static map"
	 * 0xffffff8000000000 + pa -> pa for all low physical memory: its
	 * ml_static_ptovirt() uses that alias for boot_args, the EFI map, the
	 * device tree and the page tables allocated from physfree.  One PTE
	 * page covers 2 MiB.  The DT can be allocated above ram_mb, so extend
	 * the alias only as far as its last byte.
	 */
	if (mem.dt.pa && mem.dt.size)
		alias_top = max_t(u64_t, alias_top,
		    ALIGN(mem.dt.pa + mem.dt.size, 2UL << 20));
	const unsigned long k_n_pte = DIV_ROUND_UP(alias_top, 2UL << 20);
	unsigned long pt_size, i, n;
	u64_t pa;

	memset(pt, 0, sizeof(*pt));
	pt_size = (PT_PTE_K + k_n_pte) * PAGE_SIZE;
	if (fki_x86_shared_alloc(pt_size, &pt->region))
		return -ENOMEM;
	pt->pte_k = kzalloc(k_n_pte * sizeof(*pt->pte_k), GFP_KERNEL);
	if (!pt->pte_k) {
		fki_x86_shared_free(&pt->region);
		return -ENOMEM;
	}
	pt->n_pte_pages = k_n_pte;

	pa = pt->region.pa;
	pt->pml4 = (u64_t *)pt->region.va;
	pt->pdpt_id = (u64_t *)pt_page_va(pt, pa, PT_PDPT_ID);
	pt->pde_id = (u64_t *)pt_page_va(pt, pa, PT_PDE_ID);
	pt->pdpt_k = (u64_t *)pt_page_va(pt, pa, PT_PDPT_K);
	pt->pde_k = (u64_t *)pt_page_va(pt, pa, PT_PDE_K);
	for (i = 0; i < k_n_pte; i++)
		pt->pte_k[i] =
		    (u64_t *)pt_page_va(pt, pa, PT_PTE_K + i);

	memset(pt->region.va, 0, pt_size);

	/* identity map of low physical memory (2 MiB pages) */
	pt->pml4[0] = pt_page_pa(pa, PT_PDPT_ID) | PT_E_PRESENT | PT_E_WRITABLE;
	pt->pdpt_id[0] = pt_page_pa(pa, PT_PDE_ID) | PT_E_PRESENT | PT_E_WRITABLE;
	for (i = 0; i < id_n_pde; i++)
		pt->pde_id[i] = (i << 21) | PT_E_PRESENT | PT_E_WRITABLE | PT_E_LARGE;

	/* XNU kernel half -> copied KC physical pages (4 KiB pages) */
	pt->pml4[PML4_KERNEL_INDEX] =
	    pt_page_pa(pa, PT_PDPT_K) | PT_E_PRESENT | PT_E_WRITABLE;
	pt->pdpt_k[0] = pt_page_pa(pa, PT_PDE_K) | PT_E_PRESENT | PT_E_WRITABLE;
	for (i = 0; i < k_n_pte; i++)
		pt->pde_k[i] =
		    pt_page_pa(pa, PT_PTE_K + i) | PT_E_PRESENT | PT_E_WRITABLE;

	/* ml_static_ptovirt() alias: kernel-half VA == low PA. */
	for (i = 0; i < (alias_top >> PAGE_SHIFT); i++) {
		unsigned long pd = i >> 9;
		unsigned long pt_i = i & 0x1ffUL;

		pt->pte_k[pd][pt_i] =
		    (i << PAGE_SHIFT) | PT_E_PRESENT | PT_E_WRITABLE;
	}

	/* Overlay the kernel image at its unslid link address (vmlow). */
	for (n = 0; n < k_pages; n++) {
		unsigned long idx = k_first_pt + n;
		unsigned long pd = idx >> 9;
		unsigned long pt_i = idx & 0x1ffUL;

		if (pd >= k_n_pte)
			break;
		pt->pte_k[pd][pt_i] =
		    (kc_pa + n * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	}

	/* Host pages that are live on both sides of the CR3 switch. */
	map_host_code(pt, pa, xnu_stack_va, stack_size);

	*out_phys = pa;
	return 0;
}

static void free_init_pt(struct init_pt *pt)
{
	kfree(pt->pte_k);
	if (pt->region.va)
		fki_x86_shared_free(&pt->region);
}

/*
 * Allocate a small region whose physical address is at or above @pa_min.
 * alloc_contig_pages() cannot choose a PFN range, so reserve enough pages to
 * span @pa_min and carve the usable part out of them. The caller keeps both
 * regions alive until the hosted run ends.
 */
static int shared_alloc_above(u64_t pa_min, unsigned long size,
    struct fki_region *out, struct fki_region *backing)
{
	u64_t off = 0;

	if (fki_x86_shared_alloc(size + (unsigned long)pa_min, backing))
		return -ENOMEM;
	if (backing->pa < pa_min)
		off = ALIGN(pa_min - backing->pa, PAGE_SIZE);
	if (off + size > backing->size) {
		fki_x86_shared_free(backing);
		return -ENOMEM;
	}
	out->va = (void *)((char *)backing->va + off);
	out->pa = backing->pa + off;
	out->size = size;
	out->handle = NULL; /* backing owns the pages */
	return 0;
}

/* ---- EFI memory map ------------------------------------------------------- */

#define MAX_EFI_RANGES 32
static struct x86_efi_memory_range efi_map[MAX_EFI_RANGES];
static struct fki_region efi_region;
static unsigned int efi_count;

static int build_efi_map(u64_t total, u64_t pa_min)
{
	static const u32_t types[] = {
		X86_EFI_RESERVED_MEMORY_TYPE,
		X86_EFI_LOADER_CODE,
		X86_EFI_BOOT_SERVICES_CODE,
		X86_EFI_CONVENTIONAL_MEMORY,
		X86_EFI_ACPI_RECLAIM_MEMORY,
	};
	unsigned int i;
	u64_t cursor = 0, per = DIV_ROUND_UP(total, ARRAY_SIZE(types));

	if (shared_alloc_above(pa_min, sizeof(efi_map),
	    &efi_region, &efi_backing))
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		u64_t pages = DIV_ROUND_UP(per, PAGE_SIZE);

		efi_map[i].type = types[i];
		efi_map[i].physical_start = cursor;
		efi_map[i].virtual_start = XNU_VM_MIN_KERNEL | cursor;
		efi_map[i].number_of_pages = pages;
		efi_map[i].attribute = 0xf; /* EFI_MEMORY_UC|WC|WB|WT */
		cursor += pages << PAGE_SHIFT;
	}
	efi_count = ARRAY_SIZE(types);
	memcpy(efi_region.va, efi_map, sizeof(efi_map));
	return 0;
}

/*
 * Minimal flattened device tree for XNU's PE_init_platform().
 *
 * PE_init_platform() needs /chosen:random-seed for early_random_init():
 * PE_get_random_seed() panics with "no random seed" if the property is
 * missing. The 64-byte seed matches BOOTLOADER_ENTROPY_REQUEST_SIZE on
 * x86_64.
 *
 * Layout (pexpert/pexpert/device_tree.h):
 *   DeviceTreeNode { nProperties, nChildren }
 *   DeviceTreeNodeProperty { name[32], length, value[ROUND4(length)] }
 */
struct linux64_dt_name_prop {
	char name[32];
	u32_t length;
	unsigned char value[8]; /* "root\0" / "chosen\0", padded to 4 bytes */
};

struct linux64_dt_seed_prop {
	char name[32];
	u32_t length;
	unsigned char value[64];
};

struct linux64_dt_chosen {
	u32_t n_properties;
	u32_t n_children;
	struct linux64_dt_name_prop name;
	struct linux64_dt_seed_prop seed;
};

struct linux64_dt_root {
	u32_t n_properties;
	u32_t n_children;
	struct linux64_dt_name_prop name;
	struct linux64_dt_chosen chosen;
};

#define LINUX64_DT_SIZE sizeof(struct linux64_dt_root)

static int build_device_tree(u64_t pa_min)
{
	struct linux64_dt_root *dt;
	struct linux64_dt_chosen *chosen;

	if (shared_alloc_above(pa_min, LINUX64_DT_SIZE,
	    &mem.dt, &dt_backing))
		return -ENOMEM;
	dt = mem.dt.va;

	dt->n_properties = 1;
	dt->n_children = 1;
	memcpy(dt->name.name, "name", sizeof("name"));
	dt->name.length = sizeof("root"); /* includes the NUL */
	memcpy(dt->name.value, "root", sizeof("root"));

	chosen = &dt->chosen;
	chosen->n_properties = 2;
	chosen->n_children = 0;
	memcpy(chosen->name.name, "name", sizeof("name"));
	chosen->name.length = sizeof("chosen");
	memcpy(chosen->name.value, "chosen", sizeof("chosen"));
	memcpy(chosen->seed.name, "random-seed", sizeof("random-seed"));
	chosen->seed.length = sizeof(chosen->seed.value);
	get_random_bytes(chosen->seed.value, chosen->seed.length);

	return 0;
}

/* ---- XNU globals ---------------------------------------------------------- */

static void *xnu_va_to_host(u64_t va)
{
	if (va < img.vmlow || va >= img.vmlow + mem.kc.size)
		return NULL;
	return (void *)((char *)mem.kc.va + (va - img.vmlow));
}

static void *xnu_va_translate(unsigned long va)
{
	return xnu_va_to_host(va);
}

static u64_t x86_read_cr3(void)
{
	u64_t v;

	asm volatile("mov %%cr3, %0" : "=r" (v));
	return v;
}

/* Patch 13 bytes with movabs $target, %r11 ; jmp *%r11. The image mapping and
 * module text are far apart, so a rel32 jmp cannot reach the probe. %r11 is
 * used because every patched site already treats it as caller-saved scratch:
 * at vstart+0x9c4 the loop keeps dblmap_dist in %rax, so a %rax-based jump
 * would destroy the very value the IDT remap is about to use. */
static void write_abs_jmp(void *dst, unsigned long target)
{
	u8 jmp[13] = {
	    0x49, 0xbb,
	    (u8)(target & 0xff),
	    (u8)((target >> 8) & 0xff),
	    (u8)((target >> 16) & 0xff),
	    (u8)((target >> 24) & 0xff),
	    (u8)((target >> 32) & 0xff),
	    (u8)((target >> 40) & 0xff),
	    (u8)((target >> 48) & 0xff),
	    (u8)((target >> 56) & 0xff),
	    0x41, 0xff, 0xe3,
	};

	memcpy(dst, jmp, sizeof(jmp));
}

/*
 * XNU calls linux64_hook with an indirect call. The hook points to module text
 * that is mapped in both page tables; its first instruction switches CR3 back
 * to Linux before the rest of the upcall restores the remaining world state.
 */
static int write_xnu_globals(void)
{
	u64_t hook = xnu_upcall_va, slide = xnu_slide;
	void *hook_va, *slide_va;

	if (!a_hook)
		return allow_unhosted ? 0 : -EINVAL;
	hook_va = xnu_va_to_host(a_hook);
	if (!hook_va) {
		pr_err("xnu86: _linux64_hook 0x%llx outside kernel image\n",
		    (unsigned long long)a_hook);
		return -EINVAL;
	}
	memcpy(hook_va, &hook, sizeof(hook));
	if (a_slide) {
		slide_va = xnu_va_to_host(a_slide);
		if (!slide_va) {
			pr_warn("xnu86: _linux64_slide 0x%llx outside kernel image\n",
			    (unsigned long long)a_slide);
		} else {
			memcpy(slide_va, &slide, sizeof(slide));
		}
	} else {
		pr_warn("xnu86: _linux64_slide not found\n");
	}
	pr_info("xnu86: _linux64_hook=0x%llx -> upcall=0x%llx\n",
	    (unsigned long long)a_hook, (unsigned long long)hook);
	{
		u64_t a_serial = macho_x86_sym(&img, "_pal_serial_init");
		void *serial_va = a_serial ? xnu_va_to_host(a_serial) : NULL;
		u64_t a_kprintf = macho_x86_sym(&img, "_kprintf");
		void *kprintf_va = a_kprintf ? xnu_va_to_host(a_kprintf) : NULL;

		if (serial_va)
			*(u8 *)serial_va = 0xc3; /* ret: no physical COM1 on host */
		if (kprintf_va) {
			if (kprintf_probe) {
				write_abs_jmp(kprintf_va,
				    (unsigned long)(uintptr_t)
				    xnu_world_kprintf_probe_x86);
				pr_info("xnu86: kprintf probe at 0x%llx -> %p\n",
				    (unsigned long long)a_kprintf,
				    xnu_world_kprintf_probe_x86);
			} else {
				*(u8 *)kprintf_va = 0xc3; /* ret: avoid COM1 UART */
			}
		}

		if (kprintf_probe) {
			/* vstart+0x9c4: just after doublemap_init() returns. */
			u64_t a_vstart = macho_x86_sym(&img, "_vstart");
			u8 *code = a_vstart ? xnu_va_to_host(a_vstart + 0x9c4)
			    : NULL;
			static const u8 expect[12] = {
			    0x4c, 0x8d, 0x0d, 0x35, 0xc8, 0xc1,
			    0xff, 0x0f, 0x1f, 0x44, 0x00, 0x00,
			};

			if (code && !memcmp(code, expect, sizeof(expect))) {
				write_abs_jmp(code, (unsigned long)(uintptr_t)
				    xnu_world_site1_probe_x86);
				debugf("xnu86: site1 probe at 0x%llx\n",
				    (unsigned long long)(a_vstart + 0x9c4));
			} else {
				pr_warn("xnu86: site1 pattern not found\n");
			}
		}

		if (kprintf_probe) {
			u64_t a_vstart = macho_x86_sym(&img, "_vstart");

			if (!a_vstart) {
				pr_warn("xnu86: _vstart not found for site3/4\n");
			} else {
				/* vstart+0xf35: loop exit, before set_cr3(). */
				static const u8 expect3[13] = {
				    0x48, 0x89, 0x1d, 0xdc, 0xe3, 0x99, 0x00,
				    0x8b, 0x05, 0x1e, 0xf6, 0xb7, 0x00,
				};
				u8 *code = xnu_va_to_host(a_vstart + 0xf35);

				if (code && !memcmp(code, expect3, sizeof(expect3))) {
					write_abs_jmp(code,
					    (unsigned long)(uintptr_t)
					    xnu_world_site3_probe_x86);
					debugf("xnu86: site3 probe at 0x%llx\n",
					    (unsigned long long)(a_vstart + 0xf35));
				} else {
					pr_warn("xnu86: site3 pattern not found\n");
				}

				/* Breadcrumbs through the rest of vstart(). Each entry
				 * is one 13-byte window at an instruction boundary. */
				{
					static const struct {
						u64_t off;
						const u8 expect[13];
						void (*probe)(void);
					} sites[] = {
					    { 0xf97, {
						0x8b, 0x05, 0xdb, 0xf5, 0xb7, 0x00,
						0x48, 0x8d, 0x0d, 0x84, 0xf6, 0xb7, 0x00 },
						    xnu_world_site5_probe_x86 },
					    { 0xfa4, {
						0x48, 0x89, 0x01, 0xbf, 0x01, 0x00,
						0x00, 0x00, 0xe8, 0xaf, 0x3e, 0xff, 0xff },
						    xnu_world_site6_probe_x86 },
					    { 0xfbb, {
						0xe8, 0xc0, 0x32, 0xff, 0xff, 0x48,
						0x8b, 0x3b, 0xe8, 0x28, 0x3d, 0xff, 0xff },
						    xnu_world_site7_probe_x86 },
					    { 0xfc3, {
						0xe8, 0x28, 0x3d, 0xff, 0xff, 0x48,
						0x8b, 0x3b, 0xe8, 0xa0, 0x3d, 0xff, 0xff },
						    xnu_world_site8_probe_x86 },
					    { 0xea9, {
						0x48, 0x8b, 0x04, 0xd8, 0x48, 0x8b,
						0x70, 0x40, 0xe8, 0xfa, 0x9e, 0x00, 0x00 },
						    xnu_world_site9_probe_x86 },
					};
					unsigned int s;

					for (s = 0; s < ARRAY_SIZE(sites); s++) {
						u8 *at = xnu_va_to_host(a_vstart + sites[s].off);

						if (at && !memcmp(at, sites[s].expect,
						    sizeof(sites[s].expect))) {
							write_abs_jmp(at,
							    (unsigned long)(uintptr_t)
							    sites[s].probe);
							debugf("xnu86: trace site %u at 0x%llx\n",
							    s + 5,
							    (unsigned long long)
							    (a_vstart + sites[s].off));
						} else {
							pr_warn("xnu86: trace site %u pattern not found\n",
							    s + 5);
						}
					}
				}

				/* Inside cpc_early_init(), which is the first call after
				 * the KERNEL_TABLES upcall. */
				{
					u64_t a_cpc = macho_x86_sym(&img,
					    "_cpc_early_init");

					static const struct {
						u64_t off;
						const u8 expect[13];
						void (*probe)(void);
					} cpc_sites[] = {
					    { 0x16, {
						0x85, 0xc0, 0x0f, 0x85, 0xc9, 0x00,
						0x00, 0x00, 0xe8, 0x4d, 0x27, 0xfd, 0xff },
						    xnu_world_cpc1_probe_x86 },
					    { 0x23, {
						0x80, 0xb8, 0x38, 0x01, 0x00, 0x00,
						0x02, 0x0f, 0x82, 0xb7, 0x00, 0x00, 0x00 },
						    xnu_world_cpc2_probe_x86 },
					};
					unsigned int s;

					if (!a_cpc)
						pr_warn("xnu86: _cpc_early_init not found\n");
					for (s = 0; a_cpc && s < ARRAY_SIZE(cpc_sites); s++) {
						u8 *at = xnu_va_to_host(a_cpc +
						    cpc_sites[s].off);

						if (at && !memcmp(at, cpc_sites[s].expect,
						    sizeof(cpc_sites[s].expect))) {
							write_abs_jmp(at,
							    (unsigned long)(uintptr_t)
							    cpc_sites[s].probe);
							debugf("xnu86: cpc site %u at 0x%llx\n",
							    s + 10,
							    (unsigned long long)
							    (a_cpc + cpc_sites[s].off));
						} else {
							pr_warn("xnu86: cpc site %u pattern not found\n",
							    s + 10);
						}
					}
				}

				/* cpuid_info() slow path. */
				{
					u64_t a_ci = macho_x86_sym(&img,
					    "_cpuid_info");
					static const u8 expect[13] = {
					    0xba, 0x04, 0x00, 0x00, 0x00,
					    0xe8, 0xd7, 0x77, 0x66, 0x00,
					    0xe8, 0x12, 0x00,
					};
					u8 *at = a_ci ? xnu_va_to_host(a_ci + 0x1f)
					    : NULL;

					if (at && !memcmp(at, expect,
					    sizeof(expect))) {
						write_abs_jmp(at,
						    (unsigned long)(uintptr_t)
						    xnu_world_cpuid_probe_x86);
						debugf("xnu86: cpuid probe at 0x%llx\n",
						    (unsigned long long)
						    (a_ci + 0x1f));
					} else {
						pr_warn("xnu86: cpuid probe pattern not found\n");
					}
				}

				/* Do not install site4 in this experiment. site3 now
				 * performs the CR3 switch itself and leaves a phase
				 * breadcrumb; a post-switch upcall would change the
				 * very failure we are trying to localize. */
			}
		}

		/*
		 * The host CPU is a 13th-gen Core (family 6, model 0xb7) that
		 * this XNU tree does not list.  cpuid_set_cpufamily() therefore
		 * leaves CPUFAMILY_UNKNOWN (0) and cpuid_set_info() panics with
		 * "Unsupported CPU".  Make the hosted kernel treat it as an
		 * Icelake-family CPU: force the family store and clear the
		 * "unknown" flag, retargeting the default-path jumps that used
		 * to converge on the original store.
		 */
		{
			u64_t a_cpuid = macho_x86_sym(&img, "_cpuid_set_info");
			u64_t a_info = macho_x86_sym(&img, "_cpuid_cpu_info");
			u8 *code = a_cpuid ? xnu_va_to_host(a_cpuid) : NULL;
			unsigned int i;
			unsigned int store_off = 0;
			u64_t store_addr = 0, case_addr = 0;

			for (i = 0; code && a_info && i < 0x2000 - 8; i++) {
				u32 rel;
				u64_t target;

				if (code[i] != 0x31 || code[i + 1] != 0xdb ||
				    code[i + 2] != 0x89 || code[i + 3] != 0x15)
					continue;
				rel = *(u32 *)(&code[i + 4]);
				target = a_cpuid + i + 8 + (s64)(s32)rel;
				if (target == a_info + 0x1a0) {
					store_off = i;
					store_addr = target;
					case_addr = a_cpuid + i - 5;
					break;
				}
			}
			if (store_off >= 5) {
				u32 disp = (u32)(store_addr -
				    (a_cpuid + store_off + 6));
				u8 patch[13] = {
				    0xba, 0x47, 0x55, 0x43, 0x38,
				    0x89, 0x15,
				    (u8)(disp & 0xff),
				    (u8)((disp >> 8) & 0xff),
				    (u8)((disp >> 16) & 0xff),
				    (u8)((disp >> 24) & 0xff),
				    0x31, 0xdb,
				};
				unsigned int j;

				memcpy(&code[store_off - 5], patch,
				    sizeof(patch));
				for (j = 0; j < 0x2000 - 2; j++) {
					s8 rel8;

					if (code[j] < 0x70 || code[j] > 0x7f)
						continue;
					rel8 = (s8)code[j + 1];
					if (a_cpuid + j + 2 + rel8 !=
					    a_cpuid + store_off + 2)
						continue;
					rel8 = (s8)(case_addr -
					    (a_cpuid + j + 2));
					code[j + 1] = (u8)rel8;
				}
				debugf("xnu86: map unsupported CPU to "
				    "CPUFAMILY_INTEL_ICELAKE at 0x%llx\n",
				    (unsigned long long)(a_cpuid + store_off));
			}
		}

		/*
		 * early_random() pulls in the whole HMAC-SHA2 DRBG plus the
		 * RDSEED/RDRAND sampling loop.  Under hosted boot that path
		 * wedges inside Idle_PTs_init() before its breadcrumb is
		 * reached.  Only the one-shot physmap/doublemap slides use the
		 * value here, so make it deterministic and skip the entropy
		 * machinery entirely.  Firmware is left untouched.
		 */
		{
			u64_t a_rand = macho_x86_sym(&img, "_early_random");
			u8 *code = a_rand ? xnu_va_to_host(a_rand) : NULL;
			u8 ret_zero[11] = {
			    0x48, 0xb8,
			    0, 0, 0, 0, 0, 0, 0, 0,
			    0xc3,
			};

			if (code) {
				memcpy(code, ret_zero, sizeof(ret_zero));
				debugf("xnu86: early_random returns fixed 0 at 0x%llx\n",
				    (unsigned long long)a_rand);
			}
		}
	}
	return 0;
}

/* ---- world setup ---------------------------------------------------------- */

static int setup_world(struct xnu_world_x86 *w, int cpu, u64_t cr3_phys,
    u64_t stack_top, u64_t xsave_va, u64_t xsave_size,
    u64_t linux_xsave_va, u64_t linux_xsave_size)
{
	struct fki_x86_state *s = &w->xnu_state;
	struct fki_x86_state *ls = &w->linux_state;

	memset(w, 0, sizeof(*w));
	s->cr3 = cr3_phys;
	s->idt_base = 0; /* XNU installs its own via vstart_idt_init() */
	s->idt_limit = 0;
	s->gdt_base = 0; /* XNU installs its own via cpu_desc_load() */
	s->gdt_limit = 0;
	s->tr = 0;
	s->gs_base = 0;
	s->kernel_gs_base = 0;
	s->star = s->lstar = s->cstar = s->sfmask = 0;
	s->xsave_va = xsave_va;
	s->xsave_size = xsave_size;
	/* Without this, Linux's FPU state is never saved/restored across upcalls. */
	ls->xsave_va = linux_xsave_va;
	ls->xsave_size = linux_xsave_size;
	w->xnu_stack_top = stack_top;
	w->xnu_rsp = stack_top;
	return 0;
}

/* ---- run ------------------------------------------------------------------ */

static int run_xnu(u64_t kc_pa, u64_t vmlow, u64_t vmhigh, u64_t kc_size,
    u64_t ba_phys)
{
	struct init_pt pt;
	u64_t image_size = vmhigh - vmlow;
	const u64_t stack_size = 16 << 10;
	u64_t stack_va, stack_top, xsave_va, linux_xsave_va, cr3_phys;
	u64_t apic;
	void *stack_host;
	long r;
	int cpu, err, i;

	/*
	 * The XNU stack is vmalloc-backed: it is mapped into the XNU page
	 * tables for the downcall, and Linux already maps it when an upcall
	 * pushes registers before restoring the rest of the world.
	 */
	stack_host = vmalloc(stack_size);
	if (!stack_host) {
		pr_err("xnu86: cannot allocate XNU stack\n");
		return -ENOMEM;
	}
	memset(stack_host, 0, stack_size);
	stack_va = (u64_t)(uintptr_t)stack_host;
	stack_top = stack_va + stack_size - 16;

	xnu_stack_host = stack_host;
	xnu_stack_top = stack_top;
	xnu_stack_thread = kthread_run(xnu_stack_watchdog, NULL, "xnu-stack");
	if (IS_ERR(xnu_stack_thread))
		xnu_stack_thread = NULL;

	/* Module text is mapped in both worlds, so it is a safe hook gate. */
	xnu_upcall_va = (u64_t)(uintptr_t)xnu_world_hook_trampoline_x86;
	xnu_host_cr3_x86 = x86_read_cr3();

	memset((char *)mem.kc.va + image_size, 0, kc_size - image_size);

	host_map_store.count = 0;
	/* Preserve the static aliases of our boot structures across CR3 switches. */
	record_host_map(XNU_VM_MIN_KERNEL | mem.ba.pa, mem.ba.pa);
	record_host_map(XNU_VM_MIN_KERNEL | mem.dt.pa, mem.dt.pa);
	record_host_map(XNU_VM_MIN_KERNEL | efi_region.pa, efi_region.pa);
	debugf("loader: static alias ba=0x%llx dt=0x%llx efi=0x%llx\n",
	    (unsigned long long)(XNU_VM_MIN_KERNEL | mem.ba.pa),
	    (unsigned long long)(XNU_VM_MIN_KERNEL | mem.dt.pa),
	    (unsigned long long)(XNU_VM_MIN_KERNEL | efi_region.pa));
	err = build_init_pt(&pt, kc_pa, vmlow, vmlow + kc_size, ram_mb << 20,
	    stack_va, stack_size, &cr3_phys);
	if (err) {
		pr_err("xnu86: cannot build initial page tables (%d)\n", err);
		if (xnu_stack_thread) {
			kthread_stop(xnu_stack_thread);
			xnu_stack_thread = NULL;
		}
		xnu_stack_host = NULL;
		xnu_stack_top = 0;
		vfree(stack_host);
		return err;
	}
	/* Idle_PTs_init() rebuilds page tables; hand it the host mappings. */
	((struct x86_boot_args *)mem.ba.va)->host_map = host_map_store;

	/*
	 * Both XSAVE buffers arrive zeroed. Restoring a zeroed legacy field sets
	 * MXCSR=0 and an unmasked x87 control word while XNU has no IDT yet, so
	 * seed them with a real FPU/SIMD state now. The Linux-side buffer also
	 * lets fki_x86_leave() restore our own FPU state after each upcall.
	 */
	xsave_va = 0;
	linux_xsave_va = 0;
	if (!fki_x86_shared_alloc(4096, &mem.xsave)) {
		xsave_va = (u64_t)mem.xsave.va;
		fki_x86_fpstate_save(mem.xsave.va);
	}
	if (!fki_x86_shared_alloc(4096, &mem.linux_xsave)) {
		linux_xsave_va = (u64_t)mem.linux_xsave.va;
		fki_x86_fpstate_save(mem.linux_xsave.va);
	}

	cpu = smp_processor_id();
	fki_bind_cpu(cpu); /* keep the whole downcall on one CPU */

	nr_cpus = min(nr_cpus, MAX_XNU_CPUS);
	nr_cpus = min(nr_cpus, fki_x86_num_cpus());
	for (i = 0; i < nr_cpus; i++)
		setup_world(&worlds[i], i, cr3_phys, stack_top, xsave_va,
		    mem.xsave.size, linux_xsave_va, mem.linux_xsave.size);

	/* Upcalls rediscover the world from CPUID, not from "CPU 0". */
	if (fki_x86_cpu_apic_id(cpu, &apic)) {
		pr_err("xnu86: cannot resolve APIC ID for CPU %d\n", cpu);
		err = -EINVAL;
		goto out_xsave;
	}
	xnu_worlds_x86[LINUX64_X86_CPU_SLOT(apic)] = &worlds[0];

	err = write_xnu_globals();
	if (err)
		goto out_xsave;
	debugf("run: globals ok hook=0x%llx xsave=0x%llx/0x%llx\n",
	    (unsigned long long)xnu_upcall_va,
	    (unsigned long long)xsave_va, (unsigned long long)linux_xsave_va);

	pr_info("xnu86: stack=0x%llx trampoline=0x%llx cr3=0x%llx\n",
	    (unsigned long long)stack_top,
	    (unsigned long long)xnu_upcall_va,
	    (unsigned long long)cr3_phys);
	debugf("run: cr3=0x%llx stack=0x%llx tramp=0x%llx ba=0x%llx\n",
	    (unsigned long long)cr3_phys, (unsigned long long)stack_top,
	    (unsigned long long)xnu_upcall_va, (unsigned long long)ba_phys);
	logs("xnu86: entering vstart\n");
	WRITE_ONCE(xnu_stack_dump_seq, 0);
	WRITE_ONCE(xnu_run_active, 1);
	r = xnu_world_call_x86(&worlds[0], a_vstart, ba_phys);
	WRITE_ONCE(xnu_run_active, 0);
	pr_info("xnu86: vstart returned %ld\n", r);
	debugf("run: vstart returned %ld\n", r);

out_xsave:
	if (xnu_stack_thread) {
		kthread_stop(xnu_stack_thread);
		xnu_stack_thread = NULL;
	}
	xnu_stack_host = NULL;
	xnu_stack_top = 0;
	fki_x86_shared_free(&mem.linux_xsave);
	fki_x86_shared_free(&mem.xsave);
	free_init_pt(&pt);
	vfree(stack_host);
	return err;
}

/* ---- init / exit ---------------------------------------------------------- */

static int __init xnu_x86_init(void)
{
	const void *data = NULL;
	unsigned long size = 0;
	void *blob = NULL;
	u64_t total, map_top, pa_min;
	u64_t ba_phys;
	struct x86_boot_args *ba;
	int err;

	if (!a_vstart) {
		/* resolved after the image is loaded below */
	}
	err = fki_blob_get(image, &data, &size, &blob);
	if (err) {
		pr_err("xnu86: cannot load %s from firmware (%d)\n", image, err);
		return err;
	}
	err = macho_x86_load(&img, data, size, ~0UL);
	if (err) {
		pr_err("xnu86: Mach-O parse failed (%d)\n", err);
		fki_blob_put(blob);
		return err;
	}
	if (!img.symtab ||
	    (img.filetype != MH_FILESET && img.filetype != MH_EXECUTE)) {
		pr_err("xnu86: not a supported Mach-O kernel (type %u)\n",
		    img.filetype);
		fki_blob_put(blob);
		return -EINVAL;
	}

	a_vstart = macho_x86_sym(&img, "_vstart");
	a_hook = macho_x86_sym(&img, "_linux64_hook");
	a_slide = macho_x86_sym(&img, "_linux64_slide");
	a_cpu_entry = macho_x86_sym(&img, "_linux64_cpu_entry");
	a_vint = macho_x86_sym(&img, "_linux64_vint");
	a_ipi = macho_x86_sym(&img, "_linux64_ipi_pending");
	pr_info("xnu86: _vstart=0x%llx _hook=0x%llx _slide=0x%llx\n",
	    a_vstart, a_hook, a_slide);
	debugf("loader: vstart=0x%llx hook=0x%llx slide=0x%llx\n",
	    (unsigned long long)a_vstart, (unsigned long long)a_hook,
	    (unsigned long long)a_slide);
	if (!a_vstart || (!a_hook && !allow_unhosted)) {
		pr_err("xnu86: required symbols missing (hook=%llx, allow_unhosted=%d)\n",
		    a_hook, allow_unhosted);
		fki_blob_put(blob);
		return -ENOENT;
	}

	total = (u64_t)ram_mb << 20;
	map_top = ALIGN(total, 2UL << 20);
	/*
	 * build_init_pt() maps the kernel image at its unslid link VAs, which
	 * overwrites every ml_static_ptovirt() PTE for PA < the image's link
	 * range. boot_args, the EFI map and the DT must therefore sit above it.
	 */
	pa_min = ALIGN(img.vmhigh - XNU_VM_MIN_KERNEL + PAGE_SIZE, PAGE_SIZE);
	debugf("loader: boot structures must be above pa=0x%llx\n",
	    (unsigned long long)pa_min);
	err = fki_x86_shared_alloc(img.vmhigh - img.vmlow + (8 << 20), &mem.kc);
	if (err) {
		pr_err("xnu86: cannot allocate kernel memory (%d)\n", err);
		fki_blob_put(blob);
		return err;
	}
	/* The initial page tables map the kernel at its unslid link addresses. */
	img.slide = 0;
	err = macho_x86_copy(&img, mem.kc.va);
	if (err) {
		pr_err("xnu86: image copy failed (%d)\n", err);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return err;
	}
	kc_phys = mem.kc.pa;
	kc_virt = (u64_t)mem.kc.va;
	xnu_slide = 0;
	debugf("loader: kc pa=0x%llx va=%p size=0x%llx\n",
	    (unsigned long long)kc_phys, mem.kc.va,
	    (unsigned long long)(img.vmhigh - img.vmlow));
	xnu_hook_set_translate(xnu_va_translate);

	ba = kzalloc(sizeof(*ba), GFP_KERNEL);
	if (!ba) {
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return -ENOMEM;
	}
	cmdline_copy = kstrdup(boot_line, GFP_KERNEL);
	if (shared_alloc_above(pa_min, sizeof(*ba) + PAGE_SIZE,
	    &mem.ba, &ba_backing)) {
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return -ENOMEM;
	}
	err = build_efi_map(total, pa_min);
	if (err) {
		pr_err("xnu86: cannot build EFI map (%d)\n", err);
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&mem.ba);
		fki_x86_shared_free(&ba_backing);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return err;
	}
	err = build_device_tree(pa_min);
	if (err) {
		pr_err("xnu86: cannot build device tree (%d)\n", err);
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&efi_region);
		fki_x86_shared_free(&efi_backing);
		fki_x86_shared_free(&mem.ba);
		fki_x86_shared_free(&ba_backing);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return err;
	}
	if ((u64_t)(u32_t)mem.dt.pa != mem.dt.pa) {
		pr_err("xnu86: device tree PA 0x%llx is not 32-bit\n",
		    (unsigned long long)mem.dt.pa);
		err = -EFBIG;
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&mem.dt);
		fki_x86_shared_free(&dt_backing);
		fki_x86_shared_free(&efi_region);
		fki_x86_shared_free(&efi_backing);
		fki_x86_shared_free(&mem.ba);
		fki_x86_shared_free(&ba_backing);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return err;
	}

	/*
	 * KC_hdrs_vaddr is passed through ml_static_ptovirt() by vstart(). The
	 * static alias of the kernel's link offset is exactly the unslid VA that
	 * build_init_pt() overlays with our copied image, so use the link offset
	 * rather than the arbitrary physical address of the allocation.
	 */
	/*
	 * Idle_PTs_init() remaps the image starting at &_mh_execute_header
	 * (__TEXT), not at the lowest KC segment (__HIB).  Report kaddr/ksize
	 * relative to __TEXT so its KPTphys overlay lands on the same bytes we
	 * copied.  kaddr + ksize stays kc_phys + image size, so physfree and
	 * kdata_end are unaffected.
	 */
	{
		u64_t text_delta = img.text_vmaddr - img.vmlow;

		if (!text_delta || text_delta >= img.vmhigh - img.vmlow) {
			pr_err("xnu86: bad __TEXT offset 0x%llx\n",
			    (unsigned long long)text_delta);
			err = -EINVAL;
		} else {
			err = linux64_boot_args_init(ba, kc_phys + text_delta,
			    (u32_t)(img.vmhigh - img.text_vmaddr),
			    img.text_vmaddr - XNU_VM_MIN_KERNEL, cmdline_copy,
			    (u32_t)efi_region.pa, efi_map, efi_count,
			    sizeof(efi_map[0]), total, 0, 0);
			debugf("loader: kaddr=0x%llx ksize=0x%llx (delta=0x%llx)\n",
			    (unsigned long long)(kc_phys + text_delta),
			    (unsigned long long)(img.vmhigh - img.text_vmaddr),
			    (unsigned long long)text_delta);
		}
	}
	if (err) {
		pr_err("xnu86: boot_args init failed (%d)\n", err);
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&mem.dt);
		fki_x86_shared_free(&dt_backing);
		fki_x86_shared_free(&efi_region);
		fki_x86_shared_free(&efi_backing);
		fki_x86_shared_free(&mem.ba);
		fki_x86_shared_free(&ba_backing);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return err;
	}
	debugf("loader: boot_args dt before=0x%x/%u\n",
	    ba->device_tree_p, ba->device_tree_length);
	ba->device_tree_p = (u32_t)mem.dt.pa;
	ba->device_tree_length = LINUX64_DT_SIZE;
	debugf("loader: dt pa=0x%llx size=%u\n",
	    (unsigned long long)mem.dt.pa, (unsigned)LINUX64_DT_SIZE);
	memcpy(mem.ba.va, ba, sizeof(*ba));
	ba_phys = mem.ba.pa;
	debugf("loader: boot_args pa=0x%llx efi pa=0x%llx\n",
	    (unsigned long long)ba_phys, (unsigned long long)efi_region.pa);

	run_xnu(kc_phys, img.vmlow, img.vmhigh, mem.kc.size, ba_phys);

	kfree(cmdline_copy);
	kfree(ba);
	fki_x86_shared_free(&mem.dt);
	fki_x86_shared_free(&dt_backing);
	fki_x86_shared_free(&efi_region);
	fki_x86_shared_free(&efi_backing);
	fki_x86_shared_free(&mem.ba);
	fki_x86_shared_free(&ba_backing);
	fki_x86_shared_free(&mem.kc);
	fki_blob_put(blob);
	return 0;
}

static void __exit xnu_x86_exit(void)
{
	stop_run = 1;
	xnu_hook_set_translate(NULL);
}

module_init(xnu_x86_init);
module_exit(xnu_x86_exit);
