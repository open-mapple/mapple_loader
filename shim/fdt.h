#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* The [base, end) of every reg under /reserved-memory, into OUT. Returns how many, or -1 (no such node, more than MAX,
 * odd cell sizes). */
int fdt_reserved(uintptr_t fdt, uint64_t (*out)[2], int max);
