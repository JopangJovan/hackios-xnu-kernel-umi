/*
 * /dev/hakios-touch: a queue of touch samples (see sys/hakios.input.h).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/proc.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <pexpert/pexpert.h>

#define TOUCH_QUEUE     512             /* records; a power of two */

/*
 * Boot-arg touch_lat=1: for measuring how long a touch takes to reach the panel, a line for every
 * sample posted ("tl: p TYPE FINGER X Y at US", the time in microseconds of mach_absolute_time),
 * to be matched with the window server's lines about reading it (userland/src/inject.c), drawing
 * (eagl.c) and the display driver's about the frame (MiniIOMobileFramebuffer.cpp): see
 * tools/latency.py.
 */
static uint32_t touch_lat = 2;          /* 2: not yet looked up */

extern void loader_log_line(const char *line);

/*
 * A "tl:" line, for the kernel's drivers: into the loader's log as it is written -- the kernel's own
 * log keeps only the last of a run, and a live look at the loader's (XL_FOLLOW) then misses nothing.
 */
void hakios_tl(const char *format, ...) __printflike(1, 2);

void
hakios_tl(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static bool
latency_lines(void)
{
	if (touch_lat == 2) {
		uint32_t on = 0;

		touch_lat = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	return touch_lat != 0;
}

static LCK_GRP_DECLARE(touch_lck_grp, "hakios-touch");
static LCK_MTX_DECLARE(touch_lock, &touch_lck_grp);
static struct hakios_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;        /* read from head, add at tail */

/*
 * The way out when nothing else is at hand: five fingers held on the screen for a second and a
 * half end the run -- control goes back to the loader, as when loader_return_after expires.
 * Only a driver's fingers count, not samples a program writes to the device.
 */
#define EXIT_FINGERS    5
#define EXIT_HOLD_MS    1500

extern void loader_return(void);

static thread_call_t exit_call;
static uint32_t fingers_down;           /* one bit for each finger a driver says is down */

static void
exit_hold_elapsed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("\n--- %d fingers held on the screen for %d ms: back to the loader ---\n", EXIT_FINGERS, EXIT_HOLD_MS);
	hakios_adreno_stop();
	loader_return();
}

/* With touch_lock held. */
static void
count_fingers(uint32_t type, uint32_t finger)
{
	int before = __builtin_popcount(fingers_down), now;
	uint64_t deadline;

	if (finger >= 32 || exit_call == NULL || (type != HAKIOS_TOUCH_DOWN && type != HAKIOS_TOUCH_UP)) {
		return;
	}
	if (type == HAKIOS_TOUCH_DOWN) {
		fingers_down |= 1u << finger;
	} else {
		fingers_down &= ~(1u << finger);
	}
	now = __builtin_popcount(fingers_down);
	if (now >= EXIT_FINGERS && before < EXIT_FINGERS) {
		clock_interval_to_deadline(EXIT_HOLD_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(exit_call, deadline);
	} else if (now < EXIT_FINGERS && before >= EXIT_FINGERS) {
		thread_call_cancel(exit_call);
	}
}

static void
touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, bool from_driver)
{
	struct hakios_touch *t;

	lck_mtx_lock(&touch_lock);
	if (touch_tail - touch_head == TOUCH_QUEUE) {
		touch_head++;                   /* nobody reads: drop the oldest */
	}
	t = &touch_queue[touch_tail++ % TOUCH_QUEUE];
	t->time = mach_absolute_time();
	if (latency_lines()) {
		uint64_t ns;

		absolutetime_to_nanoseconds(t->time, &ns);
		hakios_tl("tl: p %u %u %u %u at %llu\n", type, finger, x, y, ns / 1000);
	}
	t->type = type;
	t->finger = finger;
	t->x = x > 65535 ? 65535 : x;
	t->y = y > 65535 ? 65535 : y;
	if (from_driver) {
		count_fingers(type, finger);
	}
	lck_mtx_unlock(&touch_lock);
	wakeup(&touch_queue);
}

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	touch_post(type, finger, x, y, true);
}

static int
touch_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0, moved = 0;

	/* a buffer smaller than one record can never be served: say so, instead of blocking for data and then returning 0 (end of file) */
	if (uio_resid(uio) == 0) {
		return 0;
	}
	if (uio_resid(uio) < (user_ssize_t)sizeof(t)) {
		return EINVAL;
	}
	lck_mtx_lock(&touch_lock);
	while (touch_head == touch_tail) {
		error = msleep(&touch_queue, &touch_lock, PZERO | PCATCH, "hakios-touch", NULL);
		if (error) {
			lck_mtx_unlock(&touch_lock);
			return error;
		}
	}
	while (touch_head != touch_tail && uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		t = touch_queue[touch_head++ % TOUCH_QUEUE];
		lck_mtx_unlock(&touch_lock);
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		lck_mtx_lock(&touch_lock);
		if (error) {
			/* the copy-out failed: put the record back at the head so a lost DOWN or UP does not leave a finger stuck (only if the ring still has room for it) */
			if (touch_tail - touch_head < TOUCH_QUEUE) {
				touch_queue[--touch_head % TOUCH_QUEUE] = t;
			}
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&touch_lock);
	return moved ? 0 : error;
}

static int
touch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		if (error) {
			break;
		}
		if (t.type < HAKIOS_TOUCH_DOWN || t.type > HAKIOS_BUTTON_UP) {
			return EINVAL;
		}
		touch_post(t.type, t.finger, t.x, t.y, false);
	}
	return error;
}

static const struct cdevsw touch_cdevsw = {
	.d_open = touch_open, .d_close = touch_close, .d_read = touch_read, .d_write = touch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

extern void umi_touch_init(void);
extern void umi_slpi_init(void);   /* (boot-arg slpi=1: the sensor DSP probe) */
extern void umi_keys_init(void);

void
hakios_input_init(void)
{
	int major = cdevsw_add(-1, &touch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-touch") == NULL) {
		printf("hakios-touch: cannot create the device\n");
	}
	exit_call = thread_call_allocate(exit_hold_elapsed, NULL);
	hakios_cpufreq_init();
	hakios_virtio_input_init();
	umi_touch_init();
	umi_keys_init();
	hakios_goodix_init();
	hakios_buttons_init();
	hakios_battery_init();
	hakios_thermal_init();
	hakios_ufs_init();
	hakios_net_init();
	hakios_adreno_init();
	hakios_wifi_init();
	hakios_adsp_init();
	hakios_mpss_init();
	/* SLPI's registry and sensor services depend on the remote-service fabric being ready. */
	umi_slpi_init();
}

/* ---- the sensor DSP (SLPI): a probe, boot-arg slpi=1 -------------------------------------------------- */
/*
 * Everything about the accelerometer, the gyroscope and the magnetometer on this phone is behind
 * SLPI (qcom,ssc@5c00000; Android's sensor list is all Qualcomm SSC). This probe answers whether
 * that DSP can be brought up at all: load its firmware, hand the header to TrustZone, put the
 * segments in its carveout, ask for it to be started. The numbers are this phone's own tree:
 *   qcom,pas-id 0x0c; reserved-memory/pil_slpi_region@88c00000 = 0x88c00000, 0x02f00000 (47 MB);
 *   qcom,firmware-name "slpi" -> /fw/slpi.mdt and slpi.b00..b21 (relocatable ELF, flag bit 27).
 */

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern lck_mtx_t hakios_smem_alloc_lock;
extern lck_mtx_t hakios_smmu_lock;
extern kern_return_t thread_terminate(thread_t);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define SLPI_PAS                12u
#define SLPI_MEMORY             0x88c00000u
#define SLPI_SIZE               0x02f00000u
#define SLPI_MAX_SEGMENTS       48
#define SLPI_MDT_HASH(flags)    (((flags) >> 24 & 7) == 2)
#define SLPI_MDT_RELOCATABLE    (1u << 27)

struct slpi_elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct { uint32_t at, file_size, memory_size; const uint8_t *file; } segment[SLPI_MAX_SEGMENTS];
	unsigned segments;
	uint32_t size, segment_base;
	bool relocatable;
} slpi_image;

static const uint8_t *
slpi_read_file(const char *name, uint64_t *size)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);

	if (data == NULL) {
		hakios_tl("slpi: %s is not in fw/ of the small image\n", name);
	}
	return data;
}

static bool
slpi_image_load(void)
{
	struct slpi_elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0, highest = 0;
	uint32_t count, hashes = 0, lowest = ~0u;
	unsigned loadable = 0;
	uint8_t first[4] = { 0 };

	hakios_tl("slpi: find the sensor DSP's image and check it against its own header\n");
	bzero(&slpi_image, sizeof(slpi_image));
	header = slpi_read_file("slpi.mdt", &size);
	if (header == NULL) {
		return false;
	}
	slpi_image.header = header;
	slpi_image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	memcpy(first, header, size < sizeof(first) ? size : sizeof(first));
	if (size < 5 || memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > SLPI_MAX_SEGMENTS ||
	    52 + 32 * count > size) {
		hakios_tl("slpi: slpi.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", first[0],
		    first[1], first[2], first[3], count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint64_t end;

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (SLPI_MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || SLPI_MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.file_size > segment.memory_size) {
			hakios_tl("slpi: segment %u has %u file bytes but only %u memory bytes\n", i, segment.file_size,
			    segment.memory_size);
			return false;
		}
		slpi_image.relocatable |= (segment.flags & SLPI_MDT_RELOCATABLE) != 0;
		end = (uint64_t)segment.paddr + segment.memory_size;
		if (end > UINT32_MAX) {
			hakios_tl("slpi: segment %u wraps the address space\n", i);
			return false;
		}
		lowest = MIN(lowest, segment.paddr);
		highest = MAX(highest, end);
		loadable++;
	}
	slpi_image.segment_base = slpi_image.relocatable ? lowest : SLPI_MEMORY;
	if (loadable == 0 || lowest < slpi_image.segment_base || highest < slpi_image.segment_base ||
	    highest - slpi_image.segment_base > SLPI_SIZE) {
		hakios_tl("slpi: the image span %#x..%#llx does not fit the DSP's %#x-byte carveout at %#x\n", lowest,
		    highest, SLPI_SIZE, SLPI_MEMORY);
		return false;
	}
	slpi_image.size = (uint32_t)((highest - slpi_image.segment_base + 0xfff) & ~0xfffull);
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (segment.type != 1 || SLPI_MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		unsigned s = slpi_image.segments++;
		slpi_image.segment[s].at = segment.paddr - slpi_image.segment_base;
		slpi_image.segment[s].file_size = segment.file_size;
		slpi_image.segment[s].memory_size = segment.memory_size;
		slpi_image.segment[s].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "slpi.b%02u", i);
			slpi_image.segment[s].file = slpi_read_file(name, &file_size);
			if (slpi_image.segment[s].file == NULL || file_size != segment.file_size) {
				hakios_tl("slpi: fw/%s is %llu bytes, the header says %u\n", name, file_size,
				    segment.file_size);
				return false;
			}
			files += file_size;
		}
	}
	if (slpi_image.segments != loadable || 52 + 32 * count + (uint64_t)hashes != size) {
		hakios_tl("slpi: not what TrustZone takes: %u segments, %llu bytes (%u of hashes)\n", slpi_image.segments,
		    size, hashes);
		return false;
	}
	hakios_tl("slpi: the image is whole (%s): %u segments, %llu bytes, span %#x from %#x into %#x\n",
	    slpi_image.relocatable ? "relocatable" : "fixed", slpi_image.segments, files, slpi_image.size,
	    slpi_image.segment_base, SLPI_MEMORY);
	return true;
}

#define SLPI_SCM_64             (1ull << 30)
#define SLPI_SCM_SIP            (2ull << 24)
#define SLPI_SCM_CALL(s, c)     ((uint64_t)(s) << 8 | (c))
#define SLPI_SCM_INIT_IMAGE     SLPI_SCM_CALL(2, 1)
#define SLPI_SCM_MEM_SETUP      SLPI_SCM_CALL(2, 2)
#define SLPI_SCM_AUTH_RESET     SLPI_SCM_CALL(2, 5)
#define SLPI_SCM_ONE_VALUE      1
#define SLPI_SCM_THREE_VALUES   3
#define SLPI_SCM_VALUE_AND_BUF  (2 | 2 << 6)
#define SLPI_SCM_INTERRUPTED    1

static int32_t
slpi_scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third)
{
	uint64_t session = 0, answer = 0;

	function |= SLPI_SCM_64 | SLPI_SCM_SIP;
	hakios_tl("slpi: ask TrustZone %s: smc %#llx(%#llx, %#llx, %#llx)\n", what, function, first, second, third);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SLPI_SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SLPI_SCM_INTERRUPTED) {
			answer = x1;
			hakios_tl("slpi: TrustZone returns %d, answer %#llx\n", (int32_t)x0, answer);
			return (int32_t)(x0 != 0 ? x0 : answer);
		}
	}
	hakios_tl("slpi: TrustZone keeps saying the call was interrupted\n");
	return -1;
}


/* ---- SLPI's side of the shared memory: which partition it has, and the items in it ---------------- */
/*
 * GLINK's queues to the sensor DSP are items of the SMEM partition the two share (the audio DSP's
 * are 478-480 of the partition {0,2}). This walks the same tables the audio DSP's bring-up does and
 * prints, for every partition in the table of contents, its hosts; and for the one SLPI shares with
 * us, every item in it -- the numbers a GLINK bring-up needs, all in the same run.
 */

#define SLPI_SMEM_PHYS          0x80900000UL
#define SLPI_SMEM_SIZE          0x00200000u
#define SLPI_SMEM_US            0
#define SLPI_SMEM_SLPI          3               /* (dt: qcom,remote-pid of qcom,glink/dsps) */
#define SLPI_SMEM_GLOBAL        0xfffe
#define SLPI_SMEM_CANARY        0xa5a5

struct slpi_smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct slpi_smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct slpi_smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct slpi_smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};

static const volatile uint8_t *slpi_smem;

static bool
slpi_smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (slpi_smem == NULL) {
		slpi_smem = (const volatile uint8_t *)ml_io_map_unmappable(SLPI_SMEM_PHYS, SLPI_SMEM_SIZE, 0x0080);
	}
	if (slpi_smem == NULL || offset + size > SLPI_SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = slpi_smem[offset + i];
	}
	return true;
}

static void
slpi_smem_items(uint32_t partition, uint32_t cacheline, const char *label)
{
	struct slpi_smem_partition header;
	struct slpi_smem_entry entry;
	uint32_t at;
	unsigned count = 0;

	if (partition == 0 || !slpi_smem_peek(partition, &header, sizeof(header))) {
		return;
	}
	hakios_tl("slpi: partition %s: %u bytes, %u free uncached, hosts %u and %u\n", label, header.size,
	    header.free_uncached, header.host0, header.host1);
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!slpi_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SLPI_SMEM_CANARY ||
		    entry.size > header.size) {
			break;
		}
		if (count < 40) {
			hakios_tl("slpi:   item %u: %u bytes at +%#x\n", entry.item, entry.size - entry.padding_data,
			    at + (uint32_t)sizeof(entry) + entry.padding_header);
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline != 0 && (cacheline & (cacheline - 1)) == 0) {
		uint32_t step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);

		for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
			if (!slpi_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SLPI_SMEM_CANARY ||
			    entry.size > at || entry.padding_data > entry.size) {
				break;
			}
			if (count < 40) {
				hakios_tl("slpi:   item %u: %u bytes at +%#x (from the end)\n", entry.item,
				    entry.size - entry.padding_data, at - entry.size);
			}
			count++;
			if (entry.size + step_back > at) {
				break;
			}
			at -= entry.size + step_back;
		}
	}
	hakios_tl("slpi: partition %s: %u items\n", label, count);
}

