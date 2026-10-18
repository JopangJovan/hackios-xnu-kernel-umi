/*
 * The phone's UFS host controller, driven from this kernel -- read only, and shared with UEFI.
 *
 * Reading the storage through the loader (redwood_ufs.c) is one request at a time: Qualcomm's
 * UFS driver in UEFI (UFSDxe) reads synchronously on core 0, and its asynchronous reads finish
 * only from a UEFI timer that does not tick there while the kernel runs. The controller itself
 * takes up to 32 requests at once. This file puts requests of its own into it, next to UEFI's,
 * without taking the controller away from UEFI: UEFI still reads for the loader and writes the
 * run's log at its end.
 *
 * What it does not do, ever: reset or enable the controller, start the link, send UIC commands
 * (DME_GET/DME_SET), change the request lists' addresses, the interrupt enables, aggregation,
 * clocks or power modes, clear a request (UTRLCLR), or touch the task management list. It reads
 * the registers that say where UEFI's request list is and whether the controller runs, writes
 * request descriptors into slots of that list that UEFI does not use, rings their doorbell bits
 * (UTRLDBR is write-one-to-set: the other bits are not affected), and polls UTRLDBR until the
 * controller clears them. Requests are made with the interrupt bit clear, so that they raise no
 * completion status of their own in IS.
 *
 * Read only by construction: every command passes through ufshc_permitted() on its way to the
 * doorbell, a whitelist that lets through SCSI READ(10)/READ(16), TEST UNIT READY, REQUEST
 * SENSE, INQUIRY, READ CAPACITY(10/16) to a normal logical unit and query *read* requests (read
 * descriptor, attribute, flag), and nothing else; no data ever goes from host to device. What is
 * refused is logged and never reaches the doorbell. This file builds READ(10) only.
 *
 * WHAT THE PROBE FOUND ON THIS PHONE, AND WHY THIS PATH IS NOT USED THERE: Qualcomm's UFSDxe sets
 * up a list of one. The controller has 32 slots, UTRLBA points at 0xccf96000, slot 0 there is
 * UEFI's request descriptor -- and its command descriptor is at 0xccf96100, inside the 1 KB the
 * other 31 would take: "slots" 8..14 are its command UPIU, response and PRD table (pointing into
 * the loader's buffer), and 1..7 and 15..17 hold other data of the driver. No slot but UEFI's own
 * is free memory, so the probe refuses (twice over: the command descriptor inside the list, and
 * slots 16..31 that are not descriptors) and the storage is read through the loader. What
 * follows is how the path works where the list does have room.
 *
 * Which slots: UEFI's driver takes the first free slot for each request it makes, synchronously,
 * so it uses slot 0 (and during a run it reads only when the loader is asked to, and writes only
 * the log, after the run). This file keeps to the upper half of the list, 16..31, and makes its
 * requests in the even ones: a descriptor is 32 bytes and a cache line 64, and a line is only
 * ever written back here while its one request is not in flight, so nothing the controller
 * writes into a descriptor (its status) is overwritten by a stale copy of the line's other half.
 *
 * At the probe (ufshc_probe), anything unexpected leaves this path off and the loader's in use:
 * the controller not enabled and running, a version other than 2.x/3.x, fewer than 32 slots, no
 * 64-bit addressing, the list in memory this kernel manages, any of the slots 16..31 busy or
 * holding something that is not a request descriptor, a fatal error status. The slots' original
 * contents are kept and put back at the end of the run (redwood_ufs_quiesce, from
 * loader_return), after waiting until none of them is busy, so that the list is as UEFI left it
 * when it writes the run's log.
 *
 * Memory: each slot has a page for its command descriptor (command UPIU at 0, response UPIU at
 * 512, the PRD table at 1024) and a contiguous buffer the controller reads into; the descriptors
 * are cleaned to the point of coherency before the doorbell and invalidated before they are
 * read back, and so is the data, so it does not matter whether the controller's accesses are
 * coherent with the caches.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern boolean_t pmap_valid_address(uint64_t addr);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void CleanPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void IOSleep(unsigned int milliseconds);

#define UFSHC_PHYS          0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in the phone's Android device tree */
#define UFSHC_SIZE          0x3000

