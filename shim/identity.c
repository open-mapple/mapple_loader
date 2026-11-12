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
