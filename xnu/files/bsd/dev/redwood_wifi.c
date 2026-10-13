/*
 * The Wi-Fi of the POCO X5 Pro (redwood), from nothing and step by step. Every address below
 * belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The radio is a WCN6750. Its firmware does not run on the application processor but on a
 * processor of its own inside the SoC, the "WPSS" (a Hexagon DSP), which stays in reset until
 * somebody loads it: the application processor puts the signed image -- the phone's own
 * wpss.mdt and wpss.b02 to wpss.b07, from /vendor/firmware_mnt/image of its Android -- into the
 * memory reserved for it and asks TrustZone to authenticate and start it, as for the GPU's zap
 * shader (redwood_adreno.c). Everything after that is conversation with the running firmware.
 * Its first words go through what this file begins with:
 *
 *   SMEM    two megabytes of memory that every processor of the SoC can reach, laid out by the
 *           boot loader, in which each of them allocates numbered items
 *   SMP2P   a pair of such items between two processors, with named entries of 32 bits: in
 *           the DSP's are "I am up", "I have crashed", "I have stopped"
 *   IPCC    the SoC's doorbells: a register to ring another processor's with, and one that
 *           says who rang ours
 * and through one thing that only the loading needs: the mailbox of the always-on processor
 * (the "AOP", which runs RPMh, the keeper of the supply rails), which is told that the image is
 * being loaded before it honours the DSP's own requests for power.
 *
 * Boot-arg wifi=N performs steps 1 to N and stops; a step says what it is about to touch before
 * it touches it and what it found after, reads come before writes, and a step that does not
 * find what it needs ends the sequence instead of going on blind.
 *
 *   1  reads   the shared memory area: how the boot loader laid it out, and whether the DSP
 *              has left anything in it (it has if it was started since the phone was)
 *   2  reads   the always-on processor's mailbox and the doorbells, as they are
 *   3  reads   RPMh: where it has the supplies involved, and what the application processor's
 *              sequencer holds in its command slots (what was last asked for from here)
 *   4  reads   the supplies themselves, from the two PMICs that make the radio's three
 *              (PM7325 S7, S1, S8) and the two the DSP runs from (CX and MX: PM8350C S2 and
 *              S10). The only thing written is the command word that asks the PMIC arbiter for
 *              a read
 *   5  reads   the radio's pins
 *   6  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   7  writes  the always-on processor's mailbox open: five words of its message memory, and
 *              its doorbell
 *   8  asks    TrustZone to take the image's header and check its signature (image 6), and
 *              tells it where the image will be
 *   9  writes  the image into the DSP's memory, and reads it back
 *  10  writes  "the image is loaded" to the always-on processor, asks TrustZone to authenticate
 *              the image and let the DSP out of reset, and watches the shared memory and our
 *              doorbell for the DSP's first words
 * Steps 1 to 5 are the survey; they write to nothing but the arbiter's command word.
 *
 * What is started is stopped: when the steps are over, and wherever a run ends in order while
 * the DSP runs, TrustZone is asked to shut the DSP down and the always-on processor is told
 * that the image is gone. A run that ends otherwise leaves the DSP running, and the kernel
 * after it cannot ask the hardware whether that is so. But the shared memory outlives our
 * kernels (the phone is not restarted between them), and the DSP's own item in it is there if
 * and only if the DSP has run since the phone was started: when step 8 finds it, TrustZone is
 * first asked for the shutdown. On a phone just started nothing is asked that is not needed.
 *
 * AND THERE IT ENDS, FOR NOW: THE DSP STARTS ONCE BETWEEN TWO STARTS OF THE PHONE. Twice a
 * second start was tried, after a shutdown that TrustZone had answered with success, with the
 * same image written and read back again: both times the phone reset without a word at the
 * request to authenticate and start it, or within two seconds of it. Why is not known. What
 * Linux does at that point and this does not: it holds CX and MX at level 384 and the crystal
 * while the DSP starts and again around the shutdown, and before the shutdown it asks the DSP
 * to stop and waits for its word. So step 8, having shut down a DSP from before, goes no
 * further unless told to (wifi_again=1).
 *
 * Nothing here switches a supply rail or changes a pin.
 *
 * More boot-args:
 *   wifi_new=K         steps K and later have not been seen to work (default: the last one)
 *   wifi_pause=MS      how long such a step waits after announcing a group of accesses
 *                      (default 2200: the loader, watching from another core, notes the end of
 *                      our log every two seconds, and after a silent reset that is all there is)
 *   wifi_fine=1        such a step waits like that before every single write and call
 *   wifi_wait=S        how long step 10 watches for the DSP to say that it is up (default 10)
 *   wifi_watch=S       ... and how long it goes on watching once it has (default 3)
 *   wifi_dirty=1       the DSP is not stopped: left as a kernel that died leaves it
 *   wifi_again=1       start the DSP although it has been started since the phone was: SO FAR
 *                      THAT HAS RESET THE PHONE
 *   wifi_return=1      when all is over, go back to the loader instead of starting the system
 *   wifi_smmu=1        before any step: read what the boot chain has set up in the SMMU for the
 *                      radio's stream (smmu_look() below; nothing is written)
 * The system does not go on starting until all of this is over, so that the end of the log is
 * ours.
 *
 * No interrupt is taken: what the DSP and the always-on processor say is looked for. What an
 * interrupt would take is said at the end of this file.
 *
 * Addresses are from the phone's own device tree ("dt:" below: qcom,wpss@8a00000,
 * qcom,smp2p-wpss, qcom,glink/wpss, qcom,ipcc@408000, power-controller@c300000, rsc@18200000,
 * qcom,wcn6750, reserved-memory), layouts from Linux, where SC7280 is this SoC:
 * drivers/soc/qcom/{smem,smp2p,qcom_aoss,rpmh-rsc,cmd-db,mdt_loader}.c,
 * drivers/mailbox/qcom-ipcc.c, drivers/remoteproc/qcom_q6v5{,_pas}.c,
 * drivers/firmware/qcom_scm{,-smc}.c, drivers/spmi/spmi-pmic-arb.c,
 * drivers/regulator/qcom_spmi-regulator.c, drivers/pinctrl/qcom/pinctrl-msm.c.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <miscfs/packfs/packfs.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern void loader_return(void);

static uint32_t last_step, pause_ms = 2200, fine, watch_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";

/* As in redwood_adreno.c: every line goes to the log the loader reads from another core as well. */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* ---- register access ------------------------------------------------------------------------------- */

/* One access is one plain load or store (see redwood_adreno.c: the hypervisor carries some of them out). */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

