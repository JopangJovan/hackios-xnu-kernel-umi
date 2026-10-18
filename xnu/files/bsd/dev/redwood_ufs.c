/*
 * The phone's storage, read only: one block device and one character device for every GPT
 * partition of its UFS (/dev/ufs_NAME and /dev/rufs_NAME, NAME as the partition table has it).
 *
 * There is no UFS driver here, and none is needed. UEFI never exits on this port: Qualcomm's own
 * UFS driver (UFSDxe) has brought up the controller, its PHY, its clocks and power, and stays
 * resident, and xnuloader keeps serving the kernel from core 0 while the kernel runs on another
 * core (it already does for the battery). So with ufs=2 on its command line the loader publishes a
 * page (ufs_shared_pa=) with the table of partitions, a request and an answer, and a buffer; this
 * driver writes a request -- which partition, which blocks -- and core 0 reads them with that
 * driver's ReadBlocks into the buffer (xnuloader/loader.c, block_poll). Nothing in this kernel
 * touches the storage controller.
 *
 * Read only by construction: opening for writing is refused, a write that reaches the strategy
 * routine is refused, and the page has no field in which a write could be asked for -- the
 * loader's side calls ReadBlocks and nothing else.
 *
 * One request at a time (the page has one), so requests are serialized by a mutex and split into
 * pieces of the buffer's size. The kernel writes only the request's cache lines and core 0 only
 * the answer's (see the loader), both through a mapping that does not cache here, so neither side
 * ever overwrites the other's. The data buffer, which only core 0's reads fill, is mapped cacheable
 * and the part of it an answer covers is invalidated here before it is copied out.
 *
 * ufs_check=1 reads, once at start, the partitions there are images of in the ROM's
 * firmware-update/ (bluetooth, xbl, tz, ...) through the same path and prints an FNV-1a hash of
 * as many bytes as each image has: tools/ufs-survey.py compares those with the images and with
 * what the loader read itself before the kernel started.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/redwood_input.h>
#include <libkern/crc.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void IOSleep(unsigned int milliseconds);
extern void loader_return(void);        /* osfmk/arm64/loader_return.c */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */

/* the page the loader publishes: the same layout as struct loader_block in xnuloader/loader.c */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      2
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* a whole logical unit, not a partition */

struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;
	} ask __attribute__((aligned(128)));
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
};

#define UFS_PAGE           0x4000UL
#define UFS_TIMEOUT_MS     20000        /* core 0 answers in well under a second; this is for a dead loader */

static volatile struct loader_block *ufs_page;
static const uint8_t *ufs_buffer;
static uint32_t ufs_buffer_size, ufs_parts, ufs_seq;
static uint32_t ufs_bsize[BLOCK_MAX_PARTS];   /* the block size each device is counted in (spec_open sets 512) */
static int ufs_bmajor = -1, ufs_cmajor = -1;
static bool ufs_ready, ufs_dead;
static uint64_t ufs_requests, ufs_errors, ufs_bytes, ufs_wait_us;
static uint64_t ufs_copy_us;                    /* copying out of the loader's buffer */
static uint64_t ufs_small, ufs_medium, ufs_large; /* requests of <= 4 KB, <= 64 KB, more */
static uint64_t ufs_sequential;                 /* requests that start where the one before ended */
static uint32_t ufs_last_part = ~0u;
static uint64_t ufs_last_end;

/*
 * This kernel's own UFS driver (redwood_ufshc.c), with ufs_native=1: each partition's logical
 * unit and where it begins there, in bytes; reads go that way unless it fails, and then through
 * the loader again.
 */
extern bool ufshc_probe(bool only);
extern bool ufshc_available(void);
extern int ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len);
extern void ufshc_report(const char *when);
static uint8_t ufs_lun[BLOCK_MAX_PARTS];         /* 0xff: not known, read through the loader */
static uint64_t ufs_lun_offset[BLOCK_MAX_PARTS];
static bool ufs_native_mapped;
static volatile bool ufs_native_on;
static uint64_t ufs_native_fallbacks;

