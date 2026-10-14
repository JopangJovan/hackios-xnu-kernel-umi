/*
 * Battery monitor for the POCO X5 Pro (redwood). This reads the live state-of-charge, battery
 * voltage, and battery current straight from the fuel-gauge peripheral inside PM7250B over
 * SPMI, so iOS can show real numbers in the status bar instead of a fake low-battery icon.
 *
 * Enabled only with boot-arg battery=1. Read-only: we do not touch the charger here, we do not
 * reconfigure any PMIC rail, we just poll a few registers and expose the result through
 * sysctl kern.redwood.battery_*. Writing to the charger is a separate project and a much
 * larger pile of code (sm8350-kernel-OKX ships ~3000 lines of qpnp-smb5 for that).
 *
 * Hardware layout: PM7250B lives at SPMI slave 2. The Qualcomm Gauge ("QG") peripheral is at
 * SPMI peripheral 0x48, so registers live at 0x48XX in that slave's address space. The
 * fuel-gauge chip ran its initial ADC burst back in Android before we yanked the system out
 * from under it, so its state-of-charge estimate is already current and keeps drifting with
 * use. We just read what the chip computed, we do not re-run the gauge algorithm. Register
 * meanings taken from drivers/power/supply/qcom/qg-reg.h in sm8350-kernel-OKX:
 *
 *   QG_SOC_MONOTONIC (0xBF)             -- 1 byte, 0..255 maps to 0..100 % (monotonic, i.e.
 *                                          never ticks up on discharge, never down on charge;
 *                                          this is what Android shows as "battery level")
 *   QG_LAST_ADC_V_DATA (0xC0, 0xC1)     -- 2 bytes LE, battery voltage in units of 194.6 uV
 *                                          (max 12.75 V, we never go anywhere near that)
 *   QG_LAST_ADC_I_DATA (0xC2, 0xC3)     -- 2 bytes LE, battery current, signed, LSB is 61 uA
 *                                          at the chip's default 5A range (negative = charging)
 *
 * SPMI itself is already set up by the buttons driver (redwood_buttons.c) which maps the PMIC
 * arbiter at step 1 and finds channels at step 2. We do the same two steps here independently
 * so this driver works even when buttons=0; it is a few kB of code to duplicate and it keeps
 * the two drivers from stepping on each other's state.
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (arbiter version 5),
 * drivers/power/supply/qcom/qpnp-qg.c and qg-reg.h, and the phone's own pm7250b.dtsi.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- PMIC arbiter (copied verbatim from redwood_buttons.c) ---------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define READ_TIMEOUT_US         2000

static vm_offset_t arb_core, arb_cnfg, arb_observer;

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral: the high byte of its register addresses */
	uint16_t        channel;
	bool            found;
};
static struct peripheral qg = { .name = "the PM7250B fuel gauge (QG)", .id = 0x248 };

/*
 * Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL on
 * success, or a short description of what went sideways. Verbatim copy of the buttons driver's
 * version -- the SPMI arbiter is the same regardless of what peripheral we poke at.
 */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status, data[2];
	const char *problem = NULL;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(channel + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);

	if (!(status & STATUS_DONE)) {
		problem = "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		problem = "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		problem = "the transfer failed (nobody at that address?)";
	} else if (status & STATUS_DROPPED) {
		problem = "the transfer was dropped";
	} else {
		data[0] = rd(channel + OBSERVER_DATA0);
		data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	return problem;
}

/* Scan the arbiter's channel table for the SPMI peripheral we want to talk to. */
static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));
		uint32_t owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		printf("battery: channel %u is %s (%#x); written by system %u\n", n, p->name, map, owner);
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
		}
	}
	if (!p->found) {
		printf("battery: no channel of the arbiter is %s (slave %u, peripheral %#x)\n",
		    p->name, p->id >> 8, p->id & 0xff);
	}
}

/* ---- QG (fuel gauge) registers ---------------------------------------------------------------- */

#define QG_PERIPH_OFFSET            0x00    /* registers 0x4800..0x48ff on PM7250B */
#define QG_REG_PERPH_TYPE           (QG_PERIPH_OFFSET + 0x04)
#define QG_REG_PERPH_SUBTYPE        (QG_PERIPH_OFFSET + 0x05)
#define QG_REG_STATUS1              (QG_PERIPH_OFFSET + 0x08)
#define QG_STATUS1_BATTERY_PRESENT  (1u << 0)
#define QG_STATUS1_QG_OK            (1u << 7)
#define QG_REG_SOC_MONOTONIC        (QG_PERIPH_OFFSET + 0xbf)
#define QG_REG_LAST_V_DATA_LSB      (QG_PERIPH_OFFSET + 0xc0)
#define QG_REG_LAST_I_DATA_LSB      (QG_PERIPH_OFFSET + 0xc2)
#define QG_TYPE                     0x0d    /* vendor PERPH_TYPE_QG */

