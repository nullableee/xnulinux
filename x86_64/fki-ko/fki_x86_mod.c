// SPDX-License-Identifier: GPL-2.0-only
/*
 * FKI-X86 v1 host module: semantic services for the separate x86_64 XNU path.
 *
 * This is the first host-side implementation. The world switch (GPRs/stacks)
 * lives in xnu-ko/world_x86.S; this file owns the ring-0 system state each
 * world needs: CR3, IDT/GDT, TR, GS bases, syscall MSRs and XSAVE state.
 * Linux keeps its own IDT while XNU runs, so XNU interrupt sources are all
 * delivered through host calls.
 */
#include "fki.h"
#include "fki_internal.h"

#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/irqflags.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>

#include <asm/fpu/api.h>
#include <asm/msr.h>
#include <asm/ptrace.h>
#include <asm/processor.h>

/* Every CPU that can host XNU gets one wake word + completion. */
static const volatile unsigned int *x86_wake_word[NR_CPUS];
static struct completion x86_wake[NR_CPUS];
static DEFINE_PER_CPU(call_single_data_t, x86_csd);

struct x86_desc_ptr {
	u16 limit;
	u64 base;
} __packed;

static void x86_save_state(struct fki_x86_state *s)
{
	struct x86_desc_ptr idt, gdt;
	u16 tr = 0;

	memset(s, 0, sizeof(*s));
	s->cr3 = __read_cr3();
	asm volatile ("sidt %0" : "=m" (idt));
	asm volatile ("sgdt %0" : "=m" (gdt));
	asm volatile ("str %0" : "=m" (tr));
	s->idt_base = idt.base;
	s->idt_limit = idt.limit;
	s->gdt_base = gdt.base;
	s->gdt_limit = gdt.limit;
	s->tr = tr;
	rdmsrq(MSR_GS_BASE, s->gs_base);
	rdmsrq(MSR_KERNEL_GS_BASE, s->kernel_gs_base);
	rdmsrq(MSR_STAR, s->star);
	rdmsrq(MSR_LSTAR, s->lstar);
	rdmsrq(MSR_CSTAR, s->cstar);
	rdmsrq(MSR_SYSCALL_MASK, s->sfmask);
}

static void x86_load_state(const struct fki_x86_state *s)
{
	struct x86_desc_ptr idt = { .limit = s->idt_limit, .base = s->idt_base };
	struct x86_desc_ptr gdt = { .limit = s->gdt_limit, .base = s->gdt_base };

	write_cr3(s->cr3);
	asm volatile ("lgdt %0" : : "m" (gdt) : "memory");
	asm volatile ("lidt %0" : : "m" (idt) : "memory");
	if (s->tr)
		asm volatile ("ltr %w0" : : "r" (s->tr) : "memory");
	wrmsrq(MSR_KERNEL_GS_BASE, s->kernel_gs_base);
	wrmsrq(MSR_GS_BASE, s->gs_base);
	wrmsrq(MSR_STAR, s->star);
	wrmsrq(MSR_LSTAR, s->lstar);
	wrmsrq(MSR_CSTAR, s->cstar);
	wrmsrq(MSR_SYSCALL_MASK, s->sfmask);
}

/* xsave/xrstor with all currently supported feature bits. The buffer belongs
 * to the world and must be 64-byte aligned; fki_x86_shared_alloc enforces it. */
static inline void x86_xsave(void *buf)
{
	__asm__ volatile ("xsave %0"
	    : "+m" (*(volatile char (*)[64])buf)
	    : "a" (0xffffffffU), "d" (0xffffffffU)
	    : "memory");
}

static inline void x86_xrstor(const void *buf)
{
	__asm__ volatile ("xrstor %0"
	    : : "m" (*(const volatile char (*)[64])buf),
	        "a" (0xffffffffU), "d" (0xffffffffU)
	    : "memory");
}

unsigned long fki_x86_enter(const struct fki_x86_state *foreign,
    struct fki_x86_state *saved)
{
	unsigned long flags;

	if (!foreign)
		return 0;
	preempt_disable();
	local_irq_save(flags);
	x86_save_state(saved);
	if (foreign->xsave_va && foreign->xsave_size) {
		if (saved->xsave_va && saved->xsave_size)
			x86_xsave((void *)saved->xsave_va);
		x86_xrstor((const void *)foreign->xsave_va);
	}
	x86_load_state(foreign);
	return flags;
}
EXPORT_SYMBOL(fki_x86_enter);

