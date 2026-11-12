/* Parking XNU's other cores at the end of a run that hands back to Android (XNU_PARK): split-boot builds only. */

extern char xnu_park_pen[], xnu_park_pen_end[], xnu_park_flags[];

/* ---- LK's device tree: one node added to /reserved-memory ---- */

static void fdt_put(uint64_t at, uint32_t v)
{
	volatile uint8_t *p = (volatile uint8_t *)at;

	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* the offset of a name in the strings block, or -1 */
static int64_t fdt_string(uint64_t dtb, const char *name)
{
	uint64_t str = dtb + fdt_get(dtb + 12), size = fdt_get(dtb + 32);

	for (uint64_t at = 0; at < size;) {
		uint64_t n = 0;

		while (at + n < size && ((const volatile char *)str)[at + n]) {
			n++;
		}
		if (memcmp_n((const volatile char *)str + at, name, n + 1) == 0) {
			return (int64_t)at;
		}
		at += n + 1;
	}
	return -1;
}

/* where the END_NODE token of /reserved-memory is (from the tree's start), or 0 */
static uint64_t fdt_reserved_end(uint64_t dtb)
{
	static const char name[] = "reserved-memory";
	uint64_t at = fdt_get(dtb + 8), end = at + fdt_get(dtb + 36);
	int depth = 0, inside = 0;

	while (at < end) {
		uint32_t tok = fdt_get(dtb + at);

		at += 4;
		if (tok == FDT_BEGIN_NODE) {
			uint64_t n = 0;

			while (((const volatile char *)dtb)[at + n]) {
				n++;
			}
			depth++;
			if (depth == 2 && n == sizeof(name) - 1 && memcmp_n((const volatile char *)dtb + at, name, n) == 0) {
				inside = 1;
			}
			at += (n + 1 + 3) & ~3ULL;
		} else if (tok == FDT_END_NODE) {
			if (inside && depth == 2) {
				return at - 4;
			}
			depth--;
		} else if (tok == FDT_PROP) {
			at += 8 + ((fdt_get(dtb + at) + 3) & ~3u);
		} else if (tok != FDT_NOP) {
			break;
		}
	}
	return 0;
}

/* whether the node can go in: a version-17 tree, strings after the structure, /reserved-memory, "reg" and "no-map",
 * and room before the pen */
static int fdt_pen_fits(uint64_t dtb)
{
	return fdt_get(dtb) == 0xd00dfeedu && fdt_get(dtb + 20) >= 17 &&
	       fdt_get(dtb + 12) >= fdt_get(dtb + 8) + fdt_get(dtb + 36) && fdt_reserved_end(dtb) != 0 &&
	       fdt_string(dtb, "reg") >= 0 && fdt_string(dtb, "no-map") >= 0 &&
	       dtb + fdt_get(dtb + 4) + PARK_NODE_BYTES <= PARK_PA;
}

/* the pen's page as a no-map node, the last child of /reserved-memory: what follows moves up PARK_NODE_BYTES */
static void fdt_reserve_pen(uint64_t dtb)
{
	static const char node[] = PARK_NODE;
	uint64_t ins, total, w;
	uint32_t reg, nomap;
	volatile uint8_t *b = (volatile uint8_t *)dtb;

	if (!fdt_pen_fits(dtb)) {
		out("park: LK's device tree NOT changed (it no longer looks as expected): the pen is not reserved\n");
		return;
	}
	ins = fdt_reserved_end(dtb);
	total = fdt_get(dtb + 4);
	reg = (uint32_t)fdt_string(dtb, "reg");                  /* before the strings move */
	nomap = (uint32_t)fdt_string(dtb, "no-map");
	for (uint64_t i = total; i-- > ins;) {
		b[i + PARK_NODE_BYTES] = b[i];
	}
	w = dtb + ins;
	fdt_put(w, FDT_BEGIN_NODE);
	w += 4;
	for (uint32_t i = 0; i < 20; i++) {
		((volatile char *)w)[i] = i < sizeof(node) ? node[i] : 0;
	}
	w += 20;
	fdt_put(w, FDT_PROP);
	fdt_put(w + 4, 16);
	fdt_put(w + 8, reg);
	fdt_put(w + 12, 0);
	fdt_put(w + 16, (uint32_t)PARK_PA);
	fdt_put(w + 20, 0);
	fdt_put(w + 24, 0x1000);
	w += 28;
	fdt_put(w, FDT_PROP);
	fdt_put(w + 4, 0);
	fdt_put(w + 8, nomap);
	w += 12;
	fdt_put(w, FDT_END_NODE);
	fdt_put(dtb + 4, (uint32_t)(total + PARK_NODE_BYTES));
	fdt_put(dtb + 12, fdt_get(dtb + 12) + PARK_NODE_BYTES);
	fdt_put(dtb + 36, fdt_get(dtb + 36) + PARK_NODE_BYTES);
	__asm__ volatile("dsb sy");
	out("park: pen reserved in LK's device tree at ");
	out_hex(PARK_PA);
	out(" (" PARK_NODE ", no-map; the tree now ");
	out_dec(total + PARK_NODE_BYTES);
	out(" bytes)\n");
}

/* the pen copied to PARK_PA, its slots cleared, " loader_park=<PARK_PA>" on the command line -- only if LK's device tree
 * will take the node that keeps Linux off the pen */
static void xnu_park_setup(char *cl)
{
	static const char key[] = " loader_park=";
	volatile uint32_t *dst = (volatile uint32_t *)PARK_PA;
	const volatile uint32_t *src = (const volatile uint32_t *)xnu_park_pen;
	uint64_t v = PARK_PA;
	char d[24];
	uint32_t at = 0, n = 0;

	if (!fdt_pen_fits(lk_dtb)) {
		out("park: LK's device tree would not take the pen's node: no loader_park=, XNU's cores are switched off\n");
		return;
	}
	for (uint64_t i = 0; i < (uint64_t)(xnu_park_pen_end - xnu_park_pen) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy");
	while (cl[at]) {
		at++;
	}
	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	if (at + sizeof(key) - 1 + n >= BOOT_LINE_LENGTH) {
		out("park: no room on the command line\n");
		return;
	}
	for (uint32_t i = 0; key[i]; i++) {
		cl[at++] = key[i];
	}
	while (n) {
		cl[at++] = d[--n];
	}
	cl[at] = 0;
	out("park: the pen for XNU's other cores at ");
	out_hex(PARK_PA);
	out("\n");
}

/* which cores are in the pen; how many */
static int say_parked(void)
{
	const volatile uint32_t *slot = (const volatile uint32_t *)(PARK_PA + (uint64_t)(xnu_park_flags - xnu_park_pen));
	int n = 0;

	out("parked in the pen:");
	for (int c = 0; c < 8; c++) {
		if (slot[c] == 0x4b524150u) {
			out(" ");
			out_dec(c);
			n++;
		}
	}
	out("\n");
	return n;
}
