/*
 * The cameras of the POCO X5 Pro (redwood), first the macro one: an OmniVision OV02B10 (2 MP,
 * Android's camera 2, "redwood_aac_ov02b10_macro"). Every address and number below belongs to this
 * phone, from Android's own device tree (cam-sensor2 and what it points to), the sensor's module
 * file of Android's camera stack (/vendor/lib64/camera/com.qti.sensormodule.redwood_aac_ov02b10_
 * macro.bin: the slave address, the chip ID and where it is read, the power sequence) and the
 * drivers of the phone's kernel (MiCode redwood-s-oss: cam_sensor_util.c, qcom_pm8008-regulator.c,
 * camcc-yupik.c, gdsc-regulator.c, pinctrl-yupik.c, pinctrl-spmi-gpio.c), read as data:
 *
 *   the sensor     on CCI 1, master 0 (GPIO 73 data, 74 clock, function cci_i2c, internal pull-ups),
 *                  slave address 0x3c; done by hand here, as the other drivers do their I2C (the
 *                  two pins taken for each transaction and given back as they were found)
 *   chip ID        Android reads it as a 16-bit register at the 16-bit address 0x0203 and wants
 *                  0x002b (mask 0xffff); done the same way here
 *   reset          GPIO 77, high = running
 *   MCLK           camcc's mclk2 (RCG 0xe038, branch 0xe050) at 19.2 MHz on GPIO 66 (function
 *                  cam_mclk, 4 mA); Android takes the 19.2 MHz from camcc's PLL2 (M/N 1/75), here
 *                  it is the crystal's own 19.2 MHz (the RCG's source 0), with no PLL to start.
 *                  Before it Android switches on the camera's top power domain ("cam_clk":
 *                  titan_top_gdsc, camcc 0xc194), and so does this.
 *   VDIG (DOVDD)   PM8008i's LDO6 at 1.8 V; VANA (AVDD) its LDO7 at 2.8 V (the device tree's
 *                  voltages, normal power mode for the load Android asks). The PM8008i is on the
 *                  serial engine 1's I2C (GPIO 4 data, 5 clock; addresses 0x08 for the chip, 0x09
 *                  for the regulators, 16-bit registers), done by hand too; its reset pin is
 *                  PM8350C GPIO 3 (output high, as Android's pinctrl "pm8008i_active" has it)
 *   power-up       (the module file's): reset low, 1 ms; VDIG on; VANA on, 2 ms; MCLK on, 4 ms;
 *                  reset high, 9 ms. Power-down: MCLK off; reset low, 1 ms; VANA off, 7 ms; VDIG off.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   cam=1   a look, reading only: the camera's clocks and power domain as they are, the pins, the
 *           PM8008i's reset pin, and if it is out of reset the PM8008i itself (its chip enable and
 *           the two LDOs)
 *   cam=2   ... and the macro sensor powered up as Android powers it, its chip ID read, and powered
 *           down again (every write read back)
 *   cam=3   the CSID's test pattern through RDI 0 and IFE-lite 0 into memory, which the loader hands
 *           to the host after the run (xl camera): the camera subsystem's rails, PLL, clocks, SMMU
 *           and blocks without the sensor (capture_tpg() below)
 *   cam=4   the macro sensor's own frames that way, through CSIPHY 2 (capture_sensor() below)
 *
 * Limits, whatever asks: Android's voltages only (1.8 and 2.8 V; nothing else is ever written to
 * the LDOs' set points); only the registers named above are written (a list each, checked at every
 * write) and each is read back; any write that does not read back, a regulator that is not ready
 * in time or reports over-current, a byte nobody acknowledges, and everything is switched off again
 * in the power-down's order and nothing more is done. At the run's end (and on a panic's way out)
 * whatever was switched on is switched off. Everything that was found switched on is left on.
 * The camera subsystem's own registers (camcc's PLL0 and the clocks named there, CSIPHY 2, CSID-lite
 * 0, IFE-lite 0, one match register and context of the SMMU) are written the same way, read back
 * where they keep what they are given, and taken back in the reverse order.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <sys/systm.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != '\0'; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

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

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("camera: %s: nothing more is written in this run\n", why);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)
#define CFG_FUNCTION(f)         ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)

#define PIN_PMIC_SDA            4       /* the serial engine 1's I2C: the PM8008i */
#define PIN_PMIC_SCL            5
#define PIN_MCLK                66      /* function 1: cam_mclk */
#define PIN_CCI_SDA             73      /* CCI 1 master 0 (function 1: cci_i2c) */
#define PIN_CCI_SCL             74
#define PIN_RESET               77

static vm_offset_t tlmm;

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("camera: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* The pins written while powered up, as they were found, to be put back: the reset pin and MCLK's. */
static uint32_t found_reset_cfg, found_reset_io, found_mclk_cfg;
static bool pins_found;

static void
reset_pin(bool high)
{
	wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT | CFG_DRIVE_MA(2));         /* GPIO, no pull, 2 mA: Android's cam_sensor_active_macro */
}

/* ---- I2C by hand (as in redwood_haptic.c) ---------------------------------------------------------- */

struct bus {
	const char *    name;
	unsigned        sda, scl;
	uint32_t        cfg[2], io[2];  /* the pins as they were found, while taken */
};
static struct bus pmic_bus = { .name = "the PM8008i's I2C", .sda = PIN_PMIC_SDA, .scl = PIN_PMIC_SCL };
static struct bus cci_bus = { .name = "CCI 1 master 0", .sda = PIN_CCI_SDA, .scl = PIN_CCI_SCL };

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(struct bus *b)
{
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	line(b->sda, false);
	half_bit();
	line(b->scl, false);
	half_bit();
}

static void
i2c_stop(struct bus *b)
{
	line(b->sda, false);
	half_bit();
	line(b->scl, true);
	half_bit();
	line(b->sda, true);
	half_bit();
}

static bool
i2c_bit(struct bus *b, bool out)
{
	bool in;

	line(b->sda, out);
	half_bit();
	line(b->scl, true);
	for (unsigned stretched = 0; !level_of(b->scl) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(b->sda);
	line(b->scl, false);
	return in;
}

static bool
i2c_write(struct bus *b, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit(b, (byte >> bit) & 1);
	}
	return !i2c_bit(b, true);
}

static uint8_t
i2c_read(struct bus *b, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(b, true));
	}
	i2c_bit(b, !more);
	return byte;
}

static void
bus_give_back(struct bus *b)
{
	wr(GPIO_IO(b->sda), b->io[0] & 2);
	wr(GPIO_IO(b->scl), b->io[1] & 2);
	wr(GPIO_CFG(b->sda), b->cfg[0]);
	wr(GPIO_CFG(b->scl), b->cfg[1]);
}

static bool
bus_take(struct bus *b)
{
	b->cfg[0] = rd(GPIO_CFG(b->sda));
	b->cfg[1] = rd(GPIO_CFG(b->scl));
	b->io[0] = rd(GPIO_IO(b->sda));
	b->io[1] = rd(GPIO_IO(b->scl));
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	if (!level_of(b->sda) || !level_of(b->scl)) {
		bool sda = level_of(b->sda), scl = level_of(b->scl);

		bus_give_back(b);
		say("camera: %s is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", b->name, b->sda, sda, b->scl, scl);
		return false;
	}
	return true;
}

/*
 * A transaction: `out' (the register's address, and for a write the data) written, then for a read
 * `in_count' bytes read after a repeated start. False if anybody did not acknowledge.
 */
static bool
i2c_transfer(struct bus *b, uint8_t address, const uint8_t *out, unsigned out_count, uint8_t *in, unsigned in_count)
{
	bool answered;

	if (!bus_take(b)) {
		return false;
	}
	i2c_start(b);
	answered = i2c_write(b, (uint8_t)(address << 1));
	for (unsigned i = 0; answered && i < out_count; i++) {
		answered = i2c_write(b, out[i]);
	}
	if (answered && in_count != 0) {
		i2c_start(b);
		answered = i2c_write(b, (uint8_t)(address << 1 | 1));
		for (unsigned i = 0; answered && i < in_count; i++) {
			in[i] = i2c_read(b, i + 1 < in_count);
		}
	}
	i2c_stop(b);
	bus_give_back(b);
	return answered;
}

/* ---- the PMIC arbiter (version 5), for the PM8350C's GPIO 3: as in redwood_torch.c ------------------ */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0

#define ARB_VERSION             0x000
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

#define PGPIO_ID                0x28a   /* the PM8350C (slave 2), GPIO 3's peripheral (0x8800 + 0x100 x 2) */
#define PGPIO_TYPE              0x04    /* 0x10: a GPIO */
#define PGPIO_SUBTYPE           0x05
#define PGPIO_RT_STS            0x10    /* [0] what the pin is at */
#define PGPIO_MODE_CTL          0x40    /* LV/MV: [1:0] direction (1 output) */
#define PGPIO_DIG_VIN_CTL       0x41    /* [2:0] which supply: 0 = VIN0 ("power-source 0") */
#define PGPIO_DIG_PULL_CTL      0x42    /* [2:0] 5 = no pull */
#define PGPIO_OUT_SOURCE_CTL    0x44    /* LV/MV: [7] the level (inverted "normal"), [3:0] the function: 0 normal */
#define PGPIO_EN_CTL            0x46    /* [7] enabled */

static vm_offset_t arb_core, arb_cnfg, arb_observer, pgpio_write;
static uint16_t pgpio_channel;
static bool pgpio_found, pgpio_writable;

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static void
lock(void)
{
	if (!leaving) {
		redwood_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		redwood_spmi_unlock();
	}
}

static const char *
pgpio_read(uint8_t reg, uint8_t *value)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * pgpio_channel;
	const char *problem = "no channel";

	for (int tries = 0; pgpio_found && tries < 3; tries++) {
		lock();
		wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4);
		problem = wait_done(channel);
		*value = (uint8_t)rd(channel + OBSERVER_DATA0);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/* One of GPIO 3's five registers above written and read back (when `force', also after something went wrong). */
