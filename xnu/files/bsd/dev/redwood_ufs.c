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
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void IOSleep(unsigned int milliseconds);
extern void loader_return(void);        /* osfmk/arm64/loader_return.c */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */

/* the page the loader publishes: the same layout as struct loader_block in xnuloader/loader.c */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      1
#define BLOCK_MAX_PARTS    128

struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
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
	absolutetime_to_nanoseconds(ufs_page->answer.ticks, &loader_ns);
	printf("ufs: %s: %llu requests (%llu of 4 KB or less, %llu to 64 KB, %llu more), %llu errors, "
	    "%llu bytes; waiting %llu ms, of it in ReadBlocks %llu ms; copying %llu ms\n", when,
	    ufs_requests, ufs_small, ufs_medium, ufs_large, ufs_errors, ufs_bytes, ufs_wait_us / 1000,
	    loader_ns / 1000000, ufs_copy_us / 1000);
}

/*
 * Bytes [offset, offset + len) of partition part into dst, whatever their alignment: the native
 * blocks that cover them are read a buffer at a time and the wanted bytes copied out.
 */
static int
ufs_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
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

static uint64_t
ufs_part_bytes(uint32_t part)
{
	return ufs_page->part[part].blocks * ufs_page->part[part].block_size;
}

/* ---- the devices ---- */

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (!ufs_ready || (uint32_t)minor(dev) >= ufs_parts) {
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