static void
slpi_smem_probe(void)
{
	struct slpi_smem_table table;
	struct slpi_smem_table_entry entry;

	hakios_tl("slpi: the shared memory's table of contents\n");
	if (!slpi_smem_peek(SLPI_SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0) {
		hakios_tl("slpi: no $TOC at the end of the shared memory\n");
		return;
	}
	hakios_tl("slpi: $TOC version %u, %u partitions\n", table.version, table.count);
	for (uint32_t i = 0; i < table.count && i < 64; i++) {
		if (!slpi_smem_peek(SLPI_SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			break;
		}
		if (entry.offset == 0 || entry.size == 0) {
			continue;
		}
		hakios_tl("slpi: partition %u: hosts %u and %u, %u bytes at %#x\n", i, entry.host0, entry.host1, entry.size,
		    entry.offset);
		if ((entry.host0 == SLPI_SMEM_SLPI && entry.host1 == SLPI_SMEM_US) ||
		    (entry.host0 == SLPI_SMEM_US && entry.host1 == SLPI_SMEM_SLPI)) {
			slpi_smem_items(entry.offset, entry.cacheline, "ours with SLPI");
		}
	}
}

/*
 * The UMI's qcom,smp2p-dsps and qcom,glink/dsps nodes describe a different
 * remote processor from audio DSP.  This stage only creates APSS-owned SMEM
 * items and waits for SLPI-owned items; it never writes a remote queue.
 */
#define SLPI_IPCC_PHYS          0x00408000UL
#define SLPI_IPCC_SIZE          0x1000u
#define SLPI_IPCC_SEND          0x0cu
#define SLPI_IPCC_RECEIVED      0x10u
#define SLPI_IPCC_CLEAR         0x1cu
#define SLPI_IPCC_NOBODY        0xffffffffu
#define SLPI_IPCC_CLIENT          4u
#define SLPI_IPCC_GLINK           0u
#define SLPI_IPCC_SMP2P           2u
#define SLPI_ITEM_SMP2P_OURS      430u
#define SLPI_ITEM_SMP2P_THEIRS    481u
#define SLPI_ITEM_GLINK_DESCRIPTOR 478u
#define SLPI_ITEM_GLINK_OUR_QUEUE 479u
#define SLPI_ITEM_GLINK_THEIR_QUEUE 480u
#define SLPI_QUEUE_TIMEOUT_MS   5000u
#define SLPI_SMEM_LOCK_PHYS     0x01f43000UL
#define SLPI_SMEM_LOCK_OWNER    0u
#define SLPI_SMEM_LOCK_US       1u

struct slpi_smp2p {
	uint32_t magic, version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct { char name[16]; uint32_t value; } entry[16];
};

static vm_offset_t slpi_ipcc, slpi_smem_lock;

static void
slpi_settle(void)
{
	__asm__ volatile ("dsb sy" : : : "memory");
}

static bool
slpi_smem_poke(uint32_t offset, uint32_t value)
{
	if (slpi_smem == NULL || offset > SLPI_SMEM_SIZE - sizeof(uint32_t)) {
		return false;
	}
	*(volatile uint32_t *)(void *)(slpi_smem + offset) = value;
	return true;
}

static bool
slpi_transport_partition(uint32_t *partition, struct slpi_smem_partition *header)
{
	struct slpi_smem_table table;
	struct slpi_smem_table_entry entry;

	if (!slpi_smem_peek(SLPI_SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 ||
	    table.version != 1 || table.count > 64) {
		return false;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!slpi_smem_peek(SLPI_SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			return false;
		}
		if (!((entry.host0 == SLPI_SMEM_US && entry.host1 == SLPI_SMEM_SLPI) ||
		    (entry.host0 == SLPI_SMEM_SLPI && entry.host1 == SLPI_SMEM_US))) {
			continue;
		}
		if (entry.offset == 0 || entry.size < sizeof(*header) || entry.offset > SLPI_SMEM_SIZE ||
		    entry.size > SLPI_SMEM_SIZE - entry.offset || !slpi_smem_peek(entry.offset, header, sizeof(*header)) ||
		    memcmp(header->magic, "$PRT", 4) != 0 || header->size != entry.size ||
		    header->free_uncached > header->free_cached || header->free_cached > header->size) {
			return false;
		}
		*partition = entry.offset;
		return true;
	}
	return false;
}

static bool
slpi_transport_item(uint32_t partition, const struct slpi_smem_partition *header, uint16_t wanted, uint32_t *at, uint32_t *size)
{
	struct slpi_smem_entry entry;
	uint32_t here;

	for (here = sizeof(*header); here + sizeof(entry) <= header->free_uncached;) {
		if (!slpi_smem_peek(partition + here, &entry, sizeof(entry)) || entry.canary != SLPI_SMEM_CANARY ||
		    entry.size > header->size || entry.padding_data > entry.size || entry.padding_header > entry.size ||
		    here + sizeof(entry) > header->free_uncached || entry.padding_header + entry.size > header->free_uncached - here - sizeof(entry)) {
			return false;
		}
		if (entry.item == wanted) {
			*at = partition + here + sizeof(entry) + entry.padding_header;
			*size = entry.size - entry.padding_data;
			return true;
		}
		here += sizeof(entry) + entry.padding_header + entry.size;
	}
	return false;
}

/* Allocate only an APSS-owned private item, under Qualcomm's hardware lock. */
static bool
slpi_transport_make(uint32_t partition, uint16_t item, uint32_t bytes)
{
	struct slpi_smem_partition header;
	struct slpi_smem_entry entry;
	uint32_t at, owner = ~0u, rounded = (bytes + 7) & ~7u, need = sizeof(entry) + rounded;
	bool made = false;

	if (slpi_smem_lock == 0) {
		slpi_smem_lock = ml_io_map(SLPI_SMEM_LOCK_PHYS, 0x1000);
	}
	if (slpi_smem_lock == 0) {
		hakios_tl("slpi: transport: SMEM lock cannot be mapped\n");
		return false;
	}
	lck_mtx_lock(&hakios_smem_alloc_lock);
	for (uint32_t ms = 0; ms < 1000; ms++) {
		*(volatile uint32_t *)(slpi_smem_lock + SLPI_SMEM_LOCK_OWNER) = SLPI_SMEM_LOCK_US;
		owner = *(volatile uint32_t *)(slpi_smem_lock + SLPI_SMEM_LOCK_OWNER);
		if (owner == SLPI_SMEM_LOCK_US) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (owner != SLPI_SMEM_LOCK_US || !slpi_smem_peek(partition, &header, sizeof(header)) ||
	    header.free_uncached > header.free_cached || header.free_cached > header.size) {
		goto out;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!slpi_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SLPI_SMEM_CANARY ||
		    entry.size > header.size || entry.padding_data > entry.size || entry.padding_header > entry.size ||
		    entry.padding_header + entry.size > header.free_uncached - at - sizeof(entry)) {
			goto out;
		}
		if (entry.item == item) {
			made = true;
			goto out;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached || need > header.free_cached - at) {
		goto out;
	}
	for (uint32_t offset = 0; offset < rounded; offset += sizeof(uint32_t)) {
		slpi_smem_poke(partition + at + sizeof(entry) + offset, 0);
	}
	slpi_smem_poke(partition + at, SLPI_SMEM_CANARY | (uint32_t)item << 16);
	slpi_smem_poke(partition + at + 4, rounded);
	slpi_smem_poke(partition + at + 8, rounded - bytes);
	slpi_smem_poke(partition + at + 12, 0);
	slpi_settle();
	slpi_smem_poke(partition + __offsetof(struct slpi_smem_partition, free_uncached), at + need);
	slpi_settle();
	made = true;
out:
	if (owner == SLPI_SMEM_LOCK_US) {
		*(volatile uint32_t *)(slpi_smem_lock + SLPI_SMEM_LOCK_OWNER) = 0;
	}
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	if (!made) {
		hakios_tl("slpi: transport: cannot safely allocate item %u (SMEM lock owner %u)\n", item, owner);
	}
	return made;
}

static void
slpi_transport_ring(uint32_t signal)
{
	if (slpi_ipcc == 0) {
		slpi_ipcc = ml_io_map(SLPI_IPCC_PHYS, SLPI_IPCC_SIZE);
	}
	if (slpi_ipcc != 0) {
		*(volatile uint32_t *)(slpi_ipcc + SLPI_IPCC_SEND) = SLPI_IPCC_CLIENT << 16 | signal;
	}
}

static bool
slpi_transport_wait_ready(uint32_t timeout_ms)
{
	struct slpi_smem_partition header;
	uint32_t partition = 0, ours = 0, ours_size = 0, theirs = 0, theirs_size = 0, descriptor = 0, descriptor_size = 0;

	if (!slpi_transport_partition(&partition, &header) ||
	    (!slpi_transport_item(partition, &header, SLPI_ITEM_SMP2P_OURS, &ours, &ours_size) &&
	     !slpi_transport_make(partition, SLPI_ITEM_SMP2P_OURS, sizeof(struct slpi_smp2p))) ||
	    (!slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_OUR_QUEUE, &ours, &ours_size) &&
	     !slpi_transport_make(partition, SLPI_ITEM_GLINK_OUR_QUEUE, 0x4000))) {
		hakios_tl("slpi: transport: local SMEM setup is unavailable\n");
		return false;
	}
	if (!slpi_transport_partition(&partition, &header) || !slpi_transport_item(partition, &header, SLPI_ITEM_SMP2P_OURS, &ours, &ours_size) ||
	    ours_size < sizeof(struct slpi_smp2p)) {
		hakios_tl("slpi: transport: local SMP2P item is malformed\n");
		return false;
	}
	for (uint32_t offset = 0; offset < sizeof(struct slpi_smp2p); offset += sizeof(uint32_t)) {
		slpi_smem_poke(ours + offset, 0);
	}
	slpi_smem_poke(ours, 0x504d5324);                 /* $SMP */
	slpi_smem_poke(ours + 8, SLPI_SMEM_US | SLPI_SMEM_SLPI << 16);
	slpi_smem_poke(ours + 12, 16 | 1u << 16);
	slpi_smem_poke(ours + 20, 0x7473616d);            /* mast */
	slpi_smem_poke(ours + 24, 0x6b2d7265);            /* er-k */
	slpi_smem_poke(ours + 28, 0x656e7265);            /* erne */
	slpi_smem_poke(ours + 32, 0x006c);                /* l */
	slpi_settle();
	slpi_smem_poke(ours + 4, 0x0101);                 /* feature restart-ack, version 1 */
	slpi_settle();
	slpi_transport_ring(SLPI_IPCC_SMP2P);

	for (uint32_t ms = 0; ms < timeout_ms; ms++) {
		if (slpi_transport_partition(&partition, &header) &&
		    slpi_transport_item(partition, &header, SLPI_ITEM_SMP2P_THEIRS, &theirs, &theirs_size) && theirs_size >= sizeof(struct slpi_smp2p) &&
		    slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_DESCRIPTOR, &descriptor, &descriptor_size) && descriptor_size == 32 &&
		    slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_THEIR_QUEUE, &theirs, &theirs_size) && theirs_size >= 1024 &&
		    slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_OUR_QUEUE, &ours, &ours_size) && ours_size == 0x4000) {
			hakios_tl("slpi: transport: ready (descriptor %#x, queues %u/%u bytes)\n", descriptor, ours_size, theirs_size);
			return true;
		}
		if (slpi_ipcc != 0) {
			uint32_t who = *(volatile uint32_t *)(slpi_ipcc + SLPI_IPCC_RECEIVED);
			if (who != SLPI_IPCC_NOBODY && who >> 16 == SLPI_IPCC_CLIENT) {
				*(volatile uint32_t *)(slpi_ipcc + SLPI_IPCC_CLEAR) = who;
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	hakios_tl("slpi: transport: timeout waiting for SLPI GLINK items after %u ms\n", timeout_ms);
	return false;
}

/* The first GLINK exchange: agree on the wire version and open IPCRTR both ways. */
#define SLPI_GLINK_OUR_TAIL     0u
#define SLPI_GLINK_OUR_HEAD     4u
#define SLPI_GLINK_THEIR_TAIL   8u
#define SLPI_GLINK_THEIR_HEAD   12u
#define SLPI_GLINK_RESERVE      16u
#define SLPI_GLINK_CHANNEL        "IPCRTR"
#define SLPI_GLINK_FASTRPC_CHANNEL "fastrpcglink-apps-dsp"
#define SLPI_FASTRPC_INTENT_SIZE 100u
#define SLPI_FASTRPC_INTENT_COUNT 64u
#define SLPI_FASTRPC_REQUEST_SIZE 48u       /* a 40-byte FastRPC header, aligned */
#define SLPI_FASTRPC_REMOTE_INTENTS 2u
enum {
	SLPI_GLINK_VERSION = 0, SLPI_GLINK_VERSION_ACK = 1, SLPI_GLINK_OPEN = 2, SLPI_GLINK_CLOSE = 3,
	SLPI_GLINK_OPEN_ACK = 4, SLPI_GLINK_INTENT = 5, SLPI_GLINK_DONE = 6, SLPI_GLINK_INTENT_REQUEST = 7,
	SLPI_GLINK_INTENT_REQUEST_ACK = 8, SLPI_GLINK_DATA = 9, SLPI_GLINK_CLOSE_ACK = 11,
	SLPI_GLINK_DATA_MORE = 12, SLPI_GLINK_READ_NOTIFY = 13, SLPI_GLINK_DONE_REUSE = 14,
};
struct slpi_glink_message { uint16_t command, param1; uint32_t param2; };
struct slpi_fastrpc_reply_v1 {
	uint64_t context;
	int32_t result;
} __attribute__((packed));
struct slpi_fastrpc_reply_v2 {
	uint64_t context;
	int32_t result;
	uint32_t flags;
	uint32_t early_wake_time;
	uint32_t version;
} __attribute__((packed));
static void slpi_fastrpc_handle_reply(uint64_t context, int32_t result);
static void slpi_fastrpc_watch_wait(uint32_t poll_ms);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size, remote_channel;
	bool agreed, opened, acknowledged, broken;
	uint32_t fastrpc_remote_channel;
	bool fastrpc_opened, fastrpc_acknowledged;
	unsigned fastrpc_remote_cursor;
	bool fastrpc_remote_busy[96];
	unsigned fastrpc_tx_traced, fastrpc_done_traced;
	bool intents_offered;
	/* Remote intent ids are meaningful only to the GLINK endpoint which gave
	 * them to us.  Keep FastRPC's grants separate from QRTR's. */
	struct { uint16_t channel; uint32_t id, size; } remote_intent[96];
	unsigned remote_intents;
	struct { uint8_t *data; uint32_t size, fill; uint16_t channel; } local_intent[80];
	unsigned local_intents;
} slpi_glink;

enum { SLPI_QRTR_DATA = 1, SLPI_QRTR_HELLO = 2, SLPI_QRTR_NEW_SERVER = 4 };
#define SLPI_QRTR_CONTROL       0xfffffffeu
#define SLPI_QRTR_US            1u
#define SLPI_QRTR_FIRST_PORT    0x4000u
#define SLPI_SNS_CLIENT_SERVICE  0x190u
#define SLPI_SNS_CLIENT_REQUEST  0x20u
#define SLPI_SNS_CLIENT_REPORT   0x21u
#define SLPI_SNS_SUID_REQUEST    512u
#define SLPI_SNS_SUID_EVENT      768u
#define SLPI_SNS_LOOKUP_DATA_TYPE "resampler"
#define SLPI_SNS_MAG_DATA_TYPE    "mag"
struct slpi_qrtr_header { uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port; };
struct slpi_qrtr_control { uint32_t command, words[4]; };
struct hakios_qrtr_port {
	uint32_t number, service, version, instance;
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
};
static struct hakios_qrtr_port slpi_qrtr_port;
static struct { uint32_t service, instance, node, port; bool there; } slpi_qrtr_servers[32];
static unsigned slpi_qrtr_rx_traced;

static bool slpi_qrtr_send(const void *data, uint32_t size);
static void slpi_qrtr_input(const uint8_t *data, uint32_t size);

static bool
slpi_smem_read_word(uint32_t offset, uint32_t *value)
{
	return slpi_smem_peek(offset, value, sizeof(*value));
}

static uint32_t
slpi_glink_waiting(void)
{
	uint32_t head, tail;

	if (!slpi_smem_read_word(slpi_glink.descriptor + SLPI_GLINK_THEIR_HEAD, &head) ||
	    !slpi_smem_read_word(slpi_glink.descriptor + SLPI_GLINK_THEIR_TAIL, &tail) ||
	    head >= slpi_glink.theirs_size || tail >= slpi_glink.theirs_size) {
		slpi_glink.broken = true;
		return 0;
	}
	return head >= tail ? head - tail : slpi_glink.theirs_size - tail + head;
}

static bool
slpi_glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t tail;

	if (count > slpi_glink.theirs_size || !slpi_smem_read_word(slpi_glink.descriptor + SLPI_GLINK_THEIR_TAIL, &tail) ||
	    tail >= slpi_glink.theirs_size) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = slpi_smem[slpi_glink.theirs + (tail + offset + i) % slpi_glink.theirs_size];
	}
	return true;
}

static bool
slpi_glink_advance(uint32_t count)
{
	uint32_t tail;

	if (!slpi_smem_read_word(slpi_glink.descriptor + SLPI_GLINK_THEIR_TAIL, &tail) || tail >= slpi_glink.theirs_size ||
	    count > slpi_glink.theirs_size) {
		slpi_glink.broken = true;
		return false;
	}
	slpi_smem_poke(slpi_glink.descriptor + SLPI_GLINK_THEIR_TAIL, (tail + count) % slpi_glink.theirs_size);
	slpi_settle();
	return true;
}

static bool
slpi_glink_send(uint16_t command, uint16_t param1, uint32_t param2, const void *payload, uint32_t payload_size)
{
	struct slpi_glink_message message = { command, param1, param2 };
	volatile uint8_t *queue;
	uint32_t head, tail, room, length = sizeof(message) + payload_size, padded = (length + 7) & ~7u;

	if (payload_size > slpi_glink.ours_size || !slpi_smem_read_word(slpi_glink.descriptor + SLPI_GLINK_OUR_HEAD, &head) ||
	    !slpi_smem_read_word(slpi_glink.descriptor + SLPI_GLINK_OUR_TAIL, &tail) || head >= slpi_glink.ours_size ||
	    tail >= slpi_glink.ours_size) {
		slpi_glink.broken = true;
		return false;
	}
	room = tail <= head ? slpi_glink.ours_size - head + tail : tail - head;
	if (room < SLPI_GLINK_RESERVE || padded > room - SLPI_GLINK_RESERVE) {
		return false;
	}
	queue = (volatile uint8_t *)(void *)(slpi_smem + slpi_glink.ours);
	for (uint32_t i = 0; i < padded; i++) {
		const uint8_t *bytes = (const uint8_t *)&message;
		queue[(head + i) % slpi_glink.ours_size] = i < sizeof(message) ? bytes[i] :
		    i < length ? ((const uint8_t *)payload)[i - sizeof(message)] : 0;
	}
	slpi_settle();
	slpi_smem_poke(slpi_glink.descriptor + SLPI_GLINK_OUR_HEAD, (head + padded) % slpi_glink.ours_size);
	slpi_settle();
	slpi_transport_ring(SLPI_IPCC_GLINK);
	return true;
}