static bool
pgpio_put(uint8_t reg, uint8_t value, bool force)
{
	const char *problem = NULL;
	uint8_t back = 0xff;

	if ((broken && !force) || !pgpio_writable || pgpio_write == 0 || (reg != PGPIO_MODE_CTL && reg != PGPIO_DIG_VIN_CTL &&
	    reg != PGPIO_DIG_PULL_CTL && reg != PGPIO_OUT_SOURCE_CTL && reg != PGPIO_EN_CTL)) {
		give_up("a write to the PM8350C that may not be made");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(pgpio_channel)) >> 8) & 0xfff) != PGPIO_ID) {
		give_up("the arbiter's channel is no longer the PM8350C's GPIO 3");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(pgpio_write + CHANNEL_WDATA0, value);
		wr(pgpio_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(pgpio_write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = pgpio_read(reg, &back)) != NULL || back != value) {
		say("camera: PM8350C GPIO 3 %#04x <- %#04x: %s (reads %#04x)\n", reg, value, problem ? problem : "does not read back", back);
		give_up("a write to the PM8350C that failed");
		return false;
	}
	return true;
}

static void
pgpio_find(void)
{
	for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != PGPIO_ID) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("camera: channel %u is the PM8350C's GPIO 3 (%#x); written by system %u%s\n", n, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!pgpio_found || owner == ARB_SYSTEM) {
			pgpio_found = true;
			pgpio_channel = (uint16_t)n;
			pgpio_writable = owner == ARB_SYSTEM;
		}
	}
	if (pgpio_found && pgpio_writable) {
		pgpio_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * pgpio_channel, 0x1000);
	}
}

/* GPIO 3 as Android's "pm8008i_active" has it: enabled, an output of the normal function, high, VIN0, no pull. */
static uint8_t pgpio_found_regs[5];             /* DIG_VIN_CTL, DIG_PULL_CTL, MODE_CTL, OUT_SOURCE_CTL, EN_CTL as found */
static bool pgpio_changed;

static bool
pgpio_active(uint8_t *regs)
{
	return (regs[0] & 7) == 0 && (regs[1] & 7) == 5 && (regs[2] & 3) == 1 && regs[3] == 0x80 && (regs[4] & 0x80);
}

static const uint8_t pgpio_which[5] = { PGPIO_DIG_VIN_CTL, PGPIO_DIG_PULL_CTL, PGPIO_MODE_CTL, PGPIO_OUT_SOURCE_CTL, PGPIO_EN_CTL };

static bool
pgpio_read_all(uint8_t *regs)
{
	for (int i = 0; i < 5; i++) {
		if (pgpio_read(pgpio_which[i], &regs[i]) != NULL) {
			return false;
		}
	}
	return true;
}

/* ---- the PM8008i ----------------------------------------------------------------------------------- */

#define PM8008_CHIP             0x08    /* its I2C addresses: the chip ... */
#define PM8008_REGULATORS       0x09    /* ... and its regulators */
#define REVID_BASE              0x0100
#define MISC_CHIP_ENABLE        0x0950  /* [0] */
#define LDO_BASE(n)             (0x4000 + 0x100 * ((n) - 1))
#define LDO_STATUS1             0x08    /* [7] ready, [5] over-current, [1:0] mode: 3 normal, 2 low power */
#define LDO_VSET_LB             0x40    /* the set point in mV, low byte ... */
#define LDO_VSET_UB             0x41    /* ... high byte */
#define LDO_MODE_CTL1           0x45    /* [2:0] 7 normal, 4 low power */
#define LDO_ENABLE              0x46    /* [7] */
#define LDO_VDIG                6
#define LDO_VANA                7
#define VDIG_MV                 1800    /* the device tree's (pm8008i_l6, 1.8 V; a multiple of the 8 mV step) */
#define VANA_MV                 2800    /* (pm8008i_l7 for cam-sensor2: 2.8 V) */

static bool
pm8008_get(uint8_t address, uint16_t reg, uint8_t *values, unsigned count)
{
	uint8_t where[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };

	return i2c_transfer(&pmic_bus, address, where, 2, values, count);
}

/* The registers the PM8008i may be written in: the chip enable, and the set point, mode and enable of LDO6 and LDO7. */
static bool
pm8008_put(uint8_t address, uint16_t reg, uint8_t value, bool force)
{
	uint8_t out[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value }, back = 0xff;
	uint16_t base = reg & 0xff00, offset = reg & 0xff;
	bool allowed = (address == PM8008_CHIP && reg == MISC_CHIP_ENABLE) ||
	    (address == PM8008_REGULATORS && (base == LDO_BASE(LDO_VDIG) || base == LDO_BASE(LDO_VANA)) &&
	    (offset == LDO_MODE_CTL1 || offset == LDO_ENABLE || offset == LDO_VSET_LB || offset == LDO_VSET_UB));

	if ((broken && !force) || !allowed) {
		give_up(allowed ? "a write to the PM8008i after a fault" : "a PM8008i register that is not to be written");
		return false;
	}
	if (!i2c_transfer(&pmic_bus, address, out, 3, NULL, 0) || !pm8008_get(address, reg, &back, 1) || back != value) {
		say("camera: PM8008i %#04x:%#06x <- %#04x: %s (reads %#04x)\n", address, reg, value, back == 0xff ? "no answer" :
		    "does not read back", back);
		give_up("a write to the PM8008i that failed");
		return false;
	}
	return true;
}

static void
show_ldo(unsigned n)
{
	uint8_t status = 0, vset[2] = { 0, 0 }, mode = 0, enable = 0;

	if (pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_VSET_LB), vset, 2) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_MODE_CTL1), &mode, 1) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), &enable, 1)) {
		say("camera: PM8008i LDO%u: status %#04x (%sready%s, mode %u), set to %u mV, mode control %#04x, enable %#04x\n", n,
		    status, status & 0x80 ? "" : "not ", status & 0x20 ? ", OVER-CURRENT" : "", status & 3, vset[1] << 8 | vset[0], mode,
		    enable);
	} else {
		say("camera: PM8008i LDO%u: no answer\n", n);
	}
}

/*
 * An LDO on as Android's regulator framework puts it on (set point, load -> normal mode, enable),
 * then ready within 10 tries of the driver's wait, and not over its current.
 */
static bool ldo_was_on[8], ldo_switched[8];

static bool
ldo_on(unsigned n, unsigned mv)
{
	uint16_t base = (uint16_t)LDO_BASE(n);
	uint8_t enable = 0, mode = 0, status = 0;

	if ((n != LDO_VDIG || mv != VDIG_MV) && (n != LDO_VANA || mv != VANA_MV)) {
		give_up("a voltage that is not Android's");
		return false;
	}
	if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_ENABLE), &enable, 1) ||
	    !pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_MODE_CTL1), &mode, 1)) {
		give_up("a PM8008i that does not answer");
		return false;
	}
	ldo_was_on[n] = enable & 0x80;
	if (ldo_was_on[n]) {
		say("camera: PM8008i LDO%u was on already: left as it is\n", n);
		return true;
	}
	if (!pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_VSET_LB), (uint8_t)mv, false) ||
	    !pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_VSET_UB), (uint8_t)(mv >> 8), false) ||
	    !pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_MODE_CTL1), (uint8_t)((mode & ~7) | 7), false)) {
		return false;
	}
	ldo_switched[n] = true;
	if (!pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_ENABLE), (uint8_t)(enable | 0x80), false)) {
		return false;
	}
	for (int tries = 0; tries < 10; tries++) {
		spin_us(20 + mv * 1000 / 38400 + 1);   /* the driver's wait: 20 us and the set point at the stepper's 38.4 mV/us */
		if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_STATUS1), &status, 1)) {
			give_up("a PM8008i that does not answer");
			return false;
		}
		if (status & 0x20) {
			say("camera: PM8008i LDO%u reports over-current (status %#04x)\n", n, status);
			give_up("an LDO over its current");
			return false;
		}
		if (status & 0x80) {
			say("camera: PM8008i LDO%u on at %u mV (status %#04x)\n", n, mv, status);
			return true;
		}
	}
	say("camera: PM8008i LDO%u not ready (status %#04x)\n", n, status);
	give_up("an LDO that is not ready");
	return false;
}

static void
ldo_off(unsigned n)
{
	uint8_t enable = 0;

	if (!ldo_switched[n]) {
		return;
	}
	if (pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), &enable, 1) &&
	    pm8008_put(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), (uint8_t)(enable & ~0x80), true)) {
		ldo_switched[n] = false;
		say("camera: PM8008i LDO%u off\n", n);
	} else {
		say("camera: PM8008i LDO%u COULD NOT BE SWITCHED OFF\n", n);
	}
}

/* ---- the camera's clocks: camcc --------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_CAMERA_AHB_CBCR     0x26004         /* always on (gcc-yupik.c keeps it so) */
#define GCC_CAMERA_XO_CBCR      0x26028
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x20000UL
#define MCLK2_CMD_RCGR          0xe038          /* [0] update, [31] the root is off */
#define MCLK2_CFG_RCGR          0xe03c          /* [4:0] divider (2n - 1), [10:8] source (0: the crystal), [13:12] M/N mode */
#define MCLK2_M                 0xe040
#define MCLK2_N                 0xe044
#define MCLK2_D                 0xe048
#define MCLK2_CBCR              0xe050          /* [0] enable, [31] the clock is off */
#define TITAN_TOP_GDSCR         0xc194          /* [0] collapse, [11] keep the registers, [31] on */
#define CBCR_OFF                (1u << 31)
#define CMD_UPDATE              1u
#define GDSC_COLLAPSE           1u
#define GDSC_ON                 (1u << 31)

static vm_offset_t gcc, camcc;
static uint32_t found_mclk_cfg_rcgr, found_mclk_cbcr, found_gdscr;
static bool gdsc_switched, mclk_switched;

