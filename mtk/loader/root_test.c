/*
 * The root partition's checks (mtkroot.h) on the host: cc -o /tmp/root_test root_test.c && /tmp/root_test
 * Made-up partitions: the HFS+ check (whole, flash stopped, stale alternate header, too big), the tail's tar (with
 * other members, broken, empty) and which touch firmware wins. On a file standing for the partition (PART bytes; past
 * the file's end reads as zeros): root_test root FILE PART, root_test tail FILE PART.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

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
#include "mtkroot.h"

#define MIB (1ULL << 20)

/* the partition: PART bytes, zeros but for a few pieces (or a file) */
static uint64_t part;
static struct { uint64_t at; const uint8_t *p; uint64_t n; } piece[4];
static int pieces, fd = -1, reads, unread;
static uint64_t unreadable = ~0ULL;     /* reads from here on fail, as a UFS error would */
static uint8_t blk[4096];

static const volatile uint8_t *rd(uint64_t off)
{
	uint64_t b = off & ~4095ULL;

	reads++;
	if (off >= part || off >= unreadable) {
		return 0;
	}
	memset(blk, 0, sizeof(blk));
	if (fd >= 0) {
		(void)pread(fd, blk, sizeof(blk), (off_t)b);
	}
	for (int i = 0; i < pieces; i++) {
		for (uint64_t k = 0; k < 4096; k++) {
			if (b + k >= piece[i].at && b + k < piece[i].at + piece[i].n) {
				blk[k] = piece[i].p[b + k - piece[i].at];
			}
		}
	}
	return blk + (off & 4095);
}

static int fails;

static void expect(const char *what, int ok)
{
	printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
	fails += !ok;
}

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* an HFS+ header block (header at 1024 of a 4 KiB block) */
static void hfs_hdr(uint8_t *b, uint64_t at, char sig, uint32_t date, uint32_t bs, uint32_t total)
{
	uint8_t *h = b + (at & 4095);

	memset(b, 0, 4096);
	h[0] = 'H';
	h[1] = (uint8_t)sig;
	h[3] = sig == '+' ? 4 : 5;
	be32(h + 16, date);
	be32(h + 40, bs);
	be32(h + 44, total);
}

static const char *hfs_case(char sig, uint32_t date, uint32_t alt_date, int alt, uint64_t vol, uint64_t size)
{
	static uint8_t p[4096], a[4096];

	part = size;
	hfs_hdr(p, 1024, sig, date, 4096, (uint32_t)(vol / 4096));
	hfs_hdr(a, vol - 1024, sig, alt_date, 4096, (uint32_t)(vol / 4096));
	piece[0].at = 0, piece[0].p = p, piece[0].n = 4096;
	piece[1].at = (vol - 1024) & ~4095ULL, piece[1].p = a, piece[1].n = 4096;
	pieces = alt ? 2 : 1;
	return root_check(rd, part, &unread);
}

static int why_is(const char *got, const char *start)
{
	printf("    -> %s\n", got ? got : "passes");
	return start ? got && strncmp(got, start, strlen(start)) == 0 : !got;
}

/* ---- the tail: a ustar archive ---- */

static uint8_t tar[64 * 1024], item[64 + 3 * 32] __attribute__((aligned(16)));
static uint32_t tar_len;

