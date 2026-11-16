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

static size_t prop(uint8_t *b, size_t p, const char *name, const void *v, uint32_t l)
{
	strcpy((char *)b + p, name);
	memcpy(b + p + 32, &l, 4);
	memcpy(b + p + 36, v, l);
	return p + 36 + ((l + 3) & ~3u);
}

/* root (name, one property) with two children: "arm-io" (no children), then "chosen" with random-seed (64 zero bytes) */
static size_t make_tree(uint8_t *b, int chosen)
{
	static const uint8_t zero[64];
	uint32_t two = 2, one = 1, kids = 2;
	size_t p = 0;
	memset(b, 0, 1024);
	memcpy(b, &two, 4);
	memcpy(b + 4, &kids, 4);
	p = prop(b, 8, "name", "device-tree", 12);
	p = prop(b, p, "model", "x", 2);
	memcpy(b + p, &one, 4);
	memset(b + p + 4, 0, 4);
	p = prop(b, p + 8, "name", "arm-io", 7);
	memcpy(b + p, &two, 4);
	memset(b + p + 4, 0, 4);
	p = prop(b, p + 8, "name", chosen ? "chosen" : "other", chosen ? 7 : 6);
	p = prop(b, p, "random-seed", zero, 64);
	return p;
}

static size_t seed_at(uint8_t *b)
{
	for (size_t i = 0; i + 11 < 1024; i++)
		if (!memcmp(b + i, "random-seed", 12))
			return i + 36;
	return 0;
}

static int hex_is(const uint8_t *h, const char *want)
{
	char s[65];
	for (int i = 0; i < 32; i++)
		sprintf(s + 2 * i, "%02x", h[i]);
	return !strcmp(s, want);
}

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
	uint8_t h[32], t[1024], u[1024];
	sha256((const uint8_t *)"abc", 3, h);
	CHECK(hex_is(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc");
	sha256((const uint8_t *)"", 0, h);
	CHECK(hex_is(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty");
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	sha256((const uint8_t *)two, strlen(two), h);
	CHECK(hex_is(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 two blocks");
	const uint8_t e1[16] = { 0x56, 0x4d, 0x9e, 0xf9, 0xe5, 0xf8, 0x45, 0xb3, 1, 2, 3, 4 }, e2[16] = { 1 };
	size_t tn = make_tree(t, 1), s = seed_at(t);
	CHECK(adt_random_seed(t, tn, e1, 16) == 64, "seed filled");
	int zeros = 0;
	for (int i = 0; i < 64; i++)
		zeros += t[s + i] == 0;
	CHECK(zeros < 8, "seed not zeros");
	make_tree(u, 1);
	adt_random_seed(u, tn, e1, 16);
	CHECK(!memcmp(t + s, u + s, 64), "same entropy, same seed");
	make_tree(u, 1);
	adt_random_seed(u, tn, e2, 16);
	CHECK(memcmp(t + s, u + s, 64) != 0, "other entropy, other seed");
	CHECK(memcmp(t + s, t + s + 32, 32) != 0, "the two halves differ");
	size_t on = make_tree(u, 0);
	CHECK(adt_random_seed(u, on, e1, 16) == 0, "no /chosen: left alone");
	make_tree(u, 1);
	CHECK(adt_random_seed(u, s - 10, e1, 16) == 0, "a truncated ADT is left alone");
	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
