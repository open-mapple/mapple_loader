/*
 * The ilifw item's check (mtkili.h) on the host: cc -o /tmp/ili_test ili_test.c && /tmp/ili_test [ilifw.bin]
 * A made-up item, good and spoiled; with a file (mtk/tools/mkilifw.py's output), that one too.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GPIO_BASE       0
#define INFRACFG_BASE   0
#define TOPCKGEN_BASE   0
#define CLK_CFG_2       0
#define ILI_RELEASE_RST 1
#define ILI_DOWNLOAD    1
static uint32_t rd32(uint64_t a) { (void)a; return 0; }
static void wr32(uint64_t a, uint32_t v) { (void)a; (void)v; }
static void out(const char *s) { fputs(s, stdout); }
static void out_hex(uint64_t v) { printf("%#llx", (unsigned long long)v); }
static void out_dec(uint64_t v) { printf("%llu", (unsigned long long)v); }
static void dbg_delay_ms(uint32_t ms) { (void)ms; }
static void dbg_delay_us(uint32_t us) { (void)us; }
#include "mtkili.h"

static uint8_t item[64 + 3 * 32] __attribute__((aligned(16)));

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static void seal(void)
{
	uint32_t h = 0x811c9dc5u;

	for (uint32_t i = 16; i < sizeof(item); i++) {
		h = (h ^ item[i]) * 0x01000193u;
	}
	put32(item + 8, h);
}

static void make(void)
{
	memset(item, 0, sizeof(item));
	memcpy(item, "ILFW", 4);
	put32(item + 4, sizeof(item));
	put32(item + 12, 3);
	for (uint32_t i = 0; i < 3; i++) {
		put32(item + 16 + 16 * i, 0x20000 * i);
		put32(item + 20 + 16 * i, 32);
		put32(item + 24 + 16 * i, 0x1234 + i);
		put32(item + 28 + 16 * i, 64 + 32 * i);
		memset(item + 64 + 32 * i, 0xa0 + i, 32);
	}
	seal();
}

static int fails;

static void expect(const char *what, int got, int want)
{
	printf("  %s: %s\n", what, got == want ? "ok" : "FAILED");
	fails += got != want;
}

int main(int argc, char **argv)
{
	make();
	expect("good", ili_fw_load(item, sizeof(item)) && ili_nblocks == 3 && ili_blocks[2].mem == 0x40000 &&
	    ili_blocks[1].crc == 0x1235 && ili_blocks[2].data == item + 128 && strcmp(ili_blocks[0].name, "AP") == 0, 1);
	expect("missing", ili_fw_load(NULL, 0) || ili_nblocks, 0);
	item[100] ^= 1;
	expect("a firmware byte flipped", ili_fw_load(item, sizeof(item)) || ili_nblocks, 0);
	make();
	item[8] ^= 1;
	expect("bad checksum", ili_fw_load(item, sizeof(item)), 0);
	make();
	expect("item cut short", ili_fw_load(item, sizeof(item) - 16), 0);
	item[0] = 'X';
	expect("bad magic", ili_fw_load(item, sizeof(item)), 0);
	make();
	put32(item + 28 + 32, sizeof(item) - 16);
	seal();
	expect("block past the end", ili_fw_load(item, sizeof(item)), 0);
	if (argc > 1) {
		static uint8_t f[256 * 1024] __attribute__((aligned(16)));
		FILE *fp = fopen(argv[1], "rb");
		size_t n = fp ? fread(f, 1, sizeof(f), fp) : 0;

		expect(argv[1], ili_fw_load(f, (uint32_t)n), 1);
		for (uint32_t i = 0; i < ili_nblocks; i++) {
			const struct ili_block *k = &ili_blocks[i];
			const uint8_t *e = k->data + k->len - 4;

			expect(k->name, ((uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3]) == k->crc, 1);
		}
	}
	printf(fails ? "%d FAILED\n" : "all good\n", fails);
	return fails != 0;
}