/* UFSHCI registers (JESD223). 0x38..0x48, the UIC error codes, clear when read: never read here. */
#define UFSHC_CAP           0x00
#define UFSHC_VER           0x08
#define UFSHC_HCDDID        0x10
#define UFSHC_HCPMID        0x14
#define UFSHC_AHIT          0x18
#define UFSHC_IS            0x20
#define UFSHC_IE            0x24
#define UFSHC_HCS           0x30
#define UFSHC_HCE           0x34
#define UFSHC_UTRIACR       0x4c
#define UFSHC_UTRLBA        0x50
#define UFSHC_UTRLBAU       0x54
#define UFSHC_UTRLDBR       0x58
#define UFSHC_UTRLRSR       0x60
#define UFSHC_UTRLCNR       0x64
#define UFSHC_UTMRLBA       0x70
#define UFSHC_UTMRLBAU      0x74
#define UFSHC_UTMRLDBR      0x78
#define UFSHC_UTMRLRSR      0x80

#define CAP_NUTRS(c)        (((c) & 0x1f) + 1)
#define CAP_64AS            (1u << 24)
#define HCS_DP              (1u << 0)
#define HCS_UTRLRDY         (1u << 1)
#define IS_FATAL            ((1u << 2) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))  /* UE, DFES, UTPES, HCFES, SBFES */
#define IS_UTRCS            (1u << 0)

/* the request descriptor's first word: UFS storage command type, data direction */
#define UTRD_CT_UFS         (1u << 28)
#define UTRD_DD_MASK        (3u << 25)
#define UTRD_DD_NONE        (0u << 25)
#define UTRD_DD_OUT         (1u << 25)          /* host to device: never */
#define UTRD_DD_IN          (2u << 25)
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf

#define UPIU_COMMAND        0x01
#define UPIU_QUERY_REQUEST  0x16
#define UPIU_RESPONSE       0x21
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20

#define UCD_SIZE            0x4000              /* a page per slot */
#define UCD_RESPONSE        512
#define UCD_PRDT            1024
#define PRD_MAX             (256u << 10)        /* the most one table entry describes */

#define SLOT_FIRST          16                  /* this kernel's part of the list: 16..31 */
#define SLOTS_RESERVED      0xffff0000u
#define SLOT_MAX            8
#define SLOT_BUFFER_DEFAULT (512u << 10)
#define BLOCK               4096u               /* the logical block size of every unit here */

#define SPIN_US             5000                /* look without sleeping this long, then a millisecond at a time */
#define TIMEOUT_MS          10000

struct ufshc_slot {
	uint32_t number;                        /* in the list */
	volatile uint32_t *utrd;                /* its descriptor, in UEFI's list */
	uint8_t *ucd;
	uint64_t ucd_pa;
	uint8_t *data;
	uint64_t data_pa;
	uint32_t original[8];                   /* what the descriptor held at the probe, put back at the end */
};

static vm_offset_t hc;                          /* the registers */
static volatile uint32_t *hc_list;              /* UEFI's request list, cacheable */
static uint64_t hc_list_pa;
static struct ufshc_slot hc_slots[SLOT_MAX];
static uint32_t hc_nslots, hc_free, hc_slot_buffer = SLOT_BUFFER_DEFAULT;
static uint32_t hc_is_at_probe;
static bool hc_usable;                          /* the probe found it as expected and the memory is there */
static volatile bool hc_closed, hc_dead;
static uint64_t hc_requests, hc_errors, hc_bytes, hc_wait_us, hc_refused, hc_most_in_flight;
static uint32_t hc_in_flight;

static LCK_GRP_DECLARE(hc_lck_grp, "redwood-ufshc");
static LCK_MTX_DECLARE(hc_lock, &hc_lck_grp);

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_requests,
    "requests this kernel's own UFS driver has had answered");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_errors,
    "requests of this kernel's own UFS driver that failed");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_bytes,
    "bytes this kernel's own UFS driver has read");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_wait_us,
    "microseconds spent waiting for the controller, summed over the requests");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_refused,
    "commands the whitelist refused (there should never be one)");

static inline uint32_t
hc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(hc + reg);
}

/* Nothing more is sent from here on; whoever waits for a slot is told. */
static void
hc_off(void)
{
	hc_dead = true;
	lck_mtx_lock(&hc_lock);
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

static uint64_t
hc_us_since(uint64_t start)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / 1000;
}

/* ---- the whitelist: the one check every command passes on its way to the doorbell ---- */

