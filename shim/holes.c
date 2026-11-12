/*
 * mp_mem4g: what of DRAM XNU must not use. LK's own reservations (every /reserved-memory reg, no-map or not: they hold
 * the TEE, ATF, SSPM, SCP, the modem and LK's framebuffer, and every EMI MPU region that limits the AP lies inside
 * one), then what was put there for us: the payload with mtkloader's tables after it (LK's initrd range, which
 * mtkloader extends over the tables) and LK's tree itself.
 */
#include "holes.h"
#include "fdt.h"

#define PAGE_16K	0x4000ULL

static int add(uint64_t (*h)[2], int n, uint64_t base, uint64_t end)
{
	if (n < 0 || n == HOLES_MAX)
		return -1;
	h[n][0] = base;
	h[n][1] = end;
	return n + 1;
}

int mem_holes(uintptr_t dtb, uint64_t rd_start, uint64_t rd_end, uint64_t lo, uint64_t hi, uint64_t (*h)[2])
{
	int n = fdt_reserved(dtb, h, HOLES_MAX);

	if (n <= 0 || rd_end <= rd_start)
		return -1;
	n = add(h, n, rd_start, rd_end);
	n = add(h, n, dtb, dtb + __builtin_bswap32(*(volatile uint32_t *)(dtb + 4)));
#ifdef SPACED
	/* where Android puts wifi-reserve-memory and consys-reserve-memory (alloc-ranges, so not in LK's tree) */
	n = add(h, n, 0xbe000000ULL, 0xbff30000ULL);
#endif
	if (n < 0)
		return -1;

	int m = 0;
	for (int i = 0; i < n; i++) {
		uint64_t b = h[i][0] & ~(PAGE_16K - 1), e = (h[i][1] + PAGE_16K - 1) & ~(PAGE_16K - 1);
		if (b < lo)
			b = lo;
		if (e > hi)
			e = hi;
		if (b >= e)
			continue;
		int j = m++;
		for (; j > 0 && h[j - 1][0] > b; j--) {
			h[j][0] = h[j - 1][0];
			h[j][1] = h[j - 1][1];
		}
		h[j][0] = b;
		h[j][1] = e;
	}
	n = 0;
	for (int i = 0; i < m; i++) {
		if (n && h[i][0] <= h[n - 1][1]) {
			if (h[i][1] > h[n - 1][1])
				h[n - 1][1] = h[i][1];
		} else {
			h[n][0] = h[i][0];
			h[n][1] = h[i][1];
			n++;
		}
	}
	return n;
}
