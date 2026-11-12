/*
 * The loader's bring-up probes, for a new phone or a new part of one: built only by build.sh bringup, each
 * still behind its own flag. Included by mtkloader.c, after everything they use.
 */

#if TIMER_PROBE
static void gic_cpu_prepare(void)
{
	uint64_t sre, ctlr;

	/*
	 * ICC_CTLR_EL1.EOImode: with it set, ICC_EOIR1_EL1 only drops the priority and the
	 * interrupt stays active until ICC_DIR_EL1 -- and XNU never writes that, so its first
	 * timer interrupt would also be its last. Clear it for whoever runs on this core next.
	 */
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out("  ICC_CTLR_EL1 ");
	out_hex(ctlr);
	out(ctlr & 2 ? " (EOImode=1: EOI does not deactivate)" : " (EOImode=0)");
	__asm__ volatile("msr ICC_CTLR_EL1, %0; isb" : : "r"(ctlr & ~2ULL));
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out(" -> ");
	out_hex(ctlr);
	out("\n");

	__asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
	__asm__ volatile("msr ICC_SRE_EL1, %0; isb" : : "r"(sre | 1));
	__asm__ volatile("msr ICC_AP1R0_EL1, xzr");
	__asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xffUL));
	__asm__ volatile("msr ICC_BPR1_EL1, xzr");
	__asm__ volatile("msr ICC_IGRPEN1_EL1, %0; isb" : : "r"(1UL));
}
#endif

#if TIMER_PROBE
static int timer_try(int ppi, int virt)
{
	uint64_t f = dbg_freq(), now, grp;
	uint32_t before;

	/* Group 1 NS, default priority, enabled: the same as XNU's gic_v3_irq_cpu_init(). */
	wr32(GICR_SGI + 0x080, rd32(GICR_SGI + 0x080) | (1u << ppi));
	wr32(GICR_SGI + 0xd00, rd32(GICR_SGI + 0xd00) & ~(1u << ppi));
	*(volatile uint8_t *)(GICR_SGI + 0x400 + ppi) = 0xa0;
	wr32(GICR_SGI + 0x100, 1u << ppi);
	grp = rd32(GICR_SGI + 0x080);
	wr32(GICR_SGI + 0x380, 1u << ppi); /* nothing of an earlier try left active */
	irq_count = 0;
	irq_intid = 0;

  again:
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	if (virt) {
		__asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(now + f / 10), "r"(1UL));
	} else {
		__asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(now + f / 10), "r"(1UL));
	}
	before = rd32(GICR_SGI + 0x200);
	irq_wait(now + f / 2);

	out(virt ? "  virtual timer  PPI " : "  physical timer PPI ");
	out_dec((uint64_t)ppi);
	out(": IGROUPR0 ");
	out_hex(grp);
	out(grp & (1u << ppi) ? " (group 1)" : " (GROUP 0: not ours)");
	out(", ISPENDR0 ");
	out_hex(before);
	out("->");
	out_hex(rd32(GICR_SGI + 0x200));
	out(", ISACTIVER0 after EOI ");
	out_hex(rd32(GICR_SGI + 0x300));
	out(", irqs ");
	out_dec(irq_count);
	if (irq_count) {
		out(" intid ");
		out_dec(irq_intid);
	}
	out(", ISACTIVER0 ");
	out_hex(rd32(GICR_SGI + 0x300));
	out("\n");
	if (irq_count == 1) {
		goto again;     /* a second one proves the first was deactivated */
	}
	__asm__ volatile("msr cntv_ctl_el0, %0; msr cntp_ctl_el0, %0; isb" : : "r"(2UL));
	/* Leave this PPI as XNU must find it: disabled, not pending, not active. */
	wr32(GICR_SGI + 0x180, 1u << ppi); /* ICENABLER0 */
	wr32(GICR_SGI + 0x280, 1u << ppi); /* ICPENDR0   */
	wr32(GICR_SGI + 0x380, 1u << ppi); /* ICACTIVER0 */
	__asm__ volatile("dsb sy; isb");
	return irq_count != 0;
}

