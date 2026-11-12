/*
 * mtkloader: boots XNU on the realme 8i (MT6781) from boot_a, and brings Android
 * back afterwards with XNU's log in pstore.
 *
 * The boot image's kernel (tools/chainboot.py) is this program, then Android's own kernel Image
 * at CHAIN_OFFSET, then the XNU payload ("XNUP": kernel, ramdisk). Each boot, by the marker:
 *
 *   none     RUN: load XNU, give it LK's framebuffer, arm the watchdog, start it.
 *   RUNNING  XNU was cut short by the watchdog: copy its log from DRAM into ramoops, start
 *            Android.
 *   DONE     XNU came back to us (panic, early exception, orderly end) and its log was saved
 *            then: start Android.
 *
 * The log ends up in /sys/fs/pstore/console-ramoops-0 under Android. See mtkdbg.h for the
 * screen and ramoops output.
 *
 * Display: the panel is video mode and keeps scanning OVL0 layer 0 (LK's framebuffer). LK
 * enables per-pixel alpha on that layer, and XNU's console writes 0x00rrggbb pixels: fully
 * transparent. So per-pixel alpha is switched off, and XNU's color table gets alpha 0xff besides.
 */
#include <stdint.h>
#include <stddef.h>
#include "mtkdbg.h"
#include "mtkspi.h"
#include "tcm.h"
#include "../../xnuloader/identity.h"   /* made from your IPSW by xnuloader/mkidentity.sh */

/* XNU's view of memory: one contiguous range that nothing else (LK, Android, TEE, ATF, the
 * framebuffer) uses; 0xc0000000-0xfe000000 is free DRAM on this board. */
/* realme 8i (4 GB): the largest free range, up to dramc-rk0 at 0x13ffff000 (2 GB; DRAM ends at 0x140000000) */
#define XNU_PHYS_BASE       0xc0000000ULL
#define XNU_MEM_SIZE        0x7fffc000ULL
#define DRAM_END_GB         5
#define SOC_IS_MT6781       1
/* Where the system image is put, clear of the kernel, as xnuloader does it (RESIDENT_RD_OFFSET). */
#define SYSTEM_PA           (XNU_PHYS_BASE + 0x08000000ULL)
/* boot_args.virtBase, as xnuloader/loader.c has it: the kernel's text is 48 MB above it. */
#define KERNEL_VIRT_BASE    0xfffffff004000000ULL
#define XNU_SLIDE           (XNU_PHYS_BASE - KERNEL_VIRT_BASE)
#define PAGE_SIZE_MAX       0x4000ULL

/* Seconds XNU may run before the watchdog resets into Android; 0: run until reset by hand. */
#ifndef XNU_WDT_SECONDS
#define XNU_WDT_SECONDS     0
#endif
/* Seconds after which the second core takes a snapshot of XNU and resets; 0: no watcher. */
#ifndef XNU_WATCH_SECONDS
#define XNU_WATCH_SECONDS   0
#endif
/* 1: when XNU comes back, the watchdog is restarted, not switched off: it stays the backstop while the log is saved and
 * Android is started (Android's own watchdog driver takes it over); a hang there resets into Android. 0: off. */
#ifndef XNU_RETURN_WDT
#define XNU_RETURN_WDT      0
#endif
/* 1: after XNU comes back, each core's PSCI state (AFFINITY_INFO: reads only), on screen and in the log for ten
 * seconds before Android: whether XNU's other cores were switched off (cpus > 1). Then core 1 switched on again (PSCI
 * CPU_ON into start.S's cpu_off_entry, which switches it straight off), as Linux will. */
#ifndef RETURN_CPUS
#define RETURN_CPUS         0
#endif
/* 1 (MT6781): XNU's other cores end the run in a pen (start.S, xnu_park_pen) instead of switched off, since a core
 * switched off cannot be switched on again here. The pen is a page of its own, the last one before ATF's reservation
 * (0x4ce00000), which nothing uses before Linux; before Android starts it is made a no-map reserved-memory node in LK's
 * device tree (fdt_reserve_pen), so that Linux never maps it either. XNU gets loader_park=. */
#ifndef XNU_PARK
#define XNU_PARK            0
#endif
#define PARK_PA             0x4cdff000ULL
/* 1 (MT6781): at the hand-back, ramoops' console zone made as big as its 0xe0000 allows (ramoops rounds each zone down
 * to a power of two) -- LK's command line in its device tree says ramoops.console_size=0x80000 instead of 0x40000 (in
 * place, the same length), and this loader's console moves to where Android will then look for it (dump records,
 * console, ftrace 4 KB, pmsg 64 KB, in that order). */
#ifndef XNU_BIG_PSTORE
#define XNU_BIG_PSTORE      0
#endif
/* 1 (MT6781): a run that starts SSPM's CPU DVFS (the kernel's MT6781_DVFS, dvfs_step=): at the hand-back no Android
 * this boot -- the log tail stays on screen until the watchdog resets the phone (XNU_RETURN_WDT), so Android starts
 * after a whole reset, SSPM's included. */
#ifndef DVFS_RESET_END
#define DVFS_RESET_END      0
#endif
/* 1: lines of the XNU log that name what the touch screen's start-up goes through (keep_words) are saved even from
 * the part of it that is left out, up to a quarter of the room. */
#ifndef XNU_LOG_KEEP
#define XNU_LOG_KEEP        0
#endif
#define BIG_CONSOLE_PA      0x4d05f000ULL
#define BIG_CONSOLE_SIZE    0x80000u
/* 1: the XNU log is saved up to the kernel's message buffer, which XNU appends last ("--- kernel message buffer ---"):
 * the console's own lines -- tl:, programs' output, launchd's -- make the kept tail instead of the timestamped buffer. */
#ifndef XNU_LOG_CONSOLE
#define XNU_LOG_CONSOLE     0
#endif
#define PARK_NODE           "mapple-pen@4cdff000"
#define PARK_NODE_BYTES     68      /* begin, the name (20), reg (12 + 16), no-map (12), end */
/*
 * How many processors the device tree offers the kernel. It takes them from the head of
 * core_order below, so what is left of that list is nobody's but ours -- the watcher takes the
 * next one. Leaving all eight in the tree and trusting "cpus=N" to stop the kernel sooner is a
 * guess about its parsing; this is not. XNU_EXTRA_ARGS should carry the same number as "cpus=".
 *
 * A watcher on a core the kernel wants is worse than none: PSCI CPU_ON answers "already on", the
 * kernel counts a processor that never checks in, and every rendezvous between cores waits for
 * it -- which is how a 15 minute run stayed on the Apple logo.
 */
#ifndef XNU_CORES
#define XNU_CORES           4
#endif
_Static_assert(XNU_CORES >= 1 && XNU_CORES <= 7, "one core at least, and one left for the watcher");

/*
 * The processors in the order the kernel is offered them: the one this code runs on first (LK
 * leaves us on core 0), then the two Cortex-A75s, which are about twice the Cortex-A55s, then
 * the rest of the A55s. So "cpus=3" already gives iOS both fast cores.
 */
/* 1 (MT6781): the A76s too, in the Hot 20's order (0, 7, 6, 1, ...): they run at whatever clock LK left them at, with
 * nothing here to manage their heat -- for measuring, in short runs. */
#ifndef XNU_BIG_CORES
#define XNU_BIG_CORES       0
#endif
#if SOC_IS_MT6781 && !XNU_BIG_CORES
/* the realme 8i: its six Cortex-A55s first; the two A76s (6, 7) last, their clock is not set up here (big_cores_clock
 * is the Hot 20's) */
static const uint8_t core_order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
#else
static const uint8_t core_order[8] = { 0, 7, 6, 1, 2, 3, 4, 5 };
#endif
#define WATCH_CORE          core_order[XNU_CORES]
/* Checkpoint for XNU to return at (osfmk/arm/arm_init.c LOADER_CHECKPOINT), 0 = none. */
#ifndef XNU_CHECKPOINT
#define XNU_CHECKPOINT      0
#endif
/* loader_return_after: XNU comes back by itself after that long, with its log (loader_return.c) */
#ifndef XNU_RETURN_AFTER
#define XNU_RETURN_AFTER    "0"
#endif
/* 1: always start XNU (tools/xnu-cycle.sh); 0: Android unless asked once (tools/boot-xnu.sh). */
/* 1: read the keypad and the volume keys' GPIO word for 8 s and report what changes (hold the keys); then on as usual. */
#ifndef KEY_PROBE
#define KEY_PROBE           0
#endif
/* 1 (MT6781): with volume down held at the prompt, read the UFS controller's registers and LK's first transfer
 * request and report; then Android. Reads only. */
#ifndef UFS_PROBE
#define UFS_PROBE           0
#endif
/* 1 (with UFS_PROBE): after the probe, one SCSI READ(10) of LBA 1 of LUN 2 (the user unit's GPT header) through LK's
 * transfer list, slot 0, polled; the result checked and reported. A read: nothing on the flash changes. */
#ifndef UFS_READ_TEST
#define UFS_READ_TEST       0
#endif
/* 1 (MT6781): on the way to XNU, find xnusys in the user unit's GPT through LK's transfer list (reads only) and give
 * the kernel's UFS driver its extent and LK's list: mtkufs_lba=, mtkufs_blocks=, mtkufs_lkutrl= on the command line. */
#ifndef XNU_UFS
#define XNU_UFS             0
#endif
#if XNU_UFS && SOC_IS_MT6781
static void xnu_ufs_args(char *cl);
#endif
/* 1 (MT6781): with volume down held at the prompt, the touch controller on SPI4 as LK leaves it, one read with nothing
 * written, and its ID through ICE mode with the MCU running (mtkili.h); then Android. No reset. */
#ifndef ILI_PROBE
#define ILI_PROBE           0
#endif
/* 1 (MT6781): on the way to XNU, the touch controller's firmware sent (mtkili.h: reset released, ID read, download), so
 * that the kernel's MT6781_ILI (touchscreen=3) finds it reporting. */
#ifndef XNU_TOUCH_ILI
#define XNU_TOUCH_ILI       0
#endif
#if XNU_TOUCH_ILI
#define ILI_RELEASE_RST     1
#define ILI_DOWNLOAD        1
#endif
/* 1: XNU is started through viva's shim (the payload's items "shim" and "xnu") instead of by this program: the
 * MAPPLEPL payload where LK put the ramdisk, the touch firmware's table after it (mp_ili_fw=), LK's first framebuffer
 * opaque and scanned, then the shim entered the way LK enters a kernel. No return to this program. */
#ifndef XNU_SHIM
#define XNU_SHIM            0
#endif
/* 1: check at EL1 that the timer interrupts arrive at all (diagnostics, leaves GIC state). */
#ifndef TIMER_PROBE
#define TIMER_PROBE         0
#endif
/* 1: start Android at once, through UART and ramoops only: a new phone's first chain, before its
 * watchdog, display and marker addresses are known (no XNU). */
#ifndef PASS_THROUGH
#define PASS_THROUGH        0
#endif
/* 1 (with PASS_THROUGH): before Android, log what a new SoC's loader needs into ramoops: EL, MPIDR,
 * the watchdog's mode, LK's OVL0 layer, the marker. Reads only. */
#ifndef SOC_PROBE
#define SOC_PROBE           0
#endif
/* 1 (with PASS_THROUGH): once, when Android asks (printf OPENMAPPLE:MARKER-TEST > /dev/pmsg0; reboot), put a
 * pattern in the marker and reset the board; the next boot says if it survived. How: 1 the watchdog as run_xnu
 * arms it, 2 PSCI SYSTEM_RESET, 3 the watchdog with DDR reserve on (MT6781: TOPRGU MODE bit 7). */
#ifndef MENU_DEFAULT_XNU
#define MENU_DEFAULT_XNU    0
#endif
/*
 * 1: the kernel's text console on the screen (-v). It is a slow way to run -- scrolling reads
 * the whole framebuffer back -- and iOS cannot draw while it is on, so a real boot goes without
 * it and the log comes back through the loader instead (v_display = 1 says "graphics mode").
 */
#ifndef XNU_VERBOSE
#define XNU_VERBOSE         0
#endif
/* More for the kernel's command line: cores, the clock (epoch=), whatever a run wants to try. */
/* The serial number iOS is told (the device tree's serial-number): a placeholder, not any real device's. */
#ifndef XNU_SERIAL
#define XNU_SERIAL          "MAPPLE0000000000"
#endif
#ifndef XNU_EXTRA_ARGS
#define XNU_EXTRA_ARGS      "cpus=1"
#endif
/*
 * 1: the system image stays on the eMMC and the kernel reads it as it needs it (emmc_root=1,
 * bsd/dev/mapple_emmc.c), the default; 0: it is read into DRAM here as md0 -- 2.4 GB of the kernel's 4 GB,
 * wired from the start, and three and a half minutes of every boot (run 320).
 */
#ifndef XNU_ROOT_EMMC
#define XNU_ROOT_EMMC       0       /* no eMMC: the root is the payload's RAM disk */
#endif
/* 1 (MT6781, with XNU_UFS): the same on the UFS -- the system image in xnusys, read by the kernel (mtkufs_root=1,
 * mtk/mt6781/drivers/storage/mapple.mt6781_ufs.c); the payload's RAM disk stays md1. */
#ifndef XNU_ROOT_UFS
#define XNU_ROOT_UFS        0
#endif
#if XNU_ROOT_UFS && !(XNU_UFS && SOC_IS_MT6781)
#error "XNU_ROOT_UFS needs XNU_UFS=1 on the MT6781 (the kernel takes xnusys's extent from it)"
#endif
#define XNU_ROOT_FLASH      (XNU_ROOT_EMMC || XNU_ROOT_UFS)
#if XNU_ROOT_EMMC
#define XNU_ROOT_ARG        "rd=md0 emmc_root=1"     /* (rd=md0 for libignition's "ramdisk" boot) */
#elif XNU_ROOT_UFS
#define XNU_ROOT_ARG        "rd=md0 mtkufs=1 mtkufs_root=1"
#else
#define XNU_ROOT_ARG        "rd=md0"
#endif
#if XNU_VERBOSE
#define XNU_BOOT_ARGS       "debug=0x8 serial=3 " XNU_ROOT_ARG " -v " XNU_EXTRA_ARGS " loader_return_after=" XNU_RETURN_AFTER
#else
#define XNU_BOOT_ARGS       "debug=0x8 serial=3 " XNU_ROOT_ARG " " XNU_EXTRA_ARGS " loader_return_after=" XNU_RETURN_AFTER
#endif

#define CHAIN_OFFSET        0x40000u
#define KERNEL_PA           0x40080000ULL
#define CHAIN_STUB_PA       0x4a000000ULL

/* Run state, kept in DRAM across the watchdog reset (three copies: a bit may flip). */
#define MARKER_PA           0x80000000ULL
#define MARK_RUNNING        0x4e52584eu /* "NXRN" */
#define MARK_DONE           0x4e44584eu /* "NXDN" */

#define MH_MAGIC_64         0xfeedfacfu
#define LC_SEGMENT_64       0x19
#define LC_SYMTAB           0x2
#define LC_UNIXTHREAD       0x5
#define PAYLOAD_MAGIC       0x50554e58u /* "XNUP" */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

