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
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/smp.h>

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

#define MAX_XNU_CPUS 16
static struct xnu_world_x86 worlds[MAX_XNU_CPUS];
static struct xnu_region {
	struct fki_region kc;
	struct fki_region ba;
	struct fki_region pt;
	struct fki_region stack;
	struct fki_region xsave;
	struct fki_region tramp;
} mem;

static struct xnu_image_x86 img;
static u64_t a_vstart, a_hook, a_slide, a_cpu_entry, a_vint, a_ipi;
static u64_t kc_phys, kc_virt, xnu_slide;
static char *cmdline_copy;
static int stop_run;
module_param(stop_run, int, 0644);

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
 * Host kernel text is not mapped by the minimal XNU page tables. Put a tiny
 * trampoline in shared DMA32 memory instead: it switches CR3 back to Linux and
 * jumps to xnu_world_upcall_x86(), which then restores the rest of the world.
 * The trampoline and the initial XNU stack live in one 2 MiB PDE.
 */
#define XNU_EXTRA_VA      0xffffff8040000000ULL
#define XNU_UPCALL_VA     XNU_EXTRA_VA
#define XNU_STACK_VA      (XNU_EXTRA_VA + PAGE_SIZE)

struct init_pt {
	u64_t *pml4;
	u64_t *pdpt_id;
	u64_t *pde_id;
	u64_t *pdpt_k;
	u64_t *pde_k;
	u64_t *pde_extra;
	u64_t *pte_extra;
	u64_t **pte_k;
	unsigned long n_pte_pages;
	struct fki_region region;
};