static void
show_clocks(void)
{
	say("camera: GCC: camera AHB %#x, camera XO %#x\n", rd(gcc + GCC_CAMERA_AHB_CBCR), rd(gcc + GCC_CAMERA_XO_CBCR));
	say("camera: camcc: titan top GDSC %#x; mclk2 RCG cmd %#x cfg %#x m %#x n %#x d %#x, branch %#x\n", rd(camcc + TITAN_TOP_GDSCR),
	    rd(camcc + MCLK2_CMD_RCGR), rd(camcc + MCLK2_CFG_RCGR), rd(camcc + MCLK2_M), rd(camcc + MCLK2_N), rd(camcc + MCLK2_D),
	    rd(camcc + MCLK2_CBCR));
}

/* camcc's registers: only these, each read back (an RCG's update bit and a GDSC's status by waiting for them) */
static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check_mask)
{
	if (offset != TITAN_TOP_GDSCR && offset != MCLK2_CFG_RCGR && offset != MCLK2_CMD_RCGR && offset != MCLK2_CBCR) {
		give_up("a camcc register that is not to be written");
		return false;
	}
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check_mask) != (value & check_mask)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_bits(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(camcc + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

static bool
gdsc_on(void)
{
	found_gdscr = rd(camcc + TITAN_TOP_GDSCR);
	if (!(found_gdscr & GDSC_COLLAPSE) && (found_gdscr & GDSC_ON)) {
		say("camera: the titan top power domain was on already\n");
		return true;
	}
	if (!camcc_put(TITAN_TOP_GDSCR, found_gdscr & ~GDSC_COLLAPSE, GDSC_COLLAPSE)) {
		return false;
	}
	gdsc_switched = true;
	spin_us(1);
	if (!wait_bits(TITAN_TOP_GDSCR, GDSC_ON, GDSC_ON, 100)) {
		say("camera: the titan top power domain does not come on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
		give_up("a power domain that does not come on");
		return false;
	}
	say("camera: the titan top power domain on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
	return true;
}

static void
gdsc_off(void)
{
	if (!gdsc_switched) {
		return;
	}
	wr(camcc + TITAN_TOP_GDSCR, rd(camcc + TITAN_TOP_GDSCR) | GDSC_COLLAPSE);
	gdsc_switched = false;
	say("camera: the titan top power domain %s (%#x)\n", wait_bits(TITAN_TOP_GDSCR, GDSC_ON, 0, 100) ? "off" : "NOT OFF",
	    rd(camcc + TITAN_TOP_GDSCR));
}

/* MCLK at 19.2 MHz: the RCG from the crystal, divider 1, no M/N; then the branch; then the pin. */
static bool
mclk_on(void)
{
	found_mclk_cfg_rcgr = rd(camcc + MCLK2_CFG_RCGR);
	found_mclk_cbcr = rd(camcc + MCLK2_CBCR);
	if (found_mclk_cbcr & 1) {
		give_up("MCLK2 is on already (somebody else's)");
		return false;
	}
	if (!camcc_put(MCLK2_CFG_RCGR, (found_mclk_cfg_rcgr & ~0x371fu) | (0u << 8) | 1u, 0x371f) ||
	    !camcc_put(MCLK2_CMD_RCGR, rd(camcc + MCLK2_CMD_RCGR) | CMD_UPDATE, 0)) {
		return false;
	}
	if (!wait_bits(MCLK2_CMD_RCGR, CMD_UPDATE, 0, 500)) {
		say("camera: MCLK2's RCG does not take its configuration (cmd %#x)\n", rd(camcc + MCLK2_CMD_RCGR));
		give_up("an RCG that does not update");
		return false;
	}
	mclk_switched = true;
	if (!camcc_put(MCLK2_CBCR, found_mclk_cbcr | 1, 1)) {
		return false;
	}
	if (!wait_bits(MCLK2_CBCR, CBCR_OFF, 0, 500)) {
		say("camera: MCLK2's branch does not come on (%#x)\n", rd(camcc + MCLK2_CBCR));
		give_up("a clock that does not come on");
		return false;
	}
	wr(GPIO_CFG(PIN_MCLK), CFG_FUNCTION(1) | CFG_DRIVE_MA(4));    /* cam_mclk, no pull, 4 mA: cam_macro_mclk_active */
	say("camera: MCLK2 on at 19.2 MHz (RCG cfg %#x, branch %#x)\n", rd(camcc + MCLK2_CFG_RCGR), rd(camcc + MCLK2_CBCR));
	return true;
}

static void
mclk_off(void)
{
	if (!mclk_switched) {
		return;
	}
	if (pins_found) {
		wr(GPIO_CFG(PIN_MCLK), found_mclk_cfg);
	}
	wr(camcc + MCLK2_CBCR, rd(camcc + MCLK2_CBCR) & ~1u);
	(void)wait_bits(MCLK2_CBCR, CBCR_OFF, CBCR_OFF, 500);
	wr(camcc + MCLK2_CFG_RCGR, found_mclk_cfg_rcgr);
	wr(camcc + MCLK2_CMD_RCGR, rd(camcc + MCLK2_CMD_RCGR) | CMD_UPDATE);
	(void)wait_bits(MCLK2_CMD_RCGR, CMD_UPDATE, 0, 500);
	mclk_switched = false;
	say("camera: MCLK2 off (branch %#x, RCG cfg %#x as found)\n", rd(camcc + MCLK2_CBCR), rd(camcc + MCLK2_CFG_RCGR));
}

/* ---- the macro sensor -------------------------------------------------------------------------------- */

#define OV02B10_ADDRESS         0x3c            /* the module file's slave address 0x78, as 7 bits */
#define OV02B10_ID_ADDRESS      0x0203          /* ... read as Android reads it: 16-bit address, 16-bit data */
#define OV02B10_ID              0x002b
#define OV02B10_ID_MASK         0xffff

static bool chip_enable_switched, powered, reset_driven;

/* Everything that was switched on, off again in the module file's power-down order (MCLK; reset low, 1 ms; VANA, 7 ms; VDIG). */
static void
power_down(void)
{
	bool was = powered || mclk_switched || ldo_switched[LDO_VANA] || ldo_switched[LDO_VDIG] || gdsc_switched || reset_driven ||
	    chip_enable_switched || pgpio_changed;

	if (!was) {
		return;
	}
	say("camera: powering down\n");
	mclk_off();
	gdsc_off();
	if (reset_driven) {
		reset_pin(false);
		wait_ms(1);
	}
	ldo_off(LDO_VANA);
	wait_ms(7);
	ldo_off(LDO_VDIG);
	if (chip_enable_switched) {
		uint8_t enable = 0;

		if (pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1) &&
		    pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, (uint8_t)(enable & ~1), true)) {
			say("camera: the PM8008i's chip enable back off\n");
		} else {
			say("camera: the PM8008i's chip enable COULD NOT BE PUT BACK\n");
		}
		chip_enable_switched = false;
	}
	if (pgpio_changed) {
		/* (in the reverse order of the writes: disabled first) */
		uint8_t now[5];
		bool all = pgpio_read_all(now);

		for (int i = 4; all && i >= 0; i--) {
			if (now[i] != pgpio_found_regs[i]) {
				all &= pgpio_put(pgpio_which[i], pgpio_found_regs[i], true);
			}
		}
		say("camera: the PM8350C's GPIO 3 %s\n", all ? "back as it was found" : "COULD NOT BE PUT BACK");
		pgpio_changed = false;
	}
	if (reset_driven && pins_found) {
		wr(GPIO_IO(PIN_RESET), found_reset_io & 2);
		wr(GPIO_CFG(PIN_RESET), found_reset_cfg);
		reset_driven = false;
	}
	powered = false;
	say("camera: powered down\n");
}

/* The PM8008i out of reset (its reset pin as Android drives it) and enabled. */
static bool
pm8008_up(void)
{
	uint8_t regs[5], revid[4] = { 0 }, enable = 0;

	if (!pgpio_read_all(regs)) {
		give_up("the PM8350C's GPIO 3 cannot be read");
		return false;
	}
	memcpy(pgpio_found_regs, regs, sizeof(regs));
	if (!pgpio_active(regs)) {
		say("camera: the PM8008i is held in reset (GPIO 3 vin, pull, mode, source, enable: %02x %02x %02x %02x %02x): let out as "
		    "Android lets it out\n", regs[0],
		    regs[1], regs[2], regs[3], regs[4]);
		uint8_t want[5] = { (uint8_t)(regs[0] & ~7), (uint8_t)((regs[1] & ~7) | 5), (uint8_t)((regs[2] & ~3) | 1), 0x80,
			            (uint8_t)(regs[4] | 0x80) };

		pgpio_changed = true;
		for (int i = 0; i < 5; i++) {
			/* (only what differs, in the order Android's pinctrl driver writes them: the enable last) */
			if (want[i] != regs[i] && !pgpio_put(pgpio_which[i], want[i], false)) {
				return false;
			}
		}
		wait_ms(5);
	}
	if (!pm8008_get(PM8008_CHIP, REVID_BASE, revid, 4)) {
		give_up("the PM8008i does not answer");
		return false;
	}
	say("camera: the PM8008i answers: revision %02x %02x %02x %02x\n", revid[0], revid[1], revid[2], revid[3]);
	if (!pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1)) {
		give_up("the PM8008i does not answer");
		return false;
	}
	if (!(enable & 1)) {
		if (!pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, (uint8_t)(enable | 1), false)) {
			return false;
		}
		chip_enable_switched = true;
		say("camera: the PM8008i's chip enabled\n");
	}
	return true;
}

/* The module file's power-up: reset low, 1 ms; VDIG; VANA, 2 ms; MCLK (after the top power domain), 4 ms; reset high, 9 ms. */
static bool
power_up(void)
{
	powered = true;
	if (!pm8008_up()) {
		return false;
	}
	reset_driven = true;
	reset_pin(false);
	wait_ms(1);
	if (!ldo_on(LDO_VDIG, VDIG_MV) || !ldo_on(LDO_VANA, VANA_MV)) {
		return false;
	}
	wait_ms(2);
	if (!gdsc_on() || !mclk_on()) {
		return false;
	}
	wait_ms(4);
	reset_pin(true);
	wait_ms(9);
	say("camera: the macro sensor powered up\n");
	return true;
}