/* pexpert/pexpert/arm64/boot.h: virtBase comes first (start.s reads BA_VIRT_BASE, BA_PHYS_BASE). */
struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, virtBase) == 8, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, physBase) == 16, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, video) == 40, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, deviceTreeP) == 96, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, commandLine) == 108, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 720, "boot_args");

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

struct symtab_command {
	uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type, n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

struct payload_header {
	uint32_t magic;
	uint32_t size;
};

struct payload_entry {
	char name[16];
	uint32_t size;
	uint32_t reserved;
};

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *kernel_version;     /* kernel.ver (xnuloader/mkpayload.py): the kernel gets it as mapple_kver= */
	const uint8_t *shim;            /* XNU_SHIM: viva's shim, an arm64 Image */
	uint32_t shim_size;
	const uint8_t *xnu;             /* XNU_SHIM: the shim's payload (kernel, device tree, command line) */
	uint32_t xnu_size;
};

/* The XNU symbols the loader works with (kernel virtual addresses, 0 if absent). */
struct xnu_syms {
	uint64_t stub_slot, vc_colors, log, log_len, log_ring_at, log_wrapped, panic_str, panic_args, iso_font,
	    msgbufp, cpu_ttep, live, uart_tx_ready, uart_tx_data, uart_rx_ready, uart_putc, uart_putc_options, log_putc,
	    nprocs, tasks_count, initproc, gic_cpu_init,
	    gic_unhandled_count, gic_last_unhandled, gic_active_iar, host_keep, host_keep_len, host_keep_dropped,
	    hk_keep, hk_keep_len, hk_keep_dropped;
};

static struct xnu_syms xsyms; /* of the kernel that was started; kept for when it comes back */
static const uint8_t *xkernel;  /* its Mach-O file, for symbols */
/* XNU's x29, x30, TCR_EL1 and TTBR1_EL1 when it came back (start.S) */
uint64_t xnu_fp[4];
static volatile uint64_t xnu_back __attribute__((aligned(64))); /* set by xnu_returned() */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16];
	int depth;
};

extern char _start[];
extern char chain_stub[], chain_stub_end[];
extern char xnu_return_stub[];
extern void jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa);
extern void psci_system_reset(void);
extern int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context);
extern void watcher_entry(void);
extern void mmu_on(uint64_t l1_table);
extern void dcache_clean_range(uint64_t va, uint64_t len);
extern uint64_t irq_wait(uint64_t until_ticks);

/* set by the IRQ handler in start.S */
uint64_t irq_count, irq_intid;

static uint64_t lk_dtb;

/* ---- small libc (device memory: plain loops, word copies when aligned) ---- */

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	if (c == 0 && !((uintptr_t)d & 7)) {
		for (; n >= 8; n -= 8, d += 8) {
			*(volatile uint64_t *)d = 0;
		}
	}
	while (n--) {
		*(volatile uint8_t *)d++ = (uint8_t)c;
	}
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (!(((uintptr_t)d | (uintptr_t)s) & 7)) {
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			*(volatile uint64_t *)d = *(const volatile uint64_t *)s;
		}
	}
	while (n--) {
		*(volatile uint8_t *)d++ = *(const volatile uint8_t *)s++;
	}
	return dst;
}

size_t strlen(const char *s)
{
	size_t len = 0;
	while (*s++) {
		len++;
	}
	return len;
}

/* 0 when the first n bytes are the same; device memory, so byte by byte. */
static int memcmp_n(const volatile void *a, const void *b, size_t n)
{
	const volatile uint8_t *x = a;
	const uint8_t *y = b;

	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i]) {
			return 1;
		}
	}
	return 0;
}

int strcmp(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* ---- output: screen + ramoops + UART ---- */

#define MT6768_UART_BASE 0x11002000ULL

static void uart_putc(char c)
{
	uint32_t timeout = 20000;
	while (!(rd32(MT6768_UART_BASE + 0x14) & 0x20) && --timeout) {
	}
	wr32(MT6768_UART_BASE, (uint8_t)c);
}

static void out_putc(char c)
{
	if (c == '\n') {
		uart_putc('\r');
	}
	uart_putc(c);
	dbg_putc(c);
}

static void out(const char *s)
{
	while (*s) {
		out_putc(*s++);
	}
}

static void out_hex(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i = 60;
	out("0x");
	while (i > 0 && !((v >> i) & 0xf)) {
		i -= 4;
	}
	for (; i >= 0; i -= 4) {
		out_putc(hex[(v >> i) & 0xf]);
	}
}

static void out_dec(uint64_t v)
{
	char buf[21];
	int n = 0;
	do {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		out_putc(buf[--n]);
	}
}

#include "mtkconnsys.h"         /* Wi-Fi step 1: Android's device tree on CONNSYS, printed (uses out()) */
#if (ILI_PROBE || XNU_TOUCH_ILI) && SOC_IS_MT6781
#include "mtkili.h"             /* the realme 8i's touch controller (uses out()) */
#endif

/* ---- run state ---- */

static void marker_set(uint32_t v)
{
	for (int i = 0; i < 3; i++) {
		wr32(MARKER_PA + 64 * i, v);
	}
}

static uint32_t marker_get(void)
{
	uint32_t a = rd32(MARKER_PA), b = rd32(MARKER_PA + 64), c = rd32(MARKER_PA + 128);
	return (a == b || a == c) ? a : (b == c ? b : 0);
}

/* ---- the display ---- */

static uint64_t fb_base;
static uint32_t fb_pitch;

/* LK's layer as OVL0 scans it, if it is where LK said; LK's values otherwise. */
static void display_find(void)
{
	uint32_t addr = rd32(OVL0_BASE + OVL_FB(OVL_L0_ADDR));
	uint32_t pitch = rd32(OVL0_BASE + OVL_FB(OVL_L0_PITCH)) & 0xffff;

	fb_base = LK_FB_BASE;
	fb_pitch = LK_FB_PITCH;
	if (addr >= LK_FB_BASE && addr < LK_FB_BASE + LK_FB_SIZE && pitch >= PANEL_W * 4 && pitch <= LK_FB_PITCH_MAX) {
		fb_base = addr;
		fb_pitch = pitch;
	}
}

/* Opaque pixels only from here on: XNU's console writes alpha 0. */
static void display_alpha_off(void)
{
	uint32_t con = rd32(OVL0_BASE + OVL_FB(OVL_L0_CON));
	wr32(OVL0_BASE + OVL_FB(OVL_L0_CON), con & ~(1u << 8));
	out("OVL0 layer CON ");
	out_hex(con);
	out(" -> ");
	out_hex(rd32(OVL0_BASE + OVL_FB(OVL_L0_CON)));

	/*
	 * LK composes two layers: layer 0 is the picture, and layer 3 is its own overlay with the
	 * warning it prints on an unlocked phone ("Orange State: your device has been unlocked"),
	 * from a buffer of its own. Nothing of iOS writes to that buffer, so the words stayed on
	 * top of the home screen. Leave the overlay's layers out of what the hardware composes.
	 */
	uint32_t src = rd32(OVL0_BASE + OVL_SRC_CON);
	wr32(OVL0_BASE + OVL_SRC_CON, src & (1u << OVL_FB_LAYER));
	wr32(DISP_MUTEX_EN, 1);
	wr32(DISP_MUTEX_RUN, 1);
	out(", SRC_CON ");
	out_hex(src);
	out(" -> ");
	out_hex(rd32(OVL0_BASE + OVL_SRC_CON));
	out("\n");
}

/* ---- payload and the kernel's symbols ---- */

static const struct payload_header *payload_find(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	uint64_t off = hdr[6]; /* res3 */
	const struct payload_header *ph = (const struct payload_header *)((uint64_t)_start + off);

	if (off == 0 || (off & 15) || ph->magic != PAYLOAD_MAGIC) {
		return NULL;
	}
	return ph;
}

static int parse_payload(struct payload_info *pi)
{
	const struct payload_header *ph = payload_find();

	memset(pi, 0, sizeof(*pi));
	if (!ph) {
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		if (e->name[0] == 0) {
			break;
		}
		if (strcmp(e->name, "kernel") == 0) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (strcmp(e->name, "ramdisk") == 0) {
			pi->ramdisk = p + sizeof(*e);
			pi->ramdisk_size = e->size;
		} else if (strcmp(e->name, "kernel.ver") == 0 && e->size > 1) {
			pi->kernel_version = (const char *)(p + sizeof(*e));
		} else if (strcmp(e->name, "shim") == 0) {
			pi->shim = p + sizeof(*e);
			pi->shim_size = e->size;
		} else if (strcmp(e->name, "xnu") == 0) {
			pi->xnu = p + sizeof(*e);
			pi->xnu_size = e->size;
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return pi->kernel != NULL && ((const struct mach_header_64 *)pi->kernel)->magic == MH_MAGIC_64;
}

static void find_syms(const uint8_t *kernel, struct xnu_syms *s)
{
	/* in the order of struct xnu_syms: the loop below fills it field by field */
	static const char *const names[] = {
		"_loader_stub_slot", "_vc_colors", "_loader_log", "_loader_log_len", "_loader_log_ring_at",
		"_loader_log_wrapped", "_debugger_panic_str", "_debugger_panic_args", "_iso_font",
		"_msgbufp", "_cpu_ttep", "_loader_live",
		"_vmapple_uart_transmit_ready", "_vmapple_uart_transmit_data",
		"_vmapple_uart_receive_ready", "_uart_putc", "_uart_putc_options", "_loader_log_putc",
		"_nprocs", "_tasks_count", "_initproc", "_gic_v3_irq_cpu_init",
		"_gic_unhandled_count", "_gic_last_unhandled", "_gic_active_iar",
		"_mapple_host_keep", "_mapple_host_keep_len", "_mapple_host_keep_dropped",
		/* (the same in a kernel of the common tree, hakios-xnu-kernel's mtk/mt6768/drivers/gpu/hakios.mali.c) */
		"_mapple_host_keep", "_mapple_host_keep_len", "_mapple_host_keep_dropped",
	};
	_Static_assert(sizeof(struct xnu_syms) == sizeof(names), "every field needs its name, in order");
	uint64_t *slots = (uint64_t *)s;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	const struct symtab_command *st = NULL;

	memset(s, 0, sizeof(*s));
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(kernel + st->symoff);
	const char *strs = (const char *)(kernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		const char *n = strs + sym[i].n_strx;
		if (n[0] != '_' || (n[1] != 'l' && n[1] != 'v' && n[1] != 'd' && n[1] != 'i' && n[1] != 'm' && n[1] != 'c' && n[1] != 'g' && n[1] != 'n' && n[1] != 't' && n[1] != 'r' && n[1] != 'h')) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
			if (!slots[k] && strcmp(n, names[k]) == 0) {
				slots[k] = sym[i].n_value;
			}
		}
	}
}

static uint64_t kva_to_pa(uint64_t va)
{
	return va + XNU_SLIDE;
}


/* ---- device tree for XNU ---- */

static void dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, strlen(name));
	*(volatile uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)strlen(value) + 1);
}

static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, strlen(value));
	dt_prop(w, name, field, size);
}

static void dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void dt_end(struct dt_writer *w)
{
	w->depth--;
}