static int build_init_pt(struct init_pt *pt, u64_t kc_pa, u64_t vmlow,
    u64_t vmhigh, u64_t map_top, u64_t tramp_pa, u64_t stack_pa,
    u64_t stack_size, u64_t *out_phys)
{
	const unsigned long id_n_pde = DIV_ROUND_UP(map_top, 2UL << 20);
	const unsigned long k_pages = DIV_ROUND_UP(vmhigh - vmlow, PAGE_SIZE);
	const unsigned long k_first_pt = (vmlow >> PAGE_SHIFT) & 0x1ffUL;
	/*
	 * XNU expects the booter to have installed the "static map"
	 * 0xffffff8000000000 + pa -> pa for all low physical memory: its
	 * ml_static_ptovirt() uses that alias for boot_args, the EFI map and
	 * the page tables allocated from physfree.  One PTE page covers 2 MiB.
	 */
	const unsigned long k_n_pte = DIV_ROUND_UP(map_top, 2UL << 20);
	const unsigned long stack_pages = stack_size >> PAGE_SHIFT;
	unsigned long pt_size, i, n;
	u64_t pa;

	if (!stack_pages || stack_pages > 511)
		return -EINVAL;

	memset(pt, 0, sizeof(*pt));
	pt_size = 7 * PAGE_SIZE + k_n_pte * PAGE_SIZE;
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
	pt->pdpt_id = (u64_t *)((char *)pt->region.va + PAGE_SIZE);
	pt->pde_id = (u64_t *)((char *)pt->region.va + 2 * PAGE_SIZE);
	pt->pdpt_k = (u64_t *)((char *)pt->region.va + 3 * PAGE_SIZE);
	pt->pde_k = (u64_t *)((char *)pt->region.va + 4 * PAGE_SIZE);
	pt->pde_extra = (u64_t *)((char *)pt->region.va + 5 * PAGE_SIZE);
	pt->pte_extra = (u64_t *)((char *)pt->region.va + 6 * PAGE_SIZE);
	for (i = 0; i < k_n_pte; i++)
		pt->pte_k[i] =
		    (u64_t *)((char *)pt->region.va + (7 + i) * PAGE_SIZE);

	memset(pt->region.va, 0, pt_size);

	/* identity map of low physical memory (2 MiB pages) */
	pt->pml4[0] = (pa + PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	pt->pdpt_id[0] = (pa + 2 * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	for (i = 0; i < id_n_pde; i++)
		pt->pde_id[i] = (i << 21) | PT_E_PRESENT | PT_E_WRITABLE | PT_E_LARGE;

	/* XNU kernel half -> copied KC physical pages (4 KiB pages) */
	pt->pml4[PML4_KERNEL_INDEX] =
	    (pa + 3 * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	pt->pdpt_k[0] = (pa + 4 * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	for (i = 0; i < k_n_pte; i++)
		pt->pde_k[i] =
		    (pa + (7 + i) * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;

	/* ml_static_ptovirt() alias: kernel-half VA == low PA. */
	for (i = 0; i < (map_top >> PAGE_SHIFT); i++) {
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

	/* XNU-visible upcall trampoline and initial stack. */
	pt->pdpt_k[1] =
	    (pa + 5 * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	pt->pde_extra[0] =
	    (pa + 6 * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;
	pt->pte_extra[0] = tramp_pa | PT_E_PRESENT | PT_E_WRITABLE;
	for (i = 0; i < stack_pages; i++)
		pt->pte_extra[1 + i] =
		    (stack_pa + i * PAGE_SIZE) | PT_E_PRESENT | PT_E_WRITABLE;

	*out_phys = pa;
	return 0;
}

static void free_init_pt(struct init_pt *pt)
{
	kfree(pt->pte_k);
	if (pt->region.va)
		fki_x86_shared_free(&pt->region);
}

/* ---- EFI memory map ------------------------------------------------------- */

#define MAX_EFI_RANGES 32
static struct x86_efi_memory_range efi_map[MAX_EFI_RANGES];
static struct fki_region efi_region;
static unsigned int efi_count;

static int build_efi_map(u64_t total)
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

	if (fki_x86_shared_alloc(sizeof(efi_map), &efi_region))
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

/* ---- XNU globals ---------------------------------------------------------- */

static void *xnu_va_to_host(u64_t va)
{
	if (va < img.vmlow || va >= img.vmhigh)
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

/*
 * XNU calls linux64_hook with an indirect call, but host module text is not
 * mapped by XNU's page tables. Point the hook at a trampoline in shared memory:
 * switch CR3 back to Linux, then jump to the real upcall with rdi/rsi/rdx
 * untouched. The upcall restores the remaining world state on Linux's CR3.
 */
static void write_upcall_trampoline(void *dst, u64_t cr3, u64_t upcall)
{
	u8 *p = dst;

	*p++ = 0x48; *p++ = 0xb8;              /* movabs $cr3, %rax */
	memcpy(p, &cr3, sizeof(cr3)); p += sizeof(cr3);
	*p++ = 0x0f; *p++ = 0x22; *p++ = 0xd8; /* mov %rax, %cr3 */
	*p++ = 0x49; *p++ = 0xbb;              /* movabs $upcall, %r11 */
	memcpy(p, &upcall, sizeof(upcall)); p += sizeof(upcall);
	*p++ = 0x41; *p++ = 0xff; *p++ = 0xe3; /* jmp *%r11 */
}

static int write_xnu_globals(void)
{
	u64_t hook = XNU_UPCALL_VA, slide = xnu_slide;
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
	return 0;
}

/* ---- world setup ---------------------------------------------------------- */

static int setup_world(struct xnu_world_x86 *w, int cpu, u64_t cr3_phys,
    u64_t stack_top, u64_t xsave_va, u64_t xsave_size)
{
	struct fki_x86_state *s = &w->xnu_state;

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
	w->xnu_stack_top = stack_top;
	w->xnu_rsp = stack_top;
	return 0;
}

/* ---- run ------------------------------------------------------------------ */

static int run_xnu(u64_t kc_pa, u64_t vmlow, u64_t vmhigh, u64_t ba_phys)
{
	struct init_pt pt;
	u64_t stack_va, xsave_va, cr3_phys;
	u64_t apic;
	long r;
	int cpu, err, i;

	err = fki_x86_shared_alloc(16 << 10, &mem.stack);
	if (err) {
		pr_err("xnu86: cannot allocate XNU stack (%d)\n", err);
		return err;
	}
	stack_va = XNU_STACK_VA + mem.stack.size - 16;

	err = fki_x86_shared_alloc(PAGE_SIZE, &mem.tramp);
	if (err) {
		pr_err("xnu86: cannot allocate upcall trampoline (%d)\n", err);
		goto out_stack;
	}

	err = build_init_pt(&pt, kc_pa, vmlow, vmhigh, ram_mb << 20,
	    mem.tramp.pa, mem.stack.pa, mem.stack.size, &cr3_phys);
	if (err) {
		pr_err("xnu86: cannot build initial page tables (%d)\n", err);
		goto out_tramp;
	}

	/* The trampoline switches Linux's CR3 before entering host code. */
	write_upcall_trampoline(mem.tramp.va, x86_read_cr3(),
	    (u64_t)(uintptr_t)xnu_world_upcall_x86);

	xsave_va = 0;
	err = fki_x86_shared_alloc(4096, &mem.xsave);
	if (!err)
		xsave_va = (u64_t)mem.xsave.va;

	cpu = smp_processor_id();
	fki_bind_cpu(cpu); /* keep the whole downcall on one CPU */

	nr_cpus = min(nr_cpus, MAX_XNU_CPUS);
	nr_cpus = min(nr_cpus, fki_x86_num_cpus());
	for (i = 0; i < nr_cpus; i++)
		setup_world(&worlds[i], i, cr3_phys, stack_va, xsave_va,
		    mem.xsave.size);

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

	pr_info("xnu86: stack=0x%llx trampoline=0x%llx cr3=0x%llx\n",
	    (unsigned long long)stack_va,
	    (unsigned long long)XNU_UPCALL_VA,
	    (unsigned long long)cr3_phys);
	debugf("run: cr3=0x%llx stack=0x%llx tramp=0x%llx ba=0x%llx\n",
	    (unsigned long long)cr3_phys, (unsigned long long)stack_va,
	    (unsigned long long)XNU_UPCALL_VA, (unsigned long long)ba_phys);
	logs("xnu86: entering vstart\n");
	r = xnu_world_call_x86(&worlds[0], a_vstart, ba_phys);
	pr_info("xnu86: vstart returned %ld\n", r);
	debugf("run: vstart returned %ld\n", r);

out_xsave:
	fki_x86_shared_free(&mem.xsave);
	free_init_pt(&pt);
out_tramp:
	fki_x86_shared_free(&mem.tramp);
out_stack:
	fki_x86_shared_free(&mem.stack);
	return err;
}

/* ---- init / exit ---------------------------------------------------------- */

static int __init xnu_x86_init(void)
{
	const void *data = NULL;
	unsigned long size = 0;
	void *blob = NULL;
	u64_t total, map_top;
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
	if (fki_x86_shared_alloc(sizeof(*ba) + PAGE_SIZE, &mem.ba)) {
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return -ENOMEM;
	}
	build_efi_map(total);

	err = linux64_boot_args_init(ba, kc_phys, img.vmhigh - img.vmlow,
	    img.text_vmaddr, cmdline_copy, (u32_t)efi_region.pa, efi_map,
	    efi_count, sizeof(efi_map[0]), total, 0, 0);
	if (err) {
		pr_err("xnu86: boot_args init failed (%d)\n", err);
		kfree(cmdline_copy);
		kfree(ba);
		fki_x86_shared_free(&mem.ba);
		fki_x86_shared_free(&mem.kc);
		fki_blob_put(blob);
		return err;
	}
	memcpy(mem.ba.va, ba, sizeof(*ba));
	ba_phys = mem.ba.pa;
	debugf("loader: boot_args pa=0x%llx efi pa=0x%llx\n",
	    (unsigned long long)ba_phys, (unsigned long long)efi_region.pa);

	run_xnu(kc_phys, img.vmlow, img.vmhigh, ba_phys);

	kfree(cmdline_copy);
	kfree(ba);
	fki_x86_shared_free(&efi_region);
	fki_x86_shared_free(&mem.ba);
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