static void tar_add(const char *name, char type, const uint8_t *data, uint32_t n)
{
	uint8_t *h = tar + tar_len;
	uint32_t sum = 0;

	memset(h, 0, 512);
	strcpy((char *)h, name);
	sprintf((char *)h + 100, "%07o", 0644);
	sprintf((char *)h + 124, "%011o", n);
	sprintf((char *)h + 136, "%011o", 0);
	h[156] = (uint8_t)type;
	memcpy(h + 257, "ustar\0" "00", 8);
	memset(h + 148, ' ', 8);
	for (int i = 0; i < 512; i++) {
		sum += h[i];
	}
	sprintf((char *)h + 148, "%06o", sum);
	if (data) {
		memcpy(h + 512, data, n);
	}
	tar_len += 512 + (n + 511) / 512 * 512;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* ili_test.c's made-up ilifw item */
static void make_item(void)
{
	uint32_t h = 0x811c9dc5u;

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
	for (uint32_t i = 16; i < sizeof(item); i++) {
		h = (h ^ item[i]) * 0x01000193u;
	}
	put32(item + 8, h);
}

static void tail_set(void)
{
	part = 256 * MIB;
	piece[0].at = part - TAIL_FW_FROM_END, piece[0].p = tar, piece[0].n = sizeof(tar);
	pieces = 1;
}

static uint8_t buf[128 << 10];
static const char *tail_why;

static uint32_t tail(void)
{
	tail_why = 0;
	return tail_ilifw(rd, part, buf, sizeof(buf), &tail_why);
}

static uint8_t big[5000];

static int files(int argc, char **argv)
{
	const char *why = 0;
	uint32_t n;

	fd = open(argv[2], O_RDONLY);
	part = strtoull(argv[3], 0, 0);
	if (fd < 0 || part == 0) {
		fprintf(stderr, "usage: root_test root|tail FILE PART\n");
		return 2;
	}
	if (strcmp(argv[1], "root") == 0) {
		why = root_check(rd, part, &unread);
		printf("root: %s%s (%d reads)\n", why ? why : "passes", unread ? ", unread" : "", reads);
		return why != 0;
	}
	n = tail_ilifw(rd, part, buf, sizeof(buf), &why);
	if (!n) {
		printf("tail: %s\n", why);
		return 1;
	}
	printf("tail: touch/ilifw.bin %u bytes\n", n);
	if (argc > 4) {
		FILE *f = fopen(argv[4], "wb");
		fwrite(buf, 1, n, f);
		fclose(f);
	}
	return !ili_fw_load(buf, n, "tail");
}

int main(int argc, char **argv)
{
	const uint8_t *m;
	uint32_t size;

	if (argc > 3) {
		return files(argc, argv);
	}
	printf("root check\n");
	expect("whole H+ volume, 128 MiB after it", why_is(hfs_case('+', 7, 7, 1, 64 * MIB, 192 * MIB), 0));
	expect("whole HX volume, more room", why_is(hfs_case('X', 7, 7, 1, 64 * MIB, 1024 * MIB), 0));
	expect("flash stopped: no alternate header", why_is(hfs_case('+', 7, 7, 0, 64 * MIB, 192 * MIB), "no alternate"));
	expect("stale alternate header", why_is(hfs_case('+', 7, 8, 1, 64 * MIB, 192 * MIB), "the alternate header is"));
	expect("no 128 MiB after the volume", why_is(hfs_case('+', 7, 7, 1, 65 * MIB, 192 * MIB), "the volume and 128"));
	expect("headers read but wrong: not unread", why_is(hfs_case('+', 7, 8, 1, 64 * MIB, 192 * MIB), "the alternate") &&
	    !unread);
	unreadable = 0;
	expect("first block unread", why_is(hfs_case('+', 7, 7, 1, 64 * MIB, 192 * MIB), "its first block") && unread);
	unreadable = 4096;
	expect("alternate header unread", why_is(hfs_case('+', 7, 7, 1, 64 * MIB, 192 * MIB), "the alternate header's block") &&
	    unread);
	unreadable = ~0ULL;
	expect("volume past the partition", why_is(hfs_case('+', 7, 7, 1, 400 * MIB, 192 * MIB), "the volume and 128"));
	pieces = 0;
	expect("no header", why_is(root_check(rd, part, &unread), "no HFS+") && !unread);

	printf("tail\n");
	make_item();
	memset(big, 0x5a, sizeof(big));
	tar_add("connsys/", '5', 0, 0);
	tar_add("connsys/WIFI_RAM_CODE_soc3_0_1_1.bin", '0', big, sizeof(big));
	tar_add("touch/", '5', 0, 0);
	tar_add("touch/ilifw.bin.old", '0', big, 100);
	tar_add("touch/ilifw.bin", '0', item, sizeof(item));
	tail_set();
	size = tail();
	expect("found among other members", size == sizeof(item) && memcmp(buf, item, size) == 0);
	expect("and it passes the ILFW check", ili_fw_load(buf, size, "tail"));
	tar[tar_len - 512 + 100] ^= 1;  /* a byte of the member's data */
	size = tail();
	expect("a firmware byte flipped: found, fails the ILFW check", size && !ili_fw_load(buf, size, "tail"));
	tar[tar_len - 512 + 100] ^= 1;
	tar[512 + 10] ^= 1;             /* the second member's header */
	expect("a header byte flipped", tail() == 0 && why_is(tail_why, "the firmware region is no tar"));
	tar[512 + 10] ^= 1;
	memset(tar, 0, sizeof(tar));
	tar_len = 0;
	tar_add("touch/ilifw.bin", '0', 0, sizeof(buf) + 1);     /* its header only */
	expect("too big", tail() == 0 && why_is(tail_why, "touch/ilifw.bin is too big"));
	memset(tar, 0, sizeof(tar));
	expect("empty region", tail() == 0 && why_is(tail_why, "no such member"));
	memset(tar, 0xff, sizeof(tar));
	expect("not a tar", tail() == 0 && why_is(tail_why, "the firmware region is no tar"));

	printf("which firmware\n");
	memset(tar, 0, sizeof(tar));
	tar_len = 0;
	tar_add("touch/ilifw.bin", '0', item, sizeof(item));
	reads = 0;
	m = ili_pick((const uint8_t *)"payload!", 8, tail, buf, &size);
	expect("the payload's item wins, the tail not read", m && memcmp(m, "payload!", 8) == 0 && size == 8 && reads == 0);
	m = ili_pick(0, 0, tail, buf, &size);
	expect("no item: the tail's", m == buf && size == sizeof(item));
	memset(tar, 0, sizeof(tar));
	m = ili_pick(0, 0, tail, buf, &size);
	expect("neither: none", m == 0 && size == 0 && !ili_fw_load(m, size, "payload"));

	printf(fails ? "%d FAILED\n" : "all good\n", fails);
	return fails != 0;
}
