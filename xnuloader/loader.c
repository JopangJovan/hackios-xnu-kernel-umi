/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

#define PAYLOAD_BASE        0xF0000000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x04000000ULL         /* RAM reserved for the payload in the UEFI memory map */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

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

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			return bio;
		}
	}
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
static char out_buf[2 * 1024 * 1024];
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static void
write_logs(const char *klog, uint64_t klog_len)
{
	efi_block_io *bio = find_log_partition();

	compose_logs(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len; i++) {
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	if (!EFI_ERROR(bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf))) {
		bio->flush_blocks(bio);
	}
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/* Properties copied from the iPhone's device tree (identity.h). */
static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
};

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
};

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */

static void wdt_pet(void);
static int wdt_armed;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room = sizeof(out_buf) - 8192;

	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
			group1 = READ_SYSREG(S3_0_C12_C12_7);          /* ICC_IGRPEN1_EL1 */
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		usb_poll_command();
		for (volatile int i = 0; i < 20000; i++) {
		}
	}
	running_live = NULL;

	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT != &platform_redwood) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		resident_rd_size = pi.resident_rd;
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	for (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
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
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = (int)(kernel_mpidr >> plat->core_shift);

	if (more_cores) {
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr = (uint64_t)core << plat->core_shift;
				int64_t state;

				if (core < 0 || core >= plat->cores || mpidr == this_mpidr) {
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	wdt_set_ms(WDT_BITE_MS);
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
			}
			if (got == total) {
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(PHONE_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	if (dma.control != 0) {
		print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
		return;
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = PHONE_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	print("== XNULOADER v3 ==\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		if (SCREEN.base == 0xE1000000ULL) {
			PLAT = &platform_redwood;
		} else {
			ramfb_phone_screen();
		}
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);

	install_exception_hook();
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	reboot();
	return 0;
}