/* A stretch of registers; mapped when a step first needs it. */
struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(bool single_access)
{
	if (step >= new_from && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

/* What a step is about to do, in a line. */
static void
announce(const char *what)
{
	printf("wifi: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step >= new_from && fine) {
		printf("wifi: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("wifi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("wifi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	hold(true);
	wr(b->base + offset, value);
}

/* ---- step 1: the shared memory area ------------------------------------------------------------------- */

/*
 * dt: reserved-memory smem@80900000 (qcom,smem). At its end, in the last 4K, the boot loader's
 * table of partitions: one per pair of processors that share items privately, and one, the
 * "global" one, for the rest. A partition begins with a header; items are allocated from both
 * of its ends, each behind (from the top: in front of) an entry that says which item it is and
 * how long. Who is who: we are processor 0; the Wi-Fi DSP is 13 (dt: qcom,remote-pid of
 * qcom,smp2p-wpss). Nothing here writes to this memory.
 */
#define SMEM_PHYS               0x80900000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_WPSS               13
#define SMEM_GLOBAL             0xfffe          /* both "hosts" of the global partition */
#define SMEM_VERSION            (0x40 + 4 * 7)  /* of the old header at the start: the boot loader's word, [31:16] */
#define SMEM_VERSION_PARTITIONS 12              /* ... which is this where everything is in partitions */
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];                       /* "$TOC" */
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];                       /* "$PRT" */
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;                          /* with the padding */
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
/* An item found: where its data is in the area, and how long. */
struct smem_item {
	uint32_t item, offset, size;
};

/* The DSP's items (dt: qcom,smem of qcom,smp2p-wpss, qcom,smem-id of qcom,wpss@8a00000). */
#define ITEM_SMP2P_OURS         616             /* SMP2P from us to the DSP: ours to allocate, when there is something to say */
#define ITEM_SMP2P_THEIRS       617             /* ... and from the DSP to us */
#define ITEM_CRASH_REASON       626             /* a line of text the DSP leaves when it gives up */

static const volatile uint8_t *smem;

/* A piece of the area, copied: other processors write there while we look. */
static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		/* memory, not registers; and from the kernel's own map: where ml_io_map() puts things there are 8 MB in all */
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

/* The partition of two processors: where it begins in the area, and how its entries are aligned. 0: there is none. */
static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

/* The items of a partition, up to `max` of them; how many there are. */
static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	/* from the bottom: an entry, then its item */
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	/* from the top: an entry, with its item in front of it; entries are aligned as the table says */
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_WPSS }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

/* An SMP2P item: who it is from and to, and up to 16 named words. */
struct smp2p {
	uint32_t magic;                         /* "$SMP" */
	uint32_t version;                       /* [7:0]; [31:8] features: bit 0, "restarts are acknowledged" */
	uint16_t local, remote, total, valid;
	uint32_t flags;                         /* bit 0 "restart done", bit 1 "restart acknowledged" */
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,wpss@8a00000). */
#define DSP_FATAL               (1u << 0)       /* "err-fatal": it has given up */
#define DSP_READY               (1u << 1)       /* "err-ready": it is up, and would say so if it gave up */
#define DSP_HANDOVER            (1u << 2)       /* "proxy-unvote": it has asked for its own power; ours is no longer needed */
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* The DSP's SMP2P item as it is now, if there is one. */
static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;                     /* (an item, once allocated, stays where it is) */
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

/* The word of the DSP's entry "slave-kernel". */
static uint32_t
dsp_state(const struct smp2p *words)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, "slave-kernel", sizeof(words->entry[i].name)) == 0) {
			return words->entry[i].value;
		}
	}
	return 0;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("wifi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

/*
 * The DSP's last words, if it has left any: its firmware makes the item when it starts, empty,
 * and writes a line into it when it gives up.
 */
static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("wifi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("wifi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("wifi: %u: THE DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* A partition, in two or more lines. */
static void
show_partition(const char *name, uint16_t one, uint16_t other)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(one, other, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("wifi: %u: there is no partition for %s\n", step, name);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("wifi: %u: the partition for %s: at %#lx, %#x bytes, of which %#x free; %u items\n", step, name, SMEM_PHYS + partition,
	    size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("wifi: %u:   items (bytes):%s\n", step, line);
	}
}

static bool
step_smem(void)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smp2p words;
	uint32_t version = 0, size = 0, at;

	announce("map the shared memory area (0x80900000, 2 MB: memory, not registers) and read how the boot loader laid it out");
	if (!smem_peek(SMEM_VERSION, &version, sizeof(version)) || !smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table))) {
		return false;
	}
	printf("wifi: %u: the boot loader's version word is %#x; the table of partitions begins %02x %02x %02x %02x, version %u, "
	    "%u entries\n", step, version, table.magic[0], table.magic[1], table.magic[2], table.magic[3], table.version, table.count);
	if (version >> 16 != SMEM_VERSION_PARTITIONS || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 || table.count > 64) {
		printf("wifi: %u: that is not the layout this knows (version 12, everything in partitions)\n", step);
		return false;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			printf("wifi: %u: partition %u: processors %#x and %#x, at %#lx, %#x bytes, flags %#x, alignment %u\n", step, i,
			    entry.host0, entry.host1, SMEM_PHYS + entry.offset, entry.size, entry.flags, entry.cacheline);
		}
	}
	show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	show_partition("everybody (the global one)", SMEM_GLOBAL, SMEM_GLOBAL);

	at = smem_item(ITEM_SMP2P_OURS, &size);
	printf("wifi: %u: item %u, SMP2P from us to the DSP: %s\n", step, ITEM_SMP2P_OURS, at ? "there" : "not there");
	show_crash_reason();
	if (dsp_words(&words)) {
		show_words("item 617, SMP2P from the DSP to us, IS THERE (the DSP has run since the phone was started)", &words);
	} else {
		printf("wifi: %u: item %u, SMP2P from the DSP to us: not there (the DSP has not run since the phone was started)\n", step,
		    ITEM_SMP2P_THEIRS);
	}
	return true;
}

/* ---- step 2: the always-on processor's mailbox, and the doorbells -------------------------------------- */

/*
 * dt: power-controller@c300000 (qcom,sm8350-aoss-qmp; "qmp" is the name of the protocol): 1K
 * of message memory that takes 32-bit accesses only. It begins with a description of the two
 * directions -- "ucore" is the always-on processor, "mcore" is us -- each with a link state, a
 * channel state, the other side's acknowledgement of both, and where and how big its mailbox
 * is. A message is text; it goes into our mailbox behind its length, the doorbell is rung, and
 * the always-on processor has taken it when the length reads zero.
 */
