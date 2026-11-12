#pragma once
#include <stdint.h>

#define HOLES_MAX	48

/* The DRAM in [LO, HI) that XNU must leave alone with mp_mem4g=1, sorted, merged and in whole 16 KiB pages. Returns how
 * many ranges, or -1 when the list cannot be trusted. */
int mem_holes(uintptr_t dtb, uint64_t rd_start, uint64_t rd_end, uint64_t lo, uint64_t hi, uint64_t (*h)[2]);