static bool
slpi_glink_offer(uint16_t channel, uint32_t size)
{
	uint32_t words[2];
	unsigned i = slpi_glink.local_intents;

	if (i >= sizeof(slpi_glink.local_intent) / sizeof(slpi_glink.local_intent[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	slpi_glink.local_intent[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	if (slpi_glink.local_intent[i].data == NULL) {
		return false;
	}
	slpi_glink.local_intent[i].size = size;
	slpi_glink.local_intent[i].fill = 0;
	slpi_glink.local_intent[i].channel = channel;
	slpi_glink.local_intents++;
	words[0] = size;
	words[1] = i + 1;
	return slpi_glink_send(SLPI_GLINK_INTENT, channel, 1, words, sizeof(words));
}

static bool
slpi_glink_send_data(uint16_t channel, const void *data, uint32_t size)
{
	uint8_t *packet;
	uint32_t words[2];
	unsigned index, wanted = 0, usable = 0;
	bool sent;

	if (size > 8192 || size > UINT32_MAX - sizeof(words)) {
		return false;
	}
	for (index = 0; index < slpi_glink.remote_intents; index++) {
		if (slpi_glink.remote_intent[index].channel == channel &&
		    size <= slpi_glink.remote_intent[index].size) {
			usable++;
		}
	}
	if (usable == 0) {
		return false;
	}
	if (channel == 2) {
		wanted = slpi_glink.fastrpc_remote_cursor++ % usable;
	}
	for (index = 0; index < slpi_glink.remote_intents; index++) {
		if (slpi_glink.remote_intent[index].channel == channel &&
		    size <= slpi_glink.remote_intent[index].size && wanted-- == 0) {
			break;
		}
	}
	packet = kalloc_data(sizeof(words) + size, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return false;
	}
	words[0] = size;
	words[1] = 0;
	memcpy(packet, words, sizeof(words));
	memcpy(packet + sizeof(words), data, size);
	if (channel == 2 && slpi_glink.fastrpc_tx_traced++ < 32) {
		hakios_tl("slpi: glink: FastRPC TX intent %u busy %u (id %u, bytes %u)\n",
		    index, slpi_glink.fastrpc_remote_busy[index], slpi_glink.remote_intent[index].id, size);
	}
	sent = slpi_glink_send(SLPI_GLINK_DATA, channel, slpi_glink.remote_intent[index].id, packet, sizeof(words) + size);
	if (sent && channel == 2) {
		slpi_glink.fastrpc_remote_busy[index] = true;
	}
	kfree_data(packet, sizeof(words) + size);
	return sent;
}

static void
slpi_glink_pump(void)
{
	struct slpi_glink_message message;

	while (!slpi_glink.broken && slpi_glink_waiting() >= sizeof(message)) {
		uint32_t waiting = slpi_glink_waiting(), length = sizeof(message), padded;
		char name[33];

		if (!slpi_glink_peek(&message, 0, sizeof(message))) {
			return;
		}
		if (message.command == SLPI_GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == SLPI_GLINK_INTENT) {
			length += 8u * message.param2;
		} else if (message.command == SLPI_GLINK_DATA || message.command == SLPI_GLINK_DATA_MORE) {
			uint32_t words[2];
			if (waiting < 16 || !slpi_glink_peek(words, 8, sizeof(words))) {
				return;
			}
			length = 16u + words[0];
		}
		padded = (length + 7) & ~7u;
		if (length < sizeof(message) || padded > slpi_glink.theirs_size - 8 || waiting < padded) {
			if (padded > slpi_glink.theirs_size - 8) {
				slpi_glink.broken = true;
			}
			return;
		}
		switch (message.command) {
		case SLPI_GLINK_VERSION:
			slpi_glink_advance(padded);
			slpi_glink_send(SLPI_GLINK_VERSION_ACK, 1, message.param2 & 1u, NULL, 0);
			break;
		case SLPI_GLINK_VERSION_ACK:
			slpi_glink_advance(padded);
			if (message.param1 == 1) {
				slpi_glink.agreed = true;
			}
			break;
		case SLPI_GLINK_OPEN:
			if (message.param2 == 0 || message.param2 > 32 || !slpi_glink_peek(name, sizeof(message), message.param2)) {
				slpi_glink.broken = true;
				return;
			}
			name[message.param2 < sizeof(name) ? message.param2 : sizeof(name) - 1] = 0;
			slpi_glink_advance(padded);
			if (strcmp(name, SLPI_GLINK_CHANNEL) == 0) {
				slpi_glink.remote_channel = message.param1;
				slpi_glink_send(SLPI_GLINK_OPEN_ACK, message.param1, 0, NULL, 0);
				if (!slpi_glink.opened) {
					slpi_glink_send(SLPI_GLINK_OPEN, 1, sizeof(SLPI_GLINK_CHANNEL), SLPI_GLINK_CHANNEL, sizeof(SLPI_GLINK_CHANNEL));
					slpi_glink.opened = true;
				}
			} else if (strcmp(name, SLPI_GLINK_FASTRPC_CHANNEL) == 0) {
				slpi_glink.fastrpc_remote_channel = message.param1;
				slpi_glink_send(SLPI_GLINK_OPEN_ACK, message.param1, 0, NULL, 0);
			}
			break;
		case SLPI_GLINK_OPEN_ACK:
			slpi_glink_advance(padded);
			if (message.param1 == 1) {
				slpi_glink.acknowledged = true;
			} else if (message.param1 == 2) {
				slpi_glink.fastrpc_acknowledged = true;
			}
			break;
		case SLPI_GLINK_INTENT:
			if ((message.param1 == slpi_glink.remote_channel || message.param1 == slpi_glink.fastrpc_remote_channel) &&
			    message.param2 <= 64) {
				uint16_t channel = message.param1 == slpi_glink.remote_channel ? 1u : 2u;
				unsigned before = slpi_glink.remote_intents;
				for (uint32_t i = 0; i < message.param2 && slpi_glink.remote_intents < 96; i++) {
					uint32_t words[2];

					if (!slpi_glink_peek(words, sizeof(message) + i * sizeof(words), sizeof(words)) || words[0] == 0) {
						slpi_glink.broken = true;
						return;
					}
					slpi_glink.remote_intent[slpi_glink.remote_intents].channel = channel;
					slpi_glink.remote_intent[slpi_glink.remote_intents].size = words[0];
					slpi_glink.remote_intent[slpi_glink.remote_intents].id = words[1];
					slpi_glink.remote_intents++;
				}
				if (channel == 2 && slpi_glink.remote_intents != before) {
					hakios_tl("slpi: glink: FastRPC received %u remote intents\n",
					    slpi_glink.remote_intents - before);
				}
			}
			slpi_glink_advance(padded);
			break;
		case SLPI_GLINK_DATA:
		case SLPI_GLINK_DATA_MORE: {
			uint32_t words[2], intent;
			bool ipcrtr = message.param1 == slpi_glink.remote_channel;
			bool fastrpc = message.param1 == slpi_glink.fastrpc_remote_channel;
			bool have_fastrpc_reply = false;
			uint64_t fastrpc_context = 0;
			int32_t fastrpc_result = 0;

			if (!slpi_glink_peek(words, sizeof(message), sizeof(words)) || (!ipcrtr && !fastrpc) ||
			    message.param2 == 0 || (intent = message.param2 - 1) >= slpi_glink.local_intents ||
			    slpi_glink.local_intent[intent].channel != (ipcrtr ? 1u : 2u) ||
			    words[0] > slpi_glink.local_intent[intent].size - slpi_glink.local_intent[intent].fill ||
			    !slpi_glink_peek(slpi_glink.local_intent[intent].data + slpi_glink.local_intent[intent].fill, 16, words[0])) {
				slpi_glink.broken = true;
				return;
			}
			slpi_glink.local_intent[intent].fill += words[0];
			slpi_glink_advance(padded);
			if (words[1] == 0) {
				if (ipcrtr) {
					slpi_qrtr_input(slpi_glink.local_intent[intent].data, slpi_glink.local_intent[intent].fill);
				} else {
					if (slpi_glink.local_intent[intent].fill == sizeof(struct slpi_fastrpc_reply_v2)) {
						struct slpi_fastrpc_reply_v2 reply;

						memcpy(&reply, slpi_glink.local_intent[intent].data, sizeof(reply));
						hakios_tl("slpi: fastrpc: attach reply (ctx %#llx, result %d, flags %#x, early wake %u, version %u)\n",
						    reply.context, reply.result, reply.flags, reply.early_wake_time, reply.version);
						fastrpc_context = reply.context;
						fastrpc_result = reply.result;
						have_fastrpc_reply = true;
					} else if (slpi_glink.local_intent[intent].fill == sizeof(struct slpi_fastrpc_reply_v1)) {
						struct slpi_fastrpc_reply_v1 reply;

						memcpy(&reply, slpi_glink.local_intent[intent].data, sizeof(reply));
						hakios_tl("slpi: fastrpc: attach reply (ctx %#llx, result %d)\n", reply.context, reply.result);
						fastrpc_context = reply.context;
						fastrpc_result = reply.result;
						have_fastrpc_reply = true;
					} else {
						hakios_tl("slpi: fastrpc: received %u-byte packet (listener not registered)\n",
						    slpi_glink.local_intent[intent].fill);
					}
				}
				slpi_glink.local_intent[intent].fill = 0;
				slpi_glink_send(SLPI_GLINK_DONE_REUSE, message.param1, message.param2, NULL, 0);
				if (have_fastrpc_reply) {
					slpi_fastrpc_handle_reply(fastrpc_context, fastrpc_result);
				}
			}
			break;
		}
		case SLPI_GLINK_DONE:
		case SLPI_GLINK_DONE_REUSE:
		case SLPI_GLINK_CLOSE_ACK:
			if (message.command != SLPI_GLINK_CLOSE_ACK &&
			    message.param1 == slpi_glink.fastrpc_remote_channel) {
				bool matched = false;

				for (unsigned i = 0; i < slpi_glink.remote_intents; i++) {
					if (slpi_glink.remote_intent[i].channel != 2 ||
					    slpi_glink.remote_intent[i].id != message.param2) {
						continue;
					}
					matched = true;
					if (slpi_glink.fastrpc_done_traced++ < 32) {
						hakios_tl("slpi: glink: FastRPC TX done intent %u busy %u (id %u, reuse %u)\n",
						    i, slpi_glink.fastrpc_remote_busy[i], message.param2,
						    message.command == SLPI_GLINK_DONE_REUSE);
					}
					if (message.command == SLPI_GLINK_DONE_REUSE) {
						slpi_glink.fastrpc_remote_busy[i] = false;
					}
					break;
				}
				if (!matched && slpi_glink.fastrpc_done_traced++ < 32) {
					hakios_tl("slpi: glink: FastRPC TX done unknown intent %u\n", message.param2);
				}
			}
			slpi_glink_advance(padded);
			break;
		case SLPI_GLINK_INTENT_REQUEST: {
			uint16_t local_channel = message.param1 == slpi_glink.remote_channel ? 1u :
			    message.param1 == slpi_glink.fastrpc_remote_channel ? 2u : 0u;
			bool granted = false;

			slpi_glink_advance(padded);
			if (local_channel != 0) {
				granted = slpi_glink_offer(local_channel, message.param2);
				slpi_glink_send(SLPI_GLINK_INTENT_REQUEST_ACK, local_channel, granted, NULL, 0);
			}
			hakios_tl("slpi: glink: remote intent request (channel %u, size %u) %s\n",
			    message.param1, message.param2, granted ? "granted" : "denied");
			break;
		}
		case SLPI_GLINK_INTENT_REQUEST_ACK:
			slpi_glink_advance(padded);
			if (message.param1 == slpi_glink.fastrpc_remote_channel) {
				hakios_tl("slpi: glink: FastRPC intent request %s\n", message.param2 ? "granted" : "denied");
			}
			break;
		case SLPI_GLINK_READ_NOTIFY:
			slpi_glink_advance(padded);
			slpi_transport_ring(SLPI_IPCC_GLINK);
			break;
		default:
			hakios_tl("slpi: glink: unsupported message %u\n", message.command);
			slpi_glink.broken = true;
			return;
		}
		if (slpi_glink.remote_channel != 0 && slpi_glink.acknowledged && !slpi_glink.intents_offered) {
			for (unsigned i = 0; i < 5; i++) {
				if (!slpi_glink_offer(1, 0x800)) { slpi_glink.broken = true; return; }
			}
			for (unsigned i = 0; i < 3; i++) {
				if (!slpi_glink_offer(1, 0x2000)) { slpi_glink.broken = true; return; }
			}
			for (unsigned i = 0; i < 2; i++) {
				if (!slpi_glink_offer(1, 0x4400)) { slpi_glink.broken = true; return; }
			}
			slpi_glink.intents_offered = true;
		}
	}
}

static bool
slpi_qrtr_send(const void *data, uint32_t size)
{
	return slpi_glink_send_data(1, data, size);
}

/* QRTR adapter for the common QMI helpers: this is deliberately one APSS port. */
static struct hakios_qrtr_port *
slpi_qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	if (slpi_qrtr_port.number != 0 || receive == NULL || server == NULL) {
		return NULL;
	}
	bzero(&slpi_qrtr_port, sizeof(slpi_qrtr_port));
	slpi_qrtr_port.number = SLPI_QRTR_FIRST_PORT;
	slpi_qrtr_port.receive = receive;
	slpi_qrtr_port.server = server;
	slpi_qrtr_port.context = context;
	return &slpi_qrtr_port;
}

static int
slpi_qrtr_send_to(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	struct slpi_qrtr_header *header;
	uint8_t *packet;
	bool sent;

	if (port != &slpi_qrtr_port || port->number == 0 || data == NULL || length > 8192 ||
	    length > UINT32_MAX - sizeof(*header)) {
		return EINVAL;
	}
	packet = kalloc_data(sizeof(*header) + length, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	header = (struct slpi_qrtr_header *)(void *)packet;
	header->version = 1;
	header->type = SLPI_QRTR_DATA;
	header->from_node = SLPI_QRTR_US;
	header->from_port = port->number;
	header->size = (uint32_t)length;
	header->to_node = node;
	header->to_port = to_port;
	memcpy(packet + sizeof(*header), data, length);
	sent = slpi_qrtr_send(packet, sizeof(*header) + (uint32_t)length);
	kfree_data(packet, sizeof(*header) + length);
	return sent ? 0 : EBUSY;
}

static int
slpi_qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	if (port != &slpi_qrtr_port || port->number == 0) {
		return EINVAL;
	}
	port->service = service;
	port->version = version;
	port->instance = instance;
	for (unsigned i = 0; i < sizeof(slpi_qrtr_servers) / sizeof(slpi_qrtr_servers[0]); i++) {
		if (slpi_qrtr_servers[i].there && slpi_qrtr_servers[i].service == service &&
		    (version == 0 || (slpi_qrtr_servers[i].instance & 0xff) == version) &&
		    (instance == 0 || (slpi_qrtr_servers[i].instance >> 8) == instance)) {
			port->server(port->context, true, service, slpi_qrtr_servers[i].instance & 0xff,
			    slpi_qrtr_servers[i].instance >> 8, slpi_qrtr_servers[i].node, slpi_qrtr_servers[i].port);
		}
	}
	return 0;
}

static int
slpi_qrtr_poll(struct hakios_qrtr_port *port, uint32_t timeout_ms)
{
	if (port != &slpi_qrtr_port || port->number == 0) {
		return EINVAL;
	}
	for (uint32_t ms = 0; ms < timeout_ms && !slpi_glink.broken; ms++) {
		slpi_glink_pump();
		if (ms == 1000u || ms == 5000u || ms == 15000u || ms == 25000u) {
			slpi_fastrpc_watch_wait(ms);
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	slpi_glink_pump();
	return slpi_glink.broken ? EIO : 0;
}

static void
slpi_qrtr_close(struct hakios_qrtr_port *port)
{
	if (port == &slpi_qrtr_port) {
		bzero(&slpi_qrtr_port, sizeof(slpi_qrtr_port));
	}
}

static const struct hakios_qrtr_ops slpi_qrtr_ops = {
	.open = slpi_qrtr_open,
	.send = slpi_qrtr_send_to,
	.lookup = slpi_qrtr_lookup,
	.poll = slpi_qrtr_poll,
	.close = slpi_qrtr_close,
};

static void
slpi_qrtr_input(const uint8_t *data, uint32_t size)
{
	struct slpi_qrtr_header header;
	struct slpi_qrtr_control control = { 0 };

	if (size < sizeof(header)) {
		hakios_tl("slpi: qrtr: short datagram (%u bytes)\n", size);
		return;
	}
	memcpy(&header, data, sizeof(header));
	if (header.version != 1 || header.size == 0 || header.size > size - sizeof(header)) {
		hakios_tl("slpi: qrtr: malformed datagram (version %u, payload %u of %u)\n", header.version, header.size, size);
		return;
	}
	if (header.size >= sizeof(control.command)) {
		memcpy(&control, data + sizeof(header), header.size < sizeof(control) ? header.size : sizeof(control));
	}
	if (header.type == SLPI_QRTR_DATA && header.to_port == slpi_qrtr_port.number &&
	    slpi_qrtr_port.receive != NULL) {
		if (slpi_qrtr_rx_traced < 8) {
			uint32_t shown = MIN(header.size, 128u);

			hakios_tl("slpi: qrtr: %u-byte QMI reply from %u:%u\n", header.size, header.from_node, header.from_port);
			for (uint32_t offset = 0; offset < shown; offset += 16) {
				char line[16 * 3 + 1];
				uint32_t here = MIN(16u, shown - offset), at = 0;

				for (uint32_t i = 0; i < here; i++) {
					at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x ", data[sizeof(header) + offset + i]);
				}
				line[MIN(at, (uint32_t)sizeof(line) - 1)] = 0;
				hakios_tl("slpi: qrtr: reply +%u: %s\n", offset, line);
			}
			slpi_qrtr_rx_traced++;
		}
		slpi_qrtr_port.receive(slpi_qrtr_port.context, header.from_node, header.from_port,
		    data + sizeof(header), header.size);
		return;
	}
	switch (control.command) {
	case SLPI_QRTR_HELLO:
		if (header.from_port == SLPI_QRTR_CONTROL) {
			struct { struct slpi_qrtr_header header; struct slpi_qrtr_control control; } reply = { 0 };

			reply.header.version = 1;
			reply.header.type = SLPI_QRTR_HELLO;
			reply.header.from_node = 1;
			reply.header.from_port = SLPI_QRTR_CONTROL;
			reply.header.size = sizeof(reply.control);
			reply.header.to_node = header.from_node;
			reply.header.to_port = SLPI_QRTR_CONTROL;
			reply.control.command = SLPI_QRTR_HELLO;
			slpi_qrtr_send(&reply, sizeof(reply));
			hakios_tl("slpi: qrtr: HELLO from SLPI node %u\n", header.from_node);
		}
		break;
	case SLPI_QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(slpi_qrtr_servers) / sizeof(slpi_qrtr_servers[0]); i++) {
			if (!slpi_qrtr_servers[i].there ||
			    (slpi_qrtr_servers[i].node == control.words[2] && slpi_qrtr_servers[i].port == control.words[3])) {
				slpi_qrtr_servers[i].service = control.words[0];
				slpi_qrtr_servers[i].instance = control.words[1];
				slpi_qrtr_servers[i].node = control.words[2];
				slpi_qrtr_servers[i].port = control.words[3];
				slpi_qrtr_servers[i].there = true;
				break;
			}
		}
		hakios_tl("slpi: qrtr: service %#x version %u instance %u at node %u port %u\n", control.words[0],
		    control.words[1] & 0xff, control.words[1] >> 8, control.words[2], control.words[3]);
		if (slpi_qrtr_port.server != NULL && slpi_qrtr_port.service == control.words[0] &&
		    (slpi_qrtr_port.version == 0 || slpi_qrtr_port.version == (control.words[1] & 0xff)) &&
		    (slpi_qrtr_port.instance == 0 || slpi_qrtr_port.instance == (control.words[1] >> 8))) {
			slpi_qrtr_port.server(slpi_qrtr_port.context, true, control.words[0], control.words[1] & 0xff,
			    control.words[1] >> 8, control.words[2], control.words[3]);
		}
		break;
	default:
		break;
	}
}

static struct qmi_handle slpi_sns_qmi;
static struct qmi_txn slpi_sns_suid_txn;
static struct qmi_response_type_v01 slpi_sns_suid_response;
static bool slpi_sns_suid_sent;
static struct qmi_txn slpi_sns_mag_txn;
static struct qmi_response_type_v01 slpi_sns_mag_response;
static bool slpi_sns_mag_sent;

struct slpi_sns_client_request {
	uint32_t payload_len;
	uint8_t payload[512];
};
struct slpi_sns_client_report {
	uint64_t timestamp;
	uint32_t payload_len;
	uint8_t payload[2048];
};

/* The Client Manager's QMI envelope is TLV 1: uint16 length, then protobuf. */
static const struct qmi_elem_info slpi_sns_client_request_ei[] = {
	{
		.data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
		.tlv_type = 1, .offset = offsetof(struct slpi_sns_client_request, payload_len),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE, .elem_len = sizeof(((struct slpi_sns_client_request *)0)->payload),
		.elem_size = sizeof(uint8_t), .array_type = VAR_LEN_ARRAY, .tlv_type = 1,
		.offset = offsetof(struct slpi_sns_client_request, payload),
	},
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info slpi_sns_client_report_ei[] = {
	{
		.data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t), .array_type = NO_ARRAY,
		.tlv_type = 1, .offset = offsetof(struct slpi_sns_client_report, timestamp),
	},
	{
		.data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
		.tlv_type = 2, .offset = offsetof(struct slpi_sns_client_report, payload_len),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE, .elem_len = sizeof(((struct slpi_sns_client_report *)0)->payload),
		.elem_size = sizeof(uint8_t), .array_type = VAR_LEN_ARRAY, .tlv_type = 2,
		.offset = offsetof(struct slpi_sns_client_report, payload),
	},
	{ .data_type = QMI_EOTI },
};

/* QMI's mandatory result TLV is type 2; keep this explicit for the SLPI probe. */
static const struct qmi_elem_info slpi_sns_response_ei[] = {
	{
		.data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
		.tlv_type = 2, .offset = offsetof(struct qmi_response_type_v01, result),
	},
	{
		.data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
		.tlv_type = 2, .offset = offsetof(struct qmi_response_type_v01, error),
	},
	{ .data_type = QMI_EOTI },
};

/* Small bounded protobuf reader for the SNS client-event/SUID-event envelopes.
 * Each field advances the cursor or fails; unknown supported wire types can be
 * skipped without ever trusting an unbounded length from the sensor DSP. */
struct slpi_pb_cursor { const uint8_t *data; uint32_t size, at; };
struct slpi_pb_field { uint32_t number, wire, size; const uint8_t *data; uint64_t scalar; };

static bool
slpi_pb_varint(struct slpi_pb_cursor *cursor, uint64_t *value)
{
	uint64_t decoded = 0;

	for (unsigned i = 0; i < 10 && cursor->at < cursor->size; i++) {
		uint8_t byte = cursor->data[cursor->at++];

		if (i == 9 && byte > 1) {
			return false;
		}
		decoded |= (uint64_t)(byte & 0x7f) << (7 * i);
		if ((byte & 0x80) == 0) {
			*value = decoded;
			return true;
		}
	}
	return false;
}

static bool
slpi_pb_next(struct slpi_pb_cursor *cursor, struct slpi_pb_field *field)
{
	uint64_t key, length;
	uint32_t count;

	if (!slpi_pb_varint(cursor, &key) || (key >> 3) == 0 || (key >> 3) > UINT32_MAX) {
		return false;
	}
	field->number = (uint32_t)(key >> 3);
	field->wire = (uint32_t)(key & 7);
	field->size = 0;
	field->data = NULL;
	field->scalar = 0;
	switch (field->wire) {
	case 0:
		return slpi_pb_varint(cursor, &field->scalar);
	case 1:
	case 5:
		count = field->wire == 1 ? 8u : 4u;
		if (count > cursor->size - cursor->at) {
			return false;
		}
		field->size = count;
		break;
	case 2:
		if (!slpi_pb_varint(cursor, &length) || length > cursor->size - cursor->at) {
			return false;
		}
		field->size = (uint32_t)length;
		break;
	default:
		return false;
	}
	field->data = cursor->data + cursor->at;
	cursor->at += field->size;
	return true;
}

static uint64_t
slpi_pb_le(const uint8_t *bytes, uint32_t size)
{
	uint64_t value = 0;

	for (uint32_t i = 0; i < size; i++) {
		value |= (uint64_t)bytes[i] << (8 * i);
	}
	return value;
}

static bool
slpi_sns_decode_suid(const uint8_t *data, uint32_t length, uint64_t *low, uint64_t *high)
{
	struct slpi_pb_cursor cursor = { data, length, 0 };
	struct slpi_pb_field field;
	bool have_low = false, have_high = false;

	*low = *high = 0;
	while (cursor.at < cursor.size) {
		if (!slpi_pb_next(&cursor, &field)) {
			return false;
		}
		if (field.number == 1 && field.wire == 1) {
			*low = slpi_pb_le(field.data, field.size);
			have_low = true;
		} else if (field.number == 2 && field.wire == 1) {
			*high = slpi_pb_le(field.data, field.size);
			have_high = true;
		}
	}
	return have_low && have_high && (*low != 0 || *high != 0);
}

static bool
slpi_sns_decode_suid_event(const uint8_t *data, uint32_t length)
{
	struct slpi_pb_cursor cursor = { data, length, 0 };
	struct slpi_pb_field field;
	struct { uint64_t low, high; } suids[4];
	char data_type[65] = { 0 };
	uint32_t count = 0;
	bool have_type = false;

	while (cursor.at < cursor.size) {
		if (!slpi_pb_next(&cursor, &field)) {
			return false;
		}
		if (field.number == 1 && field.wire == 2) {
			if (field.size == 0 || field.size >= sizeof(data_type) || have_type) {
				return false;
			}
			if (memchr(field.data, 0, field.size) != NULL) {
				return false;
			}
			memcpy(data_type, field.data, field.size);
			data_type[field.size] = 0;
			have_type = true;
		} else if (field.number == 2 && field.wire == 2) {
			uint64_t low, high;

			if (!slpi_sns_decode_suid(field.data, field.size, &low, &high)) {
				return false;
			}
			if (count < sizeof(suids) / sizeof(suids[0])) {
				suids[count].low = low;
				suids[count].high = high;
				count++;
			}
		}
	}
	if (!have_type || (strcmp(data_type, SLPI_SNS_LOOKUP_DATA_TYPE) != 0 &&
	    strcmp(data_type, SLPI_SNS_MAG_DATA_TYPE) != 0)) {
		return false;
	}
	if (count == 0) {
		hakios_tl("slpi: sns: SUID event \"%s\": no sensor SUID yet\n", data_type);
		return true;
	}
	for (uint32_t i = 0; i < count; i++) {
		hakios_tl("slpi: sns: SUID event \"%s\": low %#llx high %#llx\n",
		    data_type, suids[i].low, suids[i].high);
	}
	return true;
}

static bool
slpi_sns_decode_client_event(const uint8_t *data, uint32_t length)
{
	struct slpi_pb_cursor cursor = { data, length, 0 };
	struct slpi_pb_field field;
	bool found = false;

	while (cursor.at < cursor.size) {
		if (!slpi_pb_next(&cursor, &field)) {
			return false;
		}
		if (field.number == 2 && field.wire == 2) {
			struct slpi_pb_cursor event = { field.data, field.size, 0 };
			struct slpi_pb_field item;
			const uint8_t *payload = NULL;
			uint32_t payload_length = 0, message = 0;

			while (event.at < event.size) {
				if (!slpi_pb_next(&event, &item)) {
					return false;
				}
				if (item.number == 1 && item.wire == 5) {
					message = (uint32_t)slpi_pb_le(item.data, item.size);
				} else if (item.number == 3 && item.wire == 2) {
					payload = item.data;
					payload_length = item.size;
				}
			}
			if (message == SLPI_SNS_SUID_EVENT && payload != NULL) {
				if (!slpi_sns_decode_suid_event(payload, payload_length)) {
					return false;
				}
				found = true;
			}
		}
	}
	return found;
}

static void
slpi_sns_report_handler(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *from,
    __unused struct qmi_txn *txn, const void *decoded)
{
	const struct slpi_sns_client_report *report = decoded;
	uint32_t shown = MIN(report->payload_len, 64u);

	hakios_tl("slpi: sns: report indication: %u protobuf bytes\n", report->payload_len);
	if (report->payload_len == 0 || report->payload_len > sizeof(report->payload)) {
		hakios_tl("slpi: sns: report has no valid protobuf payload\n");
		return;
	}
	if (slpi_sns_decode_client_event(report->payload, report->payload_len)) {
		return;
	}
	hakios_tl("slpi: sns: report has no complete SUID event\n");
	for (uint32_t offset = 0; offset < shown; offset += 16) {
		char line[16 * 3 + 1];
		uint32_t here = MIN(16u, shown - offset), at = 0;

		for (uint32_t i = 0; i < here; i++) {
			at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x ", report->payload[offset + i]);
		}
		line[MIN(at, (uint32_t)sizeof(line) - 1)] = 0;
		hakios_tl("slpi: sns: report +%u: %s\n", offset, line);
	}
}

static const struct qmi_msg_handler slpi_sns_qmi_handlers[] = {
	{
		.type = QMI_INDICATION, .msg_id = SLPI_SNS_CLIENT_REPORT, .ei = slpi_sns_client_report_ei,
		.decoded_size = sizeof(struct slpi_sns_client_report), .fn = slpi_sns_report_handler,
	},
	{ .fn = NULL },
};

static uint32_t
slpi_sns_encode_suid_lookup(const char *data_type, bool register_updates, uint8_t *payload, uint32_t capacity)
{
	static const uint8_t prefix[] = {
		0x0a, 0x12, 0x09, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
		0x11, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
		0x15, 0x00, 0x02, 0x00, 0x00,
		0x1a, 0x04, 0x08, 0x01, 0x10, 0x00,
	};
	uint32_t data_type_len = (uint32_t)strlen(data_type);
	uint32_t needed = (uint32_t)sizeof(prefix) + data_type_len + 8;
	uint8_t *at;

	if (data_type_len == 0 || data_type_len > 64 || needed > capacity) {
		return 0;
	}
	memcpy(payload, prefix, sizeof(prefix));
	at = payload + sizeof(prefix);
	*at++ = 0x22;
	*at++ = (uint8_t)(data_type_len + 6);
	*at++ = 0x12;
	*at++ = (uint8_t)(data_type_len + 4);
	*at++ = 0x0a;
	*at++ = (uint8_t)data_type_len;
	memcpy(at, data_type, data_type_len);
	at += data_type_len;
	*at++ = 0x10;
	*at++ = register_updates ? 0x01 : 0x00;
	return (uint32_t)(at - payload);
}

static void
slpi_sns_send_suid_lookup(void)
{
	struct slpi_sns_client_request request = { 0 };
	int error;

	if (slpi_sns_suid_sent) {
		return;
	}
	request.payload_len = slpi_sns_encode_suid_lookup(SLPI_SNS_LOOKUP_DATA_TYPE, false,
	    request.payload, sizeof(request.payload));
	if (request.payload_len == 0) {
		hakios_tl("slpi: sns: cannot encode SUID lookup\n");
		return;
	}
	error = qmi_txn_init(&slpi_sns_qmi, &slpi_sns_suid_txn, slpi_sns_response_ei, &slpi_sns_suid_response);
	if (error < 0) {
		hakios_tl("slpi: sns: cannot prepare SUID lookup: %d\n", error);
		return;
	}
	error = qmi_send_request(&slpi_sns_qmi, NULL, &slpi_sns_suid_txn, SLPI_SNS_CLIENT_REQUEST, 1024,
	    slpi_sns_client_request_ei, &request);
	if (error != 0) {
		qmi_txn_cancel(&slpi_sns_suid_txn);
		hakios_tl("slpi: sns: cannot send SUID lookup: %d\n", error);
		return;
	}
	slpi_sns_suid_sent = true;
	hakios_tl("slpi: sns: sent SUID lookup for \"%s\" (message %u, %u protobuf bytes)\n",
	    SLPI_SNS_LOOKUP_DATA_TYPE, SLPI_SNS_SUID_REQUEST, request.payload_len);
}

/* Use a separate transaction: the resampler reply and the magnetometer event
 * may arrive in either order, and a successful QMI reply is not a SUID. */
static void
slpi_sns_send_mag_lookup(void)
{
	struct slpi_sns_client_request request = { 0 };
	int error;

	if (slpi_sns_mag_sent) {
		return;
	}
	request.payload_len = slpi_sns_encode_suid_lookup(SLPI_SNS_MAG_DATA_TYPE, true,
	    request.payload, sizeof(request.payload));
	if (request.payload_len == 0) {
		hakios_tl("slpi: sns: cannot encode mag SUID lookup\n");
		return;
	}
	error = qmi_txn_init(&slpi_sns_qmi, &slpi_sns_mag_txn, slpi_sns_response_ei, &slpi_sns_mag_response);
	if (error < 0) {
		hakios_tl("slpi: sns: cannot prepare mag SUID lookup: %d\n", error);
		return;
	}
	error = qmi_send_request(&slpi_sns_qmi, NULL, &slpi_sns_mag_txn, SLPI_SNS_CLIENT_REQUEST, 1024,
	    slpi_sns_client_request_ei, &request);
	if (error != 0) {
		qmi_txn_cancel(&slpi_sns_mag_txn);
		hakios_tl("slpi: sns: cannot send mag SUID lookup: %d\n", error);
		return;
	}
	slpi_sns_mag_sent = true;
	hakios_tl("slpi: sns: sent SUID lookup for \"%s\" (message %u, %u protobuf bytes)\n",
	    SLPI_SNS_MAG_DATA_TYPE, SLPI_SNS_SUID_REQUEST, request.payload_len);
}

static int
slpi_sns_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	hakios_tl("slpi: sns: client manager at node %u port %u (version %u instance %u)\n", service->node,
	    service->port, service->version, service->instance);
	if (hakios_qmi_connect(qmi->sock, service->node, service->port) != 0) {
		return -EIO;
	}
	/* Only one QRTR request may use the current remote GLINK intent at a
	 * time: this transport does not yet track TX intent completion/reuse.
	 * Start with the sensor needed by the compass rather than sending the
	 * old resampler probe immediately before it on the same remote buffer. */
	slpi_sns_send_mag_lookup();
	return 0;
}

static const struct qmi_ops slpi_sns_qmi_ops = {
	.new_server = slpi_sns_server,
};

static void
slpi_sns_qmi_probe(void)
{
	int error;

	error = hakios_qmi_handle_init_transport(&slpi_sns_qmi, 8192, &slpi_sns_qmi_ops, slpi_sns_qmi_handlers, &slpi_qrtr_ops);
	if (error != 0) {
		hakios_tl("slpi: sns: cannot open the QMI client: %d\n", error);
		return;
	}
	error = qmi_add_lookup(&slpi_sns_qmi, SLPI_SNS_CLIENT_SERVICE, 0, 0);
	if (error != 0) {
		hakios_tl("slpi: sns: client-manager lookup failed: %d\n", error);
		qmi_handle_release(&slpi_sns_qmi);
		return;
	}
	hakios_tl("slpi: sns: looking up the Sensor Client Manager service %#x\n", SLPI_SNS_CLIENT_SERVICE);
}

static bool
slpi_glink_open_ipcrtr(void)
{
	struct slpi_smem_partition header;
	uint32_t partition, descriptor_size;
	bool opened = false;

	if (!slpi_transport_partition(&partition, &header) ||
	    !slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_DESCRIPTOR, &slpi_glink.descriptor, &descriptor_size) ||
	    descriptor_size != 32 || !slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_OUR_QUEUE, &slpi_glink.ours, &slpi_glink.ours_size) ||
	    !slpi_transport_item(partition, &header, SLPI_ITEM_GLINK_THEIR_QUEUE, &slpi_glink.theirs, &slpi_glink.theirs_size) ||
	    slpi_glink.ours_size < 1024 || slpi_glink.theirs_size < 1024) {
		hakios_tl("slpi: glink: validated queues are no longer available\n");
		return false;
	}
	slpi_glink.agreed = slpi_glink.opened = slpi_glink.acknowledged = slpi_glink.broken = false;
	slpi_glink.fastrpc_opened = slpi_glink.fastrpc_acknowledged = false;
	slpi_glink.fastrpc_remote_channel = 0;
	slpi_glink.remote_channel = 0;
	slpi_glink_send(SLPI_GLINK_VERSION, 1, 1, NULL, 0);
	for (uint32_t ms = 0; ms < 3000 && !slpi_glink.broken; ms++) {
		slpi_glink_pump();
		if (!opened && slpi_glink.agreed && slpi_glink.remote_channel != 0 && slpi_glink.acknowledged) {
			hakios_tl("slpi: glink: IPCRTR open (ours 1, SLPI %u)\n", slpi_glink.remote_channel);
			opened = true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (opened && !slpi_glink.broken) {
		return true;
	}
	hakios_tl("slpi: glink: IPCRTR not open (version %u, remote %u, ack %u, broken %u)\n", slpi_glink.agreed,
	    slpi_glink.remote_channel, slpi_glink.acknowledged, slpi_glink.broken);
	return false;
}

/*
 * The sensor registry is fetched by SLPI through Qualcomm's FastRPC reverse
 * listener, which lives on a second GLINK channel.  This first step only
 * proves that the signed UMI firmware accepts that channel; it deliberately
 * does not issue an RPC or expose any file data yet.
 */
static bool
slpi_glink_open_fastrpc_probe(void)
{
	if (slpi_glink.broken || !slpi_glink.acknowledged || slpi_glink.fastrpc_opened) {
		return false;
	}
	if (!slpi_glink_send(SLPI_GLINK_OPEN, 2, sizeof(SLPI_GLINK_FASTRPC_CHANNEL),
	    SLPI_GLINK_FASTRPC_CHANNEL, sizeof(SLPI_GLINK_FASTRPC_CHANNEL))) {
		hakios_tl("slpi: glink: cannot send FastRPC channel open\n");
		return false;
	}
	slpi_glink.fastrpc_opened = true;
	for (uint32_t ms = 0; ms < 3000 && !slpi_glink.broken; ms++) {
		slpi_glink_pump();
		if (slpi_glink.fastrpc_acknowledged) {
			for (unsigned i = 0; i < SLPI_FASTRPC_INTENT_COUNT; i++) {
				if (!slpi_glink_offer(2, SLPI_FASTRPC_INTENT_SIZE)) {
					hakios_tl("slpi: glink: cannot offer FastRPC intent %u\n", i);
					return false;
				}
			}
			hakios_tl("slpi: glink: FastRPC channel open (ours 2, SLPI %u)\n",
			    slpi_glink.fastrpc_remote_channel);
			hakios_tl("slpi: glink: FastRPC offered 64 intents\n");
			for (unsigned requested = 0; requested < SLPI_FASTRPC_REMOTE_INTENTS; requested++) {
				bool available = false;

				if (!slpi_glink_send(SLPI_GLINK_INTENT_REQUEST, 2, SLPI_FASTRPC_REQUEST_SIZE, NULL, 0)) {
					hakios_tl("slpi: glink: cannot request FastRPC remote intent %u\n", requested);
					return false;
				}
				for (uint32_t wait = 0; wait < 1000 && !slpi_glink.broken; wait++) {
					unsigned count = 0;

					slpi_glink_pump();
					for (unsigned i = 0; i < slpi_glink.remote_intents; i++) {
						count += slpi_glink.remote_intent[i].channel == 2;
					}
					if (count > requested) {
						available = true;
						break;
					}
					delay_for_interval(1, NSEC_PER_MSEC);
				}
				if (!available) {
					hakios_tl("slpi: glink: FastRPC did not provide remote intent %u\n", requested);
					return requested != 0;
				}
			}
			hakios_tl("slpi: glink: FastRPC requested %u 48-byte remote intents\n",
			    SLPI_FASTRPC_REMOTE_INTENTS);
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	hakios_tl("slpi: glink: FastRPC channel not open (ack %u, remote %u, broken %u)\n",
	    slpi_glink.fastrpc_acknowledged, slpi_glink.fastrpc_remote_channel, slpi_glink.broken);
	return false;
}

/* ---- two APSS 4-KiB FastRPC pages: sensors PD and optional root PD ------------------------------------- */
/*
 * UMI's first sdsprpc-smd context bank (compute_cb13) uses stream 0x541
 * and an IOVA aperture beginning at 0x80000000.  Do the page-table setup
 * separately from invoking sensorspd: a read-back failure here must never be
 * mistaken for a failed DSP RPC.
 */
#define SLPI_SMMU_PHYS          0x15000000ull
#define SLPI_FASTRPC_STREAM     0x0541u
#define SLPI_FASTRPC_IOVA       0x80000000ull
#define SLPI_FASTRPC_ROOT_IOVA  (SLPI_FASTRPC_IOVA + SLPI_FASTRPC_PAGE_SIZE)
#define SLPI_FASTRPC_CONTEXT_BANK 1ull
#define SLPI_FASTRPC_REMOTE_ADDRESS (SLPI_FASTRPC_CONTEXT_BANK << 32 | SLPI_FASTRPC_IOVA)
#define SLPI_FASTRPC_ROOT_REMOTE_ADDRESS (SLPI_FASTRPC_CONTEXT_BANK << 32 | SLPI_FASTRPC_ROOT_IOVA)
#define SLPI_FASTRPC_PAGE_SIZE  0x1000u
#define SLPI_SMMU_IDR0          0x020u
#define SLPI_SMMU_IDR1          0x024u
#define SLPI_SMMU_SMR(n)        (0x800u + 4u * (n))
#define SLPI_SMMU_S2CR(n)       (0xc00u + 4u * (n))
#define SLPI_SMMU_CBAR(n)       (4u * (n))
#define SLPI_SMMU_CBA2R(n)      (0x800u + 4u * (n))
#define SLPI_SMMU_CB_SCTLR      0x000u
#define SLPI_SMMU_CB_TCR2       0x010u
#define SLPI_SMMU_CB_TTBR0      0x020u
#define SLPI_SMMU_CB_TTBR1      0x028u
#define SLPI_SMMU_CB_TCR        0x030u
#define SLPI_SMMU_CB_MAIR0      0x038u
#define SLPI_SMMU_CB_MAIR1      0x03cu
#define SLPI_SMMU_CB_FSR        0x058u
#define SLPI_SMMU_CB_TLBIASID   0x610u
#define SLPI_SMMU_CB_TLBSYNC    0x7f0u
#define SLPI_SMMU_CB_TLBSTATUS  0x7f4u
#define SLPI_SMMU_CBAR_STAGE1   (1u << 16 | 3u << 8 | 0xfu << 12)
#define SLPI_SMMU_SCTLR_ON      (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SLPI_SMMU_TCR           (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SLPI_SMMU_TCR2          (1u | 1u << 4 | 7u << 15)
#define SLPI_SMMU_MAIR0         0xf404ff44u
#define SLPI_SMMU_PTE           (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define SLPI_SMMU_ADDRESS       0x0000fffffffff000ull

static struct {
	vm_offset_t page, tables, global, second, context;
	uint32_t match, context_number;
	bool ready;
} slpi_fastrpc_memory;

static uint32_t
slpi_smmu_read(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
slpi_smmu_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

static bool
slpi_fastrpc_prepare_memory(void)
{
	uint32_t idr0, idr1, page_size, pages, matches, contexts;
	bool used[256] = { false };
	uint64_t *root, *second, *third, table_root;

	if (slpi_fastrpc_memory.ready) {
		return true;
	}
	/* The sensors and optional root PD each have a separate 4-KiB DMA page.
	 * PAGE_SIZE can be 4 KiB at runtime, so one PAGE_SIZE is not enough. */
	if (slpi_fastrpc_memory.page == 0 &&
	    kmem_alloc(kernel_map, &slpi_fastrpc_memory.page, 2u * SLPI_FASTRPC_PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		hakios_tl("slpi: fastrpc: no memory for shared page\n");
		return false;
	}
	if (slpi_fastrpc_memory.tables == 0 &&
	    kmem_alloc(kernel_map, &slpi_fastrpc_memory.tables, 3 * PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		hakios_tl("slpi: fastrpc: no memory for page tables\n");
		return false;
	}
	slpi_fastrpc_memory.global = ml_io_map(SLPI_SMMU_PHYS, 0x2000);
	if (slpi_fastrpc_memory.global == 0) {
		hakios_tl("slpi: fastrpc: cannot map apps SMMU\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	idr0 = slpi_smmu_read(slpi_fastrpc_memory.global, SLPI_SMMU_IDR0);
	idr1 = slpi_smmu_read(slpi_fastrpc_memory.global, SLPI_SMMU_IDR1);
	page_size = idr1 >> 31 ? 0x10000u : 0x1000u;
	pages = 2u << (idr1 >> 28 & 7);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	if (2u * pages * page_size != 0x100000u || contexts > 256 || (slpi_smmu_read(slpi_fastrpc_memory.global, 0) & (1u << 3))) {
		hakios_tl("slpi: fastrpc: unknown apps SMMU layout\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	slpi_fastrpc_memory.match = ~0u;
	for (uint32_t i = 0; i < matches && SLPI_SMMU_SMR(i) < 0xc00u; i++) {
		uint32_t smr = slpi_smmu_read(slpi_fastrpc_memory.global, SLPI_SMMU_SMR(i));
		uint32_t s2cr = slpi_smmu_read(slpi_fastrpc_memory.global, SLPI_SMMU_S2CR(i));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ SLPI_FASTRPC_STREAM) & ~(smr >> 16 & 0x7fff)) == 0) {
				hakios_tl("slpi: fastrpc: stream %#x already owned by SMMU match %u\n", SLPI_FASTRPC_STREAM, i);
				lck_mtx_unlock(&hakios_smmu_lock);
				return false;
			}
		} else if (slpi_fastrpc_memory.match == ~0u) {
			slpi_fastrpc_memory.match = i;
		}
	}
	for (slpi_fastrpc_memory.context_number = 0; slpi_fastrpc_memory.context_number < contexts &&
	    used[slpi_fastrpc_memory.context_number]; slpi_fastrpc_memory.context_number++) {
	}
	if (slpi_fastrpc_memory.match == ~0u || slpi_fastrpc_memory.context_number == contexts) {
		hakios_tl("slpi: fastrpc: no free SMMU match or context\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	slpi_fastrpc_memory.second = ml_io_map(SLPI_SMMU_PHYS + page_size, PAGE_SIZE);
	slpi_fastrpc_memory.context = ml_io_map(SLPI_SMMU_PHYS + (uint64_t)(pages + slpi_fastrpc_memory.context_number) * page_size,
	    PAGE_SIZE);
	if (slpi_fastrpc_memory.second == 0 || slpi_fastrpc_memory.context == 0 ||
	    (slpi_smmu_read(slpi_fastrpc_memory.context, SLPI_SMMU_CB_SCTLR) & 1)) {
		hakios_tl("slpi: fastrpc: selected SMMU context is unavailable\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	root = (uint64_t *)slpi_fastrpc_memory.tables;
	second = (uint64_t *)(slpi_fastrpc_memory.tables + PAGE_SIZE);
	third = (uint64_t *)(slpi_fastrpc_memory.tables + 2 * PAGE_SIZE);
	bzero(root, 3 * PAGE_SIZE);
	root[SLPI_FASTRPC_IOVA >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[SLPI_FASTRPC_IOVA >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	third[SLPI_FASTRPC_IOVA >> 12 & 0x1ff] = kvtophys(slpi_fastrpc_memory.page) | SLPI_SMMU_PTE;
	third[SLPI_FASTRPC_ROOT_IOVA >> 12 & 0x1ff] =
	    kvtophys(slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE) | SLPI_SMMU_PTE;
	for (vm_offset_t line = slpi_fastrpc_memory.tables; line < slpi_fastrpc_memory.tables + 3 * PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	table_root = kvtophys(slpi_fastrpc_memory.tables) | (uint64_t)slpi_fastrpc_memory.context_number << 48;
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_SCTLR, 0);
	slpi_smmu_write(slpi_fastrpc_memory.second, SLPI_SMMU_CBA2R(slpi_fastrpc_memory.context_number), 1);
	slpi_smmu_write(slpi_fastrpc_memory.second, SLPI_SMMU_CBAR(slpi_fastrpc_memory.context_number), SLPI_SMMU_CBAR_STAGE1);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TCR2, SLPI_SMMU_TCR2);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TCR, SLPI_SMMU_TCR);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TTBR0, (uint32_t)table_root);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TTBR1, 0);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TTBR1 + 4, 0);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_MAIR0, SLPI_SMMU_MAIR0);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_MAIR1, 0);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_FSR, 0xc00001feu);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TLBIASID, slpi_fastrpc_memory.context_number);
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TLBSYNC, 1);
	for (unsigned wait = 0; slpi_smmu_read(slpi_fastrpc_memory.context, SLPI_SMMU_CB_TLBSTATUS) & 1; wait++) {
		if (wait == 100) {
			hakios_tl("slpi: fastrpc: SMMU TLB did not synchronize\n");
			lck_mtx_unlock(&hakios_smmu_lock);
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	slpi_smmu_write(slpi_fastrpc_memory.context, SLPI_SMMU_CB_SCTLR, SLPI_SMMU_SCTLR_ON);
	slpi_smmu_write(slpi_fastrpc_memory.global, SLPI_SMMU_S2CR(slpi_fastrpc_memory.match), slpi_fastrpc_memory.context_number);
	slpi_smmu_write(slpi_fastrpc_memory.global, SLPI_SMMU_SMR(slpi_fastrpc_memory.match), 1u << 31 | SLPI_FASTRPC_STREAM);
	if (slpi_smmu_read(slpi_fastrpc_memory.global, SLPI_SMMU_SMR(slpi_fastrpc_memory.match)) !=
	    (1u << 31 | SLPI_FASTRPC_STREAM) ||
	    (slpi_smmu_read(slpi_fastrpc_memory.global, SLPI_SMMU_S2CR(slpi_fastrpc_memory.match)) & 0x300ff) !=
	    slpi_fastrpc_memory.context_number) {
		hakios_tl("slpi: fastrpc: SMMU rejected stream mapping\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	slpi_fastrpc_memory.ready = true;
	lck_mtx_unlock(&hakios_smmu_lock);
	hakios_tl("slpi: fastrpc: shared page %#llx at IOVA %#llx (stream %#x, SMMU context %u)\n",
	    kvtophys(slpi_fastrpc_memory.page), SLPI_FASTRPC_IOVA, SLPI_FASTRPC_STREAM, slpi_fastrpc_memory.context_number);
	return true;
}

/* Qualcomm FastRPC's first kernel invocation: attach to the sensors PD. */
#define SLPI_FASTRPC_INIT_HANDLE       1u
#define SLPI_FASTRPC_RMID_ATTACH       0u
#define SLPI_FASTRPC_SENSORS_PD        2u
#define SLPI_FASTRPC_CONTEXT_SESSION1  (1ull << 4)
#define SLPI_FASTRPC_SESSION_BIT       (1u << 30)
#define SLPI_FASTRPC_CLIENT_TGID       1u
#define SLPI_FASTRPC_LISTENER_TID      2u
#define SLPI_FASTRPC_ROOT_TGID         3u
#define SLPI_FASTRPC_ROOT_LISTENER_TID 4u
#define SLPI_FASTRPC_MAX_FDLIST        16u
#define SLPI_FASTRPC_MAX_CRCLIST       64u
#define SLPI_FASTRPC_ALIGN128(value)   (((value) + 127u) & ~127u)
#define SLPI_FASTRPC_SCALARS(method, in, out) ((uint32_t)(method) << 24 | (uint32_t)(in) << 16 | (uint32_t)(out) << 8)
#define SLPI_FASTRPC_LISTENER_HANDLE 3u
#define SLPI_FASTRPC_LISTENER_INIT2 3u
#define SLPI_FASTRPC_LISTENER_NEXT2 4u
#define SLPI_FASTRPC_LISTENER_GET_INPUT 5u
#define SLPI_FASTRPC_LISTENER_NAME "adsp_default_listener"
#define SLPI_FASTRPC_LISTENER_INPUT_CAPACITY 512u
#define SLPI_FASTRPC_LISTENER_LOG_CAPACITY 128u
#define SLPI_FASTRPC_APPS_STD_HANDLE 0x100u
#define SLPI_FASTRPC_APPS_STD_ERROR_CAPACITY 255u
#define SLPI_FASTRPC_APPS_STD_FOPEN_WITH_ENV 19u
#define SLPI_FASTRPC_AEE_EFAILED 1u
#define SLPI_FASTRPC_ROOT_DENIAL_LIMIT 16u
#define SLPI_FASTRPC_PLATFORM_HANDLE_BASE 0x200u
/* Every value below was read back from this exact UMI's own Android install
 * (dumped via sysfs and cross-checked against sscrpcd's own FastRPC trace):
 * these are read-only hardware identifiers, not guessed protocol content.
 *
 * The ak0991x_0* entries are this device's own magnetometer persist registry
 * files, extracted from fw-sensors/registry.tar (whose tar member names are
 * themselves "registry/registry/<name>") and cross-checked byte-for-byte
 * against sscrpcd's own openat()/read() trace on Android, which reads them
 * from "/mnt/vendor/persist/sensors/registry/registry/<name>" -- the exact
 * path prefix used below. Without these, the SNS registry on the DSP has no
 * record of the ak0991x driver instance and will never hand out a non-empty
 * "mag" SUID, regardless of how well the FastRPC listener itself behaves. */
struct slpi_fastrpc_platform_file { const char *path, *content; };
static const struct slpi_fastrpc_platform_file slpi_fastrpc_platform_files[] = {
	{ "/sys/devices/soc0/hw_platform", "UMI\n" },
	{ "/sys/devices/soc0/platform_subtype", "Unknown\n" },
	{ "/sys/devices/soc0/platform_subtype_id", "0\n" },
	{ "/sys/devices/soc0/platform_version", "589824\n" },
	{ "/sys/devices/soc0/soc_id", "356\n" },
	{ "/sys/devices/soc0/revision", "2.1\n" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0",
	    "{\"ak0991x_0\":{\"owner\":\"sns_ak0991x\",\"mag\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0.mag",
	    "{\"ak0991x_0.mag\":{\"owner\":\"sns_ak0991x\",\"config\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"},\"config_2\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0.mag.config",
	    "{\"ak0991x_0.mag.config\":{\"owner\":\"sns_ak0991x\",\"is_dri\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"hw_id\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"res_idx\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"sync_stream\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0.mag.config_2",
	    "{\"ak0991x_0.mag.config_2\":{\"owner\":\"sns_ak0991x\",\"use_fifo\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"nsf\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"sdr\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform",
	    "{\"ak0991x_0_platform\":{\"owner\":\"sns_ak0991x\",\"config\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"},\"orient\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"},\"mag\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"},\"placement\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"},\"pdc_parameter\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.config",
	    "{\"ak0991x_0_platform.config\":{\"owner\":\"sns_ak0991x\",\"bus_type\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"bus_instance\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"1\"},\"slave_config\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"12\"},\"min_bus_speed_khz\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"400\"},\"max_bus_speed_khz\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"400\"},\"reg_addr_type\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"dri_irq_num\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"119\"},\"irq_pull_type\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"3\"},\"irq_is_chip_pin\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"1\"},\"irq_drive_strength\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"},\"irq_trigger_type\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"1\"},\"num_rail\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"1\"},\"rail_on_state\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"1\"},\"vddio_rail\":{\"type\":\"str\",\"ver\":\"2\",\"data\":\"/pmic/client/sensor_vddio\"},\"rigid_body_type\":{\"type\":\"int\",\"ver\":\"0\",\"data\":\"0\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.mag",
	    "{\"ak0991x_0_platform.mag\":{\"owner\":\"sns_ak0991x\",\"fac_cal\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.mag.fac_cal",
	    "{\"ak0991x_0_platform.mag.fac_cal\":{\"owner\":\"sns_ak0991x\",\"corr_mat\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"},\"bias\":{\"type\":\"grp\",\"ver\":\"0\",\"data\":\"\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.mag.fac_cal.bias",
	    "{\"ak0991x_0_platform.mag.fac_cal.bias\":{\"owner\":\"sns_ak0991x\",\"x\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"y\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"z\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.mag.fac_cal.corr_mat",
	    "{\"ak0991x_0_platform.mag.fac_cal.corr_mat\":{\"owner\":\"sns_ak0991x\",\"0_0\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.996902\"},\"0_1\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"-0.013794\"},\"0_2\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"1_0\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"-0.013794\"},\"1_1\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.997696\"},\"1_2\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"-0.015106\"},\"2_0\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"-0.018906\"},\"2_1\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"-0.015106\"},\"2_2\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"1.005402\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.orient",
	    "{\"ak0991x_0_platform.orient\":{\"owner\":\"sns_ak0991x\",\"x\":{\"type\":\"str\",\"ver\":\"2\",\"data\":\"-x\"},\"y\":{\"type\":\"str\",\"ver\":\"2\",\"data\":\"+y\"},\"z\":{\"type\":\"str\",\"ver\":\"1\",\"data\":\"-z\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.pdc_parameter",
	    "{\"ak0991x_0_platform.pdc_parameter\":{\"owner\":\"sns_ak0991x\",\"0\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"120.000000\"},\"1\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"84.000000\"},\"2\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"195.000000\"},\"3\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"13.000000\"},\"4\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"99.000000\"},\"5\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"69.000000\"},\"6\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"1.000000\"},\"7\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"130.000000\"},\"8\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"213.000000\"},\"9\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"55.000000\"},\"10\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"239.000000\"},\"11\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"87.000000\"},\"12\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"8.000000\"},\"13\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"28.000000\"},\"14\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"253.000000\"},\"15\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"232.000000\"},\"16\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"254.000000\"},\"17\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"148.000000\"},\"18\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"84.000000\"},\"19\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"97.000000\"},\"20\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"192.000000\"},\"21\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"255.000000\"},\"22\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"18.000000\"},\"23\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"227.000000\"},\"24\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"11.000000\"},\"25\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"77.000000\"},\"26\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"103.000000\"}}}" },
	{ "/mnt/vendor/persist/sensors/registry/registry/ak0991x_0_platform.placement",
	    "{\"ak0991x_0_platform.placement\":{\"owner\":\"sns_ak0991x\",\"0\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"1\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"2\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"3\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"4\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"5\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"6\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"7\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"8\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"9\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"10\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"},\"11\":{\"type\":\"flt\",\"ver\":\"0\",\"data\":\"0.000000\"}}}" },
};
#define SLPI_FASTRPC_PLATFORM_FILE_COUNT \
	(sizeof(slpi_fastrpc_platform_files) / sizeof(slpi_fastrpc_platform_files[0]))