/* Live state. These are updated by battery_thread and read by the sysctls below. */
static int battery_soc_percent = -1;    /* 0..100, or -1 if we have not read it yet */
static int battery_voltage_mv  = -1;    /* millivolts; signed only because -1 means unknown */
static int battery_current_ma  = 0;     /* negative = charging, positive = discharging, mA */
static int battery_present     = 0;     /* 1 if a battery is wired up, 0 otherwise */
static uint64_t battery_updates;        /* how many successful poll cycles so far */
static uint64_t battery_failures;       /* and failed ones */

/*
 * One polling cycle: read SOC, voltage, current, status. Each read is one SPMI transaction and
 * takes a few tens of microseconds, so the whole thing is fast enough that we can do it every
 * couple of seconds without noticing it in the system's cpu budget.
 */
static void
battery_poll(void)
{
	uint8_t soc = 0, status = 0, v[2] = { 0 }, i[2] = { 0 };
	const char *problem;

	problem = spmi_read(&qg, QG_REG_STATUS1, &status, 1);
	if (problem != NULL) {
		battery_failures++;
		return;
	}
	battery_present = (status & QG_STATUS1_BATTERY_PRESENT) ? 1 : 0;

	problem = spmi_read(&qg, QG_REG_SOC_MONOTONIC, &soc, 1);
	if (problem == NULL) {
		battery_soc_percent = (int)soc * 100 / 255;	/* the chip packs 0..100 % into 0..255 */
	}

	problem = spmi_read(&qg, QG_REG_LAST_V_DATA_LSB, v, 2);
	if (problem == NULL) {
		/* 2-byte LE raw ADC code; the chip's step is 194.6 uV per LSB. Keep math in integers:
		 * 1946 * raw / 10000 is millivolts. Range of a healthy single-cell li-ion is 3000..4400. */
		uint32_t raw = (uint32_t)v[0] | ((uint32_t)v[1] << 8);
		battery_voltage_mv = (int)((1946u * raw) / 10000u);
	}

	problem = spmi_read(&qg, QG_REG_LAST_I_DATA_LSB, i, 2);
	if (problem == NULL) {
		/* 2-byte LE SIGNED raw ADC code; LSB is 61 uA at the 5A range. Negative value = current
		 * is flowing INTO the battery (charging), positive = flowing OUT (discharging). */
		int16_t raw = (int16_t)((uint16_t)i[0] | ((uint16_t)i[1] << 8));
		battery_current_ma = (int)(((int32_t)raw * 61) / 1000);
	}

	battery_updates++;
}

/* ---- sysctl: kern.redwood.battery_* --------------------------------------------------------- */

SYSCTL_NODE(_kern, OID_AUTO, redwood, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current in milliamps (negative = charging)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if the fuel gauge sees a battery wired up, 0 otherwise");

/* ---- all of it together ---------------------------------------------------------------------- */

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version, channels;
	uint8_t subtype;
	const char *problem;

	printf("battery: step 1: map the PMIC arbiter and read its version\n");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	printf("battery: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("battery: this driver knows version 5 of the arbiter only; giving up\n");
		thread_terminate(current_thread());
		return;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}

	printf("battery: step 2: find the fuel-gauge channel in the arbiter\n");
	find_channel(&qg, channels);
	if (!qg.found) {
		thread_terminate(current_thread());
		return;
	}

	printf("battery: step 3: confirm the fuel-gauge peripheral is where we think it is\n");
	problem = spmi_read(&qg, QG_REG_PERPH_TYPE, &subtype, 1);
	if (problem != NULL) {
		printf("battery: cannot read the peripheral type (%s)\n", problem);
		thread_terminate(current_thread());
		return;
	}
	printf("battery: peripheral type is %#x (expected %#x for the fuel gauge)\n", subtype, QG_TYPE);

	printf("battery: step 4: polling every 5 seconds\n");
	for (unsigned tick = 0;; tick++) {
		battery_poll();
		if (tick % 12 == 0) {		/* once a minute at 5-second cadence */
			printf("battery: SOC=%d%%, V=%d mV, I=%d mA, %spresent (%llu reads, %llu failed)\n",
			    battery_soc_percent, battery_voltage_mv, battery_current_ma,
			    battery_present ? "" : "not ", battery_updates, battery_failures);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
redwood_battery_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
