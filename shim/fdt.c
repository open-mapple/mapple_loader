/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

uintptr_t fdt_chosen_prop(uintptr_t fdt, const char *prop, uint32_t *len_out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				*len_out = len;
				return val;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	uint32_t len = 0;
	uintptr_t val = fdt_chosen_prop(fdt, prop, &len);

	if (val && len == 4)
		*out = be32(val);
	else if (val && len == 8)
		*out = (uint64_t)be32(val) << 32 | be32(val + 4);
	else
		return 0;
	return 1;
}

static uint64_t cells(uintptr_t a, uint32_t n)
{
	return n == 2 ? (uint64_t)be32(a) << 32 | be32(a + 4) : be32(a);
}

int fdt_reserved(uintptr_t fdt, uint64_t (*out)[2], int max)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return -1;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, inside = 0, n = 0;
	uint32_t ac = 2, sc = 2;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			if (depth == 2 && streq((const char *)p, "reserved-memory"))
				inside = 1;
			p = align4(skip_str(p));
			break;
		case FDT_END_NODE:
			if (inside && depth == 2)
				return n;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *name = (const char *)(strings + nameoff);
			if (inside && depth == 2 && len == 4 && streq(name, "#address-cells"))
				ac = be32(val);
			else if (inside && depth == 2 && len == 4 && streq(name, "#size-cells"))
				sc = be32(val);
			else if (inside && depth == 3 && streq(name, "reg")) {
				if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
					return -1;
				for (uint32_t o = 0; o + 4 * (ac + sc) <= len; o += 4 * (ac + sc)) {
					if (n == max)
						return -1;
					out[n][0] = cells(val + o, ac);
					out[n][1] = out[n][0] + cells(val + o + 4 * ac, sc);
					n++;
				}
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return -1;
		}
	}
}
