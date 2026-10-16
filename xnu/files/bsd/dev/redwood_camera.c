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
 *
 * Limits, whatever asks: Android's voltages only (1.8 and 2.8 V; nothing else is ever written to
 * the LDOs' set points); only the registers named above are written (a list each, checked at every
 * write) and each is read back; any write that does not read back, a regulator that is not ready
 * in time or reports over-current, a byte nobody acknowledges, and everything is switched off again
 * in the power-down's order and nothing more is done. At the run's end (and on a panic's way out)
 * whatever was switched on is switched off. Everything that was found switched on is left on.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
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
	say("camera: cam=%u: the macro camera (OV02B10)%s\n", level, level >= 2 ? ": powered up, its chip ID read, powered down" :
	    ": a look, reading only");
	pgpio_find();
	found_reset_cfg = rd(GPIO_CFG(PIN_RESET));
	found_reset_io = rd(GPIO_IO(PIN_RESET));
	found_mclk_cfg = rd(GPIO_CFG(PIN_MCLK));
	pins_found = true;
	look();
	if (level >= 2) {
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
	level = level > 2 ? 2 : level;
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
	power_down();
}
