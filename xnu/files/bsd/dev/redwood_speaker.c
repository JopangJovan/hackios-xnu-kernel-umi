/*
 * The speakers of the POCO X5 Pro (redwood): so far only a look at what is there.
 * Only with boot-arg speaker_probe=N: every address below belongs to this phone.
 *   speaker_probe=1    reads registers and nothing else: pins, power (below)
 *   speaker_probe=2    ... and asks the amplifiers who they are, for which two pins are
 *                      reconfigured for the time it takes (I2C, below)
 * NEITHER HAS BEEN RUN ON THE PHONE YET.
 *
 * Each of the phone's two speakers has an amplifier of its own, an Awinic AW882xx ("smart
 * PA"), which is set up over I2C (bus of the SoC's serial engine 3, GPIO 12 data and 13 clock;
 * addresses 0x34, the bottom speaker, and 0x35, the top one) and takes its samples from the
 * SoC's first I2S interface ("MI2S 0": GPIO 97 bit clock, 100 word select, 98 and 99 data).
 * That interface belongs to the audio subsystem (LPASS), which under Android is run by the
 * audio DSP: there, sound goes from the kernel to the DSP's firmware as messages, and the
 * firmware drives the interface. None of that is running here, and before anything is built
 * the questions are which amplifier it is and whether it is powered without Android.
 *
 *   pins     how the firmware left the pins involved: their configuration and level are read
 *            from the pin controller, nothing is written
 *   power    the names under which the audio subsystem's supplies can be asked for (read from
 *            the command database the boot loader left in memory), and whether its bus clock
 *            runs (one register of the global clock controller, read)
 *   I2C      register 0 (the chip ID) and three more of each amplifier, read through the two
 *            pins driven by hand, open-drain: a pin is either pulled low or let go. This
 *            WRITES the configuration of GPIO 12 and 13 (to "GPIO, pull-up", and the output
 *            enable for every low bit) and puts back what was there before; if the bus is not
 *            at rest when the pins are let go, nothing more is done. The amplifiers are only
 *            read: the writes on the bus are their addresses and the numbers of the registers
 *            to read. They stay in the power-down state they start in.
 *
 * (Whether the secure world would start the audio DSP for us cannot be asked without doing it:
 * its "is this image supported" answers no for every image, the GPU's included, which it then
 * authenticates and starts all the same -- see redwood_adreno.c.)
 *
 * Register layouts: Linux sound/soc/codecs/aw88261.c and aw88395/, drivers/pinctrl/qcom/
 * pinctrl-sc7280.c, drivers/clk/qcom/gcc-sc7280.c, drivers/soc/qcom/cmd-db.c, and the phone's
 * own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_audio.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, cmd_db;
static uint32_t probe_level;

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

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

/* the serial engine's first pin is the data line, the second the clock (should the board differ, see show_amplifiers) */
static unsigned PIN_SDA = 12, PIN_SCL = 13;

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 12, "I2C data" }, { 13, "I2C clock" }, { 96, "MI2S 0 master clock" }, { 97, "MI2S 0 bit clock" },
	{ 100, "MI2S 0 word select" }, { 98, "MI2S 0 data 0" }, { 99, "MI2S 0 data 1" }, { 102, "top amplifier's interrupt" },
	{ 45, "bottom amplifier's interrupt" },
};

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		printf("speaker: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") :
		    "input", io & 1);
	}
}

/* ---- I2C by hand ----------------------------------------------------------------------------------- */

static void
half_bit(void)
{
	delay_for_interval(10, NSEC_PER_USEC);          /* 50 kHz at most; the bus does not mind slower */
}

/* A line is pulled low (the pin drives 0) or let go (the pin is an input and the pull-up raises it). */
static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

/* One bit out (the other side's bit in, if we let the data line go): the clock goes up and down once. */
static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	in = level(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

/* Returns true if the byte was acknowledged. */
static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);                 /* acknowledge all but the last byte */
	return byte;
}

/* A 16-bit register of an Awinic amplifier, most significant byte first. Returns false if nobody answers. */
static bool
amplifier_read(uint8_t address, uint8_t reg, uint16_t *value)
{
	bool answered;

	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write((uint8_t)(address << 1 | 1));
	}
	if (answered) {
		*value = (uint16_t)(i2c_read(true) << 8);
		*value |= i2c_read(false);
	}
	i2c_stop();
	return answered;
}