/* What the iPhone's own device tree says (model, compatible, product ...), from xnuloader. */
static void dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		/* the panel is sRGB, not the iPhone X's P3 (see xnuloader/loader.c) */
		if (strcmp(props[i].name, "artwork-display-gamut") == 0) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		/*
		 * The screen class iOS lays itself out for. The identity was generated with the panel's
		 * own height here, 1640, which no iPhone has: iOS takes an unknown class for a phone
		 * with a home button, and the Control Centre came up from the bottom edge as on an
		 * iPhone 8. The display driver reports the iPhone X's 1125 x 2436 whatever the panel,
		 * so the class has to be the iPhone X's too.
		 */
		if (strcmp(props[i].name, "artwork-device-subtype") == 0) {
			static const uint32_t subtype = 2436;

			dt_prop(w, props[i].name, &subtype, sizeof(subtype));
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* An empty NVRAM image in CHRP format (iokit/Kernel/IONVRAMCHRPHandler.cpp): IOKit needs one. */
#define NVRAM_BANK_SIZE 0x2000u
static uint8_t nvram_image[NVRAM_BANK_SIZE];

static void nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, strlen(name));
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* Panic log region (the device tree's "pram"): DRAM outside XNU's window, kept across a reset. */
#define PANIC_LOG_BASE 0x80100000ULL
#define PANIC_LOG_SIZE 0x80000u

/* As xnuloader/loader.c builds it for x6826b, minus what needs UEFI. */
static uint32_t build_device_tree(uint64_t dt_pa, uint64_t rd_pa, uint64_t rd_size,
    uint64_t sys_pa, uint64_t sys_size)
{
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = dbg_freq();
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(dbg_ticks() >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", XNU_SERIAL, 32);
	dt_prop_padded(&w, "mlb-serial-number", XNU_SERIAL "MLB", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop_str(&w, "darwin-board", "x6826b");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", XNU_PHYS_BASE);
	dt_prop_u64(&w, "dram-size", XNU_MEM_SIZE);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 2);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", "smc");
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	/*
	 * "RAMDisk" becomes /dev/md0, which is the root; "RAMDisk1" becomes /dev/md1. With the big
	 * system image present it is the root, and the payload's small one is the second disk --
	 * the same way round as xnuloader has it.
	 */
	dt_begin(&w, "memory-map");
	if (sys_size != 0) {
		uint64_t sys_reg[2] = { sys_pa, sys_size };
		dt_prop(&w, "RAMDisk", sys_reg, sizeof(sys_reg));
		if (rd_size != 0) {
			uint64_t rd_reg[2] = { rd_pa, rd_size };
			dt_prop(&w, "RAMDisk1", rd_reg, sizeof(rd_reg));
		}
	} else if (rd_size != 0) {
		uint64_t rd_reg[2] = { rd_pa, rd_size };
		/* (with the system image on the eMMC the payload stays md1, where stage0 mounts it) */
		dt_prop(&w, XNU_ROOT_FLASH ? "RAMDisk1" : "RAMDisk", rd_reg, sizeof(rd_reg));
	}
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	dt_begin(&w, "cpus");
	for (int i = 0; i < XNU_CORES; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		int core = core_order[i];

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)(core << 8));
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (core < 6) { /* cores 0-5 are the Cortex-A55s, 6 and 7 the A75s */
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", "x6826b");
	uint64_t ranges[3] = { 0, 0x0c000000, 0x14000000 };
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { 0x0c000000 - 0x0c000000, 0x10000, 0x0c040000 - 0x0c000000, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);

	/* The console the kernel is told about; its driver is patched in run_xnu(). */
	dt_begin(&w, "uart0");
	dt_prop_str(&w, "device_type", "uart");
	dt_prop_str(&w, "compatible", "mediatek,mt6577-uart");
	uint64_t uart_reg[2] = { 0x11002000 - 0x0c000000, 0x1000 };
	dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
	dt_prop_u32(&w, "boot-console", 1);
	dt_end(&w);

	dt_end(&w);
	dt_end(&w);
	return (uint32_t)(w.cur - w.base);
}

/* ---- leaving: Android, or a reset ---- */

static void __attribute__((noreturn)) reset_board(void)
{
	psci_system_reset();
	wdt_reset();
}


/* IMG, an arm64 Image, copied down to where LK puts a kernel and entered as LK enters one (x0: LK's tree). */
static void __attribute__((noreturn)) chain_image(const volatile uint32_t *img, uint64_t image_size)
{
	volatile uint32_t *dst = (volatile uint32_t *)CHAIN_STUB_PA;
	const volatile uint32_t *src = (const volatile uint32_t *)chain_stub;
	for (uint64_t i = 0; i < (uint64_t)(chain_stub_end - chain_stub) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)CHAIN_STUB_PA;
	stub(lk_dtb, KERNEL_PA, (uint64_t)img, image_size, KERNEL_PA);
	__builtin_unreachable();
}

static void __attribute__((noreturn)) chain_android(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size = (hdr[5] + 7) & ~7ULL; /* res2 */

	marker_set(0);
	if (image_size == 0 || img[14] != 0x644d5241u) { /* "ARM\x64" */
		out("no Android kernel in this image: reset\n");
		dbg_delay_ms(3000);
		reset_board();
	}
	out("starting Android\n");
	chain_image(img, image_size);
}

void loader_fault(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	fbc.fg = 0xffff4040u;
	out("\n*** loader fault: vector ");
	out_hex(vec);
	out(" ESR ");
	out_hex(esr);
	out(" ELR ");
	out_hex(elr);
	out(" FAR ");
	out_hex(far);
	out("\n");
	dbg_delay_ms(5000);
	chain_android();
}

/* ---- XNU's log into ramoops ---- */

struct span {
	const volatile char *p;
	uint64_t n;
};

/* A line of the log: [at, *end), *end past its '\n'; returned, where it starts once a "[ time ]: " in front is skipped. */
static uint64_t log_line(const volatile char *p, uint64_t n, uint64_t at, uint64_t *end)
{
	uint64_t e = at, key = at;

	while (e < n && p[e] != '\n') {
		e++;
	}
	*end = e < n ? e + 1 : n;
	if (p[at] == '[') {
		for (uint64_t i = at + 1; i + 2 < e && i < at + 24; i++) {
			if (p[i] == ']' && p[i + 1] == ':' && p[i + 2] == ' ') {
				key = i + 3;
				break;
			}
		}
	}
	return key;
}

static int same_line(const volatile char *p, uint64_t k1, uint64_t e1, uint64_t k2, uint64_t e2)
{
	if (e1 - k1 != e2 - k2) {
		return 0;
	}
	for (uint64_t i = 0; i < e1 - k1; i++) {
		if (p[k1 + i] != p[k2 + i]) {
			return 0;
		}
	}
	return 1;
}

/* Where the saved log is: counting (once, to know how long it comes out) or writing its start and its end -- and, with
 * XNU_LOG_KEEP, lines of the part in between (kept: their bytes, as far as cap allows; keep_line: this one goes in). */
struct save_at {
	uint64_t at, head, skip_to, kept, cap;
	int counting, keep_line;
};

static void save_c(struct save_at *s, char c)
{
	if (!s->counting) {
		if (s->at == s->head && s->head < s->skip_to) {
			dbg_puts("\n\n[... ");
			dbg_dec(s->skip_to - s->head);
			dbg_puts(XNU_LOG_KEEP ? " bytes of the XNU log left out here, but for lines with \"mapple: input\", \"backboardd[\", "
			    "\"still waiting\", \"SpringBoard[\" or ps -A's ...]\n\n" :
			    " bytes of the XNU log left out here ...]\n\n");
		}
		if (s->at < s->head || s->at >= s->skip_to || s->keep_line) {
			plog_putc(c ? c : ' ');
		}
	}
	s->at++;
}

#if XNU_LOG_KEEP
static const char *const keep_words[] = { "mapple: input", "backboardd[", "still waiting", "SpringBoard[", "PID TTY",
	" ?? " };   /* the last two: ps -A */

/* whether [from, to) has one of keep_words in it */
static int keep_this(const volatile char *p, uint64_t from, uint64_t to)
{
	for (unsigned w = 0; w < sizeof(keep_words) / sizeof(keep_words[0]); w++) {
		uint64_t n = 0;

		while (keep_words[w][n]) {
			n++;
		}
		for (uint64_t i = from; i + n <= to; i++) {
			if (p[i] == keep_words[w][0] && memcmp_n(p + i, keep_words[w], n) == 0) {
				return 1;
			}
		}
	}
	return 0;
}
#endif

/* The spans as they are saved: a run of lines that are the same (apart from a "[ time ]: " in front) as the first of
 * them, with " (xN)". */
static void save_pass(struct save_at *s, const struct span *sp, int nsp)
{
	for (int i = 0; i < nsp; i++) {
		const volatile char *p = sp[i].p;
		uint64_t n = sp[i].n;

		for (uint64_t at = 0; at < n;) {
			uint64_t end, next, end2, count = 1, key = log_line(p, n, at, &end), stop;

			for (next = end; next < n; next = end2) {
				uint64_t key2 = log_line(p, n, next, &end2);

				if (!same_line(p, key, end, key2, end2)) {
					break;
				}
				count++;
			}
			stop = count > 1 && p[end - 1] == '\n' ? end - 1 : end;
#if XNU_LOG_KEEP
			s->keep_line = 0;
			if (s->kept + (end - at) + 8 <= s->cap && keep_this(p, at, end) &&
			    (s->counting || (s->at >= s->head && s->at < s->skip_to))) {
				s->kept += (end - at) + 8;
				s->keep_line = !s->counting;
			}
#endif
			for (uint64_t k = at; k < stop; k++) {
				save_c(s, p[k]);
			}
			if (count > 1) {
				char d[24];
				int nd = 0;

				do {
					d[nd++] = (char)('0' + count % 10);
					count /= 10;
				} while (count);
				save_c(s, ' ');
				save_c(s, '(');
				save_c(s, 'x');
				while (nd) {
					save_c(s, d[--nd]);
				}
				save_c(s, ')');
				if (stop != end) {
					save_c(s, '\n');
				}
			}
			at = next;
		}
	}
	s->keep_line = 0;
}

/* Append spans (in order) to the ramoops console, folded (save_pass): all of it if it fits, else its start and as much
 * of its end as there is room for. */
static void save_log(const struct span *sp, int nsp)
{
	struct save_at s = { 0 };
	uint64_t total, room, head, tail;

	room = PLOG_CAP - PLOG_HDR->size;
	room = room > 1024 ? room - 1024 : 0;
	s.counting = 1;
	s.cap = XNU_LOG_KEEP ? room / 4 : 0;
	save_pass(&s, sp, nsp);
	total = s.at;
	if (total > room) {
		room -= s.kept;         /* the kept lines' share (counted wherever they are: a little room may go unused) */
	}
	head = total <= room ? total : room / 4;
	tail = total <= room ? 0 : room - head;

	uint64_t fb = fbc.base;
	fbc.base = 0; /* the screen gets the tail, separately */
	s.at = 0;
	s.head = head;
	s.skip_to = total - tail;
	s.kept = 0;
	s.counting = 0;
	save_pass(&s, sp, nsp);
	plog_putc('\n');
	fbc.base = fb;
}

#if XNU_BIG_PSTORE && SOC_IS_MT6781
/* ramoops' console made BIG_CONSOLE_SIZE in LK's command line, and this loader's console moved there with what it holds */
static void plog_grow(uint64_t dtb)
{
	static const char key[] = "ramoops.console_size=0x40000";
	volatile char *b = (volatile char *)dtb;
	const volatile uint8_t *t = (const volatile uint8_t *)(dtb + 4);
	uint32_t total = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
	uint64_t hit = 0;
	int hits = 0;
	uint32_t start = PLOG_HDR->start, size = PLOG_HDR->size, cap = PLOG_CAP;
	volatile char *to = (volatile char *)(BIG_CONSOLE_PA + sizeof(struct ramoops_hdr));

	for (uint64_t i = 0; i + sizeof(key) - 1 <= total; i++) {
		if (b[i] == 'r' && memcmp_n(b + i, key, sizeof(key) - 1) == 0) {
			hit = i;
			hits++;
		}
	}
	if (hits != 1 || PLOG_HDR->sig != RAMOOPS_SIG || size > cap || start > cap) {
		out("pstore: LK's command line not changed (ramoops.console_size=0x40000 found ");
		out_dec((uint64_t)hits);
		out(" times): the console stays 256 KB\n");
		return;
	}
	b[hit + sizeof(key) - 1 - 5] = '8';                    /* 0x40000 -> 0x80000 */
	for (uint32_t j = 0; j < size; j++) {                   /* oldest first (lands below the old zone) */
		to[j] = PLOG_DATA[(start + cap - size + j) % cap];
	}
	plog_base = BIG_CONSOLE_PA;
	plog_zone = BIG_CONSOLE_SIZE;
	PLOG_HDR->sig = RAMOOPS_SIG;
	PLOG_HDR->start = size;
	PLOG_HDR->size = size;
	__asm__ volatile("dsb sy");
	out("pstore: ramoops' console made ");
	out_hex(BIG_CONSOLE_SIZE);
	out(" at ");
	out_hex(BIG_CONSOLE_PA);
	out(" (LK's command line changed)\n");
}
#endif

/*
 * The kernel's lines for the host (kern.mapple_host_keep, bsd/dev/mapple_mali.c) into ramoops' pmsg
 * zone, whole: Android shows them as /sys/fs/pstore/pmsg-ramoops-0, and tools/xnu-cycle.sh appends them
 * to the run's log. Called on each way back from XNU, after the boot request in that zone was taken.
 */
static void save_host_keep(const struct xnu_syms *s)
{
	static const char head[] = "=== kept for the host by the kernel: ";
	volatile struct ramoops_hdr *h = (volatile struct ramoops_hdr *)RAMOOPS_PMSG;
	volatile char *d = (volatile char *)(RAMOOPS_PMSG + sizeof(struct ramoops_hdr));
	uint32_t cap = RAMOOPS_PMSG_SIZE - sizeof(struct ramoops_hdr), at = 0, len, dropped;
	char num[12];
	int n;

	uint64_t keep_va = s->host_keep ? s->host_keep : s->hk_keep;
	uint64_t len_va = s->host_keep ? s->host_keep_len : s->hk_keep_len;
	uint64_t dropped_va = s->host_keep ? s->host_keep_dropped : s->hk_keep_dropped;

	if (!keep_va || !len_va) {
		return;
	}
	len = rd32(kva_to_pa(len_va));
	dropped = dropped_va ? rd32(kva_to_pa(dropped_va)) : 0;
	for (const char *c = head; *c; c++) {
		d[at++] = *c;
	}
	for (int part = 0; part < 2; part++) {
		uint32_t v = part ? dropped : len;
		n = 0;
		do {
			num[n++] = (char)('0' + v % 10);
			v /= 10;
		} while (v);
		while (n) {
			d[at++] = num[--n];
		}
		const char *tail = part ? " dropped) ===\n" : " bytes (";
		for (const char *c = tail; *c; c++) {
			d[at++] = *c;
		}
	}
	const volatile char *keep = (const volatile char *)kva_to_pa(keep_va);
	for (uint32_t i = 0; i < len && at < cap; i++) {
		d[at++] = keep[i];
	}
	h->sig = RAMOOPS_SIG;
	h->start = at;          /* not wrapped: Linux takes data[start..size) then data[0..start) */
	h->size = at;
	dcache_clean_range(RAMOOPS_PMSG, RAMOOPS_PMSG_SIZE);
	out("host_keep: ");
	out_dec(len);
	out(" bytes into pmsg, ");
	out_dec(dropped);
	out(" lines dropped\n");
}

/* The last lines of what is in ramoops, on the lower part of the screen. */
static void show_log_tail(uint32_t lines)
{
	uint32_t size = PLOG_HDR->size, start = PLOG_HDR->start, n = 0;
	uint32_t top = PANEL_H - (lines + 1) * 16;
	uint64_t i;

	if (!fbc.base || size == 0 || size != start) { /* only a log that has not wrapped */
		return;
	}
	for (i = size; i > 0 && n <= lines; i--) {
		n += PLOG_DATA[i - 1] == '\n';
	}
	fb_rect(0, top - 4, PANEL_W, PANEL_H - top + 4, 0xff000000u);
	fb_rect(0, top - 4, PANEL_W, 2, 0xffff3030u);
	fbc.top = fbc.y = top;
	fbc.x = 0;
	fbc.scale = 1;
	for (; i < size; i++) {
		fbcon_putc(PLOG_DATA[i]);
	}
}

/*
 * A kernel virtual address to physical through XNU's own page tables (TTBR1_EL1; 4K or 16K
 * granule), as they were when it came back. 0 if not mapped.
 */
static uint64_t xnu_va_to_pa(uint64_t va)
{
	uint64_t tcr = xnu_fp[2], ttbr1 = xnu_fp[3];
	uint32_t t1sz = (tcr >> 16) & 0x3f, tg1 = (tcr >> 30) & 3;
	uint32_t g = tg1 == 2 ? 12 : tg1 == 1 ? 14 : 0;
	if (!g || t1sz < 16) {
		return 0;
	}
	uint32_t vabits = 64 - t1sz, per = g - 3;
	uint32_t levels = (vabits - g + per - 1) / per;
	uint64_t table = ttbr1 & 0x0000fffffffffffeULL & ~((1ULL << g) - 1);
	for (uint32_t l = 0; l < levels; l++) {
		uint32_t shift = g + per * (levels - 1 - l);
		uint64_t idx = (va >> shift) & ((1ULL << per) - 1);
		if (l == 0) {
			idx &= (1ULL << (vabits - shift)) - 1;
		}
		uint64_t d = *(volatile uint64_t *)(table + idx * 8);
		if (!(d & 1)) {
			return 0;
		}
		uint64_t oa = d & 0x0000ffffffffffffULL & ~((1ULL << g) - 1);
		if (l == levels - 1 || !(d & 2)) { /* page, or block */
			uint64_t span = 1ULL << shift;
			return (oa & ~(span - 1)) | (va & (span - 1));
		}
		table = oa;
	}
	return 0;
}

static uint64_t xnu_rd64(uint64_t va)
{
	uint64_t pa = xnu_va_to_pa(va);
	return pa && !(pa & 7) ? *(volatile uint64_t *)pa : 0;
}

static char xnu_rd8(uint64_t va)
{
	uint64_t pa = xnu_va_to_pa(va);
	return pa ? *(volatile char *)pa : 0;
}

static void out_xnu_str(uint64_t va, int max)
{
	char c;
	for (int i = 0; i < max && (c = xnu_rd8(va + i)) != 0; i++) {
		out_putc(c);
	}
}

/* AAPCS64 va_list, as debugger_panic_args points at it */
struct xnu_va_list {
	uint64_t stack, gr_top, vr_top;
	int32_t gr_offs, vr_offs;
};

static uint64_t va_next(struct xnu_va_list *ap)
{
	uint64_t v;
	if (ap->gr_offs < 0) {
		v = xnu_rd64(ap->gr_top + (int64_t)ap->gr_offs);
		ap->gr_offs += 8;
	} else {
		v = xnu_rd64(ap->stack);
		ap->stack += 8;
	}
	return v;
}

/* The panic message: debugger_panic_str formatted with debugger_panic_args (integers in hex). */
static void show_panic_string(const struct xnu_syms *x)
{
	uint64_t fmt = x->panic_str ? xnu_rd64(x->panic_str) : 0;
	uint64_t argp = x->panic_args ? xnu_rd64(x->panic_args) : 0;
	struct xnu_va_list ap = { 0 };

	if (!fmt) {
		out("(no panic string)\n");
		return;
	}
	if (argp) {
		ap.stack = xnu_rd64(argp);
		ap.gr_top = xnu_rd64(argp + 8);
		ap.vr_top = xnu_rd64(argp + 16);
		uint64_t offs = xnu_rd64(argp + 24);
		ap.gr_offs = (int32_t)offs;
		ap.vr_offs = (int32_t)(offs >> 32);
	}
	out("panic: ");
	for (int i = 0; i < 1024; i++) {
		char c = xnu_rd8(fmt + i);
		if (!c) {
			break;
		}
		if (c != '%') {
			out_putc(c);
			continue;
		}
		/* flags, width, precision, length */
		do {
			c = xnu_rd8(fmt + ++i);
		} while (c && ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == ' ' || c == '#' || c == '.' ||
		    c == 'l' || c == 'h' || c == 'z' || c == 'j' || c == 't' || c == 'q' || c == '*'));
		if (!argp && c != '%') {
			out("<?>");
			continue;
		}
		switch (c) {
		case '%':
			out_putc('%');
			break;
		case 's':
			out_xnu_str(va_next(&ap), 200);
			break;
		case 'c':
			out_putc((char)va_next(&ap));
			break;
		case 'd':
		case 'i':
		case 'u': {
			uint64_t v = va_next(&ap);
			out_dec(v);
			out("(");
			out_hex(v);
			out(")");
			break;
		}
		case 0:
			i = 1024;
			break;
		default:
			out_hex(va_next(&ap));
			break;
		}
	}
	out("\n");
}

/* The symbol at or below a kernel address, from the Mach-O symbol table: "name+0xoff". */
static void out_sym(uint64_t addr)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)xkernel;
	const struct load_command *lc;
	const struct symtab_command *st = NULL;
	uint64_t best = 0;
	const char *best_name = NULL;

	out_hex(addr);
	if (!xkernel) {
		return;
	}
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(xkernel + st->symoff);
	const char *strs = (const char *)(xkernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		if ((sym[i].n_type & 0x0e) == 0x0e && sym[i].n_value <= addr && sym[i].n_value > best) { /* N_SECT */
			best = sym[i].n_value;
			best_name = strs + sym[i].n_strx;
		}
	}
	if (best_name && addr - best < 0x100000) {
		out(" ");
		out(best_name);
		out("+");
		out_hex(addr - best);
	}
}