static bool
read_chip_id(void)
{
	uint8_t where[2] = { OV02B10_ID_ADDRESS >> 8, OV02B10_ID_ADDRESS & 0xff }, id[2] = { 0, 0 };
	uint16_t chip;

	if (!i2c_transfer(&cci_bus, OV02B10_ADDRESS, where, 2, id, 2)) {
		say("camera: the macro sensor does not answer at %#04x\n", OV02B10_ADDRESS);
		return false;
	}
	chip = (uint16_t)(id[0] << 8 | id[1]);
	say("camera: the macro sensor's chip ID: %#06x (%s)\n", chip, (chip & OV02B10_ID_MASK) == OV02B10_ID ?
	    "an OV02B10, as Android expects" : "NOT what Android expects");
	return (chip & OV02B10_ID_MASK) == OV02B10_ID;
}

/* While powered up: the two LDOs still ready and not over their current. */
static bool
healthy(void)
{
	for (unsigned n = LDO_VDIG; n <= LDO_VANA; n++) {
		uint8_t status = 0;

		if (!ldo_was_on[n] && (!pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) ||
		    !(status & 0x80) || (status & 0x20))) {
			say("camera: PM8008i LDO%u: status %#04x\n", n, status);
			give_up("an LDO that is no longer well");
			return false;
		}
	}
	return true;
}

/* ---- the camera subsystem's surroundings, read only: rails, the SMMU, camcc's PLLs ------------------- */

/*
 * What the camera's blocks need beyond the sensor (a look first, nothing written): the CX and MX
 * rails' levels (camcc's clocks at Android's rates want CX at SVS and, for PLL0's 1.2 GHz, MX at
 * SVS_L1), asked of RPMh by level positions from the boot loader's command database; what the
 * application processors' RPMh sequencer (DRV 2) holds for them; the two rails' set points in their
 * regulators (PM8350C S2 and S10); what the SMMU has for the IFEs' streams (dt: 0x800 and 0xc00 with
 * the mask 0x4e0); camcc's PLL0 and PLL6.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* DRV 2's (dt: rsc@18200000, drv-id 2) */
#define RSC_TCS_BASE            0xd00
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define RSC_TCS_COUNT           8               /* dt qcom,tcs-config: 2 active, 3 sleep, 3 wake */

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

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
static const struct cmd_db_header *cmd_db;
static vm_offset_t rsc;

/* RPMh's address for a resource, and its levels (as many as `max'); 0: not there. */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned max, unsigned *count)
{
	char id[8] = { 0 };

	*count = 0;
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return entry[i].address;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			for (unsigned l = 0; l < entry[i].length / 2u && l < max; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rail(const char *name, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;
	char line[200];
	int at;

	*address = rpmh_resource(name, levels, 16, &count);
	at = snprintf(line, sizeof(line), "camera: RPMh's %s at %#x, levels:", name, *address);
	for (unsigned l = 0; l < count && at < (int)sizeof(line) - 8; l++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", levels[l]);
	}
	say("%s\n", line);
	/* what DRV 2's sequencer holds for it, in any of its TCSs */
	for (unsigned t = 0; *address != 0 && t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = rd(rsc + RSC_TCS(t) + TCS_ENABLED);

		for (unsigned c = 0; c < 16; c++) {
			uint32_t where = rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 4);

			if (where == *address) {
				say("camera:   TCS %u command %u: value %u (position), %senabled, message id %#x, status %#x\n", t, c,
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 8), (enabled >> c) & 1 ? "" : "not ",
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c)), rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 12));
			}
		}
	}
}

/* A PMIC peripheral's registers, read through the arbiter's observer (any channel that is that peripheral's). */
static bool
pmic_peek(uint16_t id, uint8_t reg, uint8_t *values, unsigned count)
{
	for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		for (unsigned i = 0; i < count; i++) {
			vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * n;

			lock();
			wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)(reg + i) << 4);
			const char *problem = wait_done(channel);
			values[i] = (uint8_t)rd(channel + OBSERVER_DATA0);
			unlock();
			if (problem != NULL) {
				return false;
			}
		}
		return true;
	}
	return false;
}

static void
show_smps(const char *what, uint16_t id)
{
	uint8_t head[2] = { 0, 0 }, set[4] = { 0, 0, 0, 0 }, status[2] = { 0, 0 };

	if (!pmic_peek(id, 0x04, head, 2) || !pmic_peek(id, 0x40, set, 4) || !pmic_peek(id, 0x08, status, 2)) {
		say("camera: %s (peripheral %#x): cannot be read\n", what, id);
		return;
	}
	say("camera: %s (peripheral %#x): type %#04x subtype %#04x, status %02x %02x, 0x40-0x43: %02x %02x %02x %02x (%u mV?)\n",
	    what, id, head[0], head[1], status[0], status[1], set[0], set[1], set[2], set[3], set[1] << 8 | set[0]);
}

#define SMMU_PHYS               0x15000000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define IFE_STREAM              0x800                   /* dt: msm_cam_smmu_ife, iommus 0x800.. 0xce0, mask 0x4e0 */
#define IFE_STREAM_MASK         0x4e0

static vm_offset_t smmu;

static void
show_smmu(void)
{
	uint32_t scr0 = rd(smmu + SMMU_SCR0), idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), free_matches = 0;
	bool used[256] = { false };
	unsigned contexts = idr1 & 0xff, free_contexts = 0;

	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (!(smr >> 31)) {
			free_matches++;
			continue;
		}
		used[s2cr & 0xff] = true;
		for (uint32_t ife = 0; ife < 0x10; ife++) {
			uint32_t sid = (ife < 8 ? 0x800 : 0xc00) + 0x20 * (ife & 7);

			if (((stream ^ sid) & ~mask) == 0) {
				say("camera: SMMU match register %u (stream %#x mask %#x, s2cr %#x) covers the IFEs' stream %#x\n", n, stream,
				    mask, s2cr, sid);
				break;
			}
		}
	}
	for (unsigned c = 0; c < contexts; c++) {
		free_contexts += !used[c];
	}
	say("camera: the SMMU: scr0 %#x (a stream nobody matches %s), %u match registers free, %u of %u contexts unused by them\n", scr0,
	    scr0 & (1u << 10) ? "faults" : "passes", free_matches, free_contexts, contexts);
}

static void
show_plls(void)
{
	for (unsigned p = 0; p <= 6; p += 6) {
		vm_offset_t pll = camcc + 0x1000UL * p;

		say("camera: camcc PLL%u: mode %#x, l %#x, cal_l %#x, user_ctl %#x %#x, config_ctl %#x %#x %#x, opmode %#x, status %#x, "
		    "alpha %#x\n", p, rd(pll + 0x00), rd(pll + 0x04), rd(pll + 0x08), rd(pll + 0x0c), rd(pll + 0x10), rd(pll + 0x18),
		    rd(pll + 0x1c), rd(pll + 0x20), rd(pll + 0x38), rd(pll + 0x30), rd(pll + 0x40));
	}
}

static void
look_around(void)
{
	uint32_t cx = 0, mx = 0;

	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, 0x10000);
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
	}
	show_rail("cx.lvl", &cx);
	show_rail("mx.lvl", &mx);
	show_smps("PM8350C S2 (VDD_CX)", 0x217);
	show_smps("PM8350C S10 (VDD_MX)", 0x22f);
	show_smmu();
	show_plls();
}

/* ---- the camera subsystem (Titan 165): rails, PLL0, clocks ------------------------------------------- */

/*
 * cam=3: the path a picture takes, tried first with the CSID's own test pattern generator, so that
 * nothing outside the SoC is involved: CSID-lite 0 (0xacc8000, "csid-lite165": the register layout
 * of Android's cam_ife_csid_lite17x.h) makes RAW10 frames of a colour bar and sends them down its
 * RDI 0 path to IFE-lite 0 (0xacc4000, "vfe-lite165": cam_vfe_lite16x.h, the bus of
 * cam_vfe_bus_ver2.c), whose write master 0 writes each frame as it comes (Android's RDI frame mode)
 * to memory through the SMMU, as stream 0x800 with the mask 0x4e0 (dt: msm_cam_smmu_ife), into a
 * context of ours. The memory is one physically contiguous piece that the loader hands to the host
 * after the run ("xl camera"; tools/camraw.py makes pictures of it).
 *
 * What it takes, in Android's order and with Android's numbers (camcc-yupik.c, gcc-yupik.c, the
 * device tree's clock rates at the "svs" level of csid-lite 0, csiphy 2 and cpas):
 *   RPMh   CX at SVS (128) for camcc's clocks at 400 MHz, MX at SVS_L1 (192) for PLL0's 1.2 GHz --
 *          asked through DRV 2's second TCS for requests made at once, as camcc's vdd_cx and vdd_mx
 *          votes are, and given back (position 0) at the end: DRV 2 holds no other vote on them
 *          (cam=1 looks: none of its TCSs has a command for either)
 *   GCC    camera_hf_axi (0x26010), camera_sf_axi (0x2601c)
 *   camcc  PLL0 (Lucid, 1200 MHz: L 0x3e, alpha 0x8000, its config and user values), then the RCGs
 *          slow_ahb 80 MHz, camnoc_axi 150, cphy_rx 400, csi2phytimer 300, ife_lite_0_csid 400,
 *          ife_lite_0 400 (all from PLL0's even (600) or odd (400) output) and the branches
 *          cpas_ahb, camnoc_axi, csiphy2, csi2phytimer, ife_lite_0_csid, ife_lite_0_cphy_rx,
 *          ife_lite_0 (the clocks the device tree gives cpas, csiphy 2 and csid-lite 0)
 * Everything is taken back in the reverse order at the end, and on any fault.
 */
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)
#define CAMERA_TCS              1                       /* the second TCS for requests made at once (the audio DSP's driver takes the first) */
#define CX_LEVEL                128                     /* SVS */
#define MX_LEVEL                192                     /* SVS_L1 */

static bool rails_voted;

