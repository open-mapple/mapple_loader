/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#ifdef MT6781_SHIM
#include "holes.h"
const char *mapple_bootfile(const char *payload_cmdline);	/* bootrec.c */
#endif

#define BOOT_LINE_LENGTH	608
#define MAPPLE_PAYLOAD_LINE_OFF	0x1000	/* in the boot_args page, after struct boot_args */
#define PAGE_16K		0x4000UL

#ifdef MT6781_SHIM
/* mp_mem4g (1 unless mp_mem4g=0): XNU's RAM starts at DRAM's start instead of the payload's phys_base, minus the holes
 * (holes.c), whose list is left in the boot_args page for the kernel (arm_vm_init.c) */
#define DRAM_BASE		0x40000000UL
#define MAPPLE_HOLES_OFF	0x2000	/* in the boot_args page, after the payload's line */

struct mapple_holes {
	char magic[8];			/* "MPHOLES1" */
	uint32_t count, pad;
	uint64_t range[HOLES_MAX][2];	/* [base, end), sorted, 16 KiB pages */
};

static uint64_t mem4g_holes[HOLES_MAX][2];
extern char _end[];

static int has_arg(const char *line, const char *arg)
{
	for (int i = 0; i < BOOT_LINE_LENGTH && line[i]; i++) {
		if (i && line[i - 1] != ' ')
			continue;
		int k = 0;
		while (arg[k] && i + k < BOOT_LINE_LENGTH && line[i + k] == arg[k])
			k++;
		if (!arg[k] && (i + k == BOOT_LINE_LENGTH || line[i + k] == ' ' || line[i + k] == 0))
			return 1;
	}
	return 0;
}
#endif

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "MAPPLEPL";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "no MAPPLEPL payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base;		/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

#ifdef MT6781_SHIM
	/* boot-args saved by Mapple in userdata (MAPPLEBR), read before the kernel is copied anywhere */
	const char *cmdline = mapple_bootfile(p->cmdline);
#endif

	uint64_t phys_base = p->phys_base, mem_size = p->mem_size;
#ifdef MT6781_SHIM
	int nholes = -1;
	if (!has_arg(cmdline, "mp_mem4g=0")) {
		uint64_t end = p->phys_base + p->mem_size;
		uint64_t adt = p->kernel_end - p->virt_base + DRAM_BASE;
		uint64_t top = ((adt + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1)) + PAGE_16K;

		nholes = mem_holes(dtb, rd_start, rd_end, DRAM_BASE, end, mem4g_holes);
		for (int i = 0; i < nholes; i++)
			if (mem4g_holes[i][0] < top)
				nholes = -1;		/* the kernel would land in a hole */
		for (uint32_t i = 0; i < p->nsegs; i++)
			if (p->segs[i].vmaddr - p->virt_base + DRAM_BASE < (uintptr_t)_end)
				nholes = -1;		/* or over this program */
		if (nholes > 0) {
			uint64_t free = end - DRAM_BASE;
			for (int i = 0; i < nholes; i++)
				free -= mem4g_holes[i][1] - mem4g_holes[i][0];
			phys_base = DRAM_BASE;
			mem_size = end - DRAM_BASE;
			con_hex2("MEM4G", "holes=", nholes, "free MiB=", free >> 20);
		} else {
			con_str("MEM4G", "no usable hole list: the 2 GB layout");
		}
	}
#endif
	pa_of_va_base = phys_base - p->virt_base;

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > phys_base && rd_start < phys_base + mem_size && phys_base == p->phys_base)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
#ifdef MT6781_SHIM
	const char *line = cmdline;
#else
	const char *line = p->cmdline;
#endif
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = line[i];
#ifdef MT6781_SHIM
	/* the payload's own line, for the kernel when the record replaced it (xnu mapple_sysctl.c) */
	if (line != p->cmdline) {
		volatile char *keep = (volatile char *)(ba_pa + MAPPLE_PAYLOAD_LINE_OFF);
		for (int i = 0; i < 8; i++)
			keep[i] = "MAPPLEPC"[i];
		for (int i = 0; i < BOOT_LINE_LENGTH; i++)
			keep[8 + i] = p->cmdline[i];
	}
#endif
	ba->memSizeActual = mem_size;
#ifdef MT6781_SHIM
	if (nholes > 0) {
		volatile struct mapple_holes *mh = (volatile struct mapple_holes *)(ba_pa + MAPPLE_HOLES_OFF);
		for (int i = 0; i < 8; i++)
			mh->magic[i] = "MPHOLES1"[i];
		mh->count = (uint32_t)nholes;
		for (int i = 0; i < nholes; i++) {
			mh->range[i][0] = mem4g_holes[i][0];
			mh->range[i][1] = mem4g_holes[i][1];
		}
	}
#endif
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