void fki_x86_leave(unsigned long cookie, struct fki_x86_state *saved,
    struct fki_x86_state *foreign)
{
	if (!saved)
		return;
	if (foreign)
		x86_save_state(foreign);
	if (foreign && foreign->xsave_va && foreign->xsave_size)
		x86_xsave((void *)foreign->xsave_va);
	x86_load_state(saved);
	if (saved->xsave_va && saved->xsave_size)
		x86_xrstor((const void *)saved->xsave_va);
	local_irq_restore(cookie);
	preempt_enable();
}
EXPORT_SYMBOL(fki_x86_leave);

void fki_x86_swap_user_state(const struct fki_x86_state *load,
    struct fki_x86_state *save)
{
	unsigned long flags;

	local_irq_save(flags);
	if (save)
		x86_save_state(save);
	if (load)
		x86_load_state(load);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(fki_x86_swap_user_state);

/* ---- Letting Linux run while XNU is parked -------------------------------- */

static void x86_kick_fn(void *data)
{
	int cpu = (unsigned long)data;

	if (cpu >= 0 && cpu < NR_CPUS && x86_wake_word[cpu])
		complete(&x86_wake[cpu]);
}

void fki_x86_kick_cpu(int cpu)
{
	if (cpu < 0 || cpu >= nr_cpu_ids)
		return;
	if (x86_wake_word[cpu])
		complete(&x86_wake[cpu]);
	/* smp_call_function_single_async is safe from IRQ-off context. */
	smp_call_function_single_async(cpu, this_cpu_ptr(&x86_csd));
}
EXPORT_SYMBOL(fki_x86_kick_cpu);

void fki_x86_idle_until(fki_u64 counter_deadline, fki_u64 max_ns,
    const volatile unsigned int *wake)
{
	unsigned long flags;
	long timeout;
	int cpu;

	local_irq_save(flags);
	cpu = smp_processor_id();
	x86_wake_word[cpu] = wake;
	if (wake && *wake) {
		x86_wake_word[cpu] = NULL;
		local_irq_restore(flags);
		return;
	}
	reinit_completion(&x86_wake[cpu]);
	preempt_enable();
	local_irq_enable();

	/* TODO(stage 3): arm an hrtimer at counter_deadline; for now the host
	 * wakes us from its own timer or from fki_x86_kick_cpu(). */
	timeout = wait_for_completion_timeout(&x86_wake[cpu],
	    usecs_to_jiffies(max_ns / NSEC_PER_USEC));
	(void)counter_deadline;
	(void)timeout;

	local_irq_disable();
	preempt_disable();
	x86_wake_word[cpu] = NULL;
	local_irq_restore(flags);
}
EXPORT_SYMBOL(fki_x86_idle_until);

void fki_x86_yield(void)
{
	unsigned long flags;

	local_irq_save(flags);
	preempt_enable();
	local_irq_enable();
	cond_resched();
	local_irq_disable();
	preempt_disable();
	local_irq_restore(flags);
}
EXPORT_SYMBOL(fki_x86_yield);

void fki_linux_runs_ext(void (*fn)(void *), void *arg)
{
	unsigned long flags;

	local_irq_save(flags);
	preempt_enable();
	local_irq_enable();
	fn(arg);
	local_irq_disable();
	preempt_disable();
	local_irq_restore(flags);
}

/* ---- CPU discovery --------------------------------------------------------- */

int fki_x86_num_cpus(void)
{
	return num_online_cpus();
}
EXPORT_SYMBOL(fki_x86_num_cpus);

int fki_x86_cpu_apic_id(int cpu, fki_u64 *apic_id)
{
	if (!apic_id || cpu < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;
	*apic_id = cpu_data(cpu).topo.initial_apicid;
	return 0;
}
EXPORT_SYMBOL(fki_x86_cpu_apic_id);

/* ---- Basic services -------------------------------------------------------- */

int fki_version(void)
{
	return FKI_X86_VERSION;
}
EXPORT_SYMBOL(fki_version);

void fki_log(const char *s, unsigned long len)
{
	pr_info("fki-x86: %.*s\n", (int)len, s);
}
EXPORT_SYMBOL(fki_log);

void fki_log_alert(const char *s, unsigned long len)
{
	pr_alert("fki-x86: %.*s\n", (int)len, s);
}
EXPORT_SYMBOL(fki_log_alert);

fki_u64 fki_counter_frequency(void)
{
	return NSEC_PER_SEC;
}
EXPORT_SYMBOL(fki_counter_frequency);

fki_u64 fki_counter(void)
{
	return ktime_get_ns();
}
EXPORT_SYMBOL(fki_counter);

void fki_random(void *buf, unsigned long len)
{
	get_random_bytes(buf, len);
}
EXPORT_SYMBOL(fki_random);

void fki_realtime(fki_u64 *sec, fki_u64 *nsec)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	*sec = ts.tv_sec;
	*nsec = ts.tv_nsec;
}
EXPORT_SYMBOL(fki_realtime);

int fki_bind_cpu(int cpu)
{
	return set_cpus_allowed_ptr(current, cpumask_of(cpu));
}
EXPORT_SYMBOL(fki_bind_cpu);

void fki_msleep(unsigned int ms)
{
	msleep(ms);
}
EXPORT_SYMBOL(fki_msleep);

/* ---- Firmware blobs (the kernel collection, ramdisk, trust cache) ---------- */

int fki_blob_get(const char *name, const void **data, unsigned long *size,
    void **handle)
{
	const struct firmware *fw;
	int err = request_firmware(&fw, name, NULL);

	if (err)
		return err;
	*data = fw->data;
	*size = fw->size;
	*handle = (void *)fw;
	return 0;
}
EXPORT_SYMBOL(fki_blob_get);

void fki_blob_put(void *handle)
{
	release_firmware(handle);
}
EXPORT_SYMBOL(fki_blob_put);

/* ---- Host files (block devices, console, keystore) ------------------------ */

void *fki_file_open(const char *path, int writable, fki_u64 *size)
{
	struct file *f = filp_open(path,
	    (writable ? O_RDWR : O_RDONLY) | O_LARGEFILE, 0);

	if (IS_ERR(f))
		return NULL;
	if (size)
		*size = i_size_read(f->f_mapping->host);
	return f;
}
EXPORT_SYMBOL(fki_file_open);

#define FKI_FILE_CHUNK (256UL << 10)

struct fki_file_args {
	void *file;
	int write;
	void *buf;
	unsigned long len;
	fki_u64 off;
	long ret;
};

static long fki_file_rw_body(void *file, int write, void *buf,
    unsigned long len, fki_u64 off)
{
	struct file *f = file;
	unsigned long done = 0;
	long ret = 0;
	void *bounce;

	bounce = kvmalloc(min(len, FKI_FILE_CHUNK), GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;
	while (done < len) {
		unsigned long n = min(len - done, FKI_FILE_CHUNK);
		loff_t pos = off + done;
		ssize_t r;

		if (write) {
			memcpy(bounce, (char *)buf + done, n);
			r = kernel_write(f, bounce, n, &pos);
		} else {
			r = kernel_read(f, bounce, n, &pos);
			if (r > 0)
				memcpy((char *)buf + done, bounce, r);
		}
		if (r < 0) {
			ret = r;
			break;
		}
		done += r;
		if ((unsigned long)r < n)
			break;
	}
	kvfree(bounce);
	return ret < 0 && !done ? ret : (long)done;
}

static void fki_file_rw_thunk(void *p)
{
	struct fki_file_args *a = p;

	a->ret = fki_file_rw_body(a->file, a->write, a->buf, a->len, a->off);
}

long fki_file_rw(void *file, int write, void *buf, unsigned long len,
    fki_u64 off)
{
	struct fki_file_args a = { file, write, buf, len, off, -EIO };

	if (!file)
		return -EBADF;
	fki_linux_runs_ext(fki_file_rw_thunk, &a);
	return a.ret;
}
EXPORT_SYMBOL(fki_file_rw);

long fki_file_pwrite(void *file, const void *buf, unsigned long len,
    fki_u64 off)
{
	loff_t pos = off;

	if (!file)
		return -EBADF;
	return kernel_write(file, buf, len, &pos);
}
EXPORT_SYMBOL(fki_file_pwrite);

void fki_file_close(void *file)
{
	if (file)
		filp_close(file, NULL);
}
EXPORT_SYMBOL(fki_file_close);

/* ---- Memory ---------------------------------------------------------------- */

static int (*p_set_memory_x)(unsigned long addr, int numpages);

static int fki_x86_resolve(void)
{
	static unsigned long (*lookup)(const char *);
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	u32 insn;
	unsigned long addr;

	if (p_set_memory_x)
		return 0;
	if (!lookup) {
		if (register_kprobe(&kp))
			return -ENOENT;
		addr = (unsigned long)kp.addr;
		unregister_kprobe(&kp);
		/*
		 * With IBT enabled, arch_adjust_kprobe_addr() moves kp.addr
		 * past the function's ENDBR64 so that int3 can be planted
		 * there.  That address is not a legal indirect-branch target:
		 * calling it raises #CP ("Missing ENDBR").  Step back over the
		 * ENDBR64 before calling, but only when it is really there.
		 */
		if (!copy_from_kernel_nofault(&insn, (void *)(addr - 4),
		    sizeof(insn)) && insn == 0xfa1e0ff3U)
			addr -= 4;
		lookup = (void *)addr;
	}
	p_set_memory_x = (void *)lookup("set_memory_x");
	if (!p_set_memory_x)
		return -ENOENT;
	pr_info("fki-x86: set_memory_x=%pS via kallsyms_lookup_name=%pS\n",
	    (void *)p_set_memory_x, (void *)lookup);
	return 0;
}

int fki_x86_shared_alloc(unsigned long size, struct fki_region *r)
{
	dma_addr_t pa;
	void *va;
	int err;

	if (!r || !size)
		return -EINVAL;
	err = fki_x86_resolve();
	if (err)
		return err;
	va = dma_alloc_coherent(NULL, PAGE_ALIGN(size), &pa, GFP_KERNEL);
	if (!va)
		return -ENOMEM;
	if (p_set_memory_x((unsigned long)va, PFN_UP(size))) {
		dma_free_coherent(NULL, PAGE_ALIGN(size), va, pa);
		return -EPERM;
	}
	r->va = va;
	r->pa = pa;
	r->size = PAGE_ALIGN(size);
	r->handle = va;
	return 0;
}
EXPORT_SYMBOL(fki_x86_shared_alloc);

void fki_x86_shared_free(struct fki_region *r)
{
	if (!r || !r->va)
		return;
	dma_free_coherent(NULL, r->size, r->va, (dma_addr_t)r->pa);
	memset(r, 0, sizeof(*r));
}
EXPORT_SYMBOL(fki_x86_shared_free);

int fki_region_alloc(unsigned long size, struct fki_region *r)
{
	return fki_x86_shared_alloc(size, r);
}
EXPORT_SYMBOL(fki_region_alloc);

void fki_region_free(struct fki_region *r)
{
	fki_x86_shared_free(r);
}
EXPORT_SYMBOL(fki_region_free);

fki_u64 fki_phys_peek(fki_u64 pa)
{
	void __iomem *p = ioremap(pa, sizeof(u64));
	fki_u64 v = 0;

	if (p) {
		v = readq(p);
		iounmap(p);
	}
	return v;
}
EXPORT_SYMBOL(fki_phys_peek);

int fki_contig_alloc(unsigned long size, struct fki_region *r)
{
	return fki_x86_shared_alloc(size, r);
}
EXPORT_SYMBOL(fki_contig_alloc);

void fki_contig_free(struct fki_region *r)
{
	fki_x86_shared_free(r);
}
EXPORT_SYMBOL(fki_contig_free);

/* ---- Faults ---------------------------------------------------------------- */

struct x86_fault_handler {
	unsigned long lo, hi;
	int (*fn)(struct fki_x86_regs *regs, void *arg);
	void *arg;
};
static struct x86_fault_handler x86_fault;

static void x86_fill_regs(struct fki_x86_regs *out, struct pt_regs *in,
    unsigned long vector)
{
	out->vector = vector;
	out->rip = in->ip;
	out->rflags = in->flags;
	out->cr2 = read_cr2();
	out->error_code = in->orig_ax;
	out->rax = in->ax;
	out->rbx = in->bx;
	out->rcx = in->cx;
	out->rdx = in->dx;
	out->rsi = in->si;
	out->rdi = in->di;
	out->rbp = in->bp;
	out->rsp = in->sp;
	out->r8 = in->r8;
	out->r9 = in->r9;
	out->r10 = in->r10;
	out->r11 = in->r11;
	out->r12 = in->r12;
	out->r13 = in->r13;
	out->r14 = in->r14;
	out->r15 = in->r15;
}

unsigned long fki_x86_regs_pc(const struct fki_x86_regs *regs)
{
	return regs->rip;
}
EXPORT_SYMBOL(fki_x86_regs_pc);

void fki_x86_regs_set_pc(struct fki_x86_regs *regs, unsigned long pc)
{
	regs->rip = pc;
}
EXPORT_SYMBOL(fki_x86_regs_set_pc);

void fki_x86_regs_set_reg(struct fki_x86_regs *regs, unsigned int n,
    unsigned long v)
{
	static unsigned short offset[] = {
		offsetof(struct fki_x86_regs, rax), offsetof(struct fki_x86_regs, rbx),
		offsetof(struct fki_x86_regs, rcx), offsetof(struct fki_x86_regs, rdx),
		offsetof(struct fki_x86_regs, rsi), offsetof(struct fki_x86_regs, rdi),
		offsetof(struct fki_x86_regs, rbp), offsetof(struct fki_x86_regs, rsp),
		offsetof(struct fki_x86_regs, r8),  offsetof(struct fki_x86_regs, r9),
		offsetof(struct fki_x86_regs, r10), offsetof(struct fki_x86_regs, r11),
		offsetof(struct fki_x86_regs, r12), offsetof(struct fki_x86_regs, r13),
		offsetof(struct fki_x86_regs, r14), offsetof(struct fki_x86_regs, r15),
	};

	if (n < ARRAY_SIZE(offset))
		*(unsigned long *)((char *)regs + offset[n]) = v;
}
EXPORT_SYMBOL(fki_x86_regs_set_reg);

int fki_x86_fault_register(unsigned long lo, unsigned long hi,
    int (*handler)(struct fki_x86_regs *regs, void *arg), void *arg)
{
	if (!handler || x86_fault.fn)
		return -EBUSY;
	x86_fault.lo = lo;
	x86_fault.hi = hi;
	x86_fault.fn = handler;
	x86_fault.arg = arg;
	/*
	 * TODO(stage 3): install kprobes on do_page_fault/do_general_protection/
	 * do_trap and call x86_fill_regs() from their pre_handlers. The current
	 * ABI is in place; hooking Linux's fault entries is the remaining work.
	 */
	return 0;
}
EXPORT_SYMBOL(fki_x86_fault_register);

void fki_x86_fault_unregister(void)
{
	memset(&x86_fault, 0, sizeof(x86_fault));
}
EXPORT_SYMBOL(fki_x86_fault_unregister);

/* ---- Thread helpers -------------------------------------------------------- */

struct x86_thread {
	struct task_struct *task;
	int (*fn)(void *);
	void *arg;
};

static int x86_thread_fn(void *data)
{
	struct x86_thread *t = data;

	while (!kthread_should_stop())
		t->fn(t->arg);
	return 0;
}

void *fki_thread_start(int (*fn)(void *), void *arg, int cpu,
    const char *name)
{
	struct x86_thread *t = kzalloc(sizeof(*t), GFP_KERNEL);

	if (!t)
		return NULL;
	t->fn = fn;
	t->arg = arg;
	t->task = kthread_create(x86_thread_fn, t, "%s", name);
	if (IS_ERR(t->task)) {
		kfree(t);
		return NULL;
	}
	if (cpu >= 0)
		kthread_bind(t->task, cpu);
	wake_up_process(t->task);
	return t;
}
EXPORT_SYMBOL(fki_thread_start);

void fki_thread_stop(void *thread)
{
	struct x86_thread *t = thread;

	if (!t)
		return;
	kthread_stop(t->task);
	kfree(t);
}
EXPORT_SYMBOL(fki_thread_stop);

int fki_thread_should_stop(void)
{
	return kthread_should_stop();
}
EXPORT_SYMBOL(fki_thread_should_stop);

void fki_thread_wait(const volatile unsigned long *flag)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (READ_ONCE(*flag) || kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
}
EXPORT_SYMBOL(fki_thread_wait);

void fki_thread_wake(void *thread)
{
	if (thread)
		wake_up_process(thread);
}
EXPORT_SYMBOL(fki_thread_wake);

static int __init fki_x86_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		init_completion(&x86_wake[cpu]);
		INIT_CSD(&per_cpu(x86_csd, cpu), x86_kick_fn,
		    (void *)(unsigned long)cpu);
	}
	fki_hostcall_init();
	pr_info("fki-x86: FKI-X86 v%d ready\n", FKI_X86_VERSION);
	return 0;
}

static void __exit fki_x86_exit(void)
{
	fki_hostcall_exit();
	pr_info("fki-x86: unloaded\n");
}

module_init(fki_x86_init);
module_exit(fki_x86_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Foreign kernel interface for the x86_64 XNU path");