/* The position of the first level at or above `level' in RPMh's list for that rail; -1: not there. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;

	*address = rpmh_resource(name, levels, 16, &count);
	for (unsigned l = 0; *address != 0 && l < count; l++) {
		if (levels[l] >= level) {
			return (int)l;
		}
	}
	return -1;
}

/* CX and MX at the camera's levels, or given back (`on' false). True: RPMh has said done. */
static bool
rails(bool on)
{
	uint32_t addresses[2], values[2], done = 0;
	int cx = rail_position("cx.lvl", CX_LEVEL, &addresses[0]), mx = rail_position("mx.lvl", MX_LEVEL, &addresses[1]);
	vm_offset_t tcs = rsc + RSC_TCS(CAMERA_TCS);

	if (cx < 0 || mx < 0) {
		say("camera: RPMh has no position for CX %u or MX %u\n", CX_LEVEL, MX_LEVEL);
		return false;
	}
	values[0] = on ? (uint32_t)cx : 0;
	values[1] = on ? (uint32_t)mx : 0;
	if (rd(tcs + TCS_STATUS) == 0) {
		say("camera: TCS %u of the sequencer is busy (control %#x)\n", CAMERA_TCS, rd(tcs + TCS_CONTROL));
		return false;
	}
	wr(rsc + RSC_IRQ_CLEAR, 1u << CAMERA_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(tcs + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(tcs + TCS_COMMAND(r) + 4, addresses[r]);
		wr(tcs + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(tcs + TCS_WAIT_FOR_DONE, 3);
	wr(tcs + TCS_ENABLED, 3);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) | TCS_TRIGGER);
	rails_voted |= on;
	/* (each command's status: [8] issued, [16] completed -- the TCS's interrupt may not be enabled for this one) */
	for (unsigned waited = 0; waited < 100000 && !done; waited += 50) {
		done = (rd(rsc + RSC_IRQ_STATUS) & (1u << CAMERA_TCS)) ||
		    ((rd(tcs + TCS_COMMAND(0) + 12) & 1u << 16) && (rd(tcs + TCS_COMMAND(1) + 12) & 1u << 16));
		if (!done) {
			spin_us(50);
		}
	}
	say("camera: RPMh's TCS %u: interrupt status %#x (enabled %#x), the commands' status %#x %#x\n", CAMERA_TCS,
	    rd(rsc + RSC_IRQ_STATUS), rd(rsc + RSC_TCS_BASE), rd(tcs + TCS_COMMAND(0) + 12), rd(tcs + TCS_COMMAND(1) + 12));
	wr(rsc + RSC_IRQ_CLEAR, 1u << CAMERA_TCS);
	wr(tcs + TCS_CONTROL, rd(tcs + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(tcs + TCS_ENABLED, 0);
	say("camera: RPMh %s CX (%#x) position %u and MX (%#x) position %u%s\n", done ? "took" : "DID NOT SAY IT TOOK", addresses[0],
	    values[0], addresses[1], values[1], on ? "" : " (given back)");
	return done != 0;
}

/* camcc's PLL0 (Lucid): the registers of clk-alpha-pll.c's Lucid layout */
#define PLL_MODE                0x00            /* [0] outputs, [2] out of reset, [23] update bypass, [31] locked */
#define PLL_L_VAL               0x04
#define PLL_CAL_L_VAL           0x08
#define PLL_USER_CTL            0x0c            /* [2:0] the outputs: main, even, odd */
#define PLL_USER_CTL_U          0x10
#define PLL_USER_CTL_U1         0x14
#define PLL_CONFIG_CTL          0x18
#define PLL_CONFIG_CTL_U        0x1c
#define PLL_CONFIG_CTL_U1       0x20
#define PLL_OPMODE              0x38            /* 0 standby, 1 run */
#define PLL_ALPHA_VAL           0x40

static bool pll_on;

static bool
camcc_any(uint32_t offset, uint32_t value, uint32_t check)
{
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check) != (value & check)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
pll0_up(void)
{
	static const struct {
		uint32_t offset, value;
	} config[] = {                  /* camcc-yupik.c: cam_cc_pll0_config ("1200MHz Configuration"); CAL_L as clk-alpha-pll.c's default */
		{ PLL_L_VAL, 0x3e }, { PLL_CAL_L_VAL, 0x44 }, { PLL_ALPHA_VAL, 0x8000 }, { PLL_CONFIG_CTL, 0x20485699 },
		{ PLL_CONFIG_CTL_U, 0x00002261 }, { PLL_CONFIG_CTL_U1, 0x329a299c }, { PLL_USER_CTL, 0x00003101 },
		{ PLL_USER_CTL_U, 0x00000805 }, { PLL_USER_CTL_U1, 0x00000000 },
	};

	if (rd(camcc + PLL_MODE) & 1) {
		give_up("camcc's PLL0 is running already (somebody else's)");
		return false;
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (!camcc_any(config[i].offset, config[i].value, ~0u)) {
			return false;
		}
	}
	pll_on = true;
	/* clk_lucid_pll_configure(): update bypass, outputs off, standby, out of reset */
	if (!camcc_any(PLL_MODE, rd(camcc + PLL_MODE) | 1u << 23, 1u << 23) || !camcc_any(PLL_MODE, rd(camcc + PLL_MODE) & ~1u, 1) ||
	    !camcc_any(PLL_OPMODE, 0, 1) || !camcc_any(PLL_MODE, rd(camcc + PLL_MODE) | 1u << 2, 1u << 2)) {
		return false;
	}
	/* alpha_pll_lucid_enable(): run, locked, the outputs on */
	if (!camcc_any(PLL_OPMODE, 1, 1)) {
		return false;
	}
	if (!wait_bits(PLL_MODE, 1u << 31, 1u << 31, 1000)) {
		say("camera: camcc's PLL0 does not lock (mode %#x)\n", rd(camcc + PLL_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_any(PLL_USER_CTL, rd(camcc + PLL_USER_CTL) | 7, 7) || !camcc_any(PLL_MODE, rd(camcc + PLL_MODE) | 1, 1)) {
		return false;
	}
	say("camera: camcc's PLL0 locked at 1200 MHz (mode %#x, user_ctl %#x)\n", rd(camcc + PLL_MODE), rd(camcc + PLL_USER_CTL));
	return true;
}

static void
pll0_down(void)
{
	if (!pll_on) {
		return;
	}
	/* alpha_pll_lucid_disable(), then back into reset as it was found */
	wr(camcc + PLL_MODE, rd(camcc + PLL_MODE) & ~1u);
	wr(camcc + PLL_USER_CTL, rd(camcc + PLL_USER_CTL) & ~7u);
	wr(camcc + PLL_OPMODE, 0);
	wr(camcc + PLL_MODE, rd(camcc + PLL_MODE) & ~(1u << 2));
	pll_on = false;
	say("camera: camcc's PLL0 off (mode %#x)\n", rd(camcc + PLL_MODE));
}

/* An RCG set (source, divider field 2n - 1, no M/N) and updated; then branches on. */
struct rcg {
	const char *    name;
	uint32_t        cmd, source, divider;
	uint32_t        found_cfg;
	bool            set;
};
static struct rcg rcgs[] = {
	{ "slow_ahb 80 MHz", 0x7058, 2, 14 },           /* PLL0 even (600) / 7.5 */
	{ "camnoc_axi 150 MHz", 0xc124, 2, 7 },         /* / 4 */
	{ "cphy_rx 400 MHz", 0xa064, 2, 2 },            /* / 1.5 */
	{ "csi2phytimer 300 MHz", 0xe0f4, 2, 3 },       /* / 2 */
	{ "ife_lite_0_csid 400 MHz", 0xc020, 2, 2 },    /* / 1.5 */
	{ "ife_lite_0 400 MHz", 0xc004, 3, 1 },         /* PLL0 odd (400) / 1 */
};
struct branch {
	const char *    name;
	bool            gcc;
	uint32_t        cbcr;
	bool            set;
};
static struct branch branches[] = {
	{ "gcc camera_hf_axi", true, 0x26010 }, { "gcc camera_sf_axi", true, 0x2601c },
	{ "cpas_ahb", false, 0xc11c }, { "camnoc_axi", false, 0xc140 }, { "csiphy2", false, 0xe110 },
	{ "csi2phytimer", false, 0xe10c },
	{ "ife_lite_0_csid", false, 0xc038 }, { "ife_lite_0_cphy_rx", false, 0xc040 }, { "ife_lite_0", false, 0xc01c },
};

static bool
clocks_up(void)
{
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t cfg = r->cmd + 4;

		r->found_cfg = rd(camcc + cfg);
		if (!camcc_any(cfg, (r->found_cfg & ~0x371fu) | r->source << 8 | r->divider, 0x371f)) {
			return false;
		}
		r->set = true;
		wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
		if (!wait_bits(r->cmd, CMD_UPDATE, 0, 500)) {
			say("camera: the RCG %s does not take its configuration (cmd %#x)\n", r->name, rd(camcc + r->cmd));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (rd(base + b->cbcr) & 1) {
			continue;               /* (on already: left on at the end too) */
		}
		wr(base + b->cbcr, rd(base + b->cbcr) | 1);
		if (!(rd(base + b->cbcr) & 1)) {
			say("camera: the branch %s does not take its enable (%#x)\n", b->name, rd(base + b->cbcr));
			give_up("a clock that does not read back");
			return false;
		}
		b->set = true;
		for (unsigned waited = 0; rd(base + b->cbcr) & CBCR_OFF; waited += 10) {
			if (waited > 500) {
				say("camera: the branch %s does not come on (%#x)\n", b->name, rd(base + b->cbcr));
				give_up("a clock that does not come on");
				return false;
			}
			spin_us(10);
		}
	}
	say("camera: the camera's clocks on (%u RCGs, %u branches)\n", (unsigned)(sizeof(rcgs) / sizeof(rcgs[0])),
	    (unsigned)(sizeof(branches) / sizeof(branches[0])));
	return true;
}

static void
clocks_down(void)
{
	for (int i = (int)(sizeof(branches) / sizeof(branches[0])) - 1; i >= 0; i--) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (b->set) {
			wr(base + b->cbcr, rd(base + b->cbcr) & ~1u);
			b->set = false;
		}
	}
	for (int i = (int)(sizeof(rcgs) / sizeof(rcgs[0])) - 1; i >= 0; i--) {
		struct rcg *r = &rcgs[i];

		if (r->set) {
			wr(camcc + r->cmd + 4, r->found_cfg);
			wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
			(void)wait_bits(r->cmd, CMD_UPDATE, 0, 500);
			r->set = false;
		}
	}
	say("camera: the camera's clocks off\n");
}

/* ---- the frames' memory, and the SMMU ---------------------------------------------------------------- */

#define FRAME_ADDRESSES         0x40000000ull           /* the IFE's addresses for it (in dt's iova pool 0x100000-0xfff00000) */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as redwood_sound.c's */

/* What the loader hands to the host (tools/camraw.py reads it). */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, packed (four pixels in five bytes) */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames;         /* how many were written whole */
	uint32_t        frame_bytes;    /* each, from CAM_HEADER_BYTES on */
	uint32_t        source;         /* 0: the CSID's test pattern, 1: the sensor */
	char            sensor[16];
	uint32_t        last;           /* which of the frames' places holds the last one written */
};

extern uint64_t loader_recording[3];
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t redwood_smmu_lock;

static vm_offset_t frames, smmu_second, smmu_context, page_tables;
static uint64_t frames_bytes;
static uint32_t smmu_match = ~0u, smmu_context_n, smmu_old_smr, smmu_old_s2cr, smmu_pages_size, smmu_pages;
static bool smmu_on;

static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, uint32_t source)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	/*
	 * RGGB: the CSID's test pattern, and the OV02B10's picture as it comes here (the two sites of a 2x2
	 * cell that track each other, the greens, are its second and third; red against blue by the light
	 * of a room). The sensor's own colour bars (cam_pattern=1) come a line off that: GBRG.
	 */
	h->bayer = 0;
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, source ? "OV02B10" : "CSID TPG", sizeof(h->sensor));
	/* (what the processor wrote out of its caches; the IFE writes past them) */
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("camera: %llu KB of memory for %u frames at %#llx, handed to the loader for the host\n", frames_bytes >> 10, CAM_FRAMES,
	    loader_recording[0]);
	return true;
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1);

	smmu_pages_size = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu_pages * smmu_pages_size != 0x100000) {
		say("camera: the SMMU's layout is not the one known here\n");
		return false;
	}
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ IFE_STREAM) & ~((smr >> 16 & 0x7fff) | IFE_STREAM_MASK)) == 0) {
				say("camera: SMMU match register %u (%#x) is somebody's for the IFEs' streams: left alone\n", n, smr);
				return false;
			}
		} else if (smmu_match == ~0u) {
			smmu_match = n;
			smmu_old_smr = smr;
			smmu_old_s2cr = s2cr;
		}
	}
	for (smmu_context_n = 0; smmu_context_n < (idr1 & 0xff) && used[smmu_context_n]; smmu_context_n++) {
	}
	if (smmu_match == ~0u || smmu_context_n >= (idr1 & 0xff)) {
		say("camera: the SMMU has no match register or context left\n");
		return false;
	}
	smmu_second = ml_io_map(SMMU_PHYS + smmu_pages_size, 0x1000);
	smmu_context = ml_io_map(SMMU_PHYS + (uint64_t)(smmu_pages + smmu_context_n) * smmu_pages_size, 0x1000);
	if (rd(smmu_context + SMMU_CB_SCTLR) & 1) {
		say("camera: SMMU context %u is switched on although no stream uses it: somebody's\n", smmu_context_n);
		return false;
	}
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		give_up("an SMMU register that did not read back");
		return false;
	}
	return true;
}