struct slpi_fastrpc_message {
	int32_t client_id, tid;
	uint64_t context;
	uint32_t handle, scalars;
	uint64_t address, size;
} __attribute__((packed));
struct slpi_fastrpc_remote_arg { uint64_t pointer, length; };
struct slpi_fastrpc_invoke_buf { uint32_t number, page_index; };
struct slpi_fastrpc_phy_page { uint64_t address, size; };

enum slpi_fastrpc_phase {
	SLPI_FASTRPC_IDLE,
	SLPI_FASTRPC_ATTACH_PENDING,
	SLPI_FASTRPC_LISTENER_INIT_PENDING,
	SLPI_FASTRPC_LISTENER_OPEN_PENDING,
	SLPI_FASTRPC_LISTENER_REGISTER_PENDING,
	SLPI_FASTRPC_LISTENER_NEXT_PENDING,
	SLPI_FASTRPC_LISTENER_INPUT_PENDING,
};
struct slpi_fastrpc_arg_spec {
	const void *input;
	uint32_t length;
	void **mapped;
};
static enum slpi_fastrpc_phase slpi_fastrpc_phase;
static uint32_t slpi_fastrpc_listener_module;
static uint32_t *slpi_fastrpc_listener_open_output;
static uint32_t *slpi_fastrpc_listener_next_output;
static uint32_t *slpi_fastrpc_listener_input_output;
static uint8_t *slpi_fastrpc_listener_input;
static uint32_t slpi_fastrpc_listener_invoke_context;
static uint32_t slpi_fastrpc_listener_invoke_handle;
static uint32_t slpi_fastrpc_listener_invoke_scalars;
static int slpi_fastrpc_platform_open_index = -1;
static uint32_t slpi_fastrpc_platform_read_offset;
static bool slpi_fastrpc_waiting_listener_next;
static uint32_t slpi_fastrpc_tx_sequence;
static uint32_t slpi_fastrpc_rx_sequence;
static bool slpi_fastrpc_root_decision_made;
static uint32_t slpi_fastrpc_root_listener_module;
static uint32_t *slpi_fastrpc_root_listener_open_output;
static uint32_t *slpi_fastrpc_root_listener_next_output;
static uint32_t *slpi_fastrpc_root_listener_input_output;
static uint8_t *slpi_fastrpc_root_listener_input;
static uint32_t slpi_fastrpc_root_listener_invoke_context;
static uint32_t slpi_fastrpc_root_listener_invoke_handle;
static uint32_t slpi_fastrpc_root_listener_invoke_scalars;
static uint32_t slpi_fastrpc_root_denials;
static bool slpi_fastrpc_root_apps_std_opened;
static bool slpi_fastrpc_send_root_attach(void);

