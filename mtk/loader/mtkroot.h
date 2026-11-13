/*
 * The root partition (the one mp_ufsroot= names): the touch firmware out of the tail at its end. Its last 128 MiB are
 * outside the volume: the saved area, then at end - 64 MiB the tail: settings (1 MiB), the firmware region (15 MiB, a
 * ustar archive: connsys/, touch/), the log.
 *
 * Reads go through a root_rd: the partition's bytes from off (a multiple of 512) to the end of off's 4 KiB block, or
 * NULL when they could not be read. The next call may reuse the same memory.
 */
#ifndef MTKROOT_H
#define MTKROOT_H

typedef const volatile uint8_t *(*root_rd)(uint64_t off);

#define ROOT_ROOM            (128ULL << 20)
#define TAIL_FW_FROM_END     (63ULL << 20)
#define TAIL_FW_SIZE         (15ULL << 20)

/* an octal field of a tar header (leading spaces, then digits up to a NUL or space); -1 when it is not one */
static int64_t tar_octal(const volatile uint8_t *f, uint32_t n)
{
	int64_t v = 0;
	uint32_t i = 0;

	while (i < n && f[i] == ' ') {
		i++;
	}
	if (i == n || f[i] < '0' || f[i] > '7') {
		return -1;
	}
	for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
		v = v << 3 | (f[i] - '0');
	}
	return i == n || f[i] == 0 || f[i] == ' ' ? v : -1;
}

/*
 * The member `name` (a regular file) of the ustar archive at [base, base + size) of the partition: its size, its data
 * at *at. 0 and *why when it is not there or the archive is broken.
 */
static uint32_t tar_find(root_rd rd, uint64_t base, uint64_t size, const char *name, uint64_t *at, const char **why)
{
	uint32_t nl = 0;

	while (name[nl]) {
		nl++;
	}
	for (uint64_t o = 0; o + 512 <= size;) {
		const volatile uint8_t *h = rd(base + o);
		uint32_t sum = 0, zero = 1, k;
		int64_t len;

		if (!h) {
			*why = "a block of the tail not read";
			return 0;
		}
		for (uint32_t i = 0; i < 512; i++) {
			sum += i >= 148 && i < 156 ? ' ' : h[i];
			zero &= h[i] == 0;
		}
		if (zero) {
			break;                  /* the archive's end */
		}
		len = tar_octal(h + 124, 12);
		if ((int64_t)sum != tar_octal(h + 148, 8) || len < 0 || (uint64_t)len > size - o - 512) {
			*why = "the firmware region is no tar archive (or a broken one)";
			return 0;
		}
		for (k = 0; k < nl && h[k] == (uint8_t)name[k]; k++) {
		}
		if (k == nl && h[nl] == 0 && (h[156] == '0' || h[156] == 0) &&
		    !(h[257] == 'u' && h[262] == 0 && h[345] != 0)) {          /* no ustar prefix before the name */
			*at = base + o + 512;
			if (len == 0) {
				*why = "the member is empty";
			}
			return (uint32_t)len;
		}
		o += 512 + (((uint64_t)len + 511) & ~511ULL);
	}
	*why = "no such member in the firmware region";
	return 0;
}

/* touch/ilifw.bin from the tail of a partition of part bytes into buf (cap bytes): its size, or 0 and *why */
static uint32_t __attribute__((unused)) tail_ilifw(root_rd rd, uint64_t part, uint8_t *buf, uint32_t cap, const char **why)
{
	uint64_t at = 0;
	uint32_t n;

	if (part < ROOT_ROOM) {
		*why = "the partition is smaller than its areas";
		return 0;
	}
	n = tar_find(rd, part - TAIL_FW_FROM_END, TAIL_FW_SIZE, "touch/ilifw.bin", &at, why);
	if (n > cap) {
		*why = "touch/ilifw.bin is too big";
		return 0;
	}
	for (uint32_t i = 0; i < n;) {
		const volatile uint8_t *d = rd(at + i);
		uint32_t c = 4096 - (uint32_t)((at + i) & 4095);

		if (!d) {
			*why = "a block of touch/ilifw.bin not read";
			return 0;
		}
		for (c = c < n - i ? c : n - i; c; c--) {
			buf[i++] = *d++;
		}
	}
	return n;
}

/* the touch firmware to send: the payload's item when there is one (a developer's override), else what tail() put in
 * buf; NULL when neither */
static const uint8_t __attribute__((unused)) *ili_pick(const uint8_t *item, uint32_t item_size, uint32_t (*tail)(void),
    const uint8_t *buf, uint32_t *size)
{
	if (item) {
		*size = item_size;
		return item;
	}
	*size = tail();
	return *size ? buf : 0;
}

#endif /* MTKROOT_H */