/* The frames' memory behind FRAME_ADDRESSES for the IFEs' streams, through a free context with tables of ours. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);

	lck_mtx_lock(&redwood_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&redwood_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	if (kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		lck_mtx_unlock(&redwood_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = page_tables; line < page_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	say("camera: SMMU: context %u gets our tables for %llu KB at %#llx; match register %u sends streams %#x/%#x through it\n",
	    smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match, IFE_STREAM, IFE_STREAM_MASK);
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) || !smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) ||
	    !smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    !smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) || !smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) || !smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) || !smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&redwood_smmu_lock);
		return false;
	}
	wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
	wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	for (unsigned waited = 0; rd(smmu_context + SMMU_CB_TLBSTATUS) & 1; waited++) {
		if (waited > 1000) {
			lck_mtx_unlock(&redwood_smmu_lock);
			give_up("the SMMU does not finish forgetting");
			return false;
		}
		spin_us(10);
	}
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&redwood_smmu_lock);
		return false;
	}
	smmu_on = true;
	if (!smmu_put(smmu, SMMU_S2CR(smmu_match), smmu_context_n, 0x300ff) ||
	    !smmu_put(smmu, SMMU_SMR(smmu_match), 1u << 31 | (uint32_t)IFE_STREAM_MASK << 16 | IFE_STREAM, ~0u)) {
		lck_mtx_unlock(&redwood_smmu_lock);
		return false;
	}
	lck_mtx_unlock(&redwood_smmu_lock);
	return true;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("camera: the SMMU has refused the IFE nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("camera: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n",
	    smmu_context_n, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (!smmu_on && smmu_match == ~0u) {
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&redwood_smmu_lock);
	}
	if (smmu_match != ~0u) {
		wr(smmu + SMMU_SMR(smmu_match), smmu_old_smr);
		wr(smmu + SMMU_S2CR(smmu_match), smmu_old_s2cr);
	}
	if (smmu_on) {
		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	if (!leaving) {
		lck_mtx_unlock(&redwood_smmu_lock);
	}
	smmu_on = false;
	smmu_match = ~0u;
	say("camera: the SMMU's match register and context put back as they were found\n");
}

/* ---- CSID-lite 0 and IFE-lite 0 ----------------------------------------------------------------------- */

#define CSID_PHYS               0x0acc8000UL
#define IFE_PHYS                0x0acc4000UL
#define CSIPHY_PHYS             0x0ace4000UL

#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_CSI2_RX_ECC        0x164
#define CSID_CSI2_RX_CRC        0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_MASK1           0x0060
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_CLEAR0          0x2050
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_CLEAR2          0x2058
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_IRQ_GLOBAL_CLEAR    0x2068
#define BUS_COMP_ERROR          0x206c
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_IMAGE_ADDR_OFFSET   0x2218
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static vm_offset_t csid, ife, csiphy;
static bool csid_running, ife_running, tpg_running;