/* next2 may legitimately wait for the next DSP callback after any reply we
 * send (every reply we send is itself a fresh next2, tagged 0x5002).  Log
 * bounded snapshots while the kernel still responds, without resending RPCs. */
static void
slpi_fastrpc_watch_wait(uint32_t poll_ms)
{
	unsigned busy = 0;

	if (!slpi_fastrpc_waiting_listener_next || !slpi_fastrpc_memory.ready) {
		return;
	}
	for (unsigned i = 0; i < slpi_glink.remote_intents; i++) {
		if (slpi_glink.remote_intent[i].channel == 2 && slpi_glink.fastrpc_remote_busy[i]) {
			busy++;
		}
	}
	hakios_tl("slpi: fastrpc: next2 ctx 0x5002 still awaiting callback at poll +%u ms (phase %u, TX/RX %u/%u, invoke %#x/%#x/%#x, GLINK broken %u, TX busy %u, SMMU FSR %#x)\n",
	    poll_ms, slpi_fastrpc_phase, slpi_fastrpc_tx_sequence, slpi_fastrpc_rx_sequence,
	    slpi_fastrpc_listener_invoke_context, slpi_fastrpc_listener_invoke_handle,
	    slpi_fastrpc_listener_invoke_scalars, slpi_glink.broken, busy,
	    slpi_smmu_read(slpi_fastrpc_memory.context, SLPI_SMMU_CB_FSR));
}