static struct block aoss = { "mailbox", 0x0c300000, 0x400 };
#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c      /* "LIAM" */
#define QMP_VERSION             0x04
#define QMP_FEATURES            0x08
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10            /* ours to write: we have seen their link state */
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18            /* ours to write */
#define QMP_THEIR_BOX_SIZE      0x1c
#define QMP_THEIR_BOX           0x20
#define QMP_OUR_LINK            0x24            /* ours to write */
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c            /* ours to write */
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64              /* what Linux sends, whatever the text's length */

/*
 * dt: qcom,ipcc@408000 (its interrupt: SPI 229). A doorbell is a processor ("client") and one
 * of its signals; clients: the always-on processor 0, the Wi-Fi DSP 24; signals: 0 the mailbox
 * or GLINK, 2 SMP2P (dt: the mboxes of power-controller@c300000, qcom,smp2p-wpss, glink wpss).
 */
static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };
#define IPCC_SEND               0x0c            /* write client << 16 | signal: ring it */
#define IPCC_RECEIVED           0x10            /* who rang us, the same way; all ones: nobody */
#define IPCC_ENABLE             0x14            /* write: hear that client's signal */
#define IPCC_CLEAR              0x1c            /* write: that one has been heard */
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_WPSS               24u

static bool
step_mailbox(void)
{
	uint32_t magic, box, size;

	announce("read the description at the start of the always-on processor's message memory (0xc300000)");
	magic = get(&aoss, "magic", QMP_MAGIC);
	get(&aoss, "version", QMP_VERSION);
	get(&aoss, "features", QMP_FEATURES);
	get(&aoss, "their link", QMP_THEIR_LINK);
	get(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK);
	get(&aoss, "their channel", QMP_THEIR_CHANNEL);
	get(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK);
	get(&aoss, "their mailbox's size", QMP_THEIR_BOX_SIZE);
	get(&aoss, "their mailbox", QMP_THEIR_BOX);
	get(&aoss, "our link", QMP_OUR_LINK);
	get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK);
	get(&aoss, "our channel", QMP_OUR_CHANNEL);
	get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	if (magic != QMP_MAGIC_VALUE) {
		printf("wifi: %u: that is not the mailbox's description\n", step);
		return false;
	}
	if (size >= QMP_MESSAGE + 4 && box >= 0x3c && box + size <= aoss.size) {
		get(&aoss, "the length of the message in our mailbox", box);
	}

	announce("read who has rung our doorbell (IPCC, 0x408000: the first access to that block)");
	get(&ipcc, "received", IPCC_RECEIVED);
	return true;
}

/* ---- step 3: RPMh -------------------------------------------------------------------------------------- */

/* dt: reserved-memory cmd_db@80860000: the boot loader's list of what RPMh can switch (cmd-db.c; as in redwood_adreno.c) */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static bool
cmd_db_there(void)
{
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);       /* memory, not registers */
	}
	return cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c;
}

/* The resource of that name, or (name NULL) the one an address belongs to: the nearest at or below it. */
static const struct cmd_db_entry *
rpmh_resource(const char *name, uint32_t address)
{
	const struct cmd_db_entry *best = NULL;
	char id[8] = { 0 };

	if (!cmd_db_there()) {
		return NULL;
	}
	if (name != NULL) {
		strlcpy(id, name, sizeof(id));
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (name != NULL ? memcmp(entry[i].id, id, sizeof(id)) == 0 :
			    entry[i].address <= address && address - entry[i].address < 0x10 &&
			    (best == NULL || entry[i].address > best->address)) {
				best = &entry[i];
				if (name != NULL) {
					return best;
				}
			}
		}
	}
	return best;
}

static void
show_rpmh_resource(const char *name, const char *what)
{
	const struct cmd_db_entry *entry = rpmh_resource(name, 0);
	const uint8_t *data;
	char levels[160];
	int at = 0;

	if (entry == NULL) {
		printf("wifi: %u: RPMh has no \"%s\" (%s)\n", step, name, what);
		return;
	}
	levels[0] = 0;
	if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".lvl") == 0) {
		/* a rail asked for by level: the levels it has, which a request names by position */
		for (unsigned s = 0; s < 8; s++) {
			const struct cmd_db_slave *slave = &cmd_db->slave[s];
			const struct cmd_db_entry *first = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

			if (entry < first || entry >= first + slave->count ||
			    (unsigned)slave->data_offset + entry->offset + entry->length > CMD_DB_SIZE - 0x1000) {
				continue;
			}
			data = cmd_db->data + slave->data_offset + entry->offset;
			at = snprintf(levels, sizeof(levels), "; levels:");
			for (unsigned l = 0; l < entry->length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
				unsigned level = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && level == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
			}
		}
	}
	printf("wifi: %u: RPMh has \"%s\" at %#x (%s)%s\n", step, name, entry->address, what, levels);
}

/*
 * dt: rsc@18200000 (qcom,rpmh-rsc), the application processor's sequencer for requests to RPMh;
 * of its three sets of registers ("DRV") the operating system's is the third (qcom,drv-id 2).
 * Requests wait in "TCS"s of up to 16 commands, a command being an address of RPMh's and a
 * value: by the device tree two TCSs for requests made at once, then three for what is to
 * hold while the processor sleeps and three for when it wakes. What the slots hold now is what
 * was last put there by whoever ran here before us: the boot loader.
 */
static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_ID                  0x000           /* [23:16] the major version: the layout below is that of versions before 3 */
#define RSC_CONFIG              0x00c           /* per DRV six bits, the number of its TCSs; [31:27] commands in a TCS */
#define RSC_TCS(n)              (0xd00 + 672 * (n))             /* dt: qcom,tcs-offset */
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c            /* a bit per command */
#define TCS_COMMAND(c)          (0x30 + 20 * (c))               /* its flags; then its address, its value, its status */