/* A register of the three camera blocks: written, and read back where it is one that keeps what it is given. */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy");
	if (check && rd(base + offset) != value) {
		say("camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		give_up("a camera register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_reg(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

/* cam_ife_csid_global_reset() and reset_regs(): interrupts masked and cleared, then the registers, then the hardware. */
static bool
csid_reset(void)
{
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK + 0x10 * r, 0, false);
	}
	blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		if (hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		blk_put(csid, "CSID", CSID_RST_STROBES, hw ? 0x1e : 0x1, false);
		if (!hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!wait_reg(csid, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			say("camera: the CSID's %s reset does not finish (top irq %#x)\n", hw ? "hardware" : "registers'",
			    rd(csid + CSID_TOP_IRQ_STATUS));
			give_up("a CSID that does not reset");
			return false;
		}
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	}
	say("camera: CSID-lite 0 reset (version %#x)\n", rd(csid + CSID_HW_VERSION));
	return true;
}

/* The RX (from a PHY, or the test pattern generator: `phy' -1) and RDI 0, for RAW10 of vc/dt, written as it comes (packed). */
static bool
csid_config(int phy, unsigned lanes, uint32_t lane_cfg, uint32_t vc, uint32_t width, uint32_t height)
{
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0u << 27 | 0xfu << 12 | 0u << 10 | 1u << 2 | 3u;

	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, (lanes - 1) | lane_cfg << 4 | 0u << 24 | (uint32_t)(phy < 0 ? 0 : phy & 3) << 20,
	    true) || !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (phy < 0) {
		/* cam_ife_csid_config_tpg(): one data type, frames without end, blanking 0x3ff lines and 0x740 pixels, RAW10, colour bars */
		if (!blk_put(csid, "CSID", CSID_TPG_VC_CFG0, 0u << 16 | 1u << 10 | vc, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true)) {
			return false;
		}
	}
	/* cam_ife_csid_init_config_rdi_path() */
	if (!blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	/* cam_ife_csid_enable_rdi_path(): resume at the frame's boundary; the reset, overflow and count errors seen */
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

static bool
tpg_start(void)
{
	/* cam_ife_csid_tpg_start(): four lanes' worth */
	if (!blk_put(csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		return false;
	}
	tpg_running = true;
	return true;
}

/* cam_vfe_top_reset(), cam_vfe_bus_init_hw(), cam_vfe_bus_start_wm() and update_wm() for RDI 0 in frame mode. */
static bool
ife_config(uint64_t address, uint32_t frame_bytes)
{
	blk_put(ife, "IFE", IFE_IRQ_MASK0, 1u << 31, false);
	blk_put(ife, "IFE", IFE_GLOBAL_RESET, 0x3f9f, false);
	if (!wait_reg(ife, IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100000)) {
		say("camera: the IFE does not reset (irq %#x)\n", rd(ife + IFE_IRQ_STATUS0));
		give_up("an IFE that does not reset");
		return false;
	}
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	say("camera: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION));
	if (!blk_put(ife, "IFE", BUS_ADDR_SYNC_FRAME_HDR, 0, true) || !blk_put(ife, "IFE", BUS_CGC_OVD, 0xfffff, false) ||
	    !blk_put(ife, "IFE", BUS_TEST_CTRL, 0, false) || !blk_put(ife, "IFE", BUS_ADDR_SYNC_NO_SYNC, 0xffffff, false) ||
	    !blk_put(ife, "IFE", WM0_BURST_LIMIT, 0xf, true) || !blk_put(ife, "IFE", WM0_WIDTH, 0xff01, true) ||
	    !blk_put(ife, "IFE", WM0_HEIGHT, 0, true) || !blk_put(ife, "IFE", WM0_PACKER, 0, true) ||
	    !blk_put(ife, "IFE", WM0_STRIDE, 0xff01, true) || !blk_put(ife, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) || !blk_put(ife, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) || !blk_put(ife, "IFE", BUS_IRQ_MASK1, 1, false) ||
	    !blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)address, false) || !blk_put(ife, "IFE", WM0_FRAME_INC, frame_bytes, true) ||
	    !blk_put(ife, "IFE", WM0_CFG, 3, true)) {
		return false;
	}
	ife_running = true;
	/* the RDI 0 resource's start: its register update */
	return blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
}

static void
show_paths(const char *when)
{
	say("camera: %s: CSID rx irq %#x, packets %u, ecc %#x, crc %u; RDI0 irq %#x status %#x, bytes %u/%u; IFE irq %#x %#x, "
	    "violation %#x; bus irq %#x %#x %#x, comp error %#x; WM0 status %#x %#x\n", when, rd(csid + CSID_CSI2_RX_IRQ_STATUS),
	    rd(csid + CSID_CSI2_RX_PKTS), rd(csid + CSID_CSI2_RX_ECC), rd(csid + CSID_CSI2_RX_CRC), rd(csid + CSID_RDI0_IRQ_STATUS),
	    rd(csid + CSID_RDI0_STATUS), rd(csid + CSID_RDI0_BYTES_PING), rd(csid + CSID_RDI0_BYTES_PONG), rd(ife + IFE_IRQ_STATUS0),
	    rd(ife + IFE_IRQ_STATUS1), rd(ife + IFE_VIOLATION_STATUS), rd(ife + BUS_IRQ_STATUS0), rd(ife + BUS_IRQ_STATUS1),
	    rd(ife + BUS_IRQ_STATUS2), rd(ife + BUS_COMP_ERROR), rd(ife + WM0_STATUS0), rd(ife + WM0_STATUS1));
}

static void
paths_down(void)
{
	if (tpg_running) {
		wr(csid + CSID_TPG_CTRL, 0);
		tpg_running = false;
	}
	if (csid_running) {
		/* cam_ife_csid_disable_rdi_path(): halted at once, then the path off */
		wr(csid + CSID_RDI0_IRQ_MASK, 0);
		wr(csid + CSID_RDI0_CTRL, (rd(csid + CSID_RDI0_CTRL) & ~3u) | 2);
		wr(csid + CSID_RDI0_CFG0, rd(csid + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(csid + CSID_CSI2_RX_CFG0, 0);
		csid_running = false;
	}
	if (ife_running) {
		wr(ife + WM0_CFG, 0);
		wr(ife + IFE_REG_UPDATE, 0x2);
		spin_us(1000);
		ife_running = false;
	}
}

/* The camera's blocks up to their clocks; false (and everything taken back by titan_down()) on any fault. */
static bool
titan_up(void)
{
	if (csid == 0) {
		csid = ml_io_map(CSID_PHYS, 0x1000);
		ife = ml_io_map(IFE_PHYS, 0x4000);
		csiphy = ml_io_map(CSIPHY_PHYS, 0x2000);
	}
	if (!(rd(camcc + TITAN_TOP_GDSCR) & GDSC_ON) && !gdsc_on()) {
		return false;
	}
	if (!rails(true)) {
		give_up("RPMh did not take the camera's rails");
		return false;
	}
	return pll0_up() && clocks_up();
}

static void
titan_down(void)
{
	paths_down();
	smmu_down();
	clocks_down();
	pll0_down();
	if (rails_voted) {
		rails_voted = false;
		(void)rails(false);
	}
}

/* cam=3: a few frames of the CSID's test pattern through RDI 0 and IFE-lite 0 into memory. */
static void
capture_tpg(void)
{
	const uint32_t width = 1600, height = 1200, stride = width * 10 / 8;
	struct cam_header *h;

	if (!frames_alloc(width, height, stride, 0) || !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config(-1, 4, 0x3210, TPG_VC, width, height) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height)) {
		titan_down();
		return;
	}
	show_paths("configured");
	if (!tpg_start()) {
		titan_down();
		return;
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 2000; ms += 100) {
		wait_ms(100);
		if (rd(ife + BUS_IRQ_STATUS1) & 1) {
			h->frames = 1;
			say("camera: write master 0 says its frame is done after %u ms\n", ms + 100);
			break;
		}
	}
	show_paths("after the frames");
	smmu_faults();
	titan_down();
	/* (what the IFE wrote, a look: the first bytes of the frame, past the processor's caches) */
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=4: the macro sensor's own frames ------------------------------------------------------------- */

/*
 * The sensor's tables are Android's, read at run time: tools/camsensor.py makes camera-ov02b10.tab
 * out of the module file (userland/build.sh puts it into fw/ of the small image). The sequence is
 * Android's: power up, the chip ID, the initialisation ("init", 108 writes: a soft reset, then its
 * registers page by page), the resolution's own ("res0": 1600x1200 RAW10 on one lane at 665.6 Mbit/s,
 * 30 frames a second); CSIPHY 2, CSID-lite 0's RX from PHY 2 and RDI 0, IFE-lite 0's write master as
 * for cam=3; then "on", the frames, "off" (and its 30 ms), and everything down again. The sensor's
 * writes are not read back (some of its registers start something and clear themselves, a soft
 * reset among them); each byte must be acknowledged.
 *
 * CSIPHY 2 ("qcom,csiphy-v1.2.1": Android's cam_csiphy_1_2_1_hwreg.h, cam_csiphy_core.c): its reset,
 * the common registers with the lanes to enable (data lane 0 and the clock lane: the module file's
 * lane assignment 0, one lane), then the two-phase (D-PHY) settings of every lane position, the
 * settle count among them, and its interrupts (status only, nothing handled). The settle count is
 * worked out here from the data rate the way the D-PHY specification places it (the middle of the
 * window between HS-PREPARE and HS-PREPARE + HS-ZERO, in periods of the 300 MHz timer): Android's
 * comes from the camera stack and its log does not say it unless the camera has been opened.
 * Boot-arg cam_settle=N sets it instead.
 */
struct ctab_head {
	char            magic[8];       /* "RWCTAB01" */
	uint32_t        slave, id_address, id, id_mask, lanes, width, height, data_type, pixel_clock, line_length, frame_length;
	uint32_t        tables;
};
struct ctab_entry {
	uint16_t        address, data;
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay_us;
};

static const struct ctab_head *ctab;
static uint64_t ctab_size;

static bool
ctab_load(void)
{
	ctab = (const struct ctab_head *)redwood_wifi_file("camera-ov02b10.tab", &ctab_size);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB01", 8) != 0) {
		say("camera: no camera-ov02b10.tab in fw/ of the small image (made from fw-camera/ when it is built)\n");
		return false;
	}
	say("camera: the macro sensor's tables: %ux%u, data type %#x, %u lane(s), pixel clock %u, line length %u, frame length %u, "
	    "%u tables\n", ctab->width, ctab->height, ctab->data_type, ctab->lanes, ctab->pixel_clock, ctab->line_length,
	    ctab->frame_length, ctab->tables);
	return true;
}

static const struct ctab_entry *
ctab_find(const char *name, uint32_t *count)
{
	const uint8_t *at = (const uint8_t *)(ctab + 1), *end = (const uint8_t *)ctab + ctab_size;

	for (uint32_t t = 0; t < ctab->tables && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct ctab_entry) <= end) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	*count = 0;
	return NULL;
}

/* One of the sensor's tables written in order, as Android's sensor driver writes it. */
static bool
sensor_table(const char *name)
{
	uint32_t count;
	const struct ctab_entry *e = ctab_find(name, &count);

	if (e == NULL) {
		say("camera: the tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t out[4];
		unsigned n = 0;

		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 || e[i].data_bytes > 2) {
			say("camera: \"%s\" %u: an operation not known here (%u, %u/%u bytes)\n", name, i, e[i].operation, e[i].address_bytes,
			    e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		if (e[i].address_bytes == 2) {
			out[n++] = (uint8_t)(e[i].address >> 8);
		}
		out[n++] = (uint8_t)e[i].address;
		if (e[i].data_bytes == 2) {
			out[n++] = (uint8_t)(e[i].data >> 8);
		}
		out[n++] = (uint8_t)e[i].data;
		if (broken || !i2c_transfer(&cci_bus, OV02B10_ADDRESS, out, n, NULL, 0)) {
			say("camera: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
		if (e[i].delay_us != 0) {
			spin_us(e[i].delay_us > 100000 ? 100000 : e[i].delay_us);
		}
	}
	say("camera: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

static bool csiphy_running;

static bool
csiphy_up(unsigned settle)
{
	static const uint32_t lanes[5][2] = {           /* each lane position's two blocks: data lane 0, the clock lane, data lanes 1, 2, 3 */
		{ 0x000, 0x900 }, { 0x700, 0xc80 }, { 0x200, 0xa00 }, { 0x400, 0xb00 }, { 0x600, 0xc00 },
	};
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };

	/* cam_csiphy_reset(): csiphy_reset_reg_1_2_1 */
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = true;
	/* cam_csiphy_config_dev(): the common registers (two-phase), lanes: data lane 0 and the clock lane */
	if (!blk_put(csiphy, "CSIPHY", 0x814, 0x01 | 0x80, true) || !blk_put(csiphy, "CSIPHY", 0x818, 0x01, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x81c, 0x02, true)) {
		return false;
	}
	wr(csiphy + 0x800, 0x03);
	spin_us(1);
	wr(csiphy + 0x800, 0x02);
	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l][0], m = lanes[l][1];
		bool clock = l == 1;

		if (!blk_put(csiphy, "CSIPHY", b + 0x30, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x0, 0x05, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x8, 0x10, true) || !blk_put(csiphy, "CSIPHY", m + 0x4, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x4, 0x07, true) || !blk_put(csiphy, "CSIPHY", b + 0x04, 0x0c, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x01, true) || !blk_put(csiphy, "CSIPHY", b + 0x34, 0x07, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x10, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x3c, 0xb8, true) || !blk_put(csiphy, "CSIPHY", b + 0x08, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x00, clock ? 0x80 : 0x8d, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x0c, 0xa5, true)) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x38, clock ? 0x1f : 0xfe, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x60, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x28, 0x04, true)) || !blk_put(csiphy, "CSIPHY", 0x800, 0x02, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + 4 * i, irq_masks[i]);
		wr(csiphy + 0x858 + 4 * i, 0xff);
	}
	wr(csiphy + 0x828, 0x1);
	wr(csiphy + 0x828, 0x0);
	say("camera: CSIPHY 2 configured: one data lane, settle count %u\n", settle);
	return true;
}

static void
show_csiphy(const char *when)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "camera: CSIPHY 2 %s, interrupt status:", when);

	for (unsigned i = 0; i < 11 && at < (int)sizeof(line) - 6; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", rd(csiphy + 0x8b0 + 4 * i) & 0xff);
	}
	say("%s\n", line);
}

static void
csiphy_down(void)
{
	if (!csiphy_running) {
		return;
	}
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = false;
}

/* The settle count the D-PHY specification's window gives at this data rate, in periods of the 300 MHz timer. */
static unsigned
settle_count(uint64_t bits_per_second)
{
	uint64_t ui_ps = 1000000000000ull / bits_per_second;
	uint64_t prepare_max_ps = 85000 + 6 * ui_ps, prepare_zero_min_ps = 145000 + 10 * ui_ps;
	uint64_t settle_ps = (prepare_max_ps + prepare_zero_min_ps) / 2;

	return (unsigned)(settle_ps * 300 / 1000000) - 6;
}

/*
 * The exposure: the module file's table that sets it (page 1: analog gain 0x22, the frame's extra
 * lines 0x14/0x15, the exposure in lines 0x0e/0x0f, 0x9b, then 0xfe = 2 to apply it; "holdon" here),
 * as Android's camera stack writes its first one. Boot-args cam_shutter=LINES and cam_gain=N (the
 * value of 0x22: 16 is 1x) write those two instead, the same way, for a look in other light, and
 * cam_vblank=LINES (0x14/0x15) makes the frame that much longer, for a longer exposure.
 */
static bool
exposure(void)
{
	uint32_t shutter = 0, gain = 0, vblank = 0;
	uint8_t page1[2] = { 0xfd, 0x01 }, apply[2] = { 0xfe, 0x02 }, page0[2] = { 0xfd, 0x00 };

	(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	(void)PE_parse_boot_argn("cam_vblank", &vblank, sizeof(vblank));
	if (shutter == 0 && gain == 0 && vblank == 0) {
		return true;
	}
	vblank = vblank > 0x3fff ? 0x3fff : vblank;
	if (shutter > 1200 + vblank) {
		shutter = 1200 + vblank;        /* (within the frame's lines: 1237, and the extra ones of cam_vblank) */
	}
	if (gain > 0xf8) {
		gain = 0xf8;
	}
	uint8_t high[2] = { 0x0e, (uint8_t)(shutter >> 8) }, low[2] = { 0x0f, (uint8_t)shutter }, g[2] = { 0x22, (uint8_t)gain };
	uint8_t vb_high[2] = { 0x14, (uint8_t)(vblank >> 8) }, vb_low[2] = { 0x15, (uint8_t)vblank };

	if (!i2c_transfer(&cci_bus, OV02B10_ADDRESS, page1, 2, NULL, 0) ||
	    (vblank != 0 && (!i2c_transfer(&cci_bus, OV02B10_ADDRESS, vb_high, 2, NULL, 0) ||
	    !i2c_transfer(&cci_bus, OV02B10_ADDRESS, vb_low, 2, NULL, 0))) ||
	    (shutter != 0 && (!i2c_transfer(&cci_bus, OV02B10_ADDRESS, high, 2, NULL, 0) ||
	    !i2c_transfer(&cci_bus, OV02B10_ADDRESS, low, 2, NULL, 0))) ||
	    (gain != 0 && !i2c_transfer(&cci_bus, OV02B10_ADDRESS, g, 2, NULL, 0)) ||
	    !i2c_transfer(&cci_bus, OV02B10_ADDRESS, apply, 2, NULL, 0) || !i2c_transfer(&cci_bus, OV02B10_ADDRESS, page0, 2, NULL, 0)) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	say("camera: the exposure set to %u lines, gain %#x, %u extra lines a frame\n", shutter, gain, vblank);
	return true;
}

static void
capture_sensor(void)
{
	uint32_t width, height, stride, settle = 0, pattern = 0, wanted = 8;
	struct cam_header *h;
	uint64_t rate;

	if (!ctab_load()) {
		return;
	}
	width = ctab->width;
	height = ctab->height;
	stride = width * 10 / 8;
	rate = (uint64_t)ctab->pixel_clock * 10 / (ctab->lanes ? ctab->lanes : 1);
	(void)PE_parse_boot_argn("cam_frames", &wanted, sizeof(wanted));
	wanted = wanted < 1 ? 1 : wanted > 300 ? 300 : wanted;
	if (!PE_parse_boot_argn("cam_settle", &settle, sizeof(settle)) || settle == 0) {
		settle = settle_count(rate);
	}
	if (ctab->lanes != 1 || ctab->data_type != RAW10_DT) {
		say("camera: %u lanes, data type %#x: not what this is written for\n", ctab->lanes, ctab->data_type);
		return;
	}
	if (!frames_alloc(width, height, stride, 1)) {
		return;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table("res0") || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() || !csid_config(2, 1, 0x0, 0, width, height) ||
	    !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height) || !csiphy_up(settle)) {
		csiphy_down();
		titan_down();
		return;
	}
	show_paths("configured");
	h = (struct cam_header *)frames;
	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	h->bayer = pattern ? 2 : 0;
	if ((pattern == 0 || sensor_table("tpgon")) &&
	    sensor_table("on") && sensor_table("holdon") && exposure()) {
		/* frames one after the other into the two places, the last of `wanted' kept (the first ones come before the exposure) */
		for (unsigned ms = 0, done = 0; ms < 5000 && done < wanted; ms += 10) {
			wait_ms(10);
			if (!(rd(ife + BUS_IRQ_STATUS1) & 1)) {
				continue;
			}
			done++;
			say("camera: frame %u written, %u ms after the sensor was started\n", done, ms + 10);
			blk_put(ife, "IFE", BUS_IRQ_CLEAR1, 1, false);
			blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
			if (done < wanted) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES + (uint64_t)(done & 1) *
				    stride * height), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
			}
			h->frames = done < CAM_FRAMES ? done : CAM_FRAMES;
			h->last = (done - 1) & 1;
			if ((done & 7) == 0 && !healthy()) {
				break;
			}
		}
		show_paths("after the frames");
		show_csiphy("after the frames");
		(void)sensor_table("off");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- the look, and the thread ----------------------------------------------------------------------- */

static void
look(void)
{
	uint8_t regs[5], type = 0, subtype = 0, sts = 0, revid[4] = { 0 }, enable = 0;

	show_clocks();
	show_pin(PIN_PMIC_SDA, "the PM8008i's I2C data");
	show_pin(PIN_PMIC_SCL, "the PM8008i's I2C clock");
	show_pin(PIN_MCLK, "MCLK2");
	show_pin(PIN_CCI_SDA, "CCI 1 master 0 data");
	show_pin(PIN_CCI_SCL, "CCI 1 master 0 clock");
	show_pin(PIN_RESET, "the macro sensor's reset");
	if (!pgpio_found || pgpio_read(PGPIO_TYPE, &type) != NULL || pgpio_read(PGPIO_SUBTYPE, &subtype) != NULL ||
	    pgpio_read(PGPIO_RT_STS, &sts) != NULL || !pgpio_read_all(regs)) {
		say("camera: the PM8350C's GPIO 3 cannot be read\n");
		return;
	}
	say("camera: the PM8350C's GPIO 3 (the PM8008i's reset): type %#04x subtype %#04x, at %u; vin %#04x pull %#04x mode %#04x "
	    "source %#04x enable %#04x: %s\n", type, subtype, sts & 1, regs[0], regs[1], regs[2], regs[3], regs[4],
	    pgpio_active(regs) ? "as Android's pm8008i_active (out of reset)" : "not as Android's pm8008i_active");
	if (!pm8008_get(PM8008_CHIP, REVID_BASE, revid, 4)) {
		say("camera: the PM8008i does not answer (in reset, or its bus is not at rest)\n");
		return;
	}
	(void)pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1);
	say("camera: the PM8008i: revision %02x %02x %02x %02x, chip enable %#04x\n", revid[0], revid[1], revid[2], revid[3], enable);
	show_ldo(LDO_VDIG);
	show_ldo(LDO_VANA);
}

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20;

	(void)PE_parse_boot_argn("cam_after", &secs, sizeof(secs));
	wait_ms(secs * 1000);           /* (the system up first: nothing else is on these buses then) */
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, 0x30000);
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	say("camera: cam=%u: the macro camera (OV02B10)%s\n", level, level == 4 ? ": its frames through CSIPHY 2 and RDI 0 into memory" :
	    level == 3 ? ": the CSID's test pattern through RDI 0 into memory" :
	    level == 2 ? ": powered up, its chip ID read, powered down" : ": a look, reading only");
	pgpio_find();
	found_reset_cfg = rd(GPIO_CFG(PIN_RESET));
	found_reset_io = rd(GPIO_IO(PIN_RESET));
	found_mclk_cfg = rd(GPIO_CFG(PIN_MCLK));
	pins_found = true;
	look();
	look_around();
	if (level == 3) {
		capture_tpg();
		power_down();
	} else if (level == 4) {
		capture_sensor();
		power_down();
	} else if (level >= 2) {
		if (power_up() && healthy()) {
			(void)read_chip_id();
			(void)healthy();
			show_ldo(LDO_VDIG);
			show_ldo(LDO_VANA);
		}
		power_down();
		look();
	}
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
redwood_camera_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 4 ? 4 : level;
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: whatever was switched on, off. */
void
redwood_camera_quiesce(void)
{
	if (level < 2 || tlmm == 0) {
		return;
	}
	leaving = true;
	broken = false;                 /* (one last time, whatever went before) */
	csiphy_down();
	titan_down();
	power_down();
}