static const char *
amplifier_name(uint16_t id)
{
	switch (id) {
	case 0x1852: return "AW88194/AW88195";
	case 0x2013: return "AW88264";
	case 0x2032: return "AW88266";
	case 0x2049: return "AW88395";
	case 0x2055: return "AW88256";
	case 0x2066: return "AW88166";
	case 0x2071: return "AW88350";
	case 0x2113: return "AW88261";
	case 0x2183: return "AW88399";
	case 0x2308: return "AW88230";
	}
	return "an amplifier this list does not know";
}

static void
show_amplifiers(void)
{
	static const struct {
		uint8_t address;
		const char *where;
	} amplifiers[] = { { 0x34, "bottom" }, { 0x35, "top" } };
	/* the chip ID, then registers that every chip of the family has at the same place and that reading does not change */
	static const struct {
		uint8_t reg;
		const char *name;
	} more[] = { { 0x01, "status" }, { 0x04, "system control" }, { 0x06, "I2S control" } };
	const unsigned first = PIN_SDA, second = PIN_SCL;
	uint32_t first_cfg = rd(GPIO_CFG(first)), second_cfg = rd(GPIO_CFG(second));
	uint32_t first_io = rd(GPIO_IO(first)), second_io = rd(GPIO_IO(second));
	unsigned found = 0;

	line(PIN_SDA, true);
	line(PIN_SCL, true);
	delay_for_interval(1, NSEC_PER_MSEC);
	if (!level(PIN_SDA) || !level(PIN_SCL)) {
		/* nothing of ours on a bus that is not at rest: its supply may be off, and then so are the amplifiers */
		printf("speaker: the I2C bus is not idle (gpio%u reads %u, gpio%u reads %u): no supply for its pull-ups? Not touched\n",
		    PIN_SDA, level(PIN_SDA), PIN_SCL, level(PIN_SCL));
	} else {
		/* as the pins should be, and if nobody answers, the other way round */
		for (int attempt = 0; attempt < 2 && found == 0; attempt++) {
			if (attempt == 1) {
				PIN_SDA = second;
				PIN_SCL = first;
				printf("speaker: once more with gpio%u as the data line and gpio%u as the clock\n", PIN_SDA, PIN_SCL);
			}
			for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
				uint16_t id = 0, value = 0;

				if (!amplifier_read(amplifiers[i].address, 0x00, &id)) {
					printf("speaker: nobody at I2C address %#x (the %s amplifier): not powered, or not there\n",
					    amplifiers[i].address, amplifiers[i].where);
					continue;
				}
				found++;
				printf("speaker: the %s amplifier (I2C address %#x) is there: chip ID %#06x, %s\n", amplifiers[i].where,
				    amplifiers[i].address, id, amplifier_name(id));
				for (unsigned r = 0; r < sizeof(more) / sizeof(more[0]); r++) {
					if (amplifier_read(amplifiers[i].address, more[r].reg, &value)) {
						printf("speaker:   register %#04x (%s) = %#06x\n", more[r].reg, more[r].name, value);
					}
				}
			}
		}
	}
	/* as the firmware left them */
	wr(GPIO_IO(first), first_io & 2);
	wr(GPIO_IO(second), second_io & 2);
	wr(GPIO_CFG(first), first_cfg);
	wr(GPIO_CFG(second), second_cfg);
}

/* ---- power and clock of the audio subsystem --------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_CFG_NOC_LPASS_CBCR  (gcc + 0x47020)         /* bit 0: asked for, bit 31: off */

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

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static void
show_power(void)
{
	/* the subsystem's two supplies and, for the DSP, the processor's own two and the crystal */
	static const char *const supplies[] = { "lcx.lvl", "lmx.lvl", "cx.lvl", "mx.lvl", "xo.lvl" };

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		uint32_t address = rpmh_address(supplies[i]);

		if (address) {
			printf("speaker: supply \"%s\" can be asked for at RPMh address %#x\n", supplies[i], address);
		} else {
			printf("speaker: no supply \"%s\" in the command database\n", supplies[i]);
		}
	}
	/*
	 * (The audio subsystem's bus clock, GCC_CFG_NOC_LPASS_CBCR, is not read: on the phone that read
	 * stops the kernel with a synchronous external abort -- the register is not the main processors'
	 * to read while the audio subsystem belongs to its DSP.)
	 */
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	printf("speaker: a look at the phone's sound hardware (level %u; nothing is switched on)\n", probe_level);
	show_pins();
	show_power();
	if (probe_level >= 2) {
		show_amplifiers();
	}
	printf("speaker: done\n");
	thread_terminate(current_thread());
}

void
redwood_speaker_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("speaker_probe", &probe_level, sizeof(probe_level)) || probe_level == 0) {
		return;
	}
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