static bool
step_rpmh(void)
{
	static const char *const kinds[] = { "at once", "at once", "asleep", "asleep", "asleep", "awake", "awake", "awake" };
	uint32_t id, config, count, commands;

	announce("look up the supplies involved in the boot loader's list of RPMh resources (memory at 0x80860000)");
	if (!cmd_db_there()) {
		printf("wifi: %u: the list is not there\n", step);
		return false;
	}
	show_rpmh_resource("cx.lvl", "the DSP's logic; dt: vdd_cx of qcom,wpss@8a00000, asked 384 while it boots");
	show_rpmh_resource("mx.lvl", "its memories; dt: vdd_mx, the same");
	show_rpmh_resource("xo.lvl", "the crystal oscillator; dt: its clock \"xo\"");
	show_rpmh_resource("smpb7", "PM7325 S7: the radio's vdd-cx-mx");
	show_rpmh_resource("smpb1", "PM7325 S1: the radio's vdd-1.8-xo");
	show_rpmh_resource("smpb8", "PM7325 S8: the radio's vdd-1.3-rfa");

	announce("read the application processor's RPMh sequencer (0x18220000: the first access to that block): its version, "
	    "and the commands its slots hold");
	id = get(&rsc, "id", RSC_ID);
	config = get(&rsc, "configuration", RSC_CONFIG);
	if (((id >> 16) & 0xff) >= 3) {
		printf("wifi: %u: version %u of the sequencer has its registers elsewhere: not read\n", step, (id >> 16) & 0xff);
		return true;
	}
	count = (config >> 12) & 0x3f;
	commands = (config >> 27) & 0x1f;
	printf("wifi: %u: the operating system's part has %u TCSs of %u commands\n", step, count, commands);
	for (uint32_t t = 0; t < count && t < 8; t++) {
		uint32_t enabled = rd(rsc.base + RSC_TCS(t) + TCS_ENABLED);

		printf("wifi: %u: TCS %u (%s): commands enabled %#x, control %#x, status %#x\n", step, t, kinds[t], enabled,
		    rd(rsc.base + RSC_TCS(t) + TCS_CONTROL), rd(rsc.base + RSC_TCS(t) + TCS_STATUS));
		for (uint32_t c = 0; c < commands && c < 16; c++) {
			uint32_t flags = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c));
			uint32_t address = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 4);
			uint32_t value = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 8);
			const struct cmd_db_entry *entry = rpmh_resource(NULL, address);

			if (address == 0 && !(enabled & 1u << c)) {
				continue;
			}
			printf("wifi: %u:   command %u%s: %#x (%.8s + %u) = %#x, flags %#x\n", step, c, enabled & 1u << c ? "" :
			    " (not enabled)", address, entry ? entry->id : "?", entry ? address - entry->address : 0, value, flags);
		}
	}
	return true;
}

/* ---- step 4: the supplies themselves ------------------------------------------------------------------- */

/*
 * The PMIC arbiter, as in redwood_buttons.c: reads go through the "observer" channels, which
 * every system has for every peripheral, whoever owns it. dt: qcom,spmi@c440000; on the bus the
 * PM7325 is slave 1 and the PM8350C slave 2.
 */
static struct block arb_core = { "arbiter", 0x0c440000, 0x1100 };
static struct block arb_cnfg = { "arbiter's owners", 0x0c40a000, 0x1000 };
static struct block arb_observer = { "arbiter's observer", 0x0e600000, 0x10000 };
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what follows is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */

static uint32_t arb_channels;

/* The channel of a peripheral (slave << 8 | peripheral), as Linux chooses: the first, unless a later one is ours. -1: none. */
static int
spmi_channel(uint16_t id)
{
	int found = -1;

	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core.base + ARB_CHANNEL(n));

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		if (found < 0 || (rd(arb_cnfg.base + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
			found = (int)n;
		}
	}
	return found;
}

/* Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what went wrong. */
static const char *
spmi_read(int channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t at = arb_observer.base + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)channel;
	uint64_t start = now_us();
	uint32_t status, data[2];

	wr(at + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(at + OBSERVER_STATUS);
	} while (!(status & STATUS_DONE) && now_us() - start < 2000);
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	}
	if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	}
	if (status & STATUS_FAILURE) {
		return "the transfer failed (nobody at that address?)";
	}
	if (status & STATUS_DROPPED) {
		return "the transfer was dropped";
	}
	data[0] = rd(at + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(at + OBSERVER_DATA1) : 0;
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/*
 * Where the PMICs have their regulators is not in the device tree (on this phone they are
 * RPMh's to switch, and the tree knows them by RPMh's names only), so the arbiter's table is
 * asked which peripherals each PMIC has, and each peripheral what it is (registers 0x04 and
 * 0x05, its type and subtype). Of a regulator's control (qcom_spmi-regulator.c, the kinds
 * "ftsmps426" and "hfs430" that PMICs of this generation continue): 0x08 its status (bit 7: its
 * output is in regulation), 0x40/0x41 the voltage asked for in millivolts, 0x42/0x43 the voltage
 * it makes, 0x45 its mode (the low three bits: 4 retention, 5 low power, 6 automatic, 7 full
 * power), 0x46 bit 7: enabled. Whether a rail that is on was left on by the boot loader for good
 * or is held by somebody's request to RPMh, these registers do not say.
 */
#define TYPE_BUCK               0x03            /* a switching regulator's control */
#define TYPE_FTS                0x1c            /* ... of the fast kind */
#define TYPE_ULT_BUCK           0x22

static bool
step_rails(void)
{
	uint32_t version;

	announce("map the PMIC arbiter (0xc440000, 0xc40a000, 0xe600000; the buttons' driver reads through it too) and read "
	    "its version");
	version = get(&arb_core, "version", ARB_VERSION);
	arb_channels = get(&arb_core, "features", ARB_FEATURES) & 0x7ff;
	need(&arb_cnfg);
	need(&arb_observer);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("wifi: %u: this knows version 5 of the arbiter only: the supplies are not read\n", step);
		return true;
	}
	if (arb_channels == 0 || arb_channels > 512) {
		arb_channels = 512;
	}

	announce("ask every peripheral of the PM7325 (slave 1) and the PM8350C (slave 2) what it is, and the switching "
	    "regulators among them what they make (the first transfers of ours on the SPMI bus: for each a command word "
	    "written to the arbiter, nothing to a PMIC)");
	for (uint16_t slave = 1; slave <= 2; slave++) {
		static uint8_t types[256][2];
		static bool there[256];
		char line[320];
		int at = 0;

		bzero(there, sizeof(there));
		for (uint32_t n = 0; n < arb_channels; n++) {
			uint32_t map = rd(arb_core.base + ARB_CHANNEL(n)), peripheral = (map >> 8) & 0xff;

			if (map == 0 || ((map >> 16) & 0xf) != slave || there[peripheral]) {
				continue;
			}
			there[peripheral] = spmi_read((int)n, 0x04, types[peripheral], 2) == NULL;
		}
		line[0] = 0;
		for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
			if (there[peripheral]) {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x:%x/%x", peripheral, types[peripheral][0],
				    types[peripheral][1]);
			}
			if (at > 0 && (at > (int)sizeof(line) - 80 || peripheral == 255)) {
				printf("wifi: %u: slave %u (%s), peripheral:type/subtype:%s\n", step, slave, slave == 1 ? "PM7325" : "PM8350C",
				    line);
				at = 0;
			}
		}
		for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
			int channel;
			uint8_t status[2], control[8];
			const char *problem;

			if (!there[peripheral] || (types[peripheral][0] != TYPE_BUCK && types[peripheral][0] != TYPE_FTS &&
			    types[peripheral][0] != TYPE_ULT_BUCK)) {
				continue;
			}
			channel = spmi_channel((uint16_t)(slave << 8 | peripheral));
			problem = channel < 0 ? "no channel" : spmi_read(channel, 0x08, status, 2);
			if (problem == NULL) {
				problem = spmi_read(channel, 0x40, control, 8);
			}
			if (problem != NULL) {
				printf("wifi: %u: slave %u, the regulator at %#x: %s\n", step, slave, peripheral << 8, problem);
				continue;
			}
			printf("wifi: %u: slave %u, the regulator at %#x (type %#x/%#x): %s, status %#x %#x (%s); asked %u mV, makes %u "
			    "mV; mode %#x; registers 0x40..0x47: %02x %02x %02x %02x %02x %02x %02x %02x\n", step, slave, peripheral << 8,
			    types[peripheral][0], types[peripheral][1], control[6] & 0x80 ? "ENABLED" : "NOT ENABLED", status[0], status[1],
			    status[0] & 0x80 ? "in regulation" : "not in regulation", (unsigned)(control[0] | control[1] << 8),
			    (unsigned)(control[2] | control[3] << 8), control[5], control[0], control[1], control[2], control[3], control[4],
			    control[5], control[6], control[7]);
		}
	}
	return true;
}