static void show_backtrace(void)
{
	uint64_t fp = xnu_fp[0];

	out("TCR_EL1 ");
	out_hex(xnu_fp[2]);
	out(" TTBR1_EL1 ");
	out_hex(xnu_fp[3]);
	out(" fp ");
	out_hex(fp);
	out(" -> pa ");
	out_hex(xnu_va_to_pa(fp));
	out(" sym debugger_panic_str -> pa ");
	out_hex(xnu_va_to_pa(xsyms.panic_str));
	out(" (slide: ");
	out_hex(kva_to_pa(xsyms.panic_str));
	out(")\nbacktrace:\n  lr ");
	out_sym(xnu_fp[1]);
	out("\n");
	for (int i = 0; i < 24 && fp; i++) {
		uint64_t lr = xnu_rd64(fp + 8);
		if (!lr) {
			break;
		}
		out("  ");
		out_sym(lr);
		out("\n");
		fp = xnu_rd64(fp);
	}
}

/*
 * What XNU's text console shows, read back from the framebuffer: each 8x16 cell is looked up in
 * XNU's own font (iso_font, 16 bytes a character, bit 0 the leftmost pixel; video_console.c).
 * Proof that the text reached the screen, and a copy of it.
 */
static void read_screen(void)
{
	static uint32_t font_hash[256];
	uint64_t font = xsyms.iso_font ? kva_to_pa(xsyms.iso_font) : 0;
	const uint32_t cols = PANEL_W / 8, rows = PANEL_H / 16;
	uint32_t lit_cells = 0, known = 0, last_row = 0;
	static char line[PANEL_W / 8 + 1];

	if (!font || !fb_base) {
		return;
	}
	for (int ch = 0; ch < 256; ch++) {
		uint32_t h = 0;
		for (int r = 0; r < 16; r++) {
			h = h * 0x01000193u ^ *(volatile uint8_t *)(font + ch * 16 + r);
		}
		font_hash[ch] = h;
	}
	for (uint32_t row = 0; row < rows; row++) {
		for (uint32_t col = 0; col < cols; col++) {
			uint8_t bits[16];
			uint32_t h = 0, any = 0;
			for (uint32_t r = 0; r < 16; r++) {
				const volatile uint32_t *px = (const volatile uint32_t *)(fb_base + (uint64_t)(row * 16 + r) * fb_pitch) + col * 8;
				uint8_t b = 0;
				for (uint32_t x = 0; x < 8; x++) {
					uint32_t p = px[x];
					uint32_t lum = ((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff);
					if (lum > 3 * 0x60) {
						b |= (uint8_t)(1u << x);
					}
				}
				bits[r] = b;
				any |= b;
			}
			char out_c = ' ';
			if (any) {
				lit_cells++;
				for (int inv = 0; inv < 2 && out_c == ' '; inv++) {
					h = 0;
					for (int r = 0; r < 16; r++) {
						h = h * 0x01000193u ^ (uint8_t)(inv ? ~bits[r] : bits[r]);
					}
					for (int ch = 0x21; ch < 0x7f; ch++) {
						if (font_hash[ch] == h) {
							out_c = (char)ch;
							known++;
							break;
						}
					}
				}
				if (out_c == ' ') {
					out_c = '#';
				}
			}
			line[col] = out_c;
		}
		line[cols] = 0;
		/* trailing spaces off; blank rows kept short */
		int n = (int)cols;
		while (n > 0 && line[n - 1] == ' ') {
			n--;
		}
		line[n] = 0;
		if (n) {
			last_row = row + 1;
		}
		dbg_puts(line);
		dbg_putc('\n');
	}
	dbg_puts("[screen: ");
	dbg_dec(lit_cells);
	dbg_puts(" cells lit, ");
	dbg_dec(known);
	dbg_puts(" read as characters, last text row ");
	dbg_dec(last_row);
	dbg_puts("]\n");
}

/*
 * XNU is back (start.S xnu_return_stub): MMU off, our vectors, our stack. Its log was
 * linearized and written back to RAM by XNU. Save it, show its end, then reset into Android.
 */
#if (XNU_PARK || XNU_SHIM) && SOC_IS_MT6781
/* LK's device tree: big-endian, read byte by byte (device memory) */
#define FDT_BEGIN_NODE      1
#define FDT_END_NODE        2
#define FDT_PROP            3
#define FDT_NOP             4
#define FDT_END             9

static uint32_t fdt_get(uint64_t at)
{
	const volatile uint8_t *p = (const volatile uint8_t *)at;

	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
#endif

#if XNU_SHIM && SOC_IS_MT6781
/* where a property of a node just below the root of LK's tree is, or 0 */
static uint64_t lk_prop(uint64_t dtb, const char *node, const char *prop, uint32_t *len)
{
	uint64_t at = fdt_get(dtb + 8), end = at + fdt_get(dtb + 36), str = dtb + fdt_get(dtb + 12), nn = 0, pn = 0;
	int depth = 0, inside = 0;

	while (node[nn]) {
		nn++;
	}
	while (prop[pn]) {
		pn++;
	}
	while (fdt_get(dtb) == 0xd00dfeedu && at < end) {
		uint32_t tok = fdt_get(dtb + at);

		at += 4;
		if (tok == FDT_BEGIN_NODE) {
			uint64_t n = 0;

			while (((const volatile char *)dtb)[at + n]) {
				n++;
			}
			depth++;
			if (depth == 2 && n == nn && memcmp_n((const volatile char *)dtb + at, node, n) == 0) {
				inside = 1;
			}
			at += (n + 1 + 3) & ~3ULL;
		} else if (tok == FDT_END_NODE) {
			if (inside && depth == 2) {
				return 0;
			}
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t l = fdt_get(dtb + at), nameoff = fdt_get(dtb + at + 4);

			if (inside && depth == 2 && memcmp_n((const volatile char *)str + nameoff, prop, pn + 1) == 0) {
				*len = l;
				return dtb + at + 8;
			}
			at += 8 + ((l + 3) & ~3u);
		} else if (tok != FDT_NOP) {
			break;
		}
	}
	return 0;
}
#endif

#if XNU_SHIM && SOC_IS_MT6781
static uint32_t mp_lvts_golden;                    /* 0: not known */

/* " mp_lvts_golden=<n>" onto the kernel's command line, and the chip's speed bin (efuse word 7) said; the devinfo
 * words are LK's memory as it is (little-endian), not the tree's big-endian cells. With them, " mp_lvts_r=r<hex>.<hex>..."
 * (a string: a number would not hold nine): the nine sensors' calibration counts (count_r, bits 23..0 of efuse words
 * 118, 119, 190..196, in MediaTek's sensor order), for the kernel's calibration step. */
static void lvts_args(char *cl)
{
	uint32_t len = 0;
	uint64_t info = lk_prop(lk_dtb, "chosen", "atag,devinfo", &len);
	char *at = cl;

	if (info == 0 || len < 4 * 114) {
		out("lvts: no atag,devinfo in LK's tree (or too short): no golden value, no mp_lvts_golden\n");
		return;
	}
	out("lvts: LK's devinfo: ");
	out_dec(rd32(info));
	out(" words; word 7 (speed bin) ");
	out_hex(rd32(info + 4 * 8));
	out(", segment ");
	out_dec(rd32(info + 4 * 8) & 0xf);
	out("; word 112 ");
	out_hex(rd32(info + 4 * 113));
	mp_lvts_golden = rd32(info + 4 * 113) & 0xff;
	out(", golden ");
	out_dec(mp_lvts_golden);
	out("\n");
	if (mp_lvts_golden == 0) {
		return;
	}
	while (*at) {
		at++;
	}
	if ((uint32_t)(at - cl) + 90 >= BOOT_LINE_LENGTH) {
		out("lvts: no room on the command line; no mp_lvts_golden, no mp_lvts_r\n");
		return;
	}
	for (const char *s = " mp_lvts_golden="; *s; s++) {
		*at++ = *s;
	}
	if (mp_lvts_golden >= 100) {
		*at++ = (char)('0' + mp_lvts_golden / 100);
	}
	if (mp_lvts_golden >= 10) {
		*at++ = (char)('0' + mp_lvts_golden / 10 % 10);
	}
	*at++ = (char)('0' + mp_lvts_golden % 10);
	*at = 0;
	if (len < 4 * 198) {
		out("lvts: devinfo too short for the calibration counts: no mp_lvts_r\n");
		return;
	}
	out("lvts: count_r:");
	for (const char *s = " mp_lvts_r=r"; *s; s++) {
		*at++ = *s;
	}
	for (uint32_t i = 0; i < 9; i++) {
		uint32_t word = i < 2 ? 118 + i : 190 + (i - 2), v = rd32(info + 4 * (word + 1)) & 0xffffff;
		int shown = 0;

		out(" ");
		out_hex(v);
		if (i) {
			*at++ = '.';
		}
		for (int sh = 20; sh >= 0; sh -= 4) {
			uint32_t d = (v >> sh) & 0xf;

			if (d || shown || sh == 0) {
				*at++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
				shown = 1;
			}
		}
	}
	*at = 0;
	out("\n");
}

/*
 * For the kernel's DVFS: LK's three operating-point tables (dvfsp@0011bc00's L-table, B-table, CCI-table: 16 rows of
 * MHz, voltage step, post divider, clock divider each), copied to tbl as they are and " mp_dvfs_tbl=<address>" on the
 * command line (physical, as mp_ili_fw=). Nothing if a table is missing or not 64 cells.
 */
#define DVFS_TBL_WORDS      (3 * 64)

static void dvfs_args(char *cl, uint32_t *tbl)
{
	static const char *const names[3] = { "L-table", "B-table", "CCI-table" };
	static const char key[] = " mp_dvfs_tbl=";
	uint64_t v = (uint64_t)(uintptr_t)tbl;
	char d[24];
	uint32_t at = 0, n = 0;

	for (uint32_t t = 0; t < 3; t++) {
		uint32_t len = 0;
		uint64_t cells = lk_prop(lk_dtb, "dvfsp@0011bc00", names[t], &len);

		if (cells == 0 || len != 64 * 4) {
			out("dvfs: LK's ");
			out(names[t]);
			out(" missing or not 64 cells: no mp_dvfs_tbl\n");
			return;
		}
		for (uint32_t w = 0; w < 64; w++) {
			tbl[64 * t + w] = fdt_get(cells + 4 * w);
		}
	}
	while (cl[at]) {
		at++;
	}
	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	if (at + sizeof(key) - 1 + n >= BOOT_LINE_LENGTH) {
		out("dvfs: no room on the command line for mp_dvfs_tbl=\n");
		return;
	}
	for (uint32_t i = 0; key[i]; i++) {
		cl[at++] = key[i];
	}
	while (n) {
		cl[at++] = d[--n];
	}
	cl[at] = 0;
	out("dvfs: LK's tables for the kernel at ");
	out_hex((uint64_t)(uintptr_t)tbl);
	out("\n");
}

#if XNU_PARK
static uint32_t mp_dvfs_tbl[DVFS_TBL_WORDS];
#endif
#endif

#if XNU_PARK && SOC_IS_MT6781
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
#endif


#if RETURN_CPUS
/* PSCI AFFINITY_INFO (SMC64): 0 on, 1 off, 2 turning on, negative an error */
static int64_t psci_affinity_info(uint64_t mpidr)
{
	register uint64_t x0 __asm__("x0") = 0xc4000004ULL;
	register uint64_t x1 __asm__("x1") = mpidr;
	register uint64_t x2 __asm__("x2") = 0;

	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2)
	    : : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

static void say_cpus(void)
{
	out("cores as PSCI has them:");
	for (int c = 0; c < 8; c++) {
		int64_t st = psci_affinity_info((uint64_t)c << 8);

		out(" ");
		out_dec(c);
		out(st == 0 ? " on" : st == 1 ? " off" : st == 2 ? " turning-on" : " error");
		out(c < 7 ? "," : "\n");
	}
}
#endif

void xnu_returned(uint64_t log_pa, uint64_t len, uint64_t reason, uint64_t code, uint64_t elr, uint64_t far)
{
	struct span sp = { (const volatile char *)log_pa, len };

#if XNU_BIG_PSTORE && SOC_IS_MT6781
	plog_grow(lk_dtb);
#endif
#if XNU_RETURN_WDT
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);       /* its full length again, from here */
#else
	wdt_disable();
#endif
	xnu_back = 1;
	marker_set(MARK_DONE);
	uint64_t screen = fbc.base;
	fbc.base = 0; /* nothing drawn over XNU's console before it has been read */
	dbg_puts("\n=== what the screen showed when XNU came back ===\n");
	read_screen();
	fbc.base = screen;
	fbc.fg = 0xffffff40u;
	out("\n=== XNU returned to mtkloader: reason ");
	out_dec(reason);
	out(reason == 0 ? " (end)" : reason == 1 ? " (early exception)" : reason == 2 ? " (checkpoint)" : "");
	out(" code ");
	out_hex(code);
	if (reason == 1) {
		out(" ELR ");
		out_hex(elr);
		out(" FAR ");
		out_hex(far);
	}
	out("\n");
	/* Did user space ever start? tasks_count is 1 while only the kernel's task exists. */
	if (xsyms.tasks_count) {
		out("tasks ");
		out_dec(rd32(kva_to_pa(xsyms.tasks_count)));
		if (xsyms.nprocs) {
			out(" processes ");
			out_dec(rd32(kva_to_pa(xsyms.nprocs)));
		}
		if (xsyms.initproc) {
			out(" initproc ");
			out_hex(*(volatile uint64_t *)kva_to_pa(xsyms.initproc));
		}
		out("\n");
	}
	if (reason != 1) {
		show_panic_string(&xsyms);
		show_backtrace();
	}
	out("=== XNU log (");
	out_dec(len);
	out(" bytes) ===\n");
	if (len > 4 * 1024 * 1024) {
		sp.n = 0;
	}
#if XNU_LOG_CONSOLE
	{
		static const char mark[] = "\n--- kernel message buffer ---\n";

		for (uint64_t i = 0; i + sizeof(mark) - 1 <= sp.n; i++) {
			if (sp.p[i] == '\n' && memcmp_n(sp.p + i, mark, sizeof(mark) - 1) == 0) {
				out("(the kernel's message buffer left out: XNU_LOG_CONSOLE)\n");
				sp.n = i + 1;
				break;
			}
		}
	}
#endif
	save_log(&sp, 1);
	dbg_puts("=== end of XNU log ===\n");
	save_host_keep(&xsyms);
	show_log_tail(40);
	if (SOC_IS_MT6781) {
		/*
		 * No reset: no DRAM survives one on the MT6781, so Android starts in this boot,
		 * over the log just put in ramoops. XNU's loader_return left the other cores off, its shared interrupts
		 * disabled, and the stub the MMU and caches off. What XNU quiets only with bootcpu= is done here: its
		 * timer, the redistributors of the cores it had, and this core's CPU interface.
		 */
		__asm__ volatile("msr cntv_ctl_el0, xzr; isb");
		/*
		 * The SGI/PPI frame of each core XNU was given (with cpus > 1 not only core 0's: the others are off,
		 * but what XNU set up and left pending or active in their redistributors stays there for Linux to
		 * start them into). The redistributors are found by their GICR_TYPER affinity, 0x20000 apart.
		 */
		for (int i = 0; i < XNU_CORES; i++) {
			uint64_t rd = 0x0c040000ULL;
			int found = 0;

			for (int f = 0; f < 16; f++, rd += 0x20000) {
				uint32_t typer_lo = rd32(rd + 0x08), aff = rd32(rd + 0x0c);

				if (aff == (uint32_t)core_order[i] << 8) {
					wr32(rd + 0x10000 + 0x180, 0xffffffffu);   /* GICR_ICENABLER0 */
					wr32(rd + 0x10000 + 0x280, 0xffffffffu);   /* GICR_ICPENDR0 */
					wr32(rd + 0x10000 + 0x380, 0xffffffffu);   /* GICR_ICACTIVER0 */
					if (XNU_CORES > 1) {
						out("core ");
						out_dec(core_order[i]);
						out("'s redistributor at ");
						out_hex(rd);
						out(" cleared\n");
					}
					found = 1;
					break;
				}
				if (typer_lo & (1u << 4)) {     /* GICR_TYPER.Last */
					break;
				}
			}
			if (!found) {
				out("core ");
				out_dec(core_order[i]);
				out("'s redistributor NOT FOUND by its affinity");
				if (i == 0) {
					/* the first frame: core 0's */
					wr32(0x0c050000ULL + 0x180, 0xffffffffu);
					wr32(0x0c050000ULL + 0x280, 0xffffffffu);
					wr32(0x0c050000ULL + 0x380, 0xffffffffu);
					out("; the first frame cleared");
				}
				out("\n");
			}
		}
		__asm__ volatile("msr S3_0_C12_C12_7, xzr; isb");    /* ICC_IGRPEN1_EL1 */
#if XNU_PARK && SOC_IS_MT6781
		if (say_parked() > 0) {
			fdt_reserve_pen(lk_dtb);
		}
#endif
#if RETURN_CPUS
		say_cpus();
		{
			extern void cpu_off_entry(void);
			int64_t r;

			out("CPU_ON core 1 ...\n");
			r = psci_cpu_on(1u << 8, (uint64_t)(uintptr_t)cpu_off_entry, 0);
			out("CPU_ON core 1 -> ");
			out(r == 0 ? "0 (started)" : r == -4 ? "-4 (already on)" : r == -2 ? "-2 (invalid)" : "an error");
			out("\n");
			dbg_delay_ms(50);
			say_cpus();
		}
		dbg_delay_ms(10000);
#else
		dbg_delay_ms(5000);
#endif
#if DVFS_RESET_END && SOC_IS_MT6781
		out("dvfs: DVFS_RESET_END: SSPM's CPU DVFS was started this boot: no Android now -- the watchdog resets the "
		    "phone, and Android starts after it\n");
		show_log_tail(40);
		for (;;) {
			dbg_delay_ms(1000);
		}
#endif
		chain_android();
	}
	dbg_delay_ms(20000);
	reset_board();
}

/*
 * After a watchdog reset: what XNU had printed (its message buffer, through its own page tables:
 * cpu_ttep, 4K granule, 39-bit kernel addresses as TCR_EL1 had them), whether its clock ticked
 * (loader_live.heartbeat), and any panic string.
 */
static void rescue_state(void)
{
	if (xsyms.cpu_ttep) {
		xnu_fp[3] = rd32(kva_to_pa(xsyms.cpu_ttep)) | (uint64_t)rd32(kva_to_pa(xsyms.cpu_ttep) + 4) << 32;
		xnu_fp[2] = 0x22a51a251aULL;
	}
	out("cpu_ttep ");
	out_hex(xnu_fp[3]);
	if (xsyms.live) {
		out(" heartbeat ");
		out_dec(*(volatile uint64_t *)kva_to_pa(xsyms.live + 8));
	}
	out("\n");
	show_panic_string(&xsyms);
	if (!xsyms.msgbufp || !xnu_fp[3]) {
		return;
	}
	uint64_t mb = xnu_rd64(xsyms.msgbufp);
	uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
	int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
	out("msgbuf ");
	out_hex(mb);
	out(" size ");
	out_dec((uint32_t)size);
	out(" bufx ");
	out_dec((uint32_t)bufx);
	out("\n--- kernel message buffer (from DRAM) ---\n");
	if (size <= 0 || size > 1024 * 1024 || bufx < 0 || bufx > size) {
		return;
	}
	uint64_t fbsave = fbc.base;
	fbc.base = 0;
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? bufx : 0, to = pass == 0 ? size : bufx;
		for (int i = from; i < to; i++) {
			char c = xnu_rd8(bufc + i);
			if (c) {
				dbg_putc(c);
			}
		}
	}
	fbc.base = fbsave;
	dbg_puts("\n--- end of message buffer ---\n");
}

