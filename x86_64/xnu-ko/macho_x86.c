/* SPDX-License-Identifier: APSL-2.0 */
/* Mach-O parsing and loading for the x86_64 XNU kernel collection. */
#include "macho_x86.h"

#include <linux/string.h>

#define FOR_EACH_LC(img, l) \
	for (const struct lc *l = (const void *)((img)->file + sizeof(struct mh64)), \
	    *_e = (const void *)((img)->file + sizeof(struct mh64) + \
	          ((const struct mh64 *)(img)->file)->sizeofcmds); \
	     l < _e; l = (const void *)((const char *)l + l->cmdsize))

int macho_x86_load(struct xnu_image_x86 *img, const void *file,
    unsigned long size, unsigned long extra)
{
	const struct mh64 *mh = file;
	int err;

	if (size < sizeof(*mh) || mh->magic != MH_MAGIC_64)
		return -8; /* ENOEXEC */
	if (mh->cputype != CPU_TYPE_X86_64)
		return -8;

	img->file = file;
	img->file_size = size;
	img->vmlow = ~0ULL;
	img->vmhigh = 0;
	img->slide = 0;
	img->entry = 0;
	img->kernel_mh = 0;
	img->text_vmaddr = 0;
	img->symtab = NULL;

	FOR_EACH_LC(img, l) {
		if (l->cmd == LC_SEGMENT_64) {
			const struct seg64 *s = (const void *)l;
			if (!s->vmsize || !strncmp(s->segname, "__PAGEZERO", 16))
				continue;
			if (!strncmp(s->segname, "__TEXT", 16) && !img->text_vmaddr)
				img->text_vmaddr = s->vmaddr;
			if (s->vmaddr < img->vmlow)
				img->vmlow = s->vmaddr;
			if (s->vmaddr + s->vmsize > img->vmhigh)
				img->vmhigh = s->vmaddr + s->vmsize;
		} else if (l->cmd == LC_SYMTAB) {
			img->symtab = (const void *)l;
		} else if (l->cmd == LC_UNIXTHREAD) {
			/* x86_thread_state64: rax..r15, rip, rflags, cs, fs, gs
			 * (mirrors the ARM64 parser's fixed offset for now) */
			const u32_t *t = (const void *)(l + 1);
			img->entry = *(const u64_t *)(t + 2 + 2 * 16);
		}
	}
	if (img->vmhigh <= img->vmlow)
		return -8;
	img->filetype = mh->filetype;

	if (mh->filetype == MH_FILESET) {
		img->symtab = NULL;
		FOR_EACH_LC(img, l) {
			const struct fileset_entry_lc *fe;
			const struct mh64 *kh;
			const struct lc *kl;
			unsigned int i;

			if (l->cmd != LC_FILESET_ENTRY)
				continue;
			fe = (const void *)l;
			if (fe->entry_id >= fe->cmdsize ||
			    strcmp((const char *)fe + fe->entry_id, "com.apple.kernel"))
				continue;
			if (fe->fileoff + sizeof(struct mh64) > size)
				return -8;
			img->kernel_mh = ((u64_t)fe->vmaddr_hi << 32) | fe->vmaddr_lo;
			kh = (const void *)((const char *)file + fe->fileoff);
			kl = (const void *)(kh + 1);
			for (i = 0; i < kh->ncmds; i++,
			     kl = (const void *)((const char *)kl + kl->cmdsize))
				if (kl->cmd == LC_SYMTAB)
					img->symtab = (const void *)kl;
		}
		if (!img->kernel_mh || !img->symtab)
			return -8;
	}
	if (extra == ~0UL)
		return 0; /* parse only */

	err = fki_region_alloc(img->vmhigh - img->vmlow + extra, &img->region);
	if (err)
		return err;
	img->slide = (u64_t)img->region.va - img->vmlow;
	return macho_x86_copy(img, img->region.va);
}

int macho_x86_copy(struct xnu_image_x86 *img, void *dst)
{
	FOR_EACH_LC(img, l) {
		const struct seg64 *s = (const void *)l;

		if (l->cmd != LC_SEGMENT_64)
			continue;
		if (!s->vmsize || !s->filesize ||
		    !strncmp(s->segname, "__PAGEZERO", 16))
			continue;
		if (s->fileoff + s->filesize > img->file_size)
			return -8;
		memcpy((char *)dst + (s->vmaddr - img->vmlow),
		    img->file + s->fileoff, s->filesize);
	}
	return 0;
}

u64_t macho_x86_sym(struct xnu_image_x86 *img, const char *name)
{
	const struct symtab_lc *st = img->symtab;
	const struct nlist64 *nl;
	const char *strs;
	unsigned int i;

	if (!st || st->symoff + st->nsyms * sizeof(struct nlist64) > img->file_size)
		return 0;
	nl = (const void *)(img->file + st->symoff);
	strs = (const char *)img->file + st->stroff;
	for (i = 0; i < st->nsyms; i++) {
		if ((nl[i].n_type & 0x0e) != 0x0e ||
		    nl[i].n_strx >= st->strsize)
			continue; /* N_SECT */
		if (!strcmp(strs + nl[i].n_strx, name))
			return nl[i].n_value + img->slide;
	}
	return 0;
}

/* Slide the in-memory Mach-O load commands, as iBoot does for a static KC. */
int macho_x86_slide_header(struct xnu_image_x86 *img, u64_t *mh_unslid)
{
	u64_t mh_vm = 0;
	unsigned char *mh;
	const struct mh64 *h;
	struct lc *l, *e;
	int n = 0;

	FOR_EACH_LC(img, l) {
		const struct seg64 *s = (const struct seg64 *)l;
		if (l->cmd == LC_SEGMENT_64 && !strncmp(s->segname, "__TEXT", 16) &&
		    s->fileoff == 0)
			mh_vm = s->vmaddr;
	}
	if (!mh_vm)
		return -8;
	*mh_unslid = mh_vm;
	mh = (unsigned char *)(mh_vm + img->slide);
	h = (const void *)mh;
	l = (void *)(mh + sizeof(struct mh64));
	e = (void *)(mh + sizeof(struct mh64) + h->sizeofcmds);
	for (; l < e; l = (void *)((char *)l + l->cmdsize)) {
		struct seg64 *sg = (void *)l;
		struct sect64 *sc;
		unsigned int i;

		if (l->cmd != LC_SEGMENT_64 || !sg->vmsize)
			continue;
		sg->vmaddr += img->slide;
		sc = (void *)(sg + 1);
		for (i = 0; i < sg->nsects; i++)
			sc[i].addr += img->slide;
		n++;
	}
	return n;
}