/* ---- step 5: pins -------------------------------------------------------------------------------------- */

/*
 * dt: pinctrl@f000000 (pin n's registers: 0x1000 * n above 0xf100000). Pin 84 is described there
 * as the radio's enable ("cnss_wlan_en": output, high when active), but no node uses the
 * description; Bluetooth's node calls the same pin "wl-reset" and has 85 as its own reset and 86
 * as its "sw-ctrl".
 *
 * (The DSP's clocks -- three in the global clock controller have its name: gcc-sc7280.c,
 * gcc_wpss_ahb_clk at 0x9d154 and two more -- are not looked at: a read there is answered with
 * an external abort, which ends the kernel. They are TrustZone's; the device tree gives the
 * loader's node the crystal alone.)
 */
#define FIRST_PIN               84
static struct block tlmm = { "tlmm", 0x0f100000 + 0x1000 * FIRST_PIN, 0x3000 };        /* the three pins' registers */
#define GPIO_CFG(n)             (0x1000 * ((n) - FIRST_PIN))            /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (0x1000 * ((n) - FIRST_PIN) + 4)        /* bit 0 in, bit 1 out */

static bool
step_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { 84, "the radio's enable or reset" }, { 85, "Bluetooth's reset" }, { 86, "Bluetooth's sw-ctrl" } };

	announce("read how the firmware left the radio's pins (registers the touch screen's driver uses too)");
	need(&tlmm);
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(tlmm.base + GPIO_CFG(pins[i].pin)), io = rd(tlmm.base + GPIO_IO(pins[i].pin));

		printf("wifi: %u: gpio%u (%s): configuration %#x: function %u, pull %u, %u mA, %s; reads %u\n", step, pins[i].pin,
		    pins[i].what, cfg, (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & 1u << 9) ? (io & 2 ? "DRIVES HIGH" :
		    "DRIVES LOW") : "an input", io & 1);
	}
	return true;
}

/* ---- step 6: the firmware ------------------------------------------------------------------------------ */

#define PAS_WPSS                6               /* dt: qcom,pas-id of qcom,wpss@8a00000 */
#define WPSS_MEMORY             0x9ae00000u     /* dt: reserved-memory wlan@9ae00000, that node's memory-region */
#define WPSS_MEMORY_SIZE        0x01900000u

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* An entry of a directory of a packfs image in memory, by name (as in redwood_adreno.c). */
static const struct packfs_dnode *
image_entry(const uint8_t *image, const struct packfs_super *super, const struct packfs_dnode *directory, const char *name)
{
	const struct packfs_dnode *nodes = (const struct packfs_dnode *)(image + super->ps_nodes_off);
	size_t length = strlen(name);

	if (directory == NULL || !S_ISDIR(directory->pd_mode) || directory->pd_data + directory->pd_size > super->ps_node_count) {
		return NULL;
	}
	for (uint64_t i = 0; i < directory->pd_size; i++) {
		const struct packfs_dnode *node = &nodes[directory->pd_data + i];

		if (node->pd_name_len == length && (uint64_t)node->pd_name + length <= super->ps_strings_size &&
		    memcmp(image + super->ps_strings_off + node->pd_name, name, length) == 0) {
			return node;
		}
	}
	return NULL;
}

/*
 * A file of fw/ in the small image that travels with the kernel -- userland/build.sh puts what
 * is in fw-wifi/ there --, found in the memory of the RAM disk itself (see redwood_adreno.c).
 */
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	for (int disk = 1; disk >= 0; disk--) {
		const struct packfs_super *super;
		const struct packfs_dnode *file;
		vm_offset_t base = 0;
		uint64_t length = 0;
		dev_t dev = mdevlookup(disk);

		if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0 || length < PACKFS_SUPER_SIZE) {
			continue;
		}
		super = (const struct packfs_super *)base;
		if (memcmp(super->ps_magic, PACKFS_MAGIC, sizeof(super->ps_magic)) != 0 || super->ps_image_size > length ||
		    super->ps_node_count == 0 ||
		    super->ps_nodes_off + (uint64_t)super->ps_node_count * sizeof(struct packfs_dnode) > super->ps_image_size ||
		    super->ps_strings_off + super->ps_strings_size > super->ps_image_size) {
			continue;
		}
		file = (const struct packfs_dnode *)(base + super->ps_nodes_off);       /* the root directory */
		file = image_entry((const uint8_t *)base, super, file, "fw");
		file = image_entry((const uint8_t *)base, super, file, name);
		if (file != NULL && S_ISREG(file->pd_mode) && !(file->pd_flags & PACKFS_F_CHUNKED) &&
		    file->pd_data + file->pd_size <= super->ps_image_size) {
			*size = file->pd_size;
			return (const uint8_t *)base + file->pd_data;
		}
	}
	printf("wifi: %u: no fw/%s in the small image: put the phone's /vendor/firmware_mnt/image/%s into fw-wifi/ and run "
	    "userland/build.sh\n", step, name, name);
	return NULL;
}