/* The watchdog cut XNU short: its log is still in DRAM, at the addresses of its symbols. */
static void rescue_log(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	out("\n=== mtkloader: XNU ran until the watchdog reset ===\n");
	if (!parse_payload(&pi)) {
		out("no XNU payload\n");
		return;
	}
	find_syms(pi.kernel, &s);
	if (!s.log || !s.log_len || !s.log_ring_at || !s.log_wrapped) {
		out("XNU log symbols not found\n");
		return;
	}
	/* osfmk/arm64/loader_return.c: a 256 KB head, then a 1 MB ring */
	const uint64_t head_sz = 256 * 1024, ring_sz = 2560 * 1024;     /* loader_return.c's */
	const volatile char *log = (const volatile char *)kva_to_pa(s.log);
	uint32_t len = rd32(kva_to_pa(s.log_len));
	uint32_t ring_at = rd32(kva_to_pa(s.log_ring_at));
	uint8_t wrapped = *(const volatile uint8_t *)kva_to_pa(s.log_wrapped);
	struct span sp[3];
	int n = 0;

	out("loader_log_len ");
	out_dec(len);
	out(" ring_at ");
	out_dec(ring_at);
	out(" wrapped ");
	out_dec(wrapped);
	out("\n");
	if (ring_at > ring_sz) {
		ring_at = 0;
	}
	if (!wrapped) {
		if (len > head_sz + ring_sz) {
			len = head_sz + ring_sz;
		}
		sp[n++] = (struct span){ log, len };
	} else {
		sp[n++] = (struct span){ log, head_sz };
		sp[n++] = (struct span){ log + head_sz + ring_at, ring_sz - ring_at };
		sp[n++] = (struct span){ log + head_sz, ring_at };
	}
	xsyms = s;
	xkernel = pi.kernel;
	rescue_state();
	dbg_puts("=== XNU log (from DRAM; a few bits may have flipped in the reset) ===\n");
	save_log(sp, n);
	dbg_puts("=== end of XNU log ===\n");
	save_host_keep(&s);
	show_log_tail(40);
}

/*
 * Do the timers reach EL1 here? The virtual timer's PPI 27 is what XNU uses, and its interrupt
 * never arrives: the kernel idles for ever waiting for a root device that no timeout ever wakes.
 * Set up the GIC as XNU does, arm each timer in turn and see which one is taken.
 */
#define GICD_BASE   0x0c000000ULL
#define GICR_SGI    (0x0c040000ULL + 0x10000)

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

/*
 * EOImode (ICC_CTLR_EL1 bit 1) is what ATF/GenieZone leaves set on this board: ICC_EOIR1_EL1
 * then only drops the running priority, and the interrupt stays *active* until a write to
 * ICC_DIR_EL1. XNU writes only EOIR, so with EOImode set it takes one timer interrupt and never
 * sees another: it idles for ever, waiting for timeouts that do not come. Clear it here, on the
 * core the kernel is about to run on.
 */
static void gic_fix_eoimode(void)
{
	uint64_t ctlr;

	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out("ICC_CTLR_EL1 ");
	out_hex(ctlr);
	out(ctlr & 2 ? " (EOImode set: clearing)" : " (EOImode clear)");
	if (ctlr & 2) {
		__asm__ volatile("msr ICC_CTLR_EL1, %0; isb" : : "r"(ctlr & ~2ULL));
		__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
		out(" -> ");
		out_hex(ctlr);
	}
	out("\n");
}

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

/* ---- the watcher on the second core ---- */

static uint64_t watcher_l1[512] __attribute__((aligned(4096)));
static volatile uint64_t watcher_started __attribute__((aligned(64)));

static void plog_flush(void)
{
	dcache_clean_range(plog_base, plog_zone);
	dcache_clean_range(MARKER_PA, 192);
}

/*
 * XNU never came back (no timer, a hang): with its caches visible through our own cacheable
 * mapping, take what it shows and has written, put it into ramoops and reset the board.
 */
void watcher_main(void)
{
	for (int i = 0; i < 512; i++) {
		uint64_t pa = (uint64_t)i << 30;
		if (i == 0) {
			watcher_l1[i] = pa | (1u << 10) | (0u << 2) | 1;            /* MMIO: device */
		} else if (i < DRAM_END_GB) {
			watcher_l1[i] = pa | (1u << 10) | (3u << 8) | (1u << 2) | 1; /* DRAM: normal WB, inner shareable */
		} else {
			watcher_l1[i] = 0;
		}
	}
	mmu_on((uint64_t)watcher_l1);
	watcher_started = 1;
	dcache_clean_range((uint64_t)&watcher_started, 8);

	uint64_t end = dbg_ticks() + dbg_freq() * XNU_WATCH_SECONDS;
	while (dbg_ticks() < end) {
		dcache_clean_range((uint64_t)&xnu_back, 8);
		if (xnu_back) {
			for (;;) {
				__asm__ volatile("wfe");
			}
		}
		dbg_delay_ms(100);
	}
	dcache_clean_range((uint64_t)&xnu_back, 8);
	if (xnu_back) {
		for (;;) {
			__asm__ volatile("wfe");
		}
	}

	fbc.base = 0; /* the screen is XNU's: read it, do not draw on it */
	marker_set(MARK_DONE);
	dbg_puts("\n=== watcher: XNU still running after " );
	dbg_dec(XNU_WATCH_SECONDS);
	dbg_puts(" s, snapshot ===\n");
	if (xsyms.cpu_ttep) {
		xnu_fp[3] = *(volatile uint64_t *)kva_to_pa(xsyms.cpu_ttep);
		xnu_fp[2] = 0x22a51a251aULL;
	}
	dbg_puts("cpu_ttep ");
	dbg_hex(xnu_fp[3], 10);
	if (xsyms.live) {
		dbg_puts(" heartbeat ");
		dbg_dec(*(volatile uint64_t *)kva_to_pa(xsyms.live + 8));
	}
	if (xsyms.log_len) {
		dbg_puts(" loader_log_len ");
		dbg_dec(rd32(kva_to_pa(xsyms.log_len)));
	}
	/* Where the first core is: find its PMU in the cpu_dbgapb window, sample PMPCSR. */
	{
		const uint64_t win = 0x0e010000ULL; /* cpu_dbgapb, core 0 */
		uint64_t fb_saved = fbc.base, pmu = 0;
		out("\ncpu_dbgapb components:");
		for (uint64_t off = 0; off < 0x100000; off += 0x10000) {
			uint64_t b = win + off;
			uint32_t cidr1 = rd32(b + 0xff4), devarch = rd32(b + 0xfbc), devtype = rd32(b + 0xfcc);
			out("\n  +");
			out_hex(off);
			out(" CIDR1 ");
			out_hex(cidr1);
			out(" DEVARCH ");
			out_hex(devarch);
			out(" DEVTYPE ");
			out_hex(devtype);
			if ((devarch & 0xffff) == 0x2a16) { /* ARM PMU v3 */
				pmu = b;
			}
		}
		out("\n");
		if (pmu) {
			wr32(pmu + 0xfb0, 0xc5acce55u);
			out("cpu0 PC samples (PMPCSR):\n");
			for (int i = 0; i < 24; i++) {
				uint32_t lo = rd32(pmu + 0x200);
				uint32_t hi = rd32(pmu + 0x204);
				uint64_t pc = ((uint64_t)hi << 32 | lo) & 0x00ffffffffffffffULL;
				if (pc >> 47) {
					pc |= 0xff00000000000000ULL;
				}
				out("  ");
				out_sym(pc);
				out("\n");
				dbg_delay_ms(15);
			}
		}
		fbc.base = fb_saved;
	}
	/*
	 * Is the virtual timer's PPI 27 even reaching the first core? GICR frame 0 is its
	 * redistributor (SGI/PPI page at +0x10000); pending but not active means the GIC latched
	 * the timer and the CPU interface refused it, nothing pending means no timer at all.
	 */
	{
		const uint64_t gicd = 0x0c000000ULL, gicr = 0x0c040000ULL, sgi = gicr + 0x10000;
		dbg_puts("GICD_CTLR ");
		dbg_hex(rd32(gicd + 0x000), 8);
		dbg_puts(" TYPER ");
		dbg_hex(rd32(gicd + 0x004), 8);
		dbg_puts("\nGICR_CTLR ");
		dbg_hex(rd32(gicr + 0x000), 8);
		dbg_puts(" WAKER ");
		dbg_hex(rd32(gicr + 0x014), 8);
		dbg_puts(" TYPER ");
		dbg_hex(rd32(gicr + 0x008), 8);
		dbg_putc(' ');
		dbg_hex(rd32(gicr + 0x00c), 8);
		dbg_puts("\nGICR_SGI: IGROUPR0 ");
		dbg_hex(rd32(sgi + 0x080), 8);
		dbg_puts(" IGRPMODR0 ");
		dbg_hex(rd32(sgi + 0xd00), 8);
		dbg_puts(" ISENABLER0 ");
		dbg_hex(rd32(sgi + 0x100), 8);
		dbg_puts("\n          ISPENDR0 ");
		dbg_hex(rd32(sgi + 0x200), 8);
		dbg_puts(" ISACTIVER0 ");
		dbg_hex(rd32(sgi + 0x300), 8);
		dbg_puts(" IPRIORITY27 ");
		dbg_hex(*(volatile uint8_t *)(sgi + 0x400 + 27), 2);
		dbg_puts(" ICFGR1 ");
		dbg_hex(rd32(sgi + 0xc04), 8);
		dbg_puts("\n");
	}
	if (xsyms.gic_unhandled_count) {
		dbg_puts("kernel: gic_unhandled_count ");
		dbg_dec(*(volatile uint64_t *)kva_to_pa(xsyms.gic_unhandled_count));
		dbg_puts(" last_unhandled ");
		dbg_dec(rd32(kva_to_pa(xsyms.gic_last_unhandled)));
		dbg_puts(" active_iar[0] ");
		dbg_hex(*(volatile uint64_t *)kva_to_pa(xsyms.gic_active_iar), 16);
		dbg_puts("\n");
	}

	dbg_puts("\n=== what the screen shows ===\n");
	read_screen();
	show_panic_string(&xsyms);
	if (xsyms.msgbufp && xnu_fp[3]) {
		uint64_t mb = xnu_rd64(xsyms.msgbufp);
		uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
		int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
		dbg_puts("--- kernel message buffer ---\n");
		if (size > 0 && size <= 1024 * 1024 && bufx >= 0 && bufx <= size) {
			for (int pass = 0; pass < 2; pass++) {
				int from = pass == 0 ? bufx : 0, to = pass == 0 ? size : bufx;
				for (int i = from; i < to; i++) {
					char c = xnu_rd8(bufc + i);
					if (c) {
						dbg_putc(c);
					}
				}
			}
		}
		dbg_puts("\n--- end of message buffer ---\n");
	}
	if (xsyms.log && xsyms.log_len) {
		uint32_t len = rd32(kva_to_pa(xsyms.log_len));
		struct span sp = { (const volatile char *)kva_to_pa(xsyms.log), len > 256 * 1024 ? 256 * 1024 : len };
		dbg_puts("--- loader_log (head) ---\n");
		save_log(&sp, 1);
	}
	save_host_keep(&xsyms);
	dbg_puts("=== end of watcher snapshot ===\n");
	plog_flush();
	psci_system_reset();
	wdt_reset();
}