static bool
slpi_fastrpc_root_probe_enabled(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("slpi_root", &on, sizeof(on)) && on != 0;
}

static void
slpi_fastrpc_flush_page(void)
{
	for (vm_offset_t line = slpi_fastrpc_memory.page;
	    line < slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

static void
slpi_fastrpc_invalidate_page(void)
{
	for (vm_offset_t line = slpi_fastrpc_memory.page;
	    line < slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc ivac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

static void
slpi_fastrpc_flush_root_page(void)
{
	for (vm_offset_t line = slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE;
	    line < slpi_fastrpc_memory.page + 2 * SLPI_FASTRPC_PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

static void
slpi_fastrpc_invalidate_root_page(void)
{
	for (vm_offset_t line = slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE;
	    line < slpi_fastrpc_memory.page + 2 * SLPI_FASTRPC_PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc ivac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/* Build the metadata and copied argument area exactly as fastrpc_get_args(). */
static bool
slpi_fastrpc_build_args(uint32_t scalars, const struct slpi_fastrpc_arg_spec *spec, uint32_t count)
{
	struct slpi_fastrpc_remote_arg *argument;
	struct slpi_fastrpc_invoke_buf *invoke;
	struct slpi_fastrpc_phy_page *page;
	uint8_t *shared = (uint8_t *)(void *)slpi_fastrpc_memory.page;
	uint32_t expected = (scalars >> 16 & 0xff) + (scalars >> 8 & 0xff) +
	    (scalars >> 4 & 0xf) + (scalars & 0xf);
	uint32_t cursor;

	if (count != expected || count > 8) {
		return false;
	}
	bzero(shared, SLPI_FASTRPC_PAGE_SIZE);
	argument = (struct slpi_fastrpc_remote_arg *)(void *)shared;
	invoke = (struct slpi_fastrpc_invoke_buf *)(void *)(argument + count);
	page = (struct slpi_fastrpc_phy_page *)(void *)(invoke + count);
	cursor = SLPI_FASTRPC_ALIGN128((uint32_t)((uint8_t *)(void *)(page + count) - shared) +
	    sizeof(uint64_t) * SLPI_FASTRPC_MAX_FDLIST +
	    sizeof(uint32_t) * SLPI_FASTRPC_MAX_CRCLIST + sizeof(uint32_t));
	for (uint32_t i = 0; i < count; i++) {
		if (spec[i].mapped != NULL) {
			*spec[i].mapped = NULL;
		}
		argument[i].length = spec[i].length;
		invoke[i].page_index = i;
		if (spec[i].length == 0) {
			continue;
		}
		cursor = SLPI_FASTRPC_ALIGN128(cursor);
		if (cursor > SLPI_FASTRPC_PAGE_SIZE || spec[i].length > SLPI_FASTRPC_PAGE_SIZE - cursor) {
			return false;
		}
		argument[i].pointer = (uint64_t)(uintptr_t)(shared + cursor);
		invoke[i].number = 1;
		page[i].address = SLPI_FASTRPC_REMOTE_ADDRESS;
		page[i].size = SLPI_FASTRPC_PAGE_SIZE;
		if (spec[i].input != NULL) {
			memcpy(shared + cursor, spec[i].input, spec[i].length);
		}
		if (spec[i].mapped != NULL) {
			*spec[i].mapped = shared + cursor;
		}
		cursor += spec[i].length;
	}
	slpi_fastrpc_flush_page();
	return true;
}

static bool
slpi_fastrpc_build_root_args(uint32_t scalars, const struct slpi_fastrpc_arg_spec *spec, uint32_t count)
{
	struct slpi_fastrpc_remote_arg *argument;
	struct slpi_fastrpc_invoke_buf *invoke;
	struct slpi_fastrpc_phy_page *page;
	uint8_t *shared = (uint8_t *)(void *)(slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE);
	uint32_t expected = (scalars >> 16 & 0xff) + (scalars >> 8 & 0xff) +
	    (scalars >> 4 & 0xf) + (scalars & 0xf);
	uint32_t cursor;

	if (count != expected || count > 8) {
		return false;
	}
	bzero(shared, SLPI_FASTRPC_PAGE_SIZE);
	argument = (struct slpi_fastrpc_remote_arg *)(void *)shared;
	invoke = (struct slpi_fastrpc_invoke_buf *)(void *)(argument + count);
	page = (struct slpi_fastrpc_phy_page *)(void *)(invoke + count);
	cursor = SLPI_FASTRPC_ALIGN128((uint32_t)((uint8_t *)(void *)(page + count) - shared) +
	    sizeof(uint64_t) * SLPI_FASTRPC_MAX_FDLIST +
	    sizeof(uint32_t) * SLPI_FASTRPC_MAX_CRCLIST + sizeof(uint32_t));
	for (uint32_t i = 0; i < count; i++) {
		if (spec[i].mapped != NULL) {
			*spec[i].mapped = NULL;
		}
		argument[i].length = spec[i].length;
		invoke[i].page_index = i;
		if (spec[i].length == 0) {
			continue;
		}
		cursor = SLPI_FASTRPC_ALIGN128(cursor);
		if (cursor > SLPI_FASTRPC_PAGE_SIZE || spec[i].length > SLPI_FASTRPC_PAGE_SIZE - cursor) {
			return false;
		}
		argument[i].pointer = (uint64_t)(uintptr_t)(shared + cursor);
		invoke[i].number = 1;
		page[i].address = SLPI_FASTRPC_ROOT_REMOTE_ADDRESS;
		page[i].size = SLPI_FASTRPC_PAGE_SIZE;
		if (spec[i].input != NULL) {
			memcpy(shared + cursor, spec[i].input, spec[i].length);
		}
		if (spec[i].mapped != NULL) {
			*spec[i].mapped = shared + cursor;
		}
		cursor += spec[i].length;
	}
	slpi_fastrpc_flush_root_page();
	return true;
}

static bool
slpi_fastrpc_send_request(uint32_t handle, uint32_t scalars, uint64_t context, bool has_arguments)
{
	struct slpi_fastrpc_message message = { 0 };

	/* Only INIT_ATTACH is a kernel invoke (pid zero).  Listener calls belong
	 * to the synthetic APSS client which the attach argument registered. */
	message.client_id = SLPI_FASTRPC_CLIENT_TGID;
	message.tid = ((context & 0xffu) == 0x02u ?
	    SLPI_FASTRPC_LISTENER_TID : SLPI_FASTRPC_CLIENT_TGID) |
	    SLPI_FASTRPC_SESSION_BIT;
	message.context = context;
	message.handle = handle;
	message.scalars = scalars;
	message.address = has_arguments ? SLPI_FASTRPC_REMOTE_ADDRESS : 0;
	message.size = SLPI_FASTRPC_PAGE_SIZE;
	if (!slpi_glink_send_data(2, &message, sizeof(message))) {
		hakios_tl("slpi: fastrpc: cannot send listener request (handle %#x, sc %#x)\n", handle, scalars);
		slpi_fastrpc_phase = SLPI_FASTRPC_IDLE;
		return false;
	}
	slpi_fastrpc_tx_sequence++;
	hakios_tl("slpi: fastrpc: TX #%u request (ctx %#llx, pid %d, tid %#x, handle %#x, sc %#x, args %#llx/%u)\n",
	    slpi_fastrpc_tx_sequence, context, message.client_id, message.tid, handle, scalars,
	    message.address, message.size);
	return true;
}

static bool
slpi_fastrpc_send_root_request(uint32_t handle, uint32_t scalars, uint64_t context, bool has_arguments)
{
	struct slpi_fastrpc_message message = { 0 };

	message.client_id = SLPI_FASTRPC_ROOT_TGID;
	message.tid = (context == 0x6000 || context == 0x7000 ||
	    context == 0x8000 || context == 0x9000) ?
	    SLPI_FASTRPC_ROOT_LISTENER_TID : SLPI_FASTRPC_ROOT_TGID;
	message.context = context;
	message.handle = handle;
	message.scalars = scalars;
	message.address = has_arguments ? SLPI_FASTRPC_ROOT_REMOTE_ADDRESS : 0;
	message.size = SLPI_FASTRPC_PAGE_SIZE;
	if (!slpi_glink_send_data(2, &message, sizeof(message))) {
		hakios_tl("slpi: fastrpc: cannot send root listener request (handle %#x, sc %#x)\n", handle, scalars);
		return false;
	}
	hakios_tl("slpi: fastrpc: sent root request (ctx %#llx, pid %d, tid %#x, handle %#x, sc %#x)\n",
	    context, message.client_id, message.tid, handle, scalars);
	return true;
}

static bool
slpi_fastrpc_send_listener_init(void)
{
	slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_INIT_PENDING;
	return slpi_fastrpc_send_request(SLPI_FASTRPC_LISTENER_HANDLE,
	    SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_INIT2, 0, 0), 0x2012, false);
}

static bool
slpi_fastrpc_send_listener_open(void)
{
	static const char name[] = SLPI_FASTRPC_LISTENER_NAME;
	uint32_t primitive_input[2] = { sizeof(name), 255 };
	struct slpi_fastrpc_arg_spec spec[4] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ name, sizeof(name), NULL },
		{ NULL, 2 * sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_listener_open_output },
		{ NULL, 255, NULL },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(0, 2, 2);

	if (!slpi_fastrpc_build_args(scalars, spec, 4)) {
		return false;
	}
	slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_OPEN_PENDING;
	return slpi_fastrpc_send_request(0, scalars, 0x3012, true);
}

static bool
slpi_fastrpc_send_root_listener_init(void)
{
	return slpi_fastrpc_send_root_request(SLPI_FASTRPC_LISTENER_HANDLE,
	    SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_INIT2, 0, 0), 0x3000, false);
}

static bool
slpi_fastrpc_send_root_listener_open(void)
{
	static const char name[] = SLPI_FASTRPC_LISTENER_NAME;
	uint32_t primitive_input[2] = { sizeof(name), 255 };
	struct slpi_fastrpc_arg_spec spec[4] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ name, sizeof(name), NULL },
		{ NULL, 2 * sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_root_listener_open_output },
		{ NULL, 255, NULL },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(0, 2, 2);

	if (!slpi_fastrpc_build_root_args(scalars, spec, 4)) {
		return false;
	}
	return slpi_fastrpc_send_root_request(0, scalars, 0x4000, true);
}

static bool
slpi_fastrpc_send_root_listener_register(void)
{
	return slpi_fastrpc_send_root_request(slpi_fastrpc_root_listener_module, 0, 0x5000, false);
}

static bool
slpi_fastrpc_send_root_listener_next(void)
{
	uint32_t primitive_input[4] = { 0, (uint32_t)-1, 0, 0 };
	struct slpi_fastrpc_arg_spec spec[4] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ NULL, 0, NULL },
		{ NULL, 4 * sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_root_listener_next_output },
		{ NULL, 0, NULL },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_NEXT2, 2, 2);

	if (!slpi_fastrpc_build_root_args(scalars, spec, 4)) {
		return false;
	}
	return slpi_fastrpc_send_root_request(SLPI_FASTRPC_LISTENER_HANDLE, scalars, 0x6000, true);
}

static bool
slpi_fastrpc_send_root_listener_get_input(void)
{
	uint32_t primitive_input[3] = {
		slpi_fastrpc_root_listener_invoke_context, 0, SLPI_FASTRPC_LISTENER_INPUT_CAPACITY,
	};
	struct slpi_fastrpc_arg_spec spec[3] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ NULL, sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_root_listener_input_output },
		{ NULL, SLPI_FASTRPC_LISTENER_INPUT_CAPACITY, (void **)(void *)&slpi_fastrpc_root_listener_input },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_GET_INPUT, 1, 2);

	if (!slpi_fastrpc_build_root_args(scalars, spec, 3)) {
		return false;
	}
	return slpi_fastrpc_send_root_request(SLPI_FASTRPC_LISTENER_HANDLE, scalars, 0x7000, true);
}

/* Root PD must answer its own reverse RPC on its own page and listener TID.
 * next2 carries the previous callback's AEE result separately from its
 * serialized output buffers.  Never turn a DSP-supplied filename into an
 * APFS lookup here: a failed open returns AEE_EFAILED with no output. */
static bool
slpi_fastrpc_send_root_listener_reply_and_next(const void *response, uint32_t response_length, uint32_t result)
{
	uint32_t primitive_input[4] = {
		slpi_fastrpc_root_listener_invoke_context, result, response_length, 0,
	};
	struct slpi_fastrpc_arg_spec spec[4] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ response, response_length, NULL },
		{ NULL, 4 * sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_root_listener_next_output },
		{ NULL, 0, NULL },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_NEXT2, 2, 2);

	if (!slpi_fastrpc_build_root_args(scalars, spec, 4)) {
		return false;
	}
	return slpi_fastrpc_send_root_request(SLPI_FASTRPC_LISTENER_HANDLE, scalars, 0x8000, true);
}