/*
 * The image as its header describes it (mdt_loader.c): a 32-bit ELF whose header, table of
 * segments and block of hashes and certificates are the file wpss.mdt, and whose loadable
 * segments are a file each, wpss.bNN for segment NN. The segments are "relocatable": they go
 * where the memory reserved for the DSP is, which on this phone is where they say they are.
 */
#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)      /* the segment that is the block of hashes */
#define MAX_SEGMENTS            16

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;                    /* in the DSP's memory */
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;                          /* of the memory all of them take, from the start of the DSP's */
} image;

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the DSP's image in the small image that came with the kernel, and check it against its own header");
	image.segments = 0;
	header = image_file("wpss.mdt", &size);
	if (header == NULL) {
		return false;
	}
	/* a 32-bit ELF: the count of segments at 44, the table of them at 52, 32 bytes each */
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("wifi: %u: fw/wpss.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;                       /* (not one that is loaded) */
		}
		if (segment.paddr < WPSS_MEMORY || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)WPSS_MEMORY + WPSS_MEMORY_SIZE) {
			printf("wifi: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			return false;
		}
		image.segment[image.segments].at = segment.paddr - WPSS_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "wpss.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				return false;
			}
			if (file_size != segment.file_size) {
				printf("wifi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				return false;
			}
			files += file_size;
		}
		printf("wifi: %u: segment %u: %#x bytes at %#x, of which %#x from its file\n", step, i, segment.memory_size, segment.paddr,
		    segment.file_size);
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != WPSS_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("wifi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		image.segments = 0;
		return false;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - WPSS_MEMORY;
	printf("wifi: %u: the image is whole: a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x bytes "
	    "of the DSP's %#x from %#x\n", step, size, hashes, image.segments, files, image.size, WPSS_MEMORY_SIZE, WPSS_MEMORY);
	return true;
}

/* ---- step 7: the mailbox, opened ----------------------------------------------------------------------- */

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: the mailbox is not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring(uint32_t client, uint32_t signal, const char *whose)
{
	put(&ipcc, whose, IPCC_SEND, client << 16 | signal);
}

/*
 * Linux's qmp_open(), a step at a time: acknowledge their link, raise ours and wait for their
 * acknowledgement, raise our channel and wait for theirs, acknowledge that and wait for their
 * acknowledgement of ours. Whatever of it has been done already -- by a kernel before this one:
 * the always-on processor is restarted only with the phone -- is found done and left.
 */
static bool
step_mailbox_open(void)
{
	uint32_t box, size;

	announce("open the mailbox to the always-on processor: words of the message memory, and its doorbell");
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("wifi: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("wifi: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (get(&aoss, "our link", QMP_OUR_LINK) == QMP_UP && get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK) == QMP_UP &&
	    get(&aoss, "our channel", QMP_OUR_CHANNEL) == QMP_UP && get(&aoss, "their channel", QMP_THEIR_CHANNEL) == QMP_UP &&
	    get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("wifi: %u: the mailbox is open already (a kernel before this one opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, get(&aoss, "their link", QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second (it says %#x); our link is "
		    "taken down again\n", step, rd(aoss.base + QMP_OUR_LINK_ACK));
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	printf("wifi: %u: the always-on processor has seen our link\n", step);
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second (it says %#x); ours is taken down "
		    "again\n", step, rd(aoss.base + QMP_THEIR_CHANNEL));
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second (it says %#x); it is taken "
		    "down again\n", step, rd(aoss.base + QMP_OUR_CHANNEL_ACK));
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	printf("wifi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN; the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()). */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };

	if (our_box == 0) {
		return false;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("wifi: %u: about to tell the always-on processor \"%s\": %u bytes into our mailbox at %#x, its length in "
	    "front, and the doorbell\n", step, text, QMP_MESSAGE, aoss.phys + our_box);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	hold(true);
	if (rd(aoss.base + our_box) != 0) {
		printf("wifi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		return false;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		return false;
	}
	printf("wifi: %u: the always-on processor has taken it\n", step);
	return true;
}

/* ---- step 8: TrustZone takes the header ---------------------------------------------------------------- */

/* The calls, as in redwood_adreno.c (qcom_scm.c: qcom_scm_pas_*; convention: qcom_scm-smc.c). */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* two: a number, and the address of memory it may read and write */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */

static LCK_GRP_DECLARE(wifi_lock_group, "redwood-wifi");
static LCK_MTX_DECLARE(dsp_lock, &wifi_lock_group);     /* the DSP being started or stopped */
static bool dsp_started;                /* TrustZone has let it out of reset, and has not shut it down since */
static uint32_t dirty;

/* One call, as Linux makes these: one that may be preempted. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("wifi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	hold(true);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
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
		if (x0 != SCM_INTERRUPTED) {
			printf("wifi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("wifi: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

/*
 * TrustZone is to shut the DSP down (it takes the image's memory back, cleared), and the
 * always-on processor to know that the image is gone: it was told of one by us, or by the
 * kernel that left the DSP running.
 */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("wifi: %u: %s: TrustZone is asked to shut the DSP down\n", step, why);
	result = scm("to shut the Wi-Fi DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_WPSS, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT SHUT THE DSP DOWN: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: THE WI-FI DSP IS SHUT DOWN\n", step);
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
	return true;
}

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	uint32_t again = 0;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		/* (see the top of this file: it has run since the phone was started, and may be running) */
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down a DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!(PE_parse_boot_argn("wifi_again", &again, sizeof(again)) && again)) {
			printf("wifi: %u: THE DSP IS NOT STARTED AGAIN: it has been started once since the phone was, and a second start "
			    "has so far reset the phone (see the top of redwood_wifi.c; wifi_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("wifi: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started, and "
		    "there is nothing to shut down\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 6), in a page of ours");
	/* in memory of ours, page-aligned and in one piece, not touched again and never freed (see redwood_adreno.c) */
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));       /* TrustZone reads memory, not this processor's cache */
	}
	settle();
	result = scm("to take the Wi-Fi DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_WPSS, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: TRUSTZONE ACCEPTS THE WI-FI DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_WPSS, WPSS_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, WPSS_MEMORY);
	return true;
}

/* ---- step 9: the image, in the DSP's memory ------------------------------------------------------------ */

static bool image_written;