static LCK_GRP_DECLARE(ufs_lck_grp, "redwood-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_requests,
    "requests the phone's storage has answered through the loader");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_errors,
    "requests the phone's storage has refused or not answered");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_bytes,
    "bytes read from the phone's storage");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_wait_us,
    "microseconds spent waiting for core 0's answers");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_copy_us, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_copy_us,
    "microseconds spent copying out of the loader's buffer");

/*
 * Native blocks [lba, lba + count) of partition part, into dst: one request of at most the
 * buffer's size. Called with ufs_lock held.
 */
static int
ufs_request(uint32_t part, uint64_t lba, uint64_t count, uint8_t *dst)
{
	uint64_t bytes = count * ufs_page->part[part].block_size, started, waited_ms = 0;
	uint32_t seq = ++ufs_seq;
	int64_t status;

	if (ufs_dead) {
		return EIO;
	}
	if (part == ufs_last_part && lba == ufs_last_end) {
		ufs_sequential++;
	}
	ufs_last_part = part;
	ufs_last_end = lba + count;
	ufs_page->ask.part = part;
	ufs_page->ask.lba = lba;
	ufs_page->ask.count = count;
	__asm__ volatile ("dmb sy" ::: "memory");
	ufs_page->ask.request = seq;
	__asm__ volatile ("dmb sy" ::: "memory");

	started = mach_absolute_time();
	while (ufs_page->answer.answered != seq) {
		/* spin briefly for a quick answer, then sleep a millisecond at a time */
		uint64_t us;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &us);
		us /= 1000;
		if (us < 2000) {
			/* the answer to a small request comes in tens of microseconds: look often */
			__builtin_arm_yield();
			continue;
		}
		IOSleep(1);
		if (++waited_ms > UFS_TIMEOUT_MS) {
			/* no answer: the loader is gone or stuck; nothing more is asked of it */
			printf("ufs: no answer from the loader to request %u in %u s: the storage is off from now on\n",
			    seq, UFS_TIMEOUT_MS / 1000);
			ufs_dead = true;
			ufs_errors++;
			return EIO;
		}
	}
	__asm__ volatile ("dmb sy" ::: "memory");
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		ufs_wait_us += ns / 1000;
	}
	status = ufs_page->answer.status;
	if (status != 0) {
		printf("ufs: %s: blocks %llu..%llu: the loader's read failed (status %llx)\n",
		    ufs_page->part[part].name, lba, lba + count - 1, (uint64_t)status);
		ufs_errors++;
		return EIO;
	}
	started = mach_absolute_time();
	/*
	 * The buffer is mapped cacheable here (copying out of an uncached mapping went at 870 MB/s,
	 * as slow as the storage itself). This side never writes it, and core 0 cleans and
	 * invalidates it after every read; whatever this core may still hold of an earlier read --
	 * a line brought in by a prefetch, say -- goes before the copy.
	 */
	FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
	memcpy(dst, ufs_buffer, (size_t)bytes);
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		ufs_copy_us += ns / 1000;
	}
	ufs_requests++;
	ufs_bytes += bytes;
	if (bytes <= 4096) {
		ufs_small++;
	} else if (bytes <= 65536) {
		ufs_medium++;
	} else {
		ufs_large++;
	}
	return 0;
}

/* One of this driver's devices (block or character): a request to it costs tens of microseconds */
bool
redwood_ufs_dev(dev_t dev)
{
	return ufs_ready && ((ufs_bmajor >= 0 && major(dev) == ufs_bmajor) ||
	       (ufs_cmajor >= 0 && major(dev) == ufs_cmajor));
}

/*
 * Where the time of reading the storage went, so far: waiting for core 0's answer (of which
 * the loader's own clock says how long ReadBlocks itself took) and copying out of its buffer.
 */
void
redwood_ufs_report(const char *when)
{
	uint64_t loader_ns = 0;

	if (!ufs_ready) {
		return;
	}
	ufshc_report(when);
	absolutetime_to_nanoseconds(ufs_page->answer.ticks, &loader_ns);
	printf("ufs: %s: %llu requests (%llu of 4 KB or less, %llu to 64 KB, %llu more; %llu right after "
	    "the one before), %llu errors, %llu bytes; waiting %llu ms, of it in ReadBlocks %llu ms; "
	    "copying %llu ms\n", when, ufs_requests, ufs_small, ufs_medium, ufs_large, ufs_sequential,
	    ufs_errors, ufs_bytes, ufs_wait_us / 1000, loader_ns / 1000000, ufs_copy_us / 1000);
}

