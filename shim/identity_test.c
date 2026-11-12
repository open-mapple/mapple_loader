/* Host test of identity.c: cc identity_test.c identity.c -o out/it && out/it */
#include <stdio.h>
#include <string.h>
#include "identity.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)

/* a root node with "name" (8 bytes) and "serial-number" (32 bytes) */
static size_t make_adt(uint8_t *b)
{
	size_t p = 8;
	memset(b, 0, 256);
	b[0] = 2;
	strcpy((char *)b + p, "name");
	b[p + 32] = 8;
	memcpy(b + p + 36, "device-", 8);
	p += 36 + 8;
	strcpy((char *)b + p, "serial-number");
	b[p + 32] = 32;
	memcpy(b + p + 36, "MAPPLE0000000000", 16);
	return p + 36 + 32;
}

static const char *serial_of(const uint8_t *b) { return (const char *)b + 8 + 44 + 36; }

int main(void)
{
	uint8_t b[256];
	size_t n = make_adt(b);
	const char *ok = "console=tty androidboot.serialno=ABC123xyz androidboot.mode=normal";
	CHECK(adt_phone_serial(b, n, ok, (uint32_t)strlen(ok)) == 9 && !strcmp(serial_of(b), "ABC123xyz"), "serial set");
	CHECK(b[8 + 44 + 36 + 9] == 0 && b[8 + 44 + 36 + 31] == 0, "rest of the 32 bytes zeroed");
	const char *bad[] = { "androidboot.serialno=AB-12 x", "xandroidboot.serialno=ABC", "androidboot.serialno= y",
	                      "androidboot.serialno=0123456789012345678901234567890123", "quiet" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		n = make_adt(b);
		CHECK(adt_phone_serial(b, n, bad[i], (uint32_t)strlen(bad[i])) == 0 && !strcmp(serial_of(b), "MAPPLE0000000000"), bad[i]);
	}
	const char *tab = "a=1\tandroidboot.serialno=Z9";
	n = make_adt(b);
	CHECK(adt_phone_serial(b, n, tab, (uint32_t)strlen(tab)) == 2 && !strcmp(serial_of(b), "Z9"), "after a tab");
	CHECK(adt_phone_serial(b, 40, ok, (uint32_t)strlen(ok)) == 0, "a truncated ADT is left alone");
	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
