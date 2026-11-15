/* Host test of bootrec.c's pure logic (record validation, hash, argument parsing): cc bootrec_test.c -o out/vt */
#include <stdio.h>
#include <string.h>
#define MT6781_SHIM
#include "bootrec.c"

volatile uint32_t *con_fb;
void con_str(const char *l, const char *s) { (void)l; (void)s; }
void con_hex2(const char *l, const char *a, uint32_t va, const char *b, uint32_t vb) { (void)l; (void)a; (void)va; (void)b; (void)vb; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void make(struct bootrec_rec *r, const char *base, const char *cmd)
{
	memset(r, 0, sizeof(*r));
	memcpy(r->magic, BOOTREC_MAGIC, 8);
	r->version = BOOTREC_VERSION;
	r->flags = 1;
	r->base_hash = bootrec_hash(base, BOOTREC_LINE);
	strcpy(r->cmdline, cmd);
	r->line_hash = bootrec_hash(r->cmdline, BOOTREC_LINE);
}

int main(void)
{
	static struct bootrec_rec r;
	const char *base = "debug=0xc mp_ufslun=2 rd=ufsroot0", *want = "debug=0xc mp_ufslun=2 rd=ufsroot0 mp_jpeg=1";
	make(&r, base, want);
	CHECK(bootrec_valid(&r, bootrec_hash(base, BOOTREC_LINE)));
	CHECK(!bootrec_valid(&r, bootrec_hash("other", BOOTREC_LINE)));      /* boot_b reflashed: ignored */
	r.flags = 0; CHECK(!bootrec_valid(&r, r.base_hash)); r.flags = 1;     /* disabled */
	r.cmdline[3] ^= 1; CHECK(!bootrec_valid(&r, r.base_hash)); r.cmdline[3] ^= 1;   /* corrupted */
	r.magic[0] = 'X'; CHECK(!bootrec_valid(&r, r.base_hash)); r.magic[0] = 'M';
	memcpy(r.magic, "VIVA" "BOOT", 8); CHECK(!bootrec_valid(&r, r.base_hash)); memcpy(r.magic, BOOTREC_MAGIC, 8);   /* the old magic: no record */
	r.cmdline[5] = '\n'; r.line_hash = bootrec_hash(r.cmdline, BOOTREC_LINE); CHECK(!bootrec_valid(&r, r.base_hash));
	memset(r.cmdline, 'a', BOOTREC_LINE); r.line_hash = bootrec_hash(r.cmdline, BOOTREC_LINE); CHECK(!bootrec_valid(&r, r.base_hash));
	make(&r, base, want);
	CHECK(!bootrec_safe(&r));
	r.flags = 1 | BOOTREC_SAFE; CHECK(bootrec_safe(&r) && bootrec_valid(&r, r.base_hash));     /* the line still applies */
	r.flags = BOOTREC_SAFE; CHECK(bootrec_safe(&r) && !bootrec_valid(&r, r.base_hash));        /* the mark without a line */
	r.base_hash ^= 1; CHECK(bootrec_safe(&r)); r.base_hash ^= 1;                               /* whatever the payload */
	r.cmdline[3] ^= 1; CHECK(!bootrec_safe(&r)); r.cmdline[3] ^= 1;                            /* torn */
	r.version = 2; CHECK(!bootrec_safe(&r)); r.version = BOOTREC_VERSION;
	r.magic[0] = 'X'; CHECK(!bootrec_safe(&r)); r.magic[0] = 'M';
	memset(r.cmdline, 'a', BOOTREC_LINE); r.line_hash = bootrec_hash(r.cmdline, BOOTREC_LINE); CHECK(!bootrec_safe(&r));
	CHECK(bootrec_hash("", 608) == 0xcbf29ce484222325ULL);
	CHECK(bootrec_hash("a", 608) == 0xaf63dc4c8601ec8cULL);              /* FNV-1a 64 reference value */

	char v[37];
	CHECK(arg("a=1 mp_ufslun=3 x", "mp_ufslun", v, sizeof v) && !strcmp(v, "3"));
	CHECK(!arg("myufslun=3", "mp_ufslun", v, sizeof v));
	CHECK(!arg("ufslunx=3", "mp_ufslun", v, sizeof v));
	CHECK(arg("mp_ufsroot=userdata", "mp_ufsroot", v, sizeof v) && !strcmp(v, "userdata"));
	CHECK(arg("x mp_bootrec=0", "mp_bootrec", v, sizeof v) && v[0] == '0');
	CHECK(!arg("", "mp_ufslun", v, sizeof v));
	printf("%s\n", fails ? "FAILED" : "bootrec host test: all ok");
	return fails != 0;
}