/*
 * Bytes [offset, offset + len) of partition part into dst, whatever their alignment, through the
 * loader: the native blocks that cover them are read a buffer at a time and the wanted bytes
 * copied out.
 */
static int
ufs_read_mailbox(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	uint64_t nbs = ufs_page->part[part].block_size, per = ufs_buffer_size / nbs;
	static uint8_t *bounce;
	int error = 0;

	if (bounce == NULL) {
		bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
		if (bounce == NULL) {
			return ENOMEM;
		}
	}
	lck_mtx_lock(&ufs_lock);
	while (len > 0 && error == 0) {
		uint64_t first = offset / nbs, skip = offset % nbs;
		uint64_t count = (skip + len + nbs - 1) / nbs, take;

		if (count > per) {
			count = per;
		}
		take = count * nbs - skip;
		if (take > len) {
			take = len;
		}
		if (skip == 0 && take == count * nbs) {
			error = ufs_request(part, first, count, dst);          /* whole blocks: straight in */
		} else {
			error = ufs_request(part, first, count, bounce);
			if (error == 0) {
				memcpy(dst, bounce + skip, (size_t)take);
			}
		}
		offset += take;
		dst += take;
		len -= take;
	}
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* The same, through this kernel's own driver: the partition's unit, from where it begins there */
static int
ufs_read_native(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (part >= ufs_parts || ufs_lun[part] == 0xff) {
		return ENXIO;
	}
	return ufshc_read(ufs_lun[part], ufs_lun_offset[part] + offset, dst, len);
}

/* The way chosen (ufs_native=, kern.redwood.ufs_native); if this kernel's own fails, the loader's */
static int
ufs_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (ufs_native_on && ufs_lun[part] != 0xff) {
		int error = ufs_read_native(part, offset, dst, len);

		if (error == 0) {
			return 0;
		}
		ufs_native_on = false;
		ufs_native_fallbacks++;
		printf("ufs: %s: bytes %llu..%llu: this kernel's own read failed (%d); through the loader from now on\n",
		    ufs_page->part[part].name, offset, offset + len - 1, error);
	}
	return ufs_read_mailbox(part, offset, dst, len);
}

static uint64_t
ufs_part_bytes(uint32_t part)
{
	return ufs_page->part[part].blocks * ufs_page->part[part].block_size;
}

/* ---- the devices ---- */

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (!ufs_ready || (uint32_t)minor(dev) >= ufs_parts || (ufs_page->part[minor(dev)].flags & BLOCK_PART_UNIT)) {
		return ENXIO;
	}
	if (flags & FWRITE) {
		return EROFS;
	}
	return 0;
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
ufs_strategy(buf_t bp)
{
	uint32_t part = (uint32_t)minor(buf_device(bp));
	uint64_t offset, len, size;
	int error;

	if (!ufs_ready || part >= ufs_parts) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!(buf_flags(bp) & B_READ)) {
		buf_seterror(bp, EROFS);         /* this device never writes */
		buf_biodone(bp);
		return;
	}
	size = ufs_part_bytes(part);
	offset = (uint64_t)buf_blkno(bp) * ufs_bsize[part];
	len = buf_count(bp);
	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > size) {
		len = size - offset;
	}
	error = ufs_read(part, offset, (uint8_t *)buf_dataptr(bp), len);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	uint32_t part = (uint32_t)minor(dev), native;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	native = ufs_page->part[part].block_size;
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = ufs_bsize[part];
		return 0;
	case DKIOCSETBLOCKSIZE:
		/*
		 * spec_open() sets the block size to 512 to count the device in 512-byte blocks and
		 * then back, and treats a refusal as a broken device; reads are by byte here, so any
		 * power of two from 512 to the native block size will do.
		 */
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > native ||
		    (*(uint32_t *)data & (*(uint32_t *)data - 1)) != 0) {
			return EINVAL;
		}
		ufs_bsize[part] = *(uint32_t *)data;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = native;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_part_bytes(part) / ufs_bsize[part];
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = ufs_buffer_size / ufs_bsize[part];
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_psize(dev_t dev)
{
	uint32_t part = (uint32_t)minor(dev);

	return ufs_ready && part < ufs_parts ? (int)ufs_bsize[part] : -1;
}