/* Eight bytes of a segment as they are to be in the DSP's memory, from byte `at` of it: the file's, then zeroes. */
static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the DSP's memory (0x9ae00000, 25 MB: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	/*
	 * (The memory is looked at only now: TrustZone has just said that it is not its own. And it
	 * is mapped for as long as this takes: 25 MB do not fit where ml_io_map() puts what stays.)
	 */
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(WPSS_MEMORY, WPSS_MEMORY_SIZE, VM_WIMG_WCOMB);
	printf("wifi: %u: the memory begins %#llx, the image %#llx\n", step, dsp_memory[0], segment_word(0, 0));
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;

		if ((image.segment[s].at | image.segment[s].memory_size) & 7) {
			printf("wifi: %u: a segment that does not begin and end at a multiple of eight bytes: not written\n", step);
			return false;
		}
		printf("wifi: %u: about to write %#x bytes at %#x (%#x of them the file's)\n", step, image.segment[s].memory_size,
		    WPSS_MEMORY + image.segment[s].at, image.segment[s].file_size);
		snprintf(last_write, sizeof(last_write), "step %u wrote a segment of the image at %#x", step,
		    WPSS_MEMORY + image.segment[s].at);
		hold(true);
		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			to[i / 8] = segment_word(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;

		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
	}
	printf("wifi: %u: %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step, written, cleared,
	    wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, WPSS_MEMORY_SIZE);
	image_written = wrong == 0;
	return image_written;
}

/* ---- step 10: the DSP, started ------------------------------------------------------------------------- */

/*
 * Linux's order (qcom_q6v5_pas.c: adsp_start): the always-on processor is told, then TrustZone
 * asked. Linux also asks RPMh for the crystal and for CX and MX at level 384 until the DSP says
 * it has asked for its own power ("proxy" requests, dt: qcom,proxy-reg-names); that is not done
 * here -- no supply is touched from this file --, and the DSP starts on what the boot loader
 * left: level 256 for both (step 3). Then the shared memory and our doorbell are watched, every
 * 200 microseconds: for the DSP's item to appear, for every change in it, for a ring.
 *
 * What has been seen (the phone's first start of it from this kernel): 9 ms after TrustZone
 * returns the DSP rings our doorbell for GLINK, and 35 ms after, its SMP2P item is there --
 * version 1, feature bit 0, from processor 13 to 0 -- with no entries, and stays so. By
 * SMP2P's rules a side names its entries once the two have agreed on a version, for which it
 * must have seen the other's item: the DSP is waiting for ours (616), which this file does not
 * make yet. So "it is up" here means that it runs and speaks, not yet that it has said the word
 * Linux waits for (bit 1 of its entry "slave-kernel").
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false;

	if (!image_written || our_box == 0) {
		printf("wifi: %u: the image is not in place, or the mailbox not open\n", step);
		return false;
	}
	PE_parse_boot_argn("wifi_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	if (have) {
		show_words("before the start, the DSP's item says", &last);
	}
	announce("tell the doorbells that rings from before this kernel have been heard, tell the always-on processor that the "
	    "image is loaded, and ask TrustZone to authenticate it and let the DSP out of reset");
	/* (nobody answered them, and a ring after the start is to be the DSP's) */
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	rang = IPCC_NOBODY;
	lck_mtx_lock(&dsp_lock);
	if (!mailbox_send("{class: image, res: load_state, name: wpss, val: on}")) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the Wi-Fi DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_WPSS, 0, 0,
	    &answer);
	started = now_us();
	image_written = false;                  /* (whatever the answer: the memory is not ours to look at now) */
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT START THE DSP: the call returns %d, its answer is %#llx\n", step, result, answer);
		mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("wifi: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE WI-FI DSP OUT OF RESET; watching the shared memory "
	    "and our doorbell for %u s\n", step, wait_seconds);

	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		if (rang == IPCC_NOBODY) {
			rang = rd(ipcc.base + IPCC_RECEIVED);
			if (rang != IPCC_NOBODY) {
				printf("wifi: %u: +%llu.%03llu ms: OUR DOORBELL HAS BEEN RUNG: %#x (client %u, signal %u)\n", step,
				    (now - started) / 1000, (now - started) % 1000, rang, rang >> 16, rang & 0xffff);
				spoke = true;
			}
		}
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = true;
			spoke = true;
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + watch_seconds * 1000000ull;
				printf("wifi: %u: THE WI-FI DSP SAYS IT IS UP: its entry \"slave-kernel\" says %#x (bit 1: \"error ready\"), "
				    "%llu.%03llu ms after TrustZone started it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if (state & DSP_FATAL) {
				printf("wifi: %u: THE WI-FI DSP SAYS IT HAS GIVEN UP (bit 0 of \"slave-kernel\")\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	if (have) {
		show_words("at the end of the watch the DSP's item says", &last);
	}
	printf("wifi: %u: the watch is over after %llu ms; the doorbells say %#x\n", step, (now_us() - started) / 1000,
	    rd(ipcc.base + IPCC_RECEIVED));
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at != 0) {
		printf("wifi: %u: the DSP said it was up%s\n", step, state & DSP_HANDOVER ? ", and that it has asked for its own power "
		    "(bit 2)" : "");
	} else if (spoke) {
		printf("wifi: %u: THE WI-FI DSP RUNS: it has %s%s. Its SMP2P item has %u entries: it names them once it has seen an "
		    "item of ours, and we make none yet\n", step, rang != IPCC_NOBODY ? "rung our doorbell" : "", have ? (rang !=
		    IPCC_NOBODY ? " and has its item in the shared memory" : "its item in the shared memory") : "", last.valid);
	} else {
		printf("wifi: %u: NOT A WORD FROM THE DSP: no ring, and %s\n", step, have ? "its item in the shared memory is as a "
		    "kernel before this one left it" : "no item of its in the shared memory");
	}
	return ready_at != 0 || spoke;
}

/* ---- beside the steps: the radio's stream in the SMMU (boot-arg wifi_smmu=1) ------------------------------ */

/*
 * What the radio reads and writes in memory by itself (its copy engines, the rings of the data
 * path) goes through the application processors' SMMU as stream 0x1c00, with the mask 1 (dt:
 * "iommus" of qcom,wcn6750; the SMMU is apps-smmu@15000000, an Arm MMU-500 like the GPU's).
 * Whether the boot chain leaves that stream passing untranslated, translated or faulting decides
 * what has to be made before the radio is given an address. This reads the SMMU's global
 * registers -- the ones the hypervisor answers for, see redwood_adreno.c -- and says which of
 * its stream match registers cover that stream. Nothing is written.
 */
static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
#define SMMU_SCR0               0x000           /* bit 0: the whole SMMU off; bit 10: a stream nobody matches faults */
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define RADIO_STREAM            0x1c00
#define RADIO_STREAM_MASK       0x0001