static void timer_probe(void)
{
	out("timer probe at EL1:\n");
	gic_cpu_prepare();
	timer_try(27, 1);
	timer_try(30, 0);
	/* and the CPU interface as it was, so nothing of ours is left in it */
	__asm__ volatile("msr ICC_IGRPEN1_EL1, xzr; msr ICC_AP1R0_EL1, xzr; isb");
	out("  GICR after cleanup: ISENABLER0 ");
	out_hex(rd32(GICR_SGI + 0x100));
	out(" ISPENDR0 ");
	out_hex(rd32(GICR_SGI + 0x200));
	out(" ISACTIVER0 ");
	out_hex(rd32(GICR_SGI + 0x300));
	out("\n");
}
#endif /* TIMER_PROBE */

#if SOC_PROBE
/* MT6781's OVL0 (the Hot 20's OVL0_BASE is disp_ccorr0 there); only OVL0: LK's display path keeps it clocked */
#define PROBE_OVL0          0x14005000ULL
#define PROBE_OVL_L0_SRC_SIZE 0x038      /* height << 16 | width */

static void probe_reg(const char *name, uint64_t pa)
{
	out("probe: ");
	out(name);
	out(" ");
	out_hex(rd32(pa));
	out("\n");
}

static void soc_probe(void)
{
	uint64_t el, mpidr;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	out("probe: EL ");
	out_dec(el >> 2);
	out(" MPIDR ");
	out_hex(mpidr);
	out("\n");
	probe_reg("TOPRGU MODE", TOPRGU_MODE);
	probe_reg("TOPRGU LENGTH", TOPRGU_LENGTH);
	probe_reg("OVL0 SRC_CON", PROBE_OVL0 + OVL_SRC_CON);
	/* each layer's registers are 0x20 above the one before; LK's framebuffer is on one of them */
	for (uint32_t n = 0; n < 4; n++) {
		uint64_t l = PROBE_OVL0 + 0x20 * n;

		out("probe: OVL0 L");
		out_dec(n);
		out(" CON ");
		out_hex(rd32(l + OVL_L0_CON));
		out(" SRC_SIZE ");
		out_hex(rd32(l + PROBE_OVL_L0_SRC_SIZE));
		out(" PITCH ");
		out_hex(rd32(l + OVL_L0_PITCH));
		out(" ADDR ");
		out_hex(rd32(l + OVL_L0_ADDR));
		out("\n");
	}
	probe_reg("marker 0", MARKER_PA);
	probe_reg("marker 1", MARKER_PA + 64);
	probe_reg("marker 2", MARKER_PA + 128);
}
#endif

#if KEY_PROBE
/*
 * Read only. The keypad keeps a bit per key in KP_MEM1..5, clear while the key is down (the spaced device tree
 * maps volume down to its key 1; volume up is the PMIC's "reset key"); the device tree also names GPIO 80 and 81
 * for the volume keys, read here as the input word of pins 64..95 (its offset is a guess from the MT6768).
 */
static void key_probe(void)
{
	uint32_t last[6], now[6];

	for (int i = 0; i < 6; i++) {
		last[i] = 0xdeadbeefu;
	}
	out("key probe: 8 s -- hold volume down, let go, hold volume up\n");
	for (uint32_t t = 0; t < 80; t++) {
		int changed = 0;

		for (int i = 0; i < 5; i++) {
			now[i] = rd32(0x10010004ULL + 4 * i) & 0xffffu;    /* KP_MEM1..5 */
		}
		now[5] = rd32(0x10005220ULL);                             /* GPIO DIN, pins 64..95 */
		for (int i = 0; i < 6; i++) {
			changed |= now[i] != last[i];
			last[i] = now[i];
		}
		if (changed) {
			out("key probe: ");
			out_dec(t * 100);
			out(" ms KP_MEM");
			for (int i = 0; i < 5; i++) {
				out(" ");
				out_hex(now[i]);
			}
			out(" GPIO64-95 ");
			out_hex(now[5]);
			out("\n");
		}
		dbg_delay_ms(100);
	}
}
#endif