static bool
slpi_fastrpc_send_root_listener_open_reply(void)
{
	uint8_t response[24 + SLPI_FASTRPC_APPS_STD_ERROR_CAPACITY] = { 0 };
	uint32_t primitive_output[2] = { SLPI_FASTRPC_APPS_STD_HANDLE, 0 };
	uint32_t primitive_length = sizeof(primitive_output);
	uint32_t error_length = SLPI_FASTRPC_APPS_STD_ERROR_CAPACITY;

	memcpy(response, &primitive_length, sizeof(primitive_length));
	memcpy(response + 8, primitive_output, sizeof(primitive_output));
	memcpy(response + 16, &error_length, sizeof(error_length));
	if (!slpi_fastrpc_send_root_listener_reply_and_next(response, sizeof(response), 0)) {
		return false;
	}
	slpi_fastrpc_root_apps_std_opened = true;
	return true;
}

/* Android's listener clears outBufsLen when the callback returns an error.
 * For an unsupported root file open, return only AEE_EFAILED; do not send
 * the four-byte handle output or grant any DSP-provided path access. */
static bool
slpi_fastrpc_send_root_listener_open_failure(void)
{
	if (slpi_fastrpc_root_denials >= SLPI_FASTRPC_ROOT_DENIAL_LIMIT) {
		hakios_tl("slpi: fastrpc: root file-open denial limit reached\n");
		return false;
	}
	if (!slpi_fastrpc_send_root_listener_reply_and_next(NULL, 0, SLPI_FASTRPC_AEE_EFAILED)) {
		return false;
	}
	slpi_fastrpc_root_denials++;
	hakios_tl("slpi: fastrpc: denied root apps_std file open (%u/%u)\n",
	    slpi_fastrpc_root_denials, SLPI_FASTRPC_ROOT_DENIAL_LIMIT);
	return true;
}

static bool
slpi_fastrpc_send_listener_register(void)
{
	slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_REGISTER_PENDING;
	return slpi_fastrpc_send_request(slpi_fastrpc_listener_module, 0, 0x4012, false);
}

static bool
slpi_fastrpc_send_listener_next(void)
{
	uint32_t primitive_input[4] = { 0, (uint32_t)-1, 0, 0 };
	struct slpi_fastrpc_arg_spec spec[4] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ NULL, 0, NULL },
		{ NULL, 4 * sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_listener_next_output },
		{ NULL, 0, NULL },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_NEXT2, 2, 2);
	bool sent;

	if (!slpi_fastrpc_build_args(scalars, spec, 4)) {
		return false;
	}
	slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_NEXT_PENDING;
	sent = slpi_fastrpc_send_request(SLPI_FASTRPC_LISTENER_HANDLE, scalars, 0x5002, true);
	if (sent) {
		slpi_fastrpc_waiting_listener_next = true;
	}
	return sent;
}

static bool
slpi_fastrpc_send_listener_get_input(uint64_t context)
{
	uint32_t primitive_input[3] = {
		slpi_fastrpc_listener_invoke_context, 0, SLPI_FASTRPC_LISTENER_INPUT_CAPACITY,
	};
	struct slpi_fastrpc_arg_spec spec[3] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ NULL, sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_listener_input_output },
		{ NULL, SLPI_FASTRPC_LISTENER_INPUT_CAPACITY, (void **)(void *)&slpi_fastrpc_listener_input },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_GET_INPUT, 1, 2);

	/* next2 reports the required serialized input size before get_in_bufs2.
	 * Do not silently truncate a larger callback into the 512-byte probe page. */
	if (slpi_fastrpc_listener_next_output != NULL &&
	    slpi_fastrpc_listener_next_output[3] > SLPI_FASTRPC_LISTENER_INPUT_CAPACITY) {
		hakios_tl("slpi: fastrpc: listener input requires %u bytes (capacity %u); not fetched\n",
		    slpi_fastrpc_listener_next_output[3], SLPI_FASTRPC_LISTENER_INPUT_CAPACITY);
		slpi_fastrpc_phase = SLPI_FASTRPC_IDLE;
		return false;
	}
	if (!slpi_fastrpc_build_args(scalars, spec, 3)) {
		return false;
	}
	slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_INPUT_PENDING;
	return slpi_fastrpc_send_request(SLPI_FASTRPC_LISTENER_HANDLE, scalars, context, true);
}

/* Every reply we send (success or denial) is itself a fresh next2 call, so
 * request_context is always 0x5002 now: one reentrant pair of tags (0x5002
 * for next2, 0x6002 for get_input) drives the whole listener loop instead
 * of minting a unique context per expected step. */
static bool
slpi_fastrpc_send_listener_reply_and_next(const void *response, uint32_t response_length,
    int32_t result, uint64_t request_context)
{
	uint32_t primitive_input[4] = {
		slpi_fastrpc_listener_invoke_context, (uint32_t)result, response_length, 0,
	};
	struct slpi_fastrpc_arg_spec spec[4] = {
		{ primitive_input, sizeof(primitive_input), NULL },
		{ response, response_length, NULL },
		{ NULL, 4 * sizeof(uint32_t), (void **)(void *)&slpi_fastrpc_listener_next_output },
		{ NULL, 0, NULL },
	};
	uint32_t scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_LISTENER_NEXT2, 2, 2);
	bool sent;

	if (!slpi_fastrpc_build_args(scalars, spec, 4)) {
		return false;
	}
	slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_NEXT_PENDING;
	sent = slpi_fastrpc_send_request(SLPI_FASTRPC_LISTENER_HANDLE, scalars, request_context, true);
	if (sent) {
		slpi_fastrpc_waiting_listener_next = true;
	}
	return sent;
}

/* Complete apps_remotectl_open("apps_std") and atomically wait for the next
 * reverse invocation.  listener_buf.h serializes each output as a length,
 * alignment padding, then its bytes. */
static bool
slpi_fastrpc_send_listener_open_reply(void)
{
	uint8_t response[24 + SLPI_FASTRPC_APPS_STD_ERROR_CAPACITY] = { 0 };
	uint32_t primitive_output[2] = { SLPI_FASTRPC_APPS_STD_HANDLE, 0 };
	uint32_t primitive_length = sizeof(primitive_output);
	uint32_t error_length = SLPI_FASTRPC_APPS_STD_ERROR_CAPACITY;

	memcpy(response, &primitive_length, sizeof(primitive_length));
	memcpy(response + 8, primitive_output, sizeof(primitive_output));
	memcpy(response + 16, &error_length, sizeof(error_length));
	return slpi_fastrpc_send_listener_reply_and_next(response, sizeof(response), 0, 0x5002);
}

/* Search for a whitelisted path as a NUL-terminated substring anywhere in
 * the bounded 512-byte listener input, rather than trusting a byte offset
 * only ever observed for one path length (hw_platform's).  A failed search
 * only leads to a safe denial reply; it can never read past the fixed-size
 * input buffer. */
static int
slpi_fastrpc_platform_lookup(void)
{
	if (slpi_fastrpc_listener_input == NULL) {
		return -1;
	}
	for (unsigned i = 0; i < SLPI_FASTRPC_PLATFORM_FILE_COUNT; i++) {
		const char *path = slpi_fastrpc_platform_files[i].path;
		size_t path_len = strlen(path);

		for (uint32_t at = 0; at + path_len + 1 <= SLPI_FASTRPC_LISTENER_INPUT_CAPACITY; at++) {
			if (memcmp(slpi_fastrpc_listener_input + at, path, path_len + 1) == 0) {
				return (int)i;
			}
		}
	}
	return -1;
}

static bool
slpi_fastrpc_send_platform_open_reply(int index)
{
	uint8_t response[12] = { 0 };
	uint32_t output_length = sizeof(uint32_t);
	uint32_t handle = SLPI_FASTRPC_PLATFORM_HANDLE_BASE + (uint32_t)index;

	memcpy(response, &output_length, sizeof(output_length));
	memcpy(response + 8, &handle, sizeof(handle));
	slpi_fastrpc_platform_open_index = index;
	slpi_fastrpc_platform_read_offset = 0;
	hakios_tl("slpi: fastrpc: opened platform file \"%s\" (handle %#x)\n",
	    slpi_fastrpc_platform_files[index].path, handle);
	return slpi_fastrpc_send_listener_reply_and_next(response, sizeof(response), 0, 0x5002);
}

/* Serve one fread() call at a time out of the whitelisted content, exactly
 * like a real file: each call returns up to SLPI_FASTRPC_LISTENER_INPUT_CAPACITY
 * bytes starting at the file's current cursor, advances the cursor by that
 * much, and a call once the cursor has reached the end returns 0 bytes (EOF).
 * This is required for the several ak0991x registry files whose content
 * (up to 1384 bytes) exceeds a single 512-byte reply and must be drained
 * across multiple fread callbacks, the same way Android's own sscrpcd would
 * observe multiple openat()+read() rounds for one logical file. */
static bool
slpi_fastrpc_send_platform_read_reply(void)
{
	uint8_t response[24 + SLPI_FASTRPC_LISTENER_INPUT_CAPACITY] = { 0 };
	const char *content;
	uint32_t content_length;
	uint32_t primitive_length = 2 * sizeof(uint32_t);
	uint32_t primitive_output[2];
	uint32_t data_length = SLPI_FASTRPC_LISTENER_INPUT_CAPACITY;
	uint32_t chunk;

	if (slpi_fastrpc_platform_open_index < 0) {
		return false;
	}
	content = slpi_fastrpc_platform_files[slpi_fastrpc_platform_open_index].content;
	content_length = (uint32_t)strlen(content);
	if (slpi_fastrpc_platform_read_offset > content_length) {
		return false;
	}
	chunk = MIN(content_length - slpi_fastrpc_platform_read_offset, SLPI_FASTRPC_LISTENER_INPUT_CAPACITY);
	primitive_output[0] = chunk;
	/* Match apps_std_fread(): reaching the end with a non-empty read does
	 * not set feof().  The following zero-byte read is what reports EOF. */
	primitive_output[1] = chunk == 0;
	memcpy(response, &primitive_length, sizeof(primitive_length));
	memcpy(response + 8, primitive_output, sizeof(primitive_output));
	memcpy(response + 16, &data_length, sizeof(data_length));
	if (chunk > 0) {
		memcpy(response + 24, content + slpi_fastrpc_platform_read_offset, chunk);
	}
	hakios_tl("slpi: fastrpc: read platform file \"%s\" +%u: %u bytes (of %u total)\n",
	    slpi_fastrpc_platform_files[slpi_fastrpc_platform_open_index].path,
	    slpi_fastrpc_platform_read_offset, chunk, content_length);
	slpi_fastrpc_platform_read_offset += chunk;
	return slpi_fastrpc_send_listener_reply_and_next(response, sizeof(response), 0, 0x5002);
}

static bool
slpi_fastrpc_send_platform_close_reply(void)
{
	bool sent;

	if (slpi_fastrpc_platform_open_index < 0) {
		return false;
	}
	hakios_tl("slpi: fastrpc: closing platform file \"%s\"\n",
	    slpi_fastrpc_platform_files[slpi_fastrpc_platform_open_index].path);
	slpi_fastrpc_platform_open_index = -1;
	sent = slpi_fastrpc_send_listener_reply_and_next(NULL, 0, 0, 0x5002);
	if (sent && !slpi_fastrpc_root_decision_made) {
		slpi_fastrpc_root_decision_made = true;
		if (slpi_fastrpc_root_probe_enabled()) {
			(void)slpi_fastrpc_send_root_attach();
		} else {
			hakios_tl("slpi: fastrpc: sensors-only listener (use slpi_root=1 for the unrelated root-PD probe)\n");
		}
	}
	return sent;
}

static uint32_t
slpi_fastrpc_log_listener_input(void)
{
	uint32_t length = 0;

	if (slpi_fastrpc_listener_input_output != NULL && slpi_fastrpc_listener_input != NULL) {
		length = slpi_fastrpc_listener_input_output[0];
		uint32_t shown = MIN(MIN(length, SLPI_FASTRPC_LISTENER_INPUT_CAPACITY),
		    SLPI_FASTRPC_LISTENER_LOG_CAPACITY);

		hakios_tl("slpi: fastrpc: listener input: %u bytes (ctx %#x, handle %#x, sc %#x)\n",
		    length, slpi_fastrpc_listener_invoke_context, slpi_fastrpc_listener_invoke_handle,
		    slpi_fastrpc_listener_invoke_scalars);
		if (length > SLPI_FASTRPC_LISTENER_INPUT_CAPACITY) {
			hakios_tl("slpi: fastrpc: listener returned oversized input; refusing to decode\n");
			return length;
		}
		for (uint32_t offset = 0; offset < shown; offset += 16) {
			char line[16 * 3 + 1];
			uint32_t here = MIN(16u, shown - offset), at = 0;

			for (uint32_t i = 0; i < here; i++) {
				at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x ",
				    slpi_fastrpc_listener_input[offset + i]);
			}
			line[MIN(at, (uint32_t)sizeof(line) - 1)] = 0;
			hakios_tl("slpi: fastrpc: listener input +%u: %s\n", offset, line);
		}
	}
	return length;
}

/* Every reverse RPC callback gets exactly one reply: a known, whitelisted
 * request succeeds; anything else is denied with AEE_EFAILED and an empty
 * output (matching Android's listener error convention), and the loop
 * keeps calling next2 either way.  The previous implementation matched
 * only one fixed context sequence and silently stopped replying to any
 * callback it did not recognize, which would leave the DSP's FastRPC
 * session waiting forever for a reply that never comes. */
static bool
slpi_fastrpc_dispatch_listener_input(void)
{
	uint32_t length = slpi_fastrpc_log_listener_input();

	if (slpi_fastrpc_listener_invoke_handle == 0 &&
	    slpi_fastrpc_listener_invoke_scalars == SLPI_FASTRPC_SCALARS(0, 2, 2) &&
	    length >= 33 && slpi_fastrpc_listener_input != NULL &&
	    memcmp(slpi_fastrpc_listener_input + 24, "apps_std", 9) == 0) {
		return slpi_fastrpc_send_listener_open_reply();
	}
	if (slpi_fastrpc_listener_invoke_handle == SLPI_FASTRPC_APPS_STD_HANDLE &&
	    slpi_fastrpc_listener_invoke_scalars ==
	    SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_APPS_STD_FOPEN_WITH_ENV, 5, 1)) {
		int index = slpi_fastrpc_platform_lookup();

		if (index >= 0) {
			return slpi_fastrpc_send_platform_open_reply(index);
		}
	}
	if (slpi_fastrpc_listener_invoke_handle == SLPI_FASTRPC_APPS_STD_HANDLE &&
	    slpi_fastrpc_listener_invoke_scalars == SLPI_FASTRPC_SCALARS(4, 1, 2) &&
	    length == 24 && slpi_fastrpc_listener_input != NULL) {
		uint32_t handle, capacity;

		memcpy(&handle, slpi_fastrpc_listener_input + 8, sizeof(handle));
		memcpy(&capacity, slpi_fastrpc_listener_input + 12, sizeof(capacity));
		if (slpi_fastrpc_platform_open_index >= 0 &&
		    handle == SLPI_FASTRPC_PLATFORM_HANDLE_BASE + (uint32_t)slpi_fastrpc_platform_open_index &&
		    capacity == SLPI_FASTRPC_LISTENER_INPUT_CAPACITY) {
			return slpi_fastrpc_send_platform_read_reply();
		}
	}
	if (slpi_fastrpc_listener_invoke_handle == SLPI_FASTRPC_APPS_STD_HANDLE &&
	    slpi_fastrpc_listener_invoke_scalars == SLPI_FASTRPC_SCALARS(3, 1, 0) &&
	    length == 12 && slpi_fastrpc_listener_input != NULL) {
		uint32_t handle;

		memcpy(&handle, slpi_fastrpc_listener_input + 8, sizeof(handle));
		if (slpi_fastrpc_platform_open_index >= 0 &&
		    handle == SLPI_FASTRPC_PLATFORM_HANDLE_BASE + (uint32_t)slpi_fastrpc_platform_open_index) {
			return slpi_fastrpc_send_platform_close_reply();
		}
	}
	hakios_tl("slpi: fastrpc: listener callback not serviced (denying, continuing to listen)\n");
	return slpi_fastrpc_send_listener_reply_and_next(NULL, 0, SLPI_FASTRPC_AEE_EFAILED, 0x5002);
}