static int
ufs_cread(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(ufs_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))ufs_psize, uio,
	    ufs_bsize[minor(dev) < BLOCK_MAX_PARTS ? minor(dev) : 0]);
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_open,
	.d_close      = ufs_close,
	.d_read       = ufs_cread,
	.d_write      = eno_rdwrt,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/* ---- the check: the partitions there are images of, hashed as the loader hashes them ---- */

static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static bool
ufs_is_reference(const char *name, const char *ref)
{
	size_t n = strlen(ref);

	return strncmp(name, ref, n) == 0 &&
	       (name[n] == 0 || (name[n] == '_' && (name[n + 1] == 'a' || name[n + 1] == 'b') && name[n + 2] == 0));
}

static void
ufs_check(__unused void *arg, __unused wait_result_t wr)
{
	uint8_t *chunk = kalloc_data(1 << 20, Z_WAITOK);

	/*
	 * Not at once: what the kernel prints in its first minute or so does not come back with the
	 * log on the phone (the first line that does is from long after this driver has started), so
	 * the check waits until it will be read.
	 */
	IOSleep(40000);
	printf("ufs: check: %u partitions of the phone's storage, read through the loader's page\n", ufs_parts);
	for (uint32_t part = 0; chunk != NULL && part < ufs_parts; part++) {
		for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
			uint64_t h = 0xcbf29ce484222325ULL, done = 0, started = mach_absolute_time(), ns;
			uint64_t bytes = ufs_references[r].bytes;
			int error = 0;

			if (!ufs_is_reference((const char *)ufs_page->part[part].name, ufs_references[r].name)) {
				continue;
			}
			while (done < bytes && error == 0) {
				uint64_t n = bytes - done < (1 << 20) ? bytes - done : (1 << 20);

				error = ufs_read(part, done, chunk, n);
				for (uint64_t i = 0; error == 0 && i < n; i++) {
					h = (h ^ chunk[i]) * 0x100000001b3ULL;
				}
				done += n;
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
			if (error) {
				printf("ufs: check %s: read failed (%d)\n", ufs_page->part[part].name, error);
			} else {
				printf("ufs: check %s: fnv %llx over %llu bytes, %llu ms\n", ufs_page->part[part].name,
				    h, bytes, ns / 1000000);
			}
		}
	}
	if (chunk != NULL) {
		kfree_data(chunk, 1 << 20);
	}
	printf("ufs: check done: %llu requests, %llu errors, %llu bytes, %llu ms waiting\n",
	    ufs_requests, ufs_errors, ufs_bytes, ufs_wait_us / 1000);
}

/*
 * ufs_bench=1: what one request through the loader costs, by size. The partition "darwin" is
 * read from the front, a different stretch for every size so that nothing the storage itself may
 * have kept is read twice; for each size, MB/s and microseconds a request, split into the time
 * core 0 spent in ReadBlocks (its clock in the page, read before and after), the rest of waiting
 * for its answer (noticing the request, answering), and copying out of its buffer.
 */