static void watcher_start(void)
{
	if (!XNU_WATCH_SECONDS) {
		return;
	}
	watcher_started = 0;
	int64_t r = psci_cpu_on((uint64_t)WATCH_CORE << 8, (uint64_t)watcher_entry, 0);
	out("watcher on cpu");
	out_dec(WATCH_CORE);
	out(": PSCI CPU_ON ");
	out_dec((uint64_t)r);
	for (int i = 0; i < 100 && !watcher_started; i++) {
		dbg_delay_ms(10);
	}
	out(watcher_started ? ", running\n" : ", not seen\n");
}

/* ---- starting XNU ---- */

/* ---- how fast the cores are running ---- */

/*
 * What the boot loader left the cores at, in megahertz. The system counter runs at a fixed rate
 * whatever the cores do, so counting core cycles between two of its ticks gives the core's clock
 * directly. Nothing here changes a frequency -- it only asks, because a kernel that finds itself
 * on a core the boot loader parked at a third of its speed is worth knowing about before anyone
 * sets about raising it.
 */
static uint32_t cpu_mhz(void)
{
	uint64_t pmcr, cycles_before, cycles_after, ticks_before, ticks_after, hz = dbg_freq();

	__asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
	__asm__ volatile("msr pmcr_el0, %0" : : "r"((pmcr | 5u) & ~2u));   /* enable, reset the count */
	__asm__ volatile("msr pmcntenset_el0, %0" : : "r"(1ull << 31));
	__asm__ volatile("isb");

	ticks_before = dbg_ticks();
	__asm__ volatile("mrs %0, pmccntr_el0" : "=r"(cycles_before));
	dbg_delay_ms(20);
	__asm__ volatile("mrs %0, pmccntr_el0" : "=r"(cycles_after));
	ticks_after = dbg_ticks();

	if (cycles_after <= cycles_before || ticks_after <= ticks_before) {
		return 0;                       /* the counter is not ours to read */
	}
	return (uint32_t)(((cycles_after - cycles_before) * hz) /
	    ((ticks_after - ticks_before) * 1000000ull));
}

/* ---- the touch screen, before the kernel wants it ---- */

/*
 * Room for the firmware and the fourteen header bytes in front of it: plain DRAM that nothing
 * else wants. The loader's image ends before 0x400c0000, XNU's log ring sits at 0x47b1c9c0 and
 * ramoops at 0x4d0e0000, XNU's own memory starts at 0xc0000000, and DRAM covers all of it.
 */
#define TOUCH_STAGE         0x50000000ULL

/* The SPI clock mux as we found it, so that Android can be given it back that way. */
static uint32_t touch_clk_cfg2;
static int touch_clk_cfg2_seen;

/*
 * The controller keeps its firmware in RAM and loses it at every reset, so whoever boots the
 * phone has to send it afresh -- Android does this at every start, and so must we before a
 * kernel of ours can expect to read a finger. This is that sequence: open the bus, reset the
 * chip into its ROM bootloader, send the image as one message, pause, and give the go command,
 * after which the chip introduces itself unasked saying what it now runs.
 *
 * Returns the mode it reached: 1 or 2 mean the firmware is up. Nothing here is fatal -- a phone
 * whose screen cannot be touched still boots.
 */
static uint8_t touch_bring_up_once(void)
{
	static const uint32_t gate_clr[4] = { 0x8c, 0xa8, 0xc4, 0x84 };
	uint8_t buf[64], code = 0;
	uint32_t got = 0;
	uint8_t *stage = (uint8_t *)TOUCH_STAGE;

	for (int i = 0; i < 4; i++) {
		wr32(INFRACFG_BASE + gate_clr[i], 0xffffffffu);
	}
	__asm__ volatile("dsb sy");
	touch_clk_cfg2 = rd32(TOPCKGEN_BASE + CLK_CFG_2);
	touch_clk_cfg2_seen = 1;
	spi_pins_to_spi2();
	spi_setup();

	tcm_reset();
	if (tcm_read_patiently(&code, buf, sizeof(buf), &got) != 1 || got < 2) {
		out("touch: the controller said nothing after a reset (code ");
		out_hex(code);
		out(")\n");
		return 0;
	}
	out("touch: after a reset, code ");
	out_hex(code);
	out(", mode ");
	out_hex(buf[1]);
	out("\n");
	if (buf[1] != TCM_MODE_ROMBOOT) {
		out("touch: not in its ROM bootloader (mode ");
		out_hex(buf[1]);
		out("), leaving it alone\n");
		return buf[1];
	}

	/* The message whole in DRAM -- command, length, header, image -- to go out in one breath. */
	memcpy(stage + TCM_MESSAGE_HEAD + TCM_ROMBOOT_HEADER, touch_fw, TOUCH_FW_SIZE);
	tcm_load_firmware(stage);
	if (tcm_read_patiently(&code, buf, sizeof(buf), &got) != 1 || code != TCM_STATUS_OK) {
		out("touch: the image was not taken (");
		out_hex(code);
		out(")\n");
		return 0;
	}
	out("touch: the image was taken\n");
	tcm_run_firmware();
	for (uint32_t waited = 0; waited < 4000; waited += 10) {
		if (!tcm_irq()) {
			dbg_delay_ms(10);
			continue;
		}
		if (tcm_read(&code, buf, sizeof(buf), &got) != 1) {
			dbg_delay_ms(10);
			waited += 10;
			continue;
		}
		out("touch: it said code ");
		out_hex(code);
		out(" after ");
		out_hex(waited);
		out(" ms\n");
		if (code == TCM_REPORT_IDENTIFY && got >= 2) {
			out("touch: ");
			for (uint32_t i = 2; i < got && i < 18; i++) {
				out_putc(buf[i] >= 0x20 && buf[i] < 0x7f ? (char)buf[i] : ' ');
			}
			out(" is up, mode ");
			out_hex(buf[1]);
			out("\n");
			return buf[1];
		}
	}
	out("touch: it never said it was up\n");
	return 0;
}

/*
 * The whole sequence, up to six times, waiting a little longer between tries each time. Now and then the chip takes the image and then says
 * nothing after the go command -- two runs in four did -- and a fresh start from a reset brings
 * it up; this was what left iOS without a touch screen.
 */
static uint8_t touch_bring_up(void)
{
	for (int attempt = 1; attempt <= 6; attempt++) {
		uint8_t mode = touch_bring_up_once();

		if (mode == TCM_MODE_APPLICATION || mode == TCM_MODE_HOSTDOWNLOAD) {
			return mode;
		}
		out("touch: attempt ");
		out_dec((uint32_t)attempt);
		out(" failed, starting over\n");
		dbg_delay_ms(100 * (uint32_t)attempt);  /* a little longer each time for the chip to settle */
	}
	return 0;
}

/* ---- the big cores' clock ---- */

/*
 * LK leaves the big cluster (Cortex-A75) at 1087 MHz with its rail at 800 mV. MediaTek's own table
 * for this chip (cpufreq_v1, mach/mt6768, "OPP table B") runs it at 1176 MHz at that very voltage,
 * so it goes there: no rail is touched. The cluster is still off while the loader runs -- XNU wakes
 * those cores later -- so nothing is executing on the PLL while it is reprogrammed.
 *
 * ARMPLL_CON1 (apmixedsys + 0x20c): bit 31 asks for the new value, bits 24..26 the post divider
 * (here /2), bits 0..21 the multiplier of 26 MHz / 2^14 -- the same write the vendor's GPU driver
 * makes to its PLL.
 */
#define APMIXED_BASE        0x1000c000ULL
#define ARMPLL_CON1         0x20c
#define BIG_MHZ             1176u

static void big_cores_clock(void)
{
	uint32_t before = rd32(APMIXED_BASE + ARMPLL_CON1), post = (before >> 24) & 7u;
	uint32_t pcw = (uint32_t)(((uint64_t)BIG_MHZ << post) * 16384u / 26u);

	if (post != 1 || (before & 0x3fffffu) >= pcw) {
		return;                         /* not what LK is known to leave: leave it alone */
	}
	wr32(APMIXED_BASE + ARMPLL_CON1, (1u << 31) | (post << 24) | pcw);
	__asm__ volatile("dsb sy");
	dbg_delay_us(20);
	out("big cores: ");
	out_dec((uint32_t)(((26ull * (before & 0x3fffffu)) >> 14) >> post));
	out(" -> ");
	out_dec((uint32_t)(((26ull * (rd32(APMIXED_BASE + ARMPLL_CON1) & 0x3fffffu)) >> 14) >> post));
	out(" MHz at the same 800 mV\n");
}

#if XNU_SHIM
#define PAYLOAD_XNU_CMDLINE 0x48        /* the command line's offset in a MAPPLEPL payload (shim/loader.c) */

struct ili_block_copy {                 /* struct ili_block as it lies in memory */
	uint64_t name;
	uint32_t mem, len, crc, pad;
	uint64_t data;
};

static uint64_t align16k(uint64_t v)
{
	return (v + 0x3fffULL) & ~0x3fffULL;
}

#if XNU_TOUCH_ILI
/* The touch firmware's table, names and blocks copied to AT (the shim overwrites this program); where the table is. */
static uint64_t ili_table_copy(uint64_t at)
{
	const uint32_t n = sizeof(ili_blocks) / sizeof(ili_blocks[0]);
	struct ili_block_copy *t = (struct ili_block_copy *)at;
	uint64_t p = at + (n * sizeof(*t) + 15) / 16 * 16;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t nl = (uint32_t)strlen(ili_blocks[i].name) + 1;
		memcpy((void *)p, ili_blocks[i].name, nl);
		t[i].name = p;
		p += (nl + 15) / 16 * 16;
		memcpy((void *)p, ili_blocks[i].data, ili_blocks[i].len);
		t[i].mem = ili_blocks[i].mem;
		t[i].len = ili_blocks[i].len;
		t[i].crc = ili_blocks[i].crc;
		t[i].pad = 0;
		t[i].data = p;
		p += (ili_blocks[i].len + 15) / 16 * 16;
	}
	return p;
}
#endif

static void __attribute__((noreturn)) run_shim(void)
{
	struct payload_info pi;
	uint32_t len = 0;
	uint64_t rd = 0, at;

	(void)parse_payload(&pi);
	at = lk_prop(lk_dtb, "chosen", "linux,initrd-start", &len);
	if (at && len == 8) {
		rd = (uint64_t)fdt_get(at) << 32 | fdt_get(at + 4);
	} else if (at && len == 4) {
		rd = fdt_get(at);
	}
	if (!pi.shim || !pi.xnu || !rd || memcmp_n(pi.xnu, "MAPPLEPL", 8) != 0) {
		out("ERROR: no shim, MAPPLEPL payload or ramdisk address\n");
		dbg_delay_ms(5000);
		chain_android();
	}
	uint64_t fw = align16k(rd + pi.xnu_size), end = fw;
#if XNU_TOUCH_ILI
	for (uint32_t i = 0; i < sizeof(ili_blocks) / sizeof(ili_blocks[0]); i++) {
		end += 32 + 16 + ili_blocks[i].len + 16;
	}
#endif
	uint64_t tbl = (end + 15) & ~15ULL;     /* LK's OPP tables, after the most the touch firmware can take */
	end = tbl + DVFS_TBL_WORDS * 4;
	out("shim ");
	out_dec(pi.shim_size);
	out(" bytes, MAPPLEPL ");
	out_dec(pi.xnu_size);
	out(" bytes at ");
	out_hex(rd);
	out("\n");
	if ((uint64_t)pi.xnu + pi.xnu_size > rd || end > lk_dtb) {   /* LK's tree follows the ramdisk's room */
		out("ERROR: the payload and the touch firmware do not fit before LK's tree\n");
		dbg_delay_ms(5000);
		chain_android();
	}
	memcpy((void *)rd, pi.xnu, pi.xnu_size);

#if XNU_TOUCH_ILI && SOC_IS_MT6781
	if (ili_probe()) {
		ili_download(0);
		ili_bb_begin();         /* the pins stay GPIOs, chip select high, for the kernel */
		(void)ili_table_copy(fw);
		ili_xnu_args((char *)(rd + PAYLOAD_XNU_CMDLINE), fw);
	}
#endif
	lvts_args((char *)(rd + PAYLOAD_XNU_CMDLINE));     /* this phone's thermal calibration, for viva's LVTS code */
	dvfs_args((char *)(rd + PAYLOAD_XNU_CMDLINE), (uint32_t *)tbl);

	/* LK scans one of its three buffers; the shim and XNU draw into the first, so that one is scanned */
	display_find();
	display_alpha_off();
	wr32(OVL0_BASE + OVL_FB(OVL_L0_ADDR), (uint32_t)LK_FB_BASE);
	out("\nstarting the shim\n");
	marker_set(0);
	chain_image((const volatile uint32_t *)pi.shim, (pi.shim_size + 7) & ~7ULL);
}
#endif