static void
slpi_fastrpc_handle_reply(uint64_t context, int32_t result)
{
	slpi_fastrpc_rx_sequence++;
	hakios_tl("slpi: fastrpc: RX #%u ctx %#llx result %d phase %u (waiting next2 %u)\n",
	    slpi_fastrpc_rx_sequence, context, result, slpi_fastrpc_phase,
	    slpi_fastrpc_waiting_listener_next);
	if (context == 0x5002) {
		slpi_fastrpc_waiting_listener_next = false;
	}
	if (result != 0) {
		hakios_tl("slpi: fastrpc: listener request ctx %#llx failed: %d\n", context, result);
		slpi_fastrpc_phase = SLPI_FASTRPC_IDLE;
		return;
	}
	switch (context) {
	case 0x2000:
		hakios_tl("slpi: fastrpc: root PD attached\n");
		(void)slpi_fastrpc_send_root_listener_init();
		break;
	case 0x3000:
		(void)slpi_fastrpc_send_root_listener_open();
		break;
	case 0x4000:
		slpi_fastrpc_invalidate_root_page();
		if (slpi_fastrpc_root_listener_open_output == NULL ||
		    slpi_fastrpc_root_listener_open_output[1] != 0 ||
		    slpi_fastrpc_root_listener_open_output[0] == 0) {
			hakios_tl("slpi: fastrpc: root listener open failed (handle %#x, loader %d)\n",
			    slpi_fastrpc_root_listener_open_output == NULL ? 0 : slpi_fastrpc_root_listener_open_output[0],
			    slpi_fastrpc_root_listener_open_output == NULL ? -1 :
			    (int32_t)slpi_fastrpc_root_listener_open_output[1]);
			break;
		}
		slpi_fastrpc_root_listener_module = slpi_fastrpc_root_listener_open_output[0];
		hakios_tl("slpi: fastrpc: root listener opened (module %#x)\n",
		    slpi_fastrpc_root_listener_module);
		(void)slpi_fastrpc_send_root_listener_register();
		break;
	case 0x5000:
		hakios_tl("slpi: fastrpc: root listener registered (module %#x)\n",
		    slpi_fastrpc_root_listener_module);
		(void)slpi_fastrpc_send_root_listener_next();
		break;
	case 0x6000:
	case 0x8000:
		slpi_fastrpc_invalidate_root_page();
		if (slpi_fastrpc_root_listener_next_output != NULL) {
			slpi_fastrpc_root_listener_invoke_context = slpi_fastrpc_root_listener_next_output[0];
			slpi_fastrpc_root_listener_invoke_handle = slpi_fastrpc_root_listener_next_output[1];
			slpi_fastrpc_root_listener_invoke_scalars = slpi_fastrpc_root_listener_next_output[2];
			hakios_tl("slpi: fastrpc: root listener callback (ctx %#x, handle %#x, sc %#x, input %u bytes)\n",
			    slpi_fastrpc_root_listener_invoke_context, slpi_fastrpc_root_listener_invoke_handle,
			    slpi_fastrpc_root_listener_invoke_scalars, slpi_fastrpc_root_listener_next_output[3]);
			if (slpi_fastrpc_root_listener_next_output[3] <= SLPI_FASTRPC_LISTENER_INPUT_CAPACITY) {
				(void)slpi_fastrpc_send_root_listener_get_input();
			} else {
				hakios_tl("slpi: fastrpc: root listener input exceeds diagnostic buffer\n");
			}
		}
		break;
	case 0x7000:
	case 0x9000:
		slpi_fastrpc_invalidate_root_page();
		if (slpi_fastrpc_root_listener_input_output != NULL && slpi_fastrpc_root_listener_input != NULL) {
			uint32_t length = slpi_fastrpc_root_listener_input_output[0];
			uint32_t shown = MIN(length, SLPI_FASTRPC_LISTENER_LOG_CAPACITY);

			if (length > SLPI_FASTRPC_LISTENER_INPUT_CAPACITY) {
				hakios_tl("slpi: fastrpc: root listener returned oversized input %u\n", length);
				break;
			}

			hakios_tl("slpi: fastrpc: root listener input: %u bytes (ctx %#x, handle %#x, sc %#x)\n",
			    length, slpi_fastrpc_root_listener_invoke_context, slpi_fastrpc_root_listener_invoke_handle,
			    slpi_fastrpc_root_listener_invoke_scalars);
			for (uint32_t offset = 0; offset < shown; offset += 16) {
				char line[16 * 3 + 1];
				uint32_t here = MIN(16u, shown - offset), at = 0;

				for (uint32_t i = 0; i < here; i++) {
					at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x ",
					    slpi_fastrpc_root_listener_input[offset + i]);
				}
				line[MIN(at, (uint32_t)sizeof(line) - 1)] = 0;
				hakios_tl("slpi: fastrpc: root listener input +%u: %s\n", offset, line);
			}
			if (context == 0x7000 && !slpi_fastrpc_root_apps_std_opened && length == 41 &&
			    slpi_fastrpc_root_listener_invoke_handle == 0 &&
			    slpi_fastrpc_root_listener_invoke_scalars == SLPI_FASTRPC_SCALARS(0, 2, 2) &&
			    memcmp(slpi_fastrpc_root_listener_input + 24, "apps_std", 9) == 0) {
				(void)slpi_fastrpc_send_root_listener_open_reply();
			} else if (slpi_fastrpc_root_apps_std_opened &&
			    slpi_fastrpc_root_listener_invoke_handle == SLPI_FASTRPC_APPS_STD_HANDLE &&
			    slpi_fastrpc_root_listener_invoke_scalars ==
			    SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_APPS_STD_FOPEN_WITH_ENV, 5, 1)) {
				/* Deny every root-PD file path for now.  The raw, bounded input
				 * above is diagnostic only; no DSP path is ever opened on APSS. */
				(void)slpi_fastrpc_send_root_listener_open_failure();
			} else {
				hakios_tl("slpi: fastrpc: root listener callback not serviced (diagnostic only)\n");
			}
		}
		break;
	case 0x1002:
		(void)slpi_fastrpc_send_listener_init();
		break;
	case 0x2012:
		(void)slpi_fastrpc_send_listener_open();
		break;
	case 0x3012:
		slpi_fastrpc_invalidate_page();
		if (slpi_fastrpc_listener_open_output == NULL || slpi_fastrpc_listener_open_output[1] != 0 ||
		    slpi_fastrpc_listener_open_output[0] == 0) {
			hakios_tl("slpi: fastrpc: listener open failed (handle %#x, loader %d)\n",
			    slpi_fastrpc_listener_open_output == NULL ? 0 : slpi_fastrpc_listener_open_output[0],
			    slpi_fastrpc_listener_open_output == NULL ? -1 : (int32_t)slpi_fastrpc_listener_open_output[1]);
			slpi_fastrpc_phase = SLPI_FASTRPC_IDLE;
			break;
		}
		slpi_fastrpc_listener_module = slpi_fastrpc_listener_open_output[0];
		/* Android starts next2 on its listener thread immediately before the
		 * main thread registers the dynamic default-listener module. */
		if (slpi_fastrpc_send_listener_next()) {
			(void)slpi_fastrpc_send_listener_register();
		}
		break;
	case 0x4012:
		hakios_tl("slpi: fastrpc: listener registered (module %#x)\n", slpi_fastrpc_listener_module);
		slpi_fastrpc_phase = SLPI_FASTRPC_LISTENER_NEXT_PENDING;
		break;
	/* 0x5002 (next2 reply) and 0x6002 (get_input reply) are reused for
	 * every iteration of the listener loop: next2's reply always carries
	 * the newly announced callback, and get_input's reply always carries
	 * that callback's serialized bytes, regardless of which whitelisted
	 * request (or denial) came before.  Only one request is outstanding
	 * at a time, so reusing these two tags cannot confuse replies. */
	case 0x5002:
		slpi_fastrpc_invalidate_page();
		if (slpi_fastrpc_listener_next_output != NULL) {
			hakios_tl("slpi: fastrpc: next2 #%u output: invoke ctx %#x, handle %#x, sc %#x, input %u\n",
			    slpi_fastrpc_rx_sequence, slpi_fastrpc_listener_next_output[0],
			    slpi_fastrpc_listener_next_output[1], slpi_fastrpc_listener_next_output[2],
			    slpi_fastrpc_listener_next_output[3]);
			slpi_fastrpc_listener_invoke_context = slpi_fastrpc_listener_next_output[0];
			slpi_fastrpc_listener_invoke_handle = slpi_fastrpc_listener_next_output[1];
			slpi_fastrpc_listener_invoke_scalars = slpi_fastrpc_listener_next_output[2];
			hakios_tl("slpi: fastrpc: listener callback (ctx %#x, handle %#x, sc %#x, input %u bytes)\n",
			    slpi_fastrpc_listener_invoke_context, slpi_fastrpc_listener_invoke_handle,
			    slpi_fastrpc_listener_invoke_scalars, slpi_fastrpc_listener_next_output[3]);
			(void)slpi_fastrpc_send_listener_get_input(0x6002);
		}
		break;
	case 0x6002:
		slpi_fastrpc_invalidate_page();
		(void)slpi_fastrpc_dispatch_listener_input();
		break;
	default:
		hakios_tl("slpi: fastrpc: unexpected reply ctx %#llx\n", context);
		break;
	}
}

static bool
slpi_fastrpc_send_attach(void)
{
	struct slpi_fastrpc_remote_arg *argument;
	struct slpi_fastrpc_invoke_buf *invoke;
	struct slpi_fastrpc_phy_page *page;
	struct slpi_fastrpc_message message = { 0 };
	uint8_t *shared;
	uint32_t *client;

	if (!slpi_fastrpc_prepare_memory()) {
		return false;
	}
	shared = (uint8_t *)(void *)slpi_fastrpc_memory.page;
	bzero(shared, SLPI_FASTRPC_PAGE_SIZE);
	argument = (struct slpi_fastrpc_remote_arg *)(void *)shared;
	invoke = (struct slpi_fastrpc_invoke_buf *)(void *)(argument + 1);
	page = (struct slpi_fastrpc_phy_page *)(void *)(invoke + 1);
	/* Android fastrpc_get_meta_size: 40-byte argument metadata + 16 FD + 64 CRC slots. */
	client = (uint32_t *)(void *)(shared + SLPI_FASTRPC_ALIGN128(
	    sizeof(*argument) + sizeof(*invoke) + sizeof(*page) +
	    sizeof(uint64_t) * SLPI_FASTRPC_MAX_FDLIST + sizeof(uint32_t) * SLPI_FASTRPC_MAX_CRCLIST));
	*client = SLPI_FASTRPC_CLIENT_TGID; /* Android's attach input is the plain client tgid. */
	/* FastRPC pairs the APSS virtual argument pointer with its DMA page list. */
	argument->pointer = (uint64_t)(uintptr_t)client;
	argument->length = sizeof(*client);
	invoke->number = 1;
	invoke->page_index = 0;
	page->address = SLPI_FASTRPC_REMOTE_ADDRESS;
	page->size = SLPI_FASTRPC_PAGE_SIZE;
	hakios_tl("slpi: fastrpc: attach metadata ready (client %#x, pointer %#llx, page %#llx/%#llx)\n",
	    *client, argument->pointer, page->address, page->size);
	slpi_fastrpc_flush_page();
	hakios_tl("slpi: fastrpc: attach page cache flushed\n");
	/* UMI's downstream driver forces pid to zero for this kernel attach invoke. */
	message.client_id = 0;
	/* Android attaches sensorspd from the main FastRPC session.  Only the
	 * reverse-RPC listener thread below uses session 1. */
	message.tid = SLPI_FASTRPC_CLIENT_TGID;
	message.context = 0x1000ull | SLPI_FASTRPC_SENSORS_PD;
	message.handle = SLPI_FASTRPC_INIT_HANDLE;
	message.scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_RMID_ATTACH, 1, 0);
	message.address = SLPI_FASTRPC_REMOTE_ADDRESS;
	message.size = SLPI_FASTRPC_PAGE_SIZE;
	hakios_tl("slpi: fastrpc: sending attach over GLINK\n");
	slpi_fastrpc_phase = SLPI_FASTRPC_ATTACH_PENDING;
	if (!slpi_glink_send_data(2, &message, sizeof(message))) {
		hakios_tl("slpi: fastrpc: cannot send sensors PD attach\n");
		slpi_fastrpc_phase = SLPI_FASTRPC_IDLE;
		return false;
	}
	hakios_tl("slpi: fastrpc: attach request sent (sensors PD, ctx %#llx, page %#llx)\n",
	    message.context, message.address);
	return true;
}

static bool
slpi_fastrpc_send_root_attach(void)
{
	struct slpi_fastrpc_remote_arg *argument;
	struct slpi_fastrpc_invoke_buf *invoke;
	struct slpi_fastrpc_phy_page *page;
	struct slpi_fastrpc_message message = { 0 };
	uint8_t *shared = (uint8_t *)(void *)(slpi_fastrpc_memory.page + SLPI_FASTRPC_PAGE_SIZE);
	uint32_t *client;

	if (!slpi_fastrpc_memory.ready) {
		return false;
	}
	slpi_fastrpc_root_denials = 0;
	slpi_fastrpc_root_apps_std_opened = false;
	bzero(shared, SLPI_FASTRPC_PAGE_SIZE);
	argument = (struct slpi_fastrpc_remote_arg *)(void *)shared;
	invoke = (struct slpi_fastrpc_invoke_buf *)(void *)(argument + 1);
	page = (struct slpi_fastrpc_phy_page *)(void *)(invoke + 1);
	client = (uint32_t *)(void *)(shared + SLPI_FASTRPC_ALIGN128(
	    sizeof(*argument) + sizeof(*invoke) + sizeof(*page) +
	    sizeof(uint64_t) * SLPI_FASTRPC_MAX_FDLIST + sizeof(uint32_t) * SLPI_FASTRPC_MAX_CRCLIST));
	*client = SLPI_FASTRPC_ROOT_TGID;
	argument->pointer = (uint64_t)(uintptr_t)client;
	argument->length = sizeof(*client);
	invoke->number = 1;
	invoke->page_index = 0;
	page->address = SLPI_FASTRPC_ROOT_REMOTE_ADDRESS;
	page->size = SLPI_FASTRPC_PAGE_SIZE;
	for (vm_offset_t line = (vm_offset_t)shared;
	    line < (vm_offset_t)shared + SLPI_FASTRPC_PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	message.client_id = 0;
	message.tid = SLPI_FASTRPC_ROOT_TGID;
	message.context = 0x2000;
	message.handle = SLPI_FASTRPC_INIT_HANDLE;
	message.scalars = SLPI_FASTRPC_SCALARS(SLPI_FASTRPC_RMID_ATTACH, 1, 0);
	message.address = SLPI_FASTRPC_ROOT_REMOTE_ADDRESS;
	message.size = SLPI_FASTRPC_PAGE_SIZE;
	if (!slpi_glink_send_data(2, &message, sizeof(message))) {
		hakios_tl("slpi: fastrpc: cannot send root PD attach\n");
		return false;
	}
	hakios_tl("slpi: fastrpc: root PD attach request sent (ctx %#llx, page %#llx)\n",
	    message.context, message.address);
	return true;
}

static void
slpi_thread(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t page = 0;
	volatile uint8_t *memory;

	if (!slpi_image_load()) {
		goto out;
	}
	hakios_tl("slpi: hand the header to TrustZone (image %u)\n", SLPI_PAS);
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		hakios_tl("slpi: no page\n");
		goto out;
	}
	memcpy((void *)page, slpi_image.header, slpi_image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	if (slpi_scm("to take the image's header", SLPI_SCM_INIT_IMAGE, SLPI_SCM_VALUE_AND_BUF, SLPI_PAS,
	    kvtophys(page), 0) != 0) {
		hakios_tl("slpi: TRUSTZONE DOES NOT TAKE THE HEADER\n");
		goto out;
	}
	hakios_tl("slpi: TRUSTZONE ACCEPTS THE IMAGE'S SIGNATURE\n");
	if (slpi_scm("where the image will be", SLPI_SCM_MEM_SETUP, SLPI_SCM_THREE_VALUES, SLPI_PAS, SLPI_MEMORY,
	    slpi_image.size) != 0) {
		hakios_tl("slpi: TRUSTZONE DOES NOT TAKE THE PLACE\n");
		goto out;
	}
	memory = (volatile uint8_t *)ml_io_map_unmappable(SLPI_MEMORY, SLPI_SIZE, 0x0080 /* VM_WIMG_WCOMB */);
	if (memory == NULL) {
		hakios_tl("slpi: its memory cannot be mapped\n");
		goto out;
	}
	for (unsigned s = 0; s < slpi_image.segments; s++) {
		uint32_t at = slpi_image.segment[s].at;

		for (uint32_t i = 0; i < slpi_image.segment[s].memory_size; i++) {
			memory[at + i] = i < slpi_image.segment[s].file_size ? slpi_image.segment[s].file[i] : 0;
		}
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	hakios_tl("slpi: the image is in its memory (%#x bytes at %#x)\n", slpi_image.size, SLPI_MEMORY);
	if (slpi_scm("to authenticate the image and start it", SLPI_SCM_AUTH_RESET, SLPI_SCM_ONE_VALUE, SLPI_PAS, 0,
	    0) != 0) {
		hakios_tl("slpi: TRUSTZONE DOES NOT START IT\n");
		goto out;
	}
	hakios_tl("slpi: THE SENSOR DSP IS STARTED (TrustZone took the image at %#x, %#x bytes)\n", SLPI_MEMORY,
	    slpi_image.size);
	slpi_smem_probe();
	if (slpi_transport_wait_ready(SLPI_QUEUE_TIMEOUT_MS)) {
		if (slpi_glink_open_ipcrtr()) {
			(void)slpi_glink_open_fastrpc_probe();
			if (slpi_fastrpc_prepare_memory()) {
				uint32_t on = 0;

				if (PE_parse_boot_argn("slpi_attach", &on, sizeof(on)) && on != 0) {
					hakios_tl("slpi: fastrpc: attach enabled\n");
					(void)slpi_fastrpc_send_attach();
				} else {
					hakios_tl("slpi: fastrpc: attach disabled (use slpi_attach=1)\n");
				}
			}
			slpi_sns_qmi_probe();
			/* Both service announcements and the SUID result are asynchronous. */
			hakios_qmi_poll(&slpi_sns_qmi, 30000);
			if (slpi_fastrpc_memory.ready) {
				unsigned busy = 0;

				for (unsigned i = 0; i < slpi_glink.remote_intents; i++) {
					if (slpi_glink.remote_intent[i].channel == 2 && slpi_glink.fastrpc_remote_busy[i]) {
						busy++;
					}
				}
				hakios_tl("slpi: fastrpc: probe snapshot phase %u, GLINK broken %u, TX busy %u, SMMU FSR %#x\n",
				    slpi_fastrpc_phase, slpi_glink.broken, busy,
				    slpi_smmu_read(slpi_fastrpc_memory.context, SLPI_SMMU_CB_FSR));
			}
			if (slpi_sns_suid_sent) {
				hakios_tl("slpi: sns: resampler lookup %s (QMI %d, result %u/%u)\n",
				    slpi_sns_suid_txn.done ? "replied" : "awaiting reply", slpi_sns_suid_txn.result,
				    slpi_sns_suid_response.result, slpi_sns_suid_response.error);
				if (!slpi_sns_suid_txn.done) {
					qmi_txn_cancel(&slpi_sns_suid_txn);
				}
			}
			if (slpi_sns_mag_sent) {
				hakios_tl("slpi: sns: mag lookup %s (QMI %d, result %u/%u)\n",
				    slpi_sns_mag_txn.done ? "replied" : "awaiting reply", slpi_sns_mag_txn.result,
				    slpi_sns_mag_response.result, slpi_sns_mag_response.error);
				if (!slpi_sns_mag_txn.done) {
					qmi_txn_cancel(&slpi_sns_mag_txn);
				}
			}
		}
	}
out:
	if (page != 0) {
		kmem_free(kernel_map, page, PAGE_SIZE);
	}
	bzero(&slpi_image, sizeof(slpi_image));
	thread_terminate(current_thread());
}

void
umi_slpi_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("slpi", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(slpi_thread, NULL, &thread) != KERN_SUCCESS) {
		hakios_tl("slpi: no thread for the probe\n");
		return;
	}
	thread_deallocate(thread);
}