static void
ufs_bench(__unused void *arg, __unused wait_result_t wr)
{
	static const uint32_t sizes[] = { 4096, 16384, 65536, 262144, 1048576, 4194304 };
	uint8_t *dst = kalloc_data(ufs_buffer_size, Z_WAITOK);
	uint32_t part = BLOCK_MAX_PARTS;
	uint64_t base = 0;

	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp((const char *)ufs_page->part[i].name, "darwin") == 0) {
			part = i;
		}
	}
	if (dst == NULL || part == BLOCK_MAX_PARTS) {
		printf("ufs: bench: no partition darwin, or no memory\n");
		return;
	}
	for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
		uint64_t total = sizes[z] <= 16384 ? (16ULL << 20) : (128ULL << 20), done = 0, n = 0;
		uint64_t wait0 = ufs_wait_us, copy0 = ufs_copy_us, loader0 = ufs_page->answer.ticks;
		uint64_t started = mach_absolute_time(), ns, loader_ns;
		int error = 0;

		while (done < total && error == 0) {
			error = ufs_read(part, base + done, dst, sizes[z]);
			done += sizes[z];
			n++;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		absolutetime_to_nanoseconds(ufs_page->answer.ticks - loader0, &loader_ns);
		printf("ufs: bench: %7u bytes a request: %4llu MB/s, %llu requests, %llu us each: "
		    "ReadBlocks %llu, the rest of waiting %llu, copying %llu%s\n", sizes[z],
		    ns ? (done * 1000ULL) / ns : 0, n, ns / 1000 / n, loader_ns / 1000 / n,
		    (ufs_wait_us - wait0 - loader_ns / 1000) / n, (ufs_copy_us - copy0) / n,
		    error ? " (an error)" : "");
		base += total;
	}
	kfree_data(dst, ufs_buffer_size);
	redwood_ufs_report("after the bench");
}

/* ---- this kernel's own driver: which unit is which, and whether it reads what the loader reads ---- */

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/*
 * Each unit's logical unit number: the one its device path gives, if it gives one, else the first
 * of 0..7 not yet taken whose block 1 (the GPT header, with the unit's own GUID in it) is what the
 * loader reads as block 1 of that unit. Either way it is so only if the two are the same.
 */
static bool
ufs_native_map(void)
{
	uint8_t *a = kalloc_data(2 * 4096, Z_WAITOK), *b = a + 4096;
	uint8_t unit_lun[BLOCK_MAX_UNITS];
	uint32_t taken = 0, units = 0;
	bool all = true;

	memset(unit_lun, 0xff, sizeof(unit_lun));
	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	if (a == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < ufs_parts && all; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		bool found = false;

		if ((e->flags & BLOCK_PART_UNIT) == 0) {
			continue;
		}
		units++;
		if (e->unit >= BLOCK_MAX_UNITS || e->block_size != 4096 || ufs_read_mailbox(i, 4096, a, 4096) != 0 ||
		    memcmp(a, "EFI PART", 8) != 0) {
			printf("ufs: native: %s: no GPT header to know it by (block size %u)\n", e->name, e->block_size);
			all = false;
			break;
		}
		for (uint32_t k = 0; k < 8 && !found; k++) {
			uint32_t lun = e->lun != 0xff ? e->lun : (k == 0 ? e->unit : k - 1);

			if (lun >= 8 || (taken & (1u << lun)) || (e->lun != 0xff && k > 0)) {
				continue;
			}
			if (ufshc_read((uint8_t)lun, 4096, b, 4096) == 0 && memcmp(a, b, 4096) == 0) {
				unit_lun[e->unit] = (uint8_t)lun;
				taken |= 1u << lun;
				found = true;
				printf("ufs: native: %s is LUN %u (%s; its GPT header read both ways is the same)\n", e->name, lun,
				    e->lun != 0xff ? "its device path says so" : "found by its GPT header");
			}
		}
		if (!found) {
			printf("ufs: native: %s: no logical unit reads as it does through the loader\n", e->name);
			all = false;
		}
	}
	kfree_data(a, 2 * 4096);
	if (!all || units == 0) {
		memset(ufs_lun, 0xff, sizeof(ufs_lun));
		return false;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];

		if (e->unit < BLOCK_MAX_UNITS && unit_lun[e->unit] != 0xff && e->block_size == 4096) {
			ufs_lun[i] = unit_lun[e->unit];
			ufs_lun_offset[i] = e->start_lba * 4096;
		}
	}
	return true;
}

static uint64_t ufs_cmp_mailbox_ns, ufs_cmp_native_ns, ufs_cmp_bytes, ufs_cmp_regions, ufs_cmp_differ;

/*
 * Bytes [offset, offset + len) of entry part, read both ways a megabyte at a time and compared;
 * FNV-1a of what was read (each way), and of its first ref bytes if ref is not 0.
 */
