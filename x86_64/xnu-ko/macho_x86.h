/* SPDX-License-Identifier: APSL-2.0 */
/* Minimal Mach-O definitions for the x86_64 kernel collection (public format). */
#pragma once
#include "../fki-ko/fki.h"

typedef unsigned int u32_t;
typedef unsigned long long u64_t;

#define MH_MAGIC_64      0xfeedfacf
#define MH_CIGAM_64      0xcffaedfe
#define LC_SEGMENT_64    0x19
#define LC_SYMTAB        0x2
#define LC_UNIXTHREAD    0x5
#define LC_FILESET_ENTRY 0x80000035
#define MH_FILESET       12
#define MH_EXECUTE       2

#define CPU_TYPE_X86_64  0x01000007

struct mh64 {
	u32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct lc { u32_t cmd, cmdsize; };
struct seg64 {
	u32_t cmd, cmdsize;
	char segname[16];
	u64_t vmaddr, vmsize, fileoff, filesize;
	u32_t maxprot, initprot, nsects, flags;
};
struct sect64 {
	char sectname[16], segname[16];
	u64_t addr, size;
	u32_t offset, align, reloff, nreloc, flags, r1, r2, r3;
};
struct symtab_lc { u32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct nlist64 { u32_t n_strx; unsigned char n_type, n_sect; unsigned short n_desc; u64_t n_value; };
struct fileset_entry_lc { u32_t cmd, cmdsize, vmaddr_lo, vmaddr_hi, fileoff, reserved, entry_id; };

struct xnu_image_x86 {
	const unsigned char *file;
	unsigned long file_size;
	u64_t vmlow, vmhigh;
	u64_t slide;
	u64_t entry;
	u64_t kernel_mh;
	u64_t text_vmaddr;
	const struct symtab_lc *symtab;
	unsigned int filetype;
	struct fki_region region;
};

int macho_x86_load(struct xnu_image_x86 *img, const void *file,
    unsigned long size, unsigned long extra);
int macho_x86_copy(struct xnu_image_x86 *img, void *dst);
u64_t macho_x86_sym(struct xnu_image_x86 *img, const char *name);
int macho_x86_slide_header(struct xnu_image_x86 *img, u64_t *mh_unslid);