static const char *
hc_refusal(const uint8_t *upiu, uint32_t dw0)
{
	const uint8_t *cdb = upiu + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;

	if (dd == UTRD_DD_OUT || dd == UTRD_DD_MASK) {
		return "data from host to device";
	}
	if ((dw0 & 0xf0000000u) != UTRD_CT_UFS) {
		return "not a UFS storage command type";
	}
	switch (upiu[0] & 0x3f) {
	case UPIU_COMMAND:
		if (upiu[1] & UPIU_FLAG_WRITE) {
			return "a command that writes";
		}
		if (upiu[2] >= 8) {
			return "a well-known logical unit";
		}
		if (upiu[4] & 0x0f) {
			return "not the SCSI command set";
		}
		switch (cdb[0]) {
		case 0x28:      /* READ(10) */
		case 0x88:      /* READ(16) */
			return dd == UTRD_DD_IN && (upiu[1] & UPIU_FLAG_READ) ? NULL : "a read without data in";
		case 0x00:      /* TEST UNIT READY */
			return dd == UTRD_DD_NONE ? NULL : "TEST UNIT READY with data";
		case 0x03:      /* REQUEST SENSE */
		case 0x12:      /* INQUIRY */
		case 0x25:      /* READ CAPACITY(10) */
			return NULL;
		case 0x9e:      /* SERVICE ACTION IN(16): READ CAPACITY(16) only */
			return (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
		default:
			return "a SCSI operation not on the list";
		}
	case UPIU_QUERY_REQUEST:
		if (upiu[5] != 0x01) {
			return "a query that is not a standard read request";
		}
		if (upiu[12] != 0x01 && upiu[12] != 0x03 && upiu[12] != 0x05) {
			return "a query opcode other than read descriptor/attribute/flag";
		}
		return dd == UTRD_DD_NONE ? NULL : "a query with data";
	default:
		return "a transaction that is neither a command nor a query";
	}
}

static bool
ufshc_permitted(const uint8_t *upiu, uint32_t dw0)
{
	const char *why = hc_refusal(upiu, dw0);

	if (why == NULL) {
		return true;
	}
	hc_refused++;
	printf("ufs: native: REFUSED %s: UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, "
	    "descriptor %08x; nothing was sent\n", why, upiu[0], upiu[1], upiu[2], upiu[3], upiu[16], upiu[17], upiu[18],
	    upiu[19], upiu[20], upiu[21], upiu[22], upiu[23], upiu[24], upiu[25], dw0);
	return false;
}

/*
 * The whitelist tried on commands made up here and never sent: those that must be refused,
 * and the read that must pass. Pure bytes; nothing touches the controller.
 */
static bool
hc_whitelist_selftest(void)
{
	static const struct {
		uint8_t type, flags, lun, cdb0, cdb1, query_function, query_opcode;
		uint32_t dd;
		bool pass;
		const char *what;
	} cases[] = {
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 0, 0, UTRD_DD_IN, true, "READ(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x88, 0, 0, 0, UTRD_DD_IN, true, "READ(16)" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x25, 0, 0, 0, UTRD_DD_IN, true, "READ CAPACITY(10)" },
		{ UPIU_QUERY_REQUEST, 0, 0, 0, 0, 0x01, 0x01, UTRD_DD_NONE, true, "QUERY READ DESCRIPTOR" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x2a, 0, 0, 0, UTRD_DD_OUT, false, "WRITE(10)" },
		{ UPIU_COMMAND, 0, 0, 0x2a, 0, 0, 0, UTRD_DD_NONE, false, "WRITE(10) without the W flag" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x2a, 0, 0, 0, UTRD_DD_IN, false, "WRITE(10) dressed as a read" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x8a, 0, 0, 0, UTRD_DD_OUT, false, "WRITE(16)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x42, 0, 0, 0, UTRD_DD_OUT, false, "UNMAP" },
		{ UPIU_COMMAND, 0, 0, 0x04, 0, 0, 0, UTRD_DD_NONE, false, "FORMAT UNIT" },
		{ UPIU_COMMAND, 0, 0, 0x1b, 0, 0, 0, UTRD_DD_NONE, false, "START STOP UNIT" },
		{ UPIU_COMMAND, 0, 0, 0x35, 0, 0, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x3b, 0, 0, 0, UTRD_DD_OUT, false, "WRITE BUFFER" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0x44, 0x28, 0, 0, 0, UTRD_DD_IN, false, "READ(10) to the RPMB unit" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x9e, 0x11, 0, 0, UTRD_DD_IN, false, "SERVICE ACTION IN 0x11" },
		{ UPIU_QUERY_REQUEST, 0, 0, 0, 0, 0x81, 0x02, UTRD_DD_NONE, false, "QUERY WRITE DESCRIPTOR" },
		{ UPIU_QUERY_REQUEST, 0, 0, 0, 0, 0x81, 0x06, UTRD_DD_NONE, false, "QUERY SET FLAG" },
		{ UPIU_QUERY_REQUEST, 0, 0, 0, 0, 0x01, 0x04, UTRD_DD_NONE, false, "QUERY WRITE ATTRIBUTE as a read" },
		{ 0x04, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "TASK MANAGEMENT" },
		{ 0x00, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "NOP OUT" },
	};
	uint64_t refused = hc_refused;
	bool right = true;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t upiu[32] = { 0 };
		bool passed;

		upiu[0] = cases[i].type;
		upiu[1] = cases[i].flags;
		upiu[2] = cases[i].lun == 0x44 ? 0xc4 : cases[i].lun;
		upiu[5] = cases[i].query_function;
		upiu[12] = cases[i].query_opcode;
		upiu[16] = cases[i].cdb0;
		upiu[17] = cases[i].cdb1;
		passed = hc_refusal(upiu, UTRD_CT_UFS | cases[i].dd) == NULL;
		if (passed != cases[i].pass) {
			printf("ufs: native: the whitelist %s %s\n", passed ? "LETS THROUGH" : "refuses", cases[i].what);
			right = false;
		}
	}
	hc_refused = refused;
	printf("ufs: native: whitelist tried on %u commands made up and never sent (READ(10/16), READ CAPACITY, "
	    "QUERY READ DESCRIPTOR pass; WRITE(10/16), UNMAP, FORMAT UNIT, START STOP UNIT, SYNCHRONIZE CACHE, "
	    "WRITE BUFFER, RPMB, query writes and set flag, task management, NOP OUT refused): %s\n",
	    (unsigned)(sizeof(cases) / sizeof(cases[0])), right ? "as it should" : "WRONG");
	return right;
}

/* ---- one request in one slot ---- */

static int
hc_submit(struct ufshc_slot *s, uint8_t lun, uint32_t lba, uint32_t blocks)
{
	uint32_t bytes = blocks * BLOCK, n = 0, dw0 = UTRD_CT_UFS | UTRD_DD_IN;
	uint8_t *cmd = s->ucd;

	/* the command UPIU and an empty response */
	bzero(s->ucd, UCD_PRDT);
	cmd[0] = UPIU_COMMAND;
	cmd[1] = UPIU_FLAG_READ;                /* simple task attribute */
	cmd[2] = lun;
	cmd[3] = (uint8_t)s->number;            /* task tag */
	cmd[12] = (uint8_t)(bytes >> 24);
	cmd[13] = (uint8_t)(bytes >> 16);
	cmd[14] = (uint8_t)(bytes >> 8);
	cmd[15] = (uint8_t)bytes;
	cmd[16] = 0x28;                         /* READ(10) */
	cmd[18] = (uint8_t)(lba >> 24);
	cmd[19] = (uint8_t)(lba >> 16);
	cmd[20] = (uint8_t)(lba >> 8);
	cmd[21] = (uint8_t)lba;
	cmd[23] = (uint8_t)(blocks >> 8);
	cmd[24] = (uint8_t)blocks;
	/* the data's place, in pieces of at most 256 KB */
	for (uint32_t at = 0; at < bytes; at += PRD_MAX, n++) {
		volatile uint32_t *prd = (volatile uint32_t *)(s->ucd + UCD_PRDT + 16 * n);
		uint64_t pa = s->data_pa + at;
		uint32_t len = bytes - at < PRD_MAX ? bytes - at : PRD_MAX;

		prd[0] = (uint32_t)pa;
		prd[1] = (uint32_t)(pa >> 32);
		prd[2] = 0;
		prd[3] = len - 1;
	}
	FlushPoC_DcacheRegion((vm_offset_t)s->ucd, UCD_PRDT + 16 * n);
	/* nothing of the buffer is left in a cache to be written back over what the controller writes */
	FlushPoC_DcacheRegion((vm_offset_t)s->data, bytes);

	s->utrd[0] = dw0;                       /* no interrupt, no inline encryption */
	s->utrd[1] = 0;
	s->utrd[2] = OCS_INVALID;
	s->utrd[3] = 0;
	s->utrd[4] = (uint32_t)s->ucd_pa;
	s->utrd[5] = (uint32_t)(s->ucd_pa >> 32);
	s->utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);   /* response: offset, length in dwords */
	s->utrd[7] = ((UCD_PRDT / 4) << 16) | n;                        /* PRD table: offset in dwords, entries */
	FlushPoC_DcacheRegion((vm_offset_t)s->utrd, 32);

	/* what the controller will read is what the whitelist sees */
	if (!ufshc_permitted(s->ucd, s->utrd[0])) {
		return EPERM;
	}
	if (hc_closed || hc_dead) {
		return ENXIO;
	}
	if (hc_read(UFSHC_UTRLDBR) & (1u << s->number)) {
		printf("ufs: native: slot %u is busy before it is used: this path is off from now on\n", s->number);
		hc_off();
		return EIO;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(hc + UFSHC_UTRLDBR) = 1u << s->number;
	return 0;
}

/* Until the controller is done with the slot's request; then whether it went well. */
static int
hc_complete(struct ufshc_slot *s, uint8_t lun, uint32_t lba, uint32_t blocks)
{
	uint64_t started = mach_absolute_time(), us;
	const uint8_t *r = s->ucd + UCD_RESPONSE;
	uint32_t ocs;

	while (hc_read(UFSHC_UTRLDBR) & (1u << s->number)) {
		us = hc_us_since(started);
		if (us < SPIN_US) {
			__builtin_arm_yield();
			continue;
		}
		if (us > TIMEOUT_MS * 1000ULL) {
			printf("ufs: native: slot %u (LUN %u, blocks %u..%u) not done in %u s (doorbell %08x, IS %08x, HCS %08x): "
			    "this path is off from now on\n", s->number, lun, lba, lba + blocks - 1, TIMEOUT_MS / 1000,
			    hc_read(UFSHC_UTRLDBR), hc_read(UFSHC_IS), hc_read(UFSHC_HCS));
			hc_off();
			return ETIMEDOUT;
		}
		IOSleep(1);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	hc_wait_us += hc_us_since(started);
	FlushPoC_DcacheRegion((vm_offset_t)s->utrd, 32);
	FlushPoC_DcacheRegion((vm_offset_t)r, 64);
	ocs = s->utrd[2] & 0xff;
	if (ocs != OCS_SUCCESS || (r[0] & 0x3f) != UPIU_RESPONSE || r[3] != s->number || r[6] != 0 || r[7] != 0) {
		/* sense data follows the 32 bytes of the header: length (2), then the fixed format */
		FlushPoC_DcacheRegion((vm_offset_t)r + 64, 64);
		printf("ufs: native: slot %u, LUN %u, blocks %u..%u: OCS %x, response %02x tag %02x, response %02x status %02x, "
		    "sense key %x asc %02x ascq %02x\n", s->number, lun, lba, lba + blocks - 1, ocs, r[0], r[3], r[6], r[7],
		    r[34 + 2] & 0x0f, r[34 + 12], r[34 + 13]);
		hc_errors++;
		return EIO;
	}
	FlushPoC_DcacheRegion((vm_offset_t)s->data, blocks * BLOCK);
	hc_requests++;
	hc_bytes += blocks * BLOCK;
	return 0;
}

static struct ufshc_slot *
hc_take(bool wait)
{
	struct ufshc_slot *s = NULL;

	lck_mtx_lock(&hc_lock);
	while (hc_free == 0 && wait && !hc_dead) {
		msleep(&hc_free, &hc_lock, PRIBIO, "ufshc", NULL);
	}
	if (hc_free != 0) {
		uint32_t i = (uint32_t)__builtin_ctz(hc_free);

		hc_free &= ~(1u << i);
		s = &hc_slots[i];
		if (++hc_in_flight > hc_most_in_flight) {
			hc_most_in_flight = hc_in_flight;
		}
	}
	lck_mtx_unlock(&hc_lock);
	return s;
}

static void
hc_give(struct ufshc_slot *s)
{
	lck_mtx_lock(&hc_lock);
	hc_free |= 1u << (s - hc_slots);
	hc_in_flight--;
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

/*
 * Bytes [offset, offset + len) of logical unit lun into dst: the blocks that cover them, a slot's
 * buffer at a time, with as many slots in flight at once as are free (at least one), the wanted
 * bytes copied out of each as it is done.
 */
int
ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len)
{
	struct {
		struct ufshc_slot *slot;
		uint32_t lba, blocks, skip, take;
		uint8_t *to;
	} q[SLOT_MAX];
	uint32_t head = 0, tail = 0, per = hc_slot_buffer / BLOCK;
	int error = 0;

	if (!hc_usable || hc_dead || hc_closed) {
		return ENXIO;
	}
	while ((len > 0 && error == 0) || head != tail) {
		/* as many more requests as there are free slots (and one, waiting for it, if none is in flight) */
		while (len > 0 && error == 0 && tail - head < SLOT_MAX) {
			uint64_t first = offset / BLOCK, skip = offset % BLOCK;
			uint64_t blocks = (skip + len + BLOCK - 1) / BLOCK, take;
			struct ufshc_slot *s = hc_take(head == tail);
			int e;

			if (s == NULL) {
				if (head == tail) {
					error = EIO;                    /* the path went off while waiting */
				}
				break;
			}
			if (blocks > per) {
				blocks = per;
			}
			if (first + blocks > 0xffffffffULL) {
				hc_give(s);
				error = EINVAL;
				break;
			}
			take = blocks * BLOCK - skip;
			if (take > len) {
				take = len;
			}
			e = hc_submit(s, lun, (uint32_t)first, (uint32_t)blocks);
			if (e != 0) {
				hc_give(s);
				error = e;
				break;
			}
			q[tail % SLOT_MAX].slot = s;
			q[tail % SLOT_MAX].lba = (uint32_t)first;
			q[tail % SLOT_MAX].blocks = (uint32_t)blocks;
			q[tail % SLOT_MAX].skip = (uint32_t)skip;
			q[tail % SLOT_MAX].take = (uint32_t)take;
			q[tail % SLOT_MAX].to = dst;
			tail++;
			offset += take;
			dst += take;
			len -= take;
		}
		/* the oldest one done, copied out, its slot free again */
		if (head != tail) {
			typeof(q[0]) *e = &q[head % SLOT_MAX];
			int r = hc_complete(e->slot, lun, e->lba, e->blocks);

			if (r == 0 && error == 0) {
				memcpy(e->to, e->slot->data + e->skip, e->take);
			} else if (error == 0) {
				error = r;
			}
			if (r != ETIMEDOUT) {
				hc_give(e->slot);                       /* (a slot that never came back is not used again) */
			}
			head++;
		}
	}
	return error;
}

bool
ufshc_available(void)
{
	return hc_usable && !hc_dead && !hc_closed;
}

void
ufshc_report(const char *when)
{
	if (!hc_usable) {
		return;
	}
	printf("ufs: native: %s: %llu requests, %llu errors, %llu bytes, %llu ms waiting (summed over requests), "
	    "at most %llu in flight, %llu refused%s\n", when, hc_requests, hc_errors, hc_bytes, hc_wait_us / 1000,
	    hc_most_in_flight, hc_refused, hc_dead ? "; THE PATH IS OFF" : "");
}

/* ---- the probe ---- */

static bool
hc_looks_like_descriptor(const volatile uint32_t *d)
{
	bool zero = true;

	for (int i = 0; i < 8; i++) {
		zero &= d[i] == 0;
	}
	/* never used, or a UFS storage request with its command descriptor 128-byte aligned */
	return zero || ((d[0] & 0xf0000000u) == UTRD_CT_UFS && (d[4] & 0x7f) == 0 && (d[0] & UTRD_DD_MASK) != UTRD_DD_MASK);
}

/*
 * What the controller looks like as UEFI left it, and whether this kernel may put requests of
 * its own into it (only == true: say so and do not touch anything but registers' reads).
 */
bool
ufshc_probe(bool only)
{
	uint32_t cap, ver, hcs, hce, is, ie, iacr, ahit, base, baseu, db, rsr, cnr, tm_base, tm_baseu, tm_db, tm_rsr;
	uint64_t ucd0 = 0, list_end;
	const char *no = NULL;
	vm_offset_t list_va;

	if (!hc_whitelist_selftest()) {
		return false;
	}
	PE_parse_boot_argn("ufs_native_buffer", &hc_slot_buffer, sizeof(hc_slot_buffer));
	if (hc_slot_buffer < BLOCK || hc_slot_buffer > (4u << 20) || hc_slot_buffer % BLOCK) {
		hc_slot_buffer = SLOT_BUFFER_DEFAULT;
	}
	hc = ml_io_map(UFSHC_PHYS, UFSHC_SIZE);
	cap = hc_read(UFSHC_CAP);
	ver = hc_read(UFSHC_VER);
	hcs = hc_read(UFSHC_HCS);
	hce = hc_read(UFSHC_HCE);
	is = hc_read(UFSHC_IS);
	ie = hc_read(UFSHC_IE);
	iacr = hc_read(UFSHC_UTRIACR);
	ahit = hc_read(UFSHC_AHIT);
	base = hc_read(UFSHC_UTRLBA);
	baseu = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	cnr = hc_read(UFSHC_UTRLCNR);
	tm_base = hc_read(UFSHC_UTMRLBA);
	tm_baseu = hc_read(UFSHC_UTMRLBAU);
	tm_db = hc_read(UFSHC_UTMRLDBR);
	tm_rsr = hc_read(UFSHC_UTMRLRSR);
	hc_is_at_probe = is;
	printf("ufs: native: controller at %llx: CAP %08x (%u slots, 64-bit %s), VER %08x, HCDDID %08x, HCPMID %08x\n",
	    UFSHC_PHYS, cap, CAP_NUTRS(cap), (cap & CAP_64AS) ? "yes" : "no", ver, hc_read(UFSHC_HCDDID),
	    hc_read(UFSHC_HCPMID));
	printf("ufs: native: HCE %08x, HCS %08x, IS %08x, IE %08x, UTRIACR %08x, AHIT %08x\n", hce, hcs, is, ie, iacr, ahit);
	printf("ufs: native: transfer list %08x:%08x, doorbell %08x, run %08x, completion %08x; "
	    "task management list %08x:%08x, doorbell %08x, run %08x\n", baseu, base, db, rsr, cnr, tm_baseu, tm_base,
	    tm_db, tm_rsr);
	hc_list_pa = (uint64_t)baseu << 32 | base;
	list_end = hc_list_pa + 32ULL * CAP_NUTRS(cap);

	if ((hce & 1) == 0) {
		no = "the controller is not enabled";
	} else if ((hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
		no = "no device present or the transfer list not ready";
	} else if ((rsr & 1) == 0) {
		no = "the transfer list is not running";
	} else if (ver < 0x200 || ver >= 0x400) {
		no = "a controller version other than 2.x or 3.x";
	} else if (CAP_NUTRS(cap) < 32) {
		no = "fewer than 32 slots";
	} else if ((cap & CAP_64AS) == 0) {
		no = "no 64-bit addressing";
	} else if (is & IS_FATAL) {
		no = "an error status";
	} else if (hc_list_pa == 0 || (hc_list_pa & 0x3ff) != 0) {
		no = "no transfer list, or one not 1 KB aligned";
	} else if (pmap_valid_address(hc_list_pa) || pmap_valid_address(list_end - 1)) {
		no = "the transfer list is in memory this kernel manages";
	} else if (db & SLOTS_RESERVED) {
		no = "a slot of 16..31 is busy";
	}
	if (no != NULL) {
		printf("ufs: native: not used: %s; the storage is read through the loader\n", no);
		return false;
	}

	/* UEFI's list: what each slot holds now (the ones that hold anything) */
	list_va = ml_io_map_unmappable((vm_offset_t)(hc_list_pa & ~(vm_offset_t)PAGE_MASK),
	    (vm_size_t)round_page(list_end - (hc_list_pa & ~(uint64_t)PAGE_MASK)), VM_WIMG_DEFAULT);
	hc_list = (volatile uint32_t *)(list_va + (hc_list_pa & PAGE_MASK));
	FlushPoC_DcacheRegion((vm_offset_t)hc_list, 32 * 32);
	for (uint32_t i = 0; i < 32; i++) {
		const volatile uint32_t *d = hc_list + 8 * i;

		if ((d[0] | d[1] | d[2] | d[3] | d[4] | d[5] | d[6] | d[7]) != 0) {
			printf("ufs: native: slot %2u: %08x %08x %08x %08x %08x %08x %08x %08x\n", i, d[0], d[1], d[2], d[3], d[4],
			    d[5], d[6], d[7]);
		}
		if (i >= SLOT_FIRST && !hc_looks_like_descriptor(d)) {
			no = "a slot of 16..31 holds something that is not a request descriptor";
		}
		if (i < SLOT_FIRST && ucd0 == 0 && (d[4] | d[5]) != 0) {
			ucd0 = (uint64_t)d[5] << 32 | d[4];
		}
	}
	if (ucd0 != 0) {
		/* UEFI's own command descriptor: the last command it sent, and where it is */
		vm_offset_t va = ml_io_map_unmappable((vm_offset_t)(ucd0 & ~(uint64_t)PAGE_MASK), PAGE_SIZE * 2, VM_WIMG_DEFAULT);
		const volatile uint8_t *u = (const volatile uint8_t *)(va + (ucd0 & PAGE_MASK));

		FlushPoC_DcacheRegion((vm_offset_t)u, 1024);
		printf("ufs: native: UEFI's command descriptor at %llx: command UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x; response %02x .. %02x %02x\n", ucd0, u[0], u[1], u[2], u[3], u[16], u[17], u[18],
		    u[19], u[20], u[21], u[22], u[23], u[24], u[25], u[512], u[518], u[519]);
		if (ucd0 < list_end && ucd0 + 1024 > hc_list_pa) {
			no = "UEFI's command descriptor is inside the list";
		}
	}
	if (no != NULL) {
		printf("ufs: native: not used: %s; the storage is read through the loader\n", no);
		return false;
	}
	if (only) {
		printf("ufs: native: as expected; ufs_native=2 asks for no more than this look\n");
		return false;
	}

	/* this kernel's slots: the even ones of 16..31, a page of descriptor and a buffer each */
	for (uint32_t i = 0; i < SLOT_MAX; i++) {
		struct ufshc_slot *s = &hc_slots[i];
		vm_offset_t ucd = 0, data = 0;

		if (kmem_alloc_contig(kernel_map, &ucd, UCD_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
		    kmem_alloc_contig(kernel_map, &data, round_page(hc_slot_buffer), PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("ufs: native: no memory for slot buffers; the storage is read through the loader\n");
			return false;
		}
		s->number = SLOT_FIRST + 2 * i;
		s->utrd = hc_list + 8 * s->number;
		s->ucd = (uint8_t *)ucd;
		s->ucd_pa = kvtophys(ucd);
		s->data = (uint8_t *)data;
		s->data_pa = kvtophys(data);
		for (int w = 0; w < 8; w++) {
			s->original[w] = s->utrd[w];
		}
		FlushPoC_DcacheRegion(ucd, UCD_SIZE);
		FlushPoC_DcacheRegion(data, hc_slot_buffer);
	}
	hc_nslots = SLOT_MAX;
	hc_free = (1u << SLOT_MAX) - 1;
	hc_usable = true;
	printf("ufs: native: in use: slots 16, 18, .. 30 of UEFI's list at %llx, %u KB a request; UEFI keeps the rest\n",
	    hc_list_pa, hc_slot_buffer >> 10);
	return true;
}

/*
 * Before the kernel gives the phone back to the loader (loader_return, the other processors
 * already stopped): nothing more is sent, what is in flight is waited for, and the slots'
 * descriptors are put back as they were, so that UEFI finds its list as it left it.
 */
void
redwood_ufs_quiesce(void)
{
	uint64_t started = mach_absolute_time();
	uint32_t db = 0, is;

	if (!hc_usable) {
		return;
	}
	hc_closed = true;
	__asm__ volatile ("dsb sy" ::: "memory");
	while ((db = hc_read(UFSHC_UTRLDBR)) & SLOTS_RESERVED) {
		if (hc_us_since(started) > 2000000) {
			break;
		}
	}
	is = hc_read(UFSHC_IS);
	if (db & SLOTS_RESERVED) {
		printf("ufs: native: at the end, slots still busy after 2 s (doorbell %08x, IS %08x): the descriptors "
		    "are left as they are\n", db, is);
		return;
	}
	for (uint32_t i = 0; i < hc_nslots; i++) {
		struct ufshc_slot *s = &hc_slots[i];

		for (int w = 0; w < 8; w++) {
			s->utrd[w] = s->original[w];
		}
		FlushPoC_DcacheRegion((vm_offset_t)s->utrd, 32);
	}
	printf("ufs: native: at the end: none of slots 16..31 busy (doorbell %08x), their descriptors as they were; "
	    "IS %08x (at the probe %08x); %llu requests, %llu errors, %llu refused\n", db, is, hc_is_at_probe,
	    hc_requests, hc_errors, hc_refused);
}
