/*
 * MAPPLEBR: the boot-args of the next boot kept in the root partition (`userdata`), outside the HFS+ volume, because
 * the MediaTek bootloader write-protects boot_a/boot_b while iOS runs. Written by the kernel (kern.mapple_nextbootargs,
 * bsd/dev/mtk_ufs.c, patch 0180), read by the shim before XNU (bootrec.c). Same layout on both sides.
 *
 * Place: the first 4 KiB block of the last MiB of the partition (block nblocks - BOOTREC_TAIL_BLOCKS). Little endian.
 * The record applies only while the boot partition's payload still holds the command line it was written against
 * (base_hash): flashing boot_b with another command line drops it; `mp_bootrec=0` in that command line ignores it.
 */
#pragma once
#include <stdint.h>

#define BOOTREC_MAGIC          "MAPPLEBR"
#define BOOTREC_VERSION        1
#define BOOTREC_TAIL_BLOCKS    256u            /* last 1 MiB of 4 KiB blocks */
#define BOOTREC_LINE           608             /* = boot_args.CommandLine */

struct bootrec_rec {
	char magic[8];
	uint32_t version;
	uint32_t flags;                         /* bit 0: enabled */
	uint64_t base_hash;                     /* FNV-1a 64 of the payload command line it replaces */
	uint64_t seq;                           /* incremented on every write */
	uint64_t line_hash;                     /* FNV-1a 64 of cmdline */
	uint8_t reserved[24];
	char cmdline[BOOTREC_LINE];            /* NUL-terminated, printable ASCII */
};

_Static_assert(sizeof(struct bootrec_rec) == 672, "MAPPLEBR layout");

/* FNV-1a 64 of a NUL-terminated string of at most max bytes. */
static inline uint64_t bootrec_hash(const char *s, unsigned max)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < max && s[i]; i++) {
		h ^= (uint8_t)s[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* 1 when rec is an enabled record for a payload whose command line hashes to base. */
static inline int bootrec_valid(const struct bootrec_rec *rec, uint64_t base)
{
	static const char want[8] = { 'M', 'A', 'P', 'P', 'L', 'E', 'B', 'R' };
	for (int i = 0; i < 8; i++)
		if (rec->magic[i] != want[i])
			return 0;
	if (rec->version != BOOTREC_VERSION || !(rec->flags & 1) || rec->base_hash != base)
		return 0;
	unsigned n = 0;
	while (n < BOOTREC_LINE && rec->cmdline[n]) {
		if (rec->cmdline[n] < 0x20 || rec->cmdline[n] > 0x7e)
			return 0;
		n++;
	}
	if (n == BOOTREC_LINE)
		return 0;
	return bootrec_hash(rec->cmdline, BOOTREC_LINE) == rec->line_hash;
}