static bool
ufs_compare(uint32_t part, uint64_t offset, uint64_t len, uint64_t ref, const char *what, bool quiet)
{
	static uint8_t *x, *y;
	uint64_t hx = 0xcbf29ce484222325ULL, hy = hx, href = 0, done = 0, t, ns, mailbox_ns = 0, native_ns = 0;
	int ex = 0, ey = 0;
	bool same = true;

	if (x == NULL) {
		x = kalloc_data(2 << 20, Z_WAITOK);
		y = x + (1 << 20);
	}
	while (done < len && same && ex == 0 && ey == 0) {
		uint64_t n = len - done < (1 << 20) ? len - done : (1 << 20);

		t = mach_absolute_time();
		ex = ufs_read_mailbox(part, offset + done, x, n);
		absolutetime_to_nanoseconds(mach_absolute_time() - t, &ns);
		mailbox_ns += ns;
		t = mach_absolute_time();
		ey = ufs_read_native(part, offset + done, y, n);
		absolutetime_to_nanoseconds(mach_absolute_time() - t, &ns);
		native_ns += ns;
		if (ex == 0 && ey == 0 && memcmp(x, y, n) != 0) {
			uint64_t at = 0;

			while (x[at] == y[at]) {
				at++;
			}
			printf("ufs: native check: %s %s: DIFFERENT at byte %llu (%02x through the loader, %02x here)\n",
			    ufs_page->part[part].name, what, offset + done + at, x[at], y[at]);
			same = false;
		}
		if (ref != 0 && done < ref) {
			uint64_t m = ref - done < n ? ref - done : n;

			href = ufs_fnv(done == 0 ? 0xcbf29ce484222325ULL : href, x, m);
		}
		hx = ufs_fnv(hx, x, n);
		hy = ufs_fnv(hy, y, n);
		done += n;
	}
	if (ex != 0 || ey != 0) {
		printf("ufs: native check: %s %s: read failed (through the loader %d, here %d)\n", ufs_page->part[part].name,
		    what, ex, ey);
		same = false;
	}
	ufs_cmp_regions++;
	ufs_cmp_bytes += done;
	ufs_cmp_mailbox_ns += mailbox_ns;
	ufs_cmp_native_ns += native_ns;
	ufs_cmp_differ += !same;
	if (!quiet || !same) {
		printf("ufs: native check: %s %s: %llu bytes from %llu %s: fnv %llx / %llx; loader %llu ms, here %llu ms\n",
		    ufs_page->part[part].name, what, done, offset, same ? "the same" : "NOT THE SAME", hx, hy,
		    mailbox_ns / 1000000, native_ns / 1000000);
		if (ref != 0) {
			printf("ufs: native check: %s: fnv %llx over the first %llu bytes (the loader's and ufs_check's hash)\n",
			    ufs_page->part[part].name, href, ref);
		}
	}
	return same;
}

static uint32_t
ufs_entry(const char *name)
{
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp((const char *)ufs_page->part[i].name, name) == 0) {
			return i;
		}
	}
	return BLOCK_MAX_PARTS;
}

/*
 * ufs_native_check=1: what this kernel's own driver reads is what the loader reads, byte for
 * byte: every unit's GPT (protective MBR, header, table, and the backup table and header at the
 * end), the partitions the loader hashes (the ROM's images: xbl, tz, modem, ...) whole, the start
 * of super (its geometry and metadata), and of darwin 64 spread megabytes, 512 scattered blocks
 * and some reads that begin and end inside a block.
 */
