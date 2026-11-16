/*
 * About's serial number: the phone's own, told to iOS at each boot and stored nowhere (the ADT carries a placeholder).
 * ADT node: u32 property count, u32 child count, then properties: 32-byte name, u32 length (bit 31 a flag), the
 * value padded to 4 bytes.
 */
#include "identity.h"

static const char key[] = "androidboot.serialno=";

static int alnum(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int name_is(const volatile uint8_t *name, const char *want)
{
	int i = 0;
	for (; want[i]; i++)
		if (name[i] != (uint8_t)want[i])
			return 0;
	return name[i] == 0;
}

int adt_phone_serial(volatile uint8_t *adt, uint64_t adt_size, const volatile char *a, uint32_t len)
{
	const uint32_t k = sizeof(key) - 1;
	uint32_t at = 0, n = 0;

	for (uint32_t i = 0; a && i + k < len && !n; i++) {
		if (i && a[i - 1] != ' ' && a[i - 1] != '\t')
			continue;
		uint32_t j = 0;
		while (j < k && a[i + j] == key[j])
			j++;
		if (j < k)
			continue;
		at = i + k;
		while (at + n < len && alnum(a[at + n]))
			n++;
		if (n == 0 || n > 31 || (at + n < len && a[at + n] && a[at + n] != ' ' && a[at + n] != '\t'))
			return 0;
	}
	if (!n || adt_size < 8)
		return 0;
	uint32_t props = (uint32_t)adt[0] | (uint32_t)adt[1] << 8 | (uint32_t)adt[2] << 16 | (uint32_t)adt[3] << 24;
	uint64_t p = 8;
	for (uint32_t i = 0; i < props && p + 36 <= adt_size; i++) {
		uint32_t l = ((uint32_t)adt[p + 32] | (uint32_t)adt[p + 33] << 8 | (uint32_t)adt[p + 34] << 16 |
		    (uint32_t)adt[p + 35] << 24) & 0x7fffffff;
		if (p + 36 + l > adt_size)
			return 0;
		if (name_is(adt + p, "serial-number")) {
			if (l < 32)
				return 0;
			for (uint32_t j = 0; j < l; j++)
				adt[p + 36 + j] = j < n ? (uint8_t)a[at + j] : 0;
			return (int)n;
		}
		p += 36 + ((l + 3) & ~3u);
	}
	return 0;
}

/* ---- SHA-256 (FIPS 180-2) ---- */

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
	0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
	0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
	0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void block(uint32_t h[8], const volatile uint8_t *p)
{
	uint32_t w[64], a[8];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	for (int i = 0; i < 8; i++)
		a[i] = h[i];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = a[7] + (ror(a[4], 6) ^ ror(a[4], 11) ^ ror(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
		uint32_t t2 = (ror(a[0], 2) ^ ror(a[0], 13) ^ ror(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
		for (int j = 7; j > 0; j--)
			a[j] = a[j - 1];
		a[4] += t1;
		a[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++)
		h[i] += a[i];
}

void sha256(const uint8_t *data, uint64_t n, uint8_t out[32])
{
	static const volatile uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint32_t h[8];
	volatile uint8_t last[128];		/* volatile, as h0: the freestanding build has no memset or memcpy */
	for (int i = 0; i < 8; i++)
		h[i] = h0[i];
	for (int i = 0; i < 128; i++)
		last[i] = 0;
	uint64_t full = n & ~63ull, rest = n - full;
	for (uint64_t i = 0; i < full; i += 64)
		block(h, data + i);
	for (uint64_t i = 0; i < rest; i++)
		last[i] = data[full + i];
	last[rest] = 0x80;
	uint32_t blocks = rest < 56 ? 1 : 2;
	for (int i = 0; i < 8; i++)
		last[blocks * 64 - 1 - i] = (uint8_t)((n * 8) >> (8 * i));
	for (uint32_t i = 0; i < blocks; i++)
		block(h, last + 64 * i);
	for (int i = 0; i < 32; i++)
		out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- /chosen/random-seed ---- */

static uint32_t rd32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The end of the node at `p' (its properties, then its children), or 0 if it runs past `size'. */
static uint64_t node_end(const volatile uint8_t *adt, uint64_t size, uint64_t p, int depth)
{
	if (p + 8 > size || depth > 16)
		return 0;
	uint32_t props = rd32(adt + p), kids = rd32(adt + p + 4);
	p += 8;
	for (uint32_t i = 0; i < props; i++) {
		if (p + 36 > size)
			return 0;
		uint32_t l = rd32(adt + p + 32) & 0x7fffffff;
		p += 36 + ((l + 3) & ~3u);
		if (p > size)
			return 0;
	}
	for (uint32_t i = 0; i < kids && p; i++)
		p = node_end(adt, size, p, depth + 1);
	return p;
}

/* The value of property `prop' of the node at `p' (*len its length), or 0. */
static uint64_t node_prop(const volatile uint8_t *adt, uint64_t size, uint64_t p, const char *prop, uint32_t *len)
{
	uint32_t props = rd32(adt + p);
	p += 8;
	for (uint32_t i = 0; i < props && p + 36 <= size; i++) {
		uint32_t l = rd32(adt + p + 32) & 0x7fffffff;
		if (p + 36 + l > size)
			return 0;
		if (name_is(adt + p, prop)) {
			*len = l;
			return p + 36;
		}
		p += 36 + ((l + 3) & ~3u);
	}
	return 0;
}

uint32_t adt_random_seed(volatile uint8_t *adt, uint64_t adt_size, const uint8_t *entropy, uint32_t len)
{
	if (adt_size < 8 || len > 192)
		return 0;
	uint32_t props = rd32(adt), kids = rd32(adt + 4);
	uint64_t p = 8;
	for (uint32_t i = 0; i < props && p; i++) {
		if (p + 36 > adt_size)
			return 0;
		p += 36 + (((rd32(adt + p + 32) & 0x7fffffff) + 3) & ~3u);
	}
	for (uint32_t i = 0; i < kids && p && p < adt_size; i++) {
		uint32_t nl = 0, sl = 0;
		uint64_t name = node_prop(adt, adt_size, p, "name", &nl);
		if (name && nl >= 7 && name_is(adt + name, "chosen")) {
			uint64_t seed = node_prop(adt, adt_size, p, "random-seed", &sl);
			if (!seed || sl == 0 || sl > 256)
				return 0;
			uint8_t in[256 + 192 + 4], out[32];
			uint32_t n = 0;
			for (uint32_t j = 0; j < sl; j++)
				in[n++] = adt[seed + j];
			for (uint32_t j = 0; j < len; j++)
				in[n++] = entropy[j];
			for (uint32_t done = 0, c = 0; done < sl; c++) {
				in[n] = (uint8_t)c;
				in[n + 1] = (uint8_t)(c >> 8);
				in[n + 2] = 'R';
				in[n + 3] = 'S';
				sha256(in, n + 4, out);
				for (uint32_t j = 0; j < 32 && done < sl; j++)
					adt[seed + done++] = out[j];
			}
			return sl;
		}
		p = node_end(adt, adt_size, p, 0);
	}
	return 0;
}