static void __attribute__((noreturn)) run_xnu(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	if (!parse_payload(&pi)) {
		out("ERROR: no XNU payload in this image\n");
		dbg_delay_ms(5000);
		chain_android();
	}
	out("XNU kernel ");
	out_dec(pi.kernel_size);
	out(" bytes, ramdisk ");
	out_dec(pi.ramdisk_size);
	out(" bytes\n");

	const struct mach_header_64 *mh = (const struct mach_header_64 *)pi.kernel;
	uint64_t vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
			uint64_t dst_pa = kva_to_pa(sg->vmaddr);
			memset((void *)dst_pa, 0, sg->vmsize);
			memcpy((void *)dst_pa, pi.kernel + sg->fileoff, sg->filesize);
		} else if (lc->cmd == LC_UNIXTHREAD) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	out("kernel at ");
	out_hex(kva_to_pa(0xfffffff007004000ULL));
	out(", entry ");
	out_hex(kva_to_pa(entry_va));
	out("\n");

	find_syms(pi.kernel, &s);
	xsyms = s;
	xkernel = pi.kernel;

	/* XNU's console colors (32 bpp column) opaque. */
	if (s.vc_colors) {
		volatile uint32_t *c = (volatile uint32_t *)kva_to_pa(s.vc_colors);
		for (int i = 0; i < 8; i++) {
			c[i * 4 + 2] |= 0xff000000u;
		}
		out("vc_colors: alpha set\n");
	}

	/*
	 * This kernel's serial console is a PL011 driver (vmapple_uart): its transmit_ready waits
	 * for a flag the MediaTek UART does not have, so "serial=3" hangs there forever. Make
	 * transmit_ready return 1 and transmit_data do nothing; kprintf then reaches loader_log
	 * (and mt6577_uart_putc, which the kernel calls as well and which drives this UART right).
	 */
	if (s.uart_tx_ready && s.uart_tx_data) {
		volatile uint32_t *r = (volatile uint32_t *)kva_to_pa(s.uart_tx_ready);
		volatile uint32_t *d = (volatile uint32_t *)kva_to_pa(s.uart_tx_data);
		r[0] = 0x52800020u; /* mov w0, #1 */
		r[1] = 0xd65f03c0u; /* ret        */
		d[0] = 0xd65f03c0u; /* ret        */
		/*
		 * ... and the byte itself goes to loader_log_putc, which keeps it in the kernel's log
		 * buffer: that is how anything the first programs print reaches us at all. (This
		 * build of loader_log_putc calls nothing, so there is no way back round.)
		 */
		if (s.log_putc) {
			int64_t rel = (int64_t)(s.log_putc - s.uart_tx_data) >> 2;

			if (rel >= -(1 << 25) && rel < (1 << 25)) {
				d[0] = 0x14000000u | ((uint32_t)rel & 0x03ffffffu);
			}
		}
		/* and no input: the kernel's serial keyboard would otherwise read this UART's
		 * receive register, which here is another register entirely, for ever. */
		if (s.uart_rx_ready) {
			volatile uint32_t *rx = (volatile uint32_t *)kva_to_pa(s.uart_rx_ready);
			rx[0] = 0x52800000u; /* mov w0, #0 */
			rx[1] = 0xd65f03c0u; /* ret        */
		}
		out("vmapple_uart: transmit_ready patched, transmit_data -> loader_log_putc, receive off\n");
	}

	/*
	 * Everything written to /dev/console -- the kernel's own text and whatever the first
	 * programs print -- goes through uart_putc, and on this phone that is a PL011 driver
	 * writing into nothing. Send it to loader_log_putc instead (same argument, a character in
	 * w0), which keeps it in the kernel's log buffer and puts it out of the MediaTek UART as
	 * well. Without this the only voice userland has is lost.
	 */
	if (s.log_putc) {
		/* uart_putc is kprintf's way out, uart_putc_options the console's (and so userland's) */
		const uint64_t from[2] = { 0, 0 };

		for (int i = 0; i < 2; i++) {
			if (!from[i]) {
				continue;
			}
			volatile uint32_t *at = (volatile uint32_t *)kva_to_pa(from[i]);
			int64_t rel = (int64_t)(s.log_putc - from[i]) >> 2;

			if (rel >= -(1 << 25) && rel < (1 << 25)) {
				at[0] = 0x14000000u | ((uint32_t)rel & 0x03ffffffu); /* b loader_log_putc */
			}
		}
		out("uart_putc and uart_putc_options -> loader_log_putc\n");
	}

	/*
	 * EOImode is per processor, and gic_fix_eoimode() below only reaches the one this code runs
	 * on. Every other core comes out of PSCI CPU_ON with the bit set again, takes one timer
	 * interrupt and goes deaf: with four cores iOS crawled on the one that worked, and the
	 * kernel's own heartbeat beat 3 times in 840 seconds.
	 *
	 * Each core calls gic_v3_irq_cpu_init() for itself, and in it writes zero to ICC_BPR1_EL1.
	 * That instruction becomes a write of zero to ICC_CTLR_EL1, which clears EOImode (and CBPR,
	 * which wants to be zero anyway). What is given up is BPR1 = 0, the preemption grouping,
	 * which keeps its reset value -- a small thing beside a core whose timer never fires again.
	 */
	if (s.gic_cpu_init) {
		volatile uint32_t *at = (volatile uint32_t *)kva_to_pa(s.gic_cpu_init);
		int done = 0;

		for (int i = 0; i < 256 && !done; i++) {
			if (at[i] == 0xd518cc7fu) {          /* msr ICC_BPR1_EL1, xzr */
				at[i] = 0xd518cc9fu;         /* msr ICC_CTLR_EL1, xzr */
				done = 1;
			}
		}
		out(done ? "gic_v3_irq_cpu_init: every core clears EOImode\n" :
		    "gic_v3_irq_cpu_init: the BPR1 write is not where it was -- EOImode stays set\n");
	}

	/* Have XNU come back here on a panic or an early exception. */
	if (s.stub_slot) {
		volatile uint64_t *slot = (volatile uint64_t *)kva_to_pa(s.stub_slot);
		if (slot[0] == 0x545352444c554e58ULL) { /* "XNULDRST" */
			slot[1] = (uint64_t)xnu_return_stub;
			slot[2] = XNU_CHECKPOINT;
			slot[3] = XNU_SLIDE;
			out("return stub at ");
			out_hex(slot[1]);
			out("\n");
		}
	}

	uint64_t kern_hi = (kva_to_pa(vm_hi) + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	if (rd_size) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
	}
	uint64_t sys_size = 0;
	uint64_t sys_pa = sys_size ? SYSTEM_PA : 0;
	uint64_t sys_span = (sys_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

	uint64_t dt_pa = sys_size ? sys_pa + sys_span : rd_pa + rd_size;
	uint32_t dt_len = build_device_tree(dt_pa, rd_pa, rd_size, sys_pa, sys_span);
	uint64_t ba_pa = (dt_pa + dt_len + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->physBase = XNU_PHYS_BASE;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->memSize = XNU_MEM_SIZE;
	ba->memSizeActual = XNU_MEM_SIZE;
	ba->topOfKernelData = top;
	ba->deviceTreeP = dt_pa - XNU_SLIDE;
	ba->deviceTreeLength = dt_len;
	ba->video.v_baseAddr = fb_base;
	ba->video.v_display = XNU_VERBOSE ? 0 : 1; /* 0: the kernel's text console, 1: for iOS to draw */
	ba->video.v_rowBytes = fb_pitch;
	ba->video.v_width = PANEL_W;
	ba->video.v_height = PANEL_H;
	ba->video.v_depth = 32;
	memcpy(ba->commandLine, XNU_BOOT_ARGS, sizeof(XNU_BOOT_ARGS));
	/* the kernel's version, for sysctl kern.mapple.version and its log, as xnuloader gives it (one word) */
	if (pi.kernel_version != NULL) {
		static const char prefix[] = " mapple_kver=";
		uint32_t n = sizeof(XNU_BOOT_ARGS) - 1, len = 0;

		while (pi.kernel_version[len] != 0 && pi.kernel_version[len] != ' ' && len < 128) {
			len++;
		}
		if (n + sizeof(prefix) - 1 + len < BOOT_LINE_LENGTH) {
			memcpy(ba->commandLine + n, prefix, sizeof(prefix) - 1);
			memcpy(ba->commandLine + n + sizeof(prefix) - 1, pi.kernel_version, len);
			ba->commandLine[n + sizeof(prefix) - 1 + len] = 0;
			out("kernel.ver=");
			for (uint32_t i = 0; i < len; i++) {
				out_putc(pi.kernel_version[i]);
			}
			out("\n");
		}
	}
#if XNU_UFS && SOC_IS_MT6781
	xnu_ufs_args(ba->commandLine);
#endif
#if XNU_TOUCH_ILI && SOC_IS_MT6781
	ili_xnu_args(ba->commandLine, (uint64_t)(uintptr_t)ili_blocks);
#endif
#if XNU_PARK && SOC_IS_MT6781
	xnu_park_setup(ba->commandLine);
#endif

	out("md0 ");
	out_hex(sys_size ? sys_pa : rd_pa);
	out(sys_size ? " (the system image), md1 " : XNU_ROOT_EMMC ? " is none: md1 (root.packfs), the system image on the eMMC" :
	    XNU_ROOT_UFS ? " is none: md1 (root.apfs), the system image in xnusys on the UFS" :
	    " (root.packfs only)");
	if (sys_size) {
		out_hex(rd_pa);
	}
	out("\nboot-args: " XNU_BOOT_ARGS "\nframebuffer ");
	out_hex(fb_base);
	out(" pitch ");
	out_dec(fb_pitch);
	out(", XNU memory ");
	out_hex(XNU_PHYS_BASE);
	out("+");
	out_hex(XNU_MEM_SIZE);
	out("\n");
	display_alpha_off();

	if (XNU_WDT_SECONDS) {
		uint32_t t = XNU_WDT_SECONDS > 31 ? 31 : XNU_WDT_SECONDS;
		marker_set(MARK_RUNNING);
		wr32(TOPRGU_LENGTH, ((t * 32768u / 512u) << 5) | 0x8u);
		wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
		wr32(TOPRGU_MODE, TOPRGU_MODE_KEY | 0x1u);
		out("watchdog: back to Android in ");
		out_dec(t);
		out(" s\n");
	} else {
		marker_set(0);
	}
	/* The timers XNU will use: a virtual counter offset by EL2 (MediaTek's GenieZone) would
	 * leave its deadlines forever in the future, and the kernel idle with nothing to wake it. */
	{
		uint64_t pct, vct, frq, kctl, pfr0;
		__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntvct_el0; mrs %2, cntfrq_el0;"
		    "mrs %3, cntkctl_el1; mrs %4, id_aa64pfr0_el1"
		    : "=r"(pct), "=r"(vct), "=r"(frq), "=r"(kctl), "=r"(pfr0));
		out("timers: CNTPCT ");
		out_hex(pct);
		out(" CNTVCT ");
		out_hex(vct);
		out(" delta ");
		out_hex(pct - vct);
		out(" CNTFRQ ");
		out_dec(frq);
		out(" CNTKCTL ");
		out_hex(kctl);
		out(" EL2 ");
		out_dec((pfr0 >> 8) & 0xf);
		out("\n");
	}

	{
		uint32_t mhz = cpu_mhz();

		out("this core is running at ");
		if (mhz) {
			out_dec(mhz);
			out(" MHz\n");
		} else {
			out("a speed its cycle counter will not say\n");
		}
	}

	/* must happen on the core the kernel runs on, and before it sets its GIC up */
	gic_fix_eoimode();
#if TIMER_PROBE
	timer_probe();
#endif
	watcher_start();
	/*
	 * The touch screen, while there is still a console to complain to. The controller has to be
	 * handed its firmware before anything can read a finger out of it, and the kernel is a bad
	 * place to do it from: it would have to carry those hundred kilobytes itself, and it comes
	 * up long after the chip has forgotten them.
	 */
	if (!SOC_IS_MT6781) {     /* the Hot 20's touch controller (SPI2) and big cores' PLL */
		touch_bring_up();
		big_cores_clock();
	}
#if XNU_TOUCH_ILI && SOC_IS_MT6781
	if (ili_probe()) {
		ili_download(0);
		ili_bb_begin();         /* the pins stay GPIOs, chip select high, for the kernel (it reads by hand too) */
	}
#endif
	/*
	 * Nothing of ours is left on the panel: iOS paints only what it draws, and where it does not
	 * reach, what LK wrote there stays -- its "Orange State: your device has been unlocked"
	 * showed through the home screen.
	 */
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	for (uint64_t at = fb_base; at < fb_base + (uint64_t)PANEL_H * fb_pitch; at += 64) {
		__asm__ volatile("dc cvac, %0" : : "r"(at));
	}
	__asm__ volatile("dsb sy");
	out("Jumping to XNU...\n");
	jump_to_kernel(kva_to_pa(entry_va), ba_pa);
	__builtin_unreachable();
}




/* ---- choosing what to boot ---- */

/*
 * Android unless asked for XNU, once. The keypad's state registers change by themselves (a scan
 * pattern), so keys are not used. The request is a line written to /dev/pmsg0 under Android
 * (tools/boot-xnu.sh): ramoops keeps it in its pmsg zone across the reboot, where this finds it
 * and wipes it, so the boot after XNU is Android again.
 */
static const char boot_xnu_flag[] = "OPENMAPPLE:BOOT-XNU";

/*
 * Whether Android left `flag` in the pmsg zone before rebooting, and if so wipe it, so it is taken
 * once: a run that hangs costs one reboot, after which the loader finds nothing and starts Android.
 */
static int pmsg_take(const char *flag, uint32_t n)
{
	volatile struct ramoops_hdr *h = (volatile struct ramoops_hdr *)RAMOOPS_PMSG;
	volatile char *d = (volatile char *)(RAMOOPS_PMSG + sizeof(struct ramoops_hdr));
	uint32_t cap = RAMOOPS_PMSG_SIZE - sizeof(struct ramoops_hdr);
	int found = 0;

	if (h->sig != RAMOOPS_SIG || h->size > cap) {
		return 0;
	}
	for (uint32_t at = 0; at + n <= cap; at++) {
		uint32_t k = 0;
		while (k < n && d[at + k] == flag[k]) {
			k++;
		}
		if (k == n) {
			for (k = 0; k < n; k++) {
				d[at + k] = '-';
			}
			found = 1;
		}
	}
	return found;
}

/*
 * On the MT6781 Android's pmsg flag does not always get here: LK can put ramoops back from expdb over it.
 * So volume down, held while this asks, is a request too: keypad key 1, bit 1 of KP_MEM1, clear while the
 * key is down (KEY_PROBE). Three samples in a row, so that a bounce does not count.
 */