static void
ufs_native_check(void)
{
	uint8_t *h = kalloc_data(4096, Z_WAITOK);
	uint32_t part;
	uint64_t seed = 0x9e3779b97f4a7c15ULL;

	printf("ufs: native check: through the loader and through this kernel's own driver, compared\n");
	for (uint32_t i = 0; h != NULL && i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		uint64_t entries_lba, count, size, table, alt, alt_entries;

		if ((e->flags & BLOCK_PART_UNIT) == 0 || ufs_read_mailbox(i, 4096, h, 4096) != 0) {
			continue;
		}
		entries_lba = *(uint64_t *)(h + 72);
		count = *(uint32_t *)(h + 80);
		size = *(uint32_t *)(h + 84);
		alt = *(uint64_t *)(h + 32);
		table = (count * size + 4095) / 4096;
		{
			uint32_t want = *(uint32_t *)(h + 16), got;

			*(uint32_t *)(h + 16) = 0;
			got = crc32(0, h, *(uint32_t *)(h + 12));
			printf("ufs: native check: %s: GPT header crc %s, table at block %llu, %llu entries of %llu bytes, backup "
			    "header at block %llu\n", e->name, got == want ? "right" : "WRONG", entries_lba, count, size, alt);
		}
		ufs_compare(i, 0, (entries_lba + table) * 4096, 0, "GPT (MBR, header, table)", false);
		if (alt < e->blocks && ufs_read_mailbox(i, alt * 4096, h, 4096) == 0 && memcmp(h, "EFI PART", 8) == 0) {
			alt_entries = *(uint64_t *)(h + 72);
			if (alt_entries <= alt) {
				ufs_compare(i, alt_entries * 4096, (alt + 1 - alt_entries) * 4096, 0, "backup GPT (table, header)",
				    false);
			}
		}
	}
	for (part = 0; part < ufs_parts; part++) {
		if (ufs_page->part[part].flags & BLOCK_PART_UNIT) {
			continue;
		}
		for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
			if (ufs_is_reference((const char *)ufs_page->part[part].name, ufs_references[r].name)) {
				ufs_compare(part, 0, ufs_part_bytes(part), ufs_references[r].bytes, "whole", false);
			}
		}
	}
	if ((part = ufs_entry("super")) < BLOCK_MAX_PARTS) {
		ufs_compare(part, 0, 8 << 20, 0, "first 8 MB (geometry, metadata)", false);
	}
	if ((part = ufs_entry("darwin")) < BLOCK_MAX_PARTS) {
		uint64_t bytes = ufs_part_bytes(part), r0 = ufs_cmp_regions, d0 = ufs_cmp_differ;

		for (uint64_t k = 0; k < 64; k++) {
			ufs_compare(part, (bytes - (1 << 20)) / 64 * k / 4096 * 4096, 1 << 20, 0, "spread megabyte", true);
		}
		for (uint64_t k = 0; k < 512; k++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			ufs_compare(part, (seed >> 16) % (bytes / 4096) * 4096, 4096, 0, "scattered block", true);
		}
		for (uint64_t k = 0; k < 64; k++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			ufs_compare(part, (seed >> 16) % (bytes - (2 << 20)), 1 + (seed >> 40) % (1 << 20), 0, "unaligned piece", true);
		}
		printf("ufs: native check: darwin: %llu spread megabytes, scattered blocks and unaligned pieces: %llu not the same\n",
		    ufs_cmp_regions - r0, ufs_cmp_differ - d0);
	}
	if (h != NULL) {
		kfree_data(h, 4096);
	}
	printf("ufs: native check done: %llu regions, %llu bytes, %llu NOT THE SAME; through the loader %llu ms, "
	    "through this kernel's own driver %llu ms\n", ufs_cmp_regions, ufs_cmp_bytes, ufs_cmp_differ,
	    ufs_cmp_mailbox_ns / 1000000, ufs_cmp_native_ns / 1000000);
	ufshc_report("after the check");
}

/* kern.redwood.ufs_native: 1 while reads go through this kernel's own driver; 0 or 1 may be set */
static int
ufs_sysctl_native(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_native_on, error, changed = 0;

	error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error != 0 || !changed) {
		return error;
	}
	if (value != 0 && !(ufs_native_mapped && ufshc_available())) {
		return ENXIO;
	}
	ufs_native_on = value != 0;
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, ufs_native, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_native, "I", "1: the phone's storage is read by this kernel's own UFS driver, 0: through the loader");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_fallbacks, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_native_fallbacks,
    "times a read of this kernel's own UFS driver failed and the loader's was used instead");

/* ---- bringing it up ---- */