#if UFS_PROBE && SOC_IS_MT6781
/*
 * Read only. The MT6781's UFS host controller (ufshci@11270000, "mediatek,mt8183-ufshci": standard UFSHCI
 * registers, JESD223) as LK leaves it: LK brings UFS up early and its last use of it is just before it starts the
 * kernel, with no shutdown in its log. Also LK's first UTP transfer request descriptor and the command descriptor
 * it points to, in DRAM, to see the list LK set up.
 */
static void ufs_probe(void)
{
	static const struct { const char *name; uint32_t at; } regs[] = {
		{ "CAP", 0x00 }, { "VER", 0x08 }, { "IS", 0x20 }, { "IE", 0x24 }, { "HCS", 0x30 }, { "HCE", 0x34 },
		{ "UECPA", 0x38 }, { "UECDL", 0x3c }, { "UECN", 0x40 }, { "UECT", 0x44 }, { "UECDME", 0x48 },
		{ "UTRIACR", 0x4c }, { "UTRLBA", 0x50 }, { "UTRLBAU", 0x54 }, { "UTRLDBR", 0x58 }, { "UTRLRSR", 0x60 },
		{ "UTMRLBA", 0x70 }, { "UTMRLBAU", 0x74 }, { "UTMRLDBR", 0x78 }, { "UTMRLRSR", 0x80 },
	};

	for (uint32_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		out("ufs probe: ");
		out(regs[i].name);
		out(" ");
		out_hex(rd32(UFSHCI_BASE + regs[i].at));
		out("\n");
	}
	uint64_t utrl = rd32(UFSHCI_BASE + 0x50) | (uint64_t)rd32(UFSHCI_BASE + 0x54) << 32;
	if (utrl < 0x40000000ULL || utrl >= 0x140000000ULL) {
		out("ufs probe: no transfer request list in DRAM\n");
		return;
	}
	out("ufs probe: UTRD 0:");
	for (uint32_t w = 0; w < 8; w++) {
		out(" ");
		out_hex(rd32(utrl + 4 * w));
	}
	out("\n");
	uint64_t ucd = (rd32(utrl + 16) & ~0x7fu) | (uint64_t)rd32(utrl + 20) << 32;
	if (ucd >= 0x40000000ULL && ucd < 0x140000000ULL) {
		out("ufs probe: UCD 0 at ");
		out_hex(ucd);
		out(":");
		for (uint32_t w = 0; w < 8; w++) {
			out(" ");
			out_hex(rd32(ucd + 4 * w));
		}
		out("\n");
	}
}

#endif

#if UFS_READ_TEST && UFS_PROBE && SOC_IS_MT6781
/* One 4 KiB read: LBA 1, the GPT header. */
static int ufs_read_test(void)
{
	const uint64_t utrl = ufs_lk_list();
	volatile uint8_t *data = ufs_data;

	if (utrl == 0) {
		return 0;
	}
	if (ufs_lk_read(utrl, 1, 1) == 1) {
		return 1;
	}
	out("ufs read: LBA 1 starts \"");
	for (uint32_t i = 0; i < 8; i++) {
		char c[2] = { (data[i] >= 0x20 && data[i] < 0x7f) ? (char)data[i] : '.', 0 };
		out(c);
	}
	uint64_t alt = ufs_le(data + 32, 8);
	out("\", backup header at LBA ");
	out_dec(alt);
	out(alt == 15620095 ? " (as the GPT says)\n" : " (NOT 15620095)\n");
	return 0;
}
#endif