static void
smmu_look(void)
{
	static const char *const kinds[] = { "translated by context", "PASSED UNTRANSLATED (bypass; the number is", "MADE TO FAULT "
		                             "(the number is", "of a reserved kind (the number is" };
	uint32_t scr0, idr0, idr1, matches, valid = 0, covering = 0;

	printf("wifi: 0: read the global registers of the application processors' SMMU (0x15000000: the first access of ours to "
	    "that block; the hypervisor answers for them)\n");
	sleep_ms(pause_ms);
	scr0 = get(&apps_smmu, "scr0", SMMU_SCR0);
	idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	idr1 = get(&apps_smmu, "idr1", SMMU_IDR1);
	matches = idr0 & 0xff;
	printf("wifi: 0: the SMMU is %s; %u stream match registers, %u contexts; a stream nobody matches %s\n", scr0 & 1 ? "OFF" : "on",
	    matches, idr1 & 0xff, scr0 & 1u << 10 ? "faults" : "passes untranslated");
	for (uint32_t n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		/* with 16-bit stream numbers switched on (scr0 bit 15) "valid" moves from bit 31 of the one to bit 10 of the other */
		uint32_t wide = scr0 & 1u << 15, stream = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

		if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
			continue;
		}
		valid++;
		if (((stream ^ RADIO_STREAM) & ~(mask | RADIO_STREAM_MASK)) != 0) {
			continue;
		}
		printf("wifi: 0: match register %u covers the radio's stream: stream %#x mask %#x (%#x), %s %u; s2cr %#x)\n", n, stream,
		    mask, smr, kinds[(s2cr >> 16) & 3], s2cr & 0xff, s2cr);
		covering++;
	}
	printf("wifi: 0: %u of the %u match registers are in use; %u cover stream %#x%s\n", valid, matches, covering, RADIO_STREAM,
	    covering ? "" : ": THE BOOT CHAIN HAS SET NOTHING UP FOR THE RADIO'S STREAM");
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_smem, step_mailbox, step_rpmh, step_rails, step_pins, step_firmware, step_mailbox_open, step_header, step_image,
	step_start,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/*
 * Where a run ends in order while the DSP runs (the run's time is up, the host asks for its
 * end), and when the steps are over: the DSP is stopped. Linux first asks the DSP itself to
 * stop, through our SMP2P item, and waits for its acknowledgement; we have no such item yet,
 * and do what Linux does when no acknowledgement comes.
 */
void
redwood_wifi_stop(void)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && !dirty) {
		dsp_shutdown("the run is about to end");
	}
	lck_mtx_unlock(&dsp_lock);
}

#define REDWOOD_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin (its "redwood") */

extern vm_offset_t pe_arm_get_soc_base_phys(void);

/*
 * Called while the system starts, before there is a root file system, and the start waits (see
 * redwood_adreno_init()).
 */
void
redwood_wifi_init(void)
{
	uint32_t back = 0, done = 0;

	if (!PE_parse_boot_argn("wifi", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != REDWOOD_SOC_BASE) {
		printf("wifi: wifi=N is for the phone, and this is not it\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	new_from = last_step;
	PE_parse_boot_argn("wifi_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("wifi_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("wifi_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("wifi_watch", &watch_seconds, sizeof(watch_seconds));
	PE_parse_boot_argn("wifi_dirty", &dirty, sizeof(dirty));

	printf("wifi: steps up to %u; those from %u on are new and wait %u ms %s\n", last_step, new_from, pause_ms,
	    fine ? "before every write and call" : "after each announcement");
	if (PE_parse_boot_argn("wifi_smmu", &back, sizeof(back)) && back) {
		smmu_look();
	}
	back = 0;
	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("wifi: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("wifi: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;                   /* (what follows has been announced) */
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && dirty) {
		printf("wifi: THE DSP IS LEFT RUNNING (wifi_dirty), as a kernel that died would leave it\n");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
	printf("wifi: over after step %u; %s\n", done, last_write);
	if (PE_parse_boot_argn("wifi_return", &back, sizeof(back)) && back) {
		printf("wifi: back to the loader (wifi_return)\n");
		loader_return();
	}
	printf("wifi: the system goes on starting\n");
}

/*
 * What interrupts would take, instead of looking. The buttons want the same (the PMIC arbiter's
 * line, see the end of redwood_buttons.c), and the radio will: so the general case first.
 *
 * The general case is one shared peripheral interrupt (an "SPI": the interrupt controller's
 * number for it is 32 more than the device tree's) with a handler. osfmk/arm64/gic_v3_irq.c
 * drives the processors' interfaces, takes the timer's interrupt and the two that processors
 * send each other, and masks and counts anything else that fires. What it lacks:
 *   - a table from interrupt number to handler and argument, and a call with which a driver
 *     fills an entry (number, level or edge, handler, argument); gic_irq_handler() calls the
 *     handler where it now masks, between taking the interrupt (ICC_IAR1_EL1) and ending it
 *     (ICC_EOIR1_EL1);
 *   - for that number, in the distributor (it is mapped; only its control register and the
 *     "disable" registers are written today): group 1 like everything here (GICD_IGROUPR), the
 *     trigger (GICD_ICFGR), a priority (GICD_IPRIORITYR), the processor it goes to
 *     (GICD_IROUTER: the boot processor), and enabled (GICD_ISENABLER); and the reverse when
 *     the driver lets go and before the kernel returns to the loader, which must find the
 *     lines as it left them.
 *   Whether the hypervisor passes this kernel's writes to the distributor for these lines is
 *   not known: none has been made. A handler runs with interrupts off on whatever thread was
 *   interrupted; it should quiet its device and wake a thread, no more.
 *
 * Ours, given that:
 *   - The doorbells have one line for everything rung at the application processor: SPI 229,
 *     level, high (dt: qcom,ipcc@408000). Per doorbell, IPCC wants to be told that it is
 *     listened for: client << 16 | signal written to IPCC_ENABLE. (Without that a ring is still
 *     recorded: IPCC_RECEIVED showed the DSP's GLINK ring, never enabled, across kernels.) The
 *     handler reads IPCC_RECEIVED until it says nobody, writes each word it gets to IPCC_CLEAR,
 *     and passes it on: client 0 signal 0 is the always-on processor's mailbox (every wait in
 *     mailbox_wait() above), client 24 signal 2 the Wi-Fi DSP's SMP2P item (the watch of step
 *     10), client 24 signal 0 its GLINK.
 *   - The DSP's watchdog has a line of its own (SPI 587), raised when its firmware hangs
 *     without managing to say so in the shared memory.
 *   - The radio itself, later, raises 32 more (SPI 768 to 799, edge), by writing their numbers
 *     to a register of the interrupt controller (dt: "msi_addr" of qcom,wcn6750): its copy
 *     engines and the rings of the data path. Those can be looked at in turn as well, at the
 *     price of a thread that never sleeps while the link is up.
 */