void
redwood_ufs_init(void)
{
	uint64_t pa = 0, buffer_pa;
	uint32_t check = 0, made = 0;
	vm_offset_t base;

	if (!PE_parse_boot_argn("ufs_shared_pa", &pa, sizeof(pa)) || pa == 0) {
		return;                 /* the loader was not asked for ufs=2: there is nothing to read through */
	}
	base = ml_io_map_unmappable((vm_offset_t)(pa & ~(UFS_PAGE - 1)),
	    ((pa & (UFS_PAGE - 1)) + sizeof(struct loader_block) + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_WCOMB);
	ufs_page = (volatile struct loader_block *)(base + (pa & (UFS_PAGE - 1)));
	if (ufs_page->magic != BLOCK_MAGIC || ufs_page->version != BLOCK_VERSION ||
	    ufs_page->size != sizeof(struct loader_block) || ufs_page->parts > BLOCK_MAX_PARTS) {
		printf("ufs: the loader's page at %llx is not one this kernel knows (magic %llx, version %u, size %u)\n",
		    pa, ufs_page->magic, ufs_page->version, ufs_page->size);
		return;
	}
	ufs_parts = ufs_page->parts;
	ufs_buffer_size = ufs_page->buffer_size;
	buffer_pa = ufs_page->buffer_pa;
	ufs_buffer = (const uint8_t *)(ml_io_map_unmappable((vm_offset_t)(buffer_pa & ~(UFS_PAGE - 1)),
	    ((buffer_pa & (UFS_PAGE - 1)) + ufs_buffer_size + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_DEFAULT) +
	    (buffer_pa & (UFS_PAGE - 1)));
	ufs_seq = ufs_page->answer.answered;      /* a loader that served an earlier kernel counts on from there */

	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	ufs_cmajor = ufs_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor);
	if (ufs_cmajor < 0) {
		printf("ufs: no device numbers to be had\n");
		return;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		char name[48];

		ufs_bsize[i] = ufs_page->part[i].block_size;
		if (ufs_page->part[i].flags & BLOCK_PART_UNIT) {
			continue;               /* the units are for this kernel's own driver to be checked against */
		}
		snprintf(name, sizeof(name), "ufs_%s", ufs_page->part[i].name);
		if (devfs_make_node(makedev(ufs_bmajor, i), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "%s", name) == NULL) {
			printf("ufs: no node for %s (the same name twice?)\n", name);
			continue;
		}
		snprintf(name, sizeof(name), "rufs_%s", ufs_page->part[i].name);
		(void)devfs_make_node(makedev(ufs_cmajor, i), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "%s", name);
		made++;
	}
	ufs_ready = true;
	printf("ufs: %u partitions of the phone's storage as /dev/ufs_NAME and /dev/rufs_NAME, read only, "
	    "through the loader's page at %llx and its %u KB buffer\n", made, pa, ufs_buffer_size >> 10);

	/* ufs_native=1: through this kernel's own driver instead, if the controller is as expected; 2: only look */
	check = 0;
	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	if (PE_parse_boot_argn("ufs_native", &check, sizeof(check)) && check != 0) {
		if (ufshc_probe(check == 2) && ufs_native_map()) {
			ufs_native_mapped = true;
			ufs_native_on = true;
			printf("ufs: the partitions are read by this kernel's own UFS driver (kern.redwood.ufs_native=0: "
			    "through the loader)\n");
		} else if (check != 2) {
			printf("ufs: this kernel's own UFS driver is not used: through the loader\n");
		}
		check = 0;
		if (ufs_native_mapped && PE_parse_boot_argn("ufs_native_check", &check, sizeof(check)) && check != 0) {
			/* right here, in bsd_init's thread, and then back to the loader */
			ufs_native_check();
			redwood_ufs_report("after the check");
			loader_return();
		}
	}

	if (PE_parse_boot_argn("ufs_bench", &check, sizeof(check)) && check != 0) {
		/* right here, in bsd_init's thread, and then back to the loader: nothing else is wanted */
		ufs_bench(NULL, THREAD_AWAKENED);
		loader_return();
	}
	check = 0;
	if (PE_parse_boot_argn("ufs_check", &check, sizeof(check)) && check != 0) {
		thread_t thread;

		if (kernel_thread_start(ufs_check, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