static int volume_down_held(const char *what)
{
	uint32_t run = 0;

	out("hold volume down now for ");
	out(what);
	out(" (3 s)\n");
	for (uint32_t t = 0; t < 30; t++) {
		run = (rd32(0x10010004ULL) & (1u << 1)) ? 0 : run + 1;
		if (run == 3) {
			return 1;
		}
		dbg_delay_ms(100);
	}
	return 0;
}

static int xnu_requested(void)
{
	int asked = pmsg_take(boot_xnu_flag, sizeof(boot_xnu_flag) - 1);

	asked = asked || volume_down_held("Darwin/XNU");
	return asked;
}

static int boot_choice(void)
{
	int xnu = MENU_DEFAULT_XNU || xnu_requested();
	uint64_t screen = fbc.base;

	fbc.base = 0;
	dbg_puts(xnu ? "boot: Darwin / XNU (requested)\n" : "boot: Android\n");
	fbc.base = screen;
	fb_rect(0, 600, PANEL_W, 160, 0xff000000u);
	fbc.scale = 3;
	fbc.x = 48;
	fbc.y = 640;
	fbc.fg = xnu ? 0xff40ff40u : 0xffffffffu;
	for (const char *c = xnu ? "Booting Darwin / XNU" : "Booting Android"; *c; c++) {
		fbcon_putc(*c);
	}
	fbc.scale = 1;
	fbc.fg = 0xffffffffu;
	dbg_delay_ms(1000);
	return xnu;
}


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

#if (UFS_PROBE || XNU_UFS) && SOC_IS_MT6781
#define UFSHCI_BASE         0x11270000ULL
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

#if (UFS_READ_TEST || XNU_UFS) && SOC_IS_MT6781
/*
 * Reads, the UFSHCI way (JESD223; Linux's ufshcd lays it out the same): a UTP transfer request descriptor in slot 0
 * of LK's list, pointing at a command descriptor here -- the COMMAND UPIU at 0, the RESPONSE UPIU at 512, one PRDT
 * entry at 1024, offsets in dwords -- with the CDB READ(10), LUN 2 (the user unit, 15620096 blocks of 4096 bytes). The
 * MMU is off, so the controller and this code see the same memory. Nothing of the controller's setup is changed: only
 * the descriptor, doorbell bit 0 and, after, the completion bit in IS.
 */
#define UFS_BLOCK           4096u
static uint8_t ufs_ucd[1024 + 16] __attribute__((aligned(128)));
static uint8_t ufs_data[4 * UFS_BLOCK] __attribute__((aligned(4096)));

/* LK's list, if the controller is as the probe saw it (device present, lists ready, nothing in flight, running); else 0 */
static uint64_t ufs_lk_list(void)
{
	const uint64_t utrl = rd32(UFSHCI_BASE + 0x50) | (uint64_t)rd32(UFSHCI_BASE + 0x54) << 32;

	if ((rd32(UFSHCI_BASE + 0x30) & 0xf) != 0xf || rd32(UFSHCI_BASE + 0x58) != 0 || rd32(UFSHCI_BASE + 0x60) != 1 ||
	    utrl < 0x40000000ULL || utrl >= 0x140000000ULL) {
		out("ufs read: the controller is not as the probe saw it; nothing sent\n");
		return 0;
	}
	return utrl;
}

/* Blocks [lba, lba + n) of LUN 2 into ufs_data, n <= 4. 0: done, good status; 1: no completion (slot cleared, list
 * stopped); 2: completed with an error. */
static int ufs_lk_read(uint64_t utrl, uint32_t lba, uint32_t n)
{
	volatile uint32_t *utrd = (volatile uint32_t *)utrl;
	volatile uint8_t *cmd = ufs_ucd, *rsp = ufs_ucd + 512;
	volatile uint32_t *prdt = (volatile uint32_t *)(ufs_ucd + 1024);
	const uint32_t bytes = n * UFS_BLOCK;
	uint32_t t;

	if (n == 0 || n > 4) {
		return 2;
	}
	for (uint32_t i = 0; i < sizeof(ufs_ucd); i++) {
		ufs_ucd[i] = 0;
	}
	for (uint32_t i = 0; i < bytes; i++) {
		ufs_data[i] = 0xa5;
	}
	/* COMMAND UPIU: type 0x01, flags R, LUN 2, task tag 0; expected length (big-endian); CDB READ(10) */
	cmd[0] = 0x01;
	cmd[1] = 0x40;
	cmd[2] = 2;
	cmd[3] = 0;
	cmd[12] = (uint8_t)(bytes >> 24);
	cmd[13] = (uint8_t)(bytes >> 16);
	cmd[14] = (uint8_t)(bytes >> 8);
	cmd[15] = (uint8_t)bytes;
	cmd[16] = 0x28;
	cmd[18] = (uint8_t)(lba >> 24);
	cmd[19] = (uint8_t)(lba >> 16);
	cmd[20] = (uint8_t)(lba >> 8);
	cmd[21] = (uint8_t)lba;
	cmd[23] = (uint8_t)(n >> 8);
	cmd[24] = (uint8_t)n;
	/* PRDT: the data buffer, byte count - 1 */
	prdt[0] = (uint32_t)(uint64_t)ufs_data;
	prdt[1] = (uint32_t)((uint64_t)ufs_data >> 32);
	prdt[2] = 0;
	prdt[3] = bytes - 1;
	/* UTRD, slot 0: UFS storage command, device to host, no interrupt; OCS 0xf until the controller writes it */
	utrd[0] = (1u << 28) | 0x04000000u;
	utrd[1] = 0;
	utrd[2] = 0xf;
	utrd[3] = 0;
	utrd[4] = (uint32_t)(uint64_t)ufs_ucd;
	utrd[5] = (uint32_t)((uint64_t)ufs_ucd >> 32);
	/* DW6: response UPIU length (15:0) and offset (31:16); DW7: PRDT length (15:0) and offset (31:16), in dwords */
	utrd[6] = (128u << 16) | 128u;    /* response UPIU at 512, 512 bytes long */
	utrd[7] = (256u << 16) | 1u;      /* PRDT at 1024, one entry */
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(UFSHCI_BASE + 0x58, 1);      /* doorbell, slot 0 */
	for (t = 0; t < 1000 && (rd32(UFSHCI_BASE + 0x58) & 1); t++) {
		dbg_delay_ms(1);
	}
	__asm__ volatile("dsb sy" ::: "memory");    /* what the controller wrote, read from memory */
	if (rd32(UFSHCI_BASE + 0x58) & 1) {
		/* still out: clear the slot and stop the list, so that nothing is written into this memory later */
		wr32(UFSHCI_BASE + 0x5c, ~1u);   /* UTRLCLR: 0 in bit 0 clears slot 0 */
		wr32(UFSHCI_BASE + 0x60, 0);     /* UTRLRSR: stop */
		dbg_delay_ms(10);
		out("ufs read: NO COMPLETION in 1 s; slot cleared and list stopped (UTRLDBR ");
		out_hex(rd32(UFSHCI_BASE + 0x58));
		out(", UTRLRSR ");
		out_hex(rd32(UFSHCI_BASE + 0x60));
		out(", OCS ");
		out_hex(utrd[2] & 0xff);
		out(")\n");
		return 1;
	}
	out("ufs read: LBA ");
	out_dec(lba);
	out("+");
	out_dec(n);
	out(" completed after ");
	out_dec(t);
	out(" ms, OCS ");
	out_hex(utrd[2] & 0xff);
	out(", IS ");
	out_hex(rd32(UFSHCI_BASE + 0x20));
	out(", response UPIU type ");
	out_hex(rsp[0]);
	out(" response ");
	out_hex(rsp[6]);
	out(" SCSI status ");
	out_hex(rsp[7]);
	out("\n");
	wr32(UFSHCI_BASE + 0x20, 1);      /* UTRCS, write 1 to clear */
	return (utrd[2] & 0xff) == 0 && rsp[0] == 0x21 && rsp[6] == 0 && rsp[7] == 0 ? 0 : 2;
}

/* little-endian, from the GPT */
static uint64_t ufs_le(const volatile uint8_t *p, int n)
{
	uint64_t v = 0;

	for (int i = n - 1; i >= 0; i--) {
		v = v << 8 | p[i];
	}
	return v;
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

#if XNU_UFS && SOC_IS_MT6781
static char *cl_put(char *at, const char *s)
{
	while (*s) {
		*at++ = *s++;
	}
	*at = 0;
	return at;
}

static char *cl_dec(char *at, uint64_t v)
{
	char d[24];
	int n = 0;

	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		*at++ = d[--n];
	}
	*at = 0;
	return at;
}

/* a partition's first and last block among the GPT's entries in ufs_data, or 0, 0 */
static void ufs_gpt_find(const char *name, uint32_t count, uint32_t size, uint64_t *first, uint64_t *last)
{
	const volatile uint8_t *d = ufs_data;

	*first = *last = 0;
	for (uint32_t i = 0; i < count; i++) {
		const volatile uint8_t *e = d + i * size;
		uint32_t k;

		for (k = 0; name[k] && e[56 + 2 * k] == (uint8_t)name[k] && e[57 + 2 * k] == 0; k++) {
		}
		if (name[k] == 0 && e[56 + 2 * k] == 0 && e[57 + 2 * k] == 0) {
			*first = ufs_le(e + 32, 8);
			*last = ufs_le(e + 40, 8);
			return;
		}
	}
}

/*
 * xnusys, found in the user unit's GPT (header at LBA 1, its entries after), for the kernel's UFS driver
 * (mtk/mt6781/drivers/storage/mapple.mt6781_ufs.c): " mtkufs_lba=<first block> mtkufs_blocks=<count>
 * mtkufs_lkutrl=<LK's list>" onto the command line. When anything is not as expected nothing is added, and the
 * driver then leaves the controller alone. darwin as well, if it is there and clear of xnusys: " mtkufs_dlba=
 * mtkufs_dblocks=" (the driver opens it only with mtkufs_write=1, and checks it against the GPT itself).
 */
static void xnu_ufs_args(char *cl)
{
	const volatile uint8_t *d = ufs_data;
	uint64_t utrl = ufs_lk_list(), alt, entries, first = 0, last = 0, dfirst = 0, dlast = 0;
	uint32_t count, size;
	char *at = cl;

	if (utrl == 0 || ufs_lk_read(utrl, 1, 1) != 0 || memcmp_n(d, "EFI PART", 8) != 0) {
		out("xnu ufs: no GPT header at LBA 1 of LUN 2; no mtkufs_ arguments\n");
		return;
	}
	alt = ufs_le(d + 32, 8);
	entries = ufs_le(d + 72, 8);
	count = (uint32_t)ufs_le(d + 80, 4);
	size = (uint32_t)ufs_le(d + 84, 4);
	if (size != 128 || count == 0 || count > 128 || entries < 2 || entries + 4 > alt ||
	    ufs_lk_read(utrl, (uint32_t)entries, (count * size + UFS_BLOCK - 1) / UFS_BLOCK) != 0) {
		out("xnu ufs: the GPT's entries not read; no mtkufs_ arguments\n");
		return;
	}
	ufs_gpt_find("xnusys", count, size, &first, &last);
	ufs_gpt_find("darwin", count, size, &dfirst, &dlast);
	if (first < entries + 4 || last < first || last >= alt || last > 0xffffffffULL) {
		out("xnu ufs: no usable xnusys in the GPT; no mtkufs_ arguments\n");
		return;
	}
	if (dfirst < entries + 4 || dlast < dfirst || dlast >= alt || dlast > 0xffffffffULL ||
	    !(dlast < first || dfirst > last)) {
		dfirst = dlast = 0;                     /* not there, or not clear of xnusys: not given */
	}
	while (*at) {
		at++;
	}
	if ((uint32_t)(at - cl) + 130 >= BOOT_LINE_LENGTH) {
		out("xnu ufs: no room on the command line; no mtkufs_ arguments\n");
		return;
	}
	at = cl_put(at, " mtkufs_lba=");
	at = cl_dec(at, first);
	at = cl_put(at, " mtkufs_blocks=");
	at = cl_dec(at, last - first + 1);
	at = cl_put(at, " mtkufs_lkutrl=");
	at = cl_dec(at, utrl);
	if (dlast != 0) {
		at = cl_put(at, " mtkufs_dlba=");
		at = cl_dec(at, dfirst);
		at = cl_put(at, " mtkufs_dblocks=");
		cl_dec(at, dlast - dfirst + 1);
	}
	out("xnu ufs: xnusys at LBA ");
	out_dec(first);
	out(", ");
	out_dec(last - first + 1);
	out(" blocks; darwin ");
	if (dlast != 0) {
		out("at LBA ");
		out_dec(dfirst);
		out(", ");
		out_dec(dlast - dfirst + 1);
		out(" blocks");
	} else {
		out("not found");
	}
	out("; LK's list ");
	out_hex(utrl);
	out("\n");
}
#endif

void mtkloader_main(uint64_t dtb)
{
#if PASS_THROUGH
	lk_dtb = dtb;
	plog_reset();
	out("mtkloader: pass-through, LK dtb ");
	out_hex(dtb);
	out("\n");
#if SOC_PROBE
	soc_probe();
#endif
	chain_android();
#endif
	uint32_t mark = marker_get();

	wdt_disable();
	lk_dtb = dtb;
	display_find();
	fbcon_init(fb_base, fb_pitch, 0, 1);

	if (mark == MARK_DONE) {
		/* XNU's log was saved when it came back; leave ramoops as it is. */
		chain_android();
	}
	if (mark == MARK_RUNNING) {
		rescue_log();
		dbg_delay_ms(5000);
		chain_android();
	}

	plog_reset();
#if UFS_PROBE && SOC_IS_MT6781
	if (volume_down_held("the UFS probe")) {
		out("== mtkloader: UFS controller probe ==\n");
		ufs_probe();
#if UFS_READ_TEST
		if (ufs_read_test()) {
			dbg_delay_ms(15000);        /* on screen, to be photographed */
			reset_board();              /* not Android: a transfer may have been under way */
		}
		dbg_delay_ms(11000);
#endif
		dbg_delay_ms(4000);
	}
	chain_android();
#endif
#if ILI_PROBE && SOC_IS_MT6781
	if (volume_down_held("the touch probe")) {
		out("== mtkloader: ILI7807S touch probe ==\n");
		if (ili_probe() && ILI_DOWNLOAD) {
#if ILI_DOWNLOAD
			ili_download(5000);
#endif
		}
		out("ili: probe done\n");
		dbg_delay_ms(15000);        /* on screen, to be photographed */
	}
	chain_android();
#endif
#if KEY_PROBE
	key_probe();
#endif
	if (!boot_choice()) {
		chain_android();
	}
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	fbc.x = 0;
	fbc.y = fbc.top = 0;
	fbc.fg = 0xff40ff40u;
	out(SOC_IS_MT6781 ? "== mtkloader: Darwin/XNU on realme 8i (MT6781) ==\n" :
	    "== mtkloader: Darwin/XNU on X6826B (MT6768) ==\n");
	fbc.fg = 0xffffffffu;
	out("LK dtb ");
	out_hex(dtb);
	out("\n");
	connsys_dt_report(dtb);         /* Wi-Fi step 1: what Android's tree says of CONNSYS (mtkconnsys.h) */
#if XNU_SHIM
	run_shim();
#endif
	run_xnu();
}
