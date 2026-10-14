/*
 * Touch screen driver for the POCO X5 Pro (redwood). Depending on the panel the phone shipped
 * with, this is either a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 hanging off the SoC's
 * QUP serial engine 13 over SPI. We feed samples into /dev/redwood-touch (sys/redwood_input.h),
 * which userland's inject.c then turns into HID events for backboardd. Only runs when the kernel
 * gets touchscreen=1 on the boot args.
 *
 * This kernel has no clock / pin / regulator / SPI framework, so this file is the whole stack
 * from registers upward. The bring-up order matches what Linux does for both chips:
 *
 *   power    the chip's I/O supply is LDO2 of the PM8350C, switched through RPMh (one request
 *            written into the "always on" processor's command slots); the analog supply is
 *            behind a GPIO load switch
 *   clocks   the serial engine's three clocks, by vote in the global clock controller (GCC)
 *   pins     four GPIOs for SPI, plus reset / interrupt / analog supply enable
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; or (used for both chips in
 *            practice because GENI returns zeroes) the same four pins driven by hand
 *   panel    GPIO96=1 and GPIO68=0 means the FocalTech variant, anything else is Goodix.
 *            Same discriminator sm8350-kernel-OKX's fts_init_sysfs() uses.
 *   chip     Goodix: find the registers that hold the touch report, then poll them whenever
 *                    the IRQ line signals a new one
 *            FocalTech: whole different animal -- the chip comes up totally brain-dead and
 *                       won't do anything until the host uploads its ~114 KB firmware blob
 *                       into PRAM via the boot ROM every single power cycle. See the big
 *                       comment block above focaltech_probe() for the whole dance.
 *
 * No interrupt is wired up: a thread polls the INT line 125 times a second. Good enough for
 * finger input, saves us from writing an interrupt controller stub.
 *
 * References: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,rpmh-rsc,
 * cmd-db}.c, drivers/clk/qcom/gcc-sc7280.c, drivers/pinctrl/qcom/pinctrl-sc7280.c,
 * drivers/input/touchscreen/{goodix_berlin_*,focaltech_3680/*}.c (OKX fork for the FTS bits),
 * and the phone's own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

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
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_MISO                52
#define PIN_MOSI                53
#define PIN_CLK                 54
#define PIN_CS                  55
#define PIN_INT                 81      /* low = the chip has a report */
#define PIN_RESET               105     /* low = in reset */
#define PIN_AVDD                107     /* high = analog supply on */
#define PIN_FOCALTECH_ID        68      /* FocalTech if this reads 0 ... */
#define PIN_GOODIX_ID           96      /* ... and this reads 1 (sm8350-kernel-OKX convention) */
#define PIN_FUNCTION_SPI        1       /* "qup15" */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

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
		printf("goodix: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
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

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("goodix: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("goodix: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* The chip's I/O supply: 1.8 V from LDO2 of the PM8350C ("ldoc2"). Nothing else is connected to it. */
static bool
io_supply_on(void)
{
	uint32_t base = rpmh_address("ldoc2");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1800, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("goodix: RPMh: no regulator \"ldoc2\" (address %#x)\n", base);
		return false;
	}
	printf("goodix: RPMh: ldoc2 is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", base,
	    rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("goodix: I/O supply on (1.8 V)\n");
	return true;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP1_CORE_2X       (1u << 18)
#define VOTE_QUP1_CORE          (1u << 19)
#define VOTE_QUP1_M_AHB         (1u << 20)
#define VOTE_QUP1_S_AHB         (1u << 21)
#define VOTE_QUP1_SE5           (1u << 27)             /* serial engine 13 is the 6th of the second group */
#define GCC_QUP1_M_AHB_CBCR     (gcc + 0x18004)         /* bit 31: the clock is off */
#define GCC_QUP1_S_AHB_CBCR     (gcc + 0x18008)
#define GCC_QUP1_SE5_CBCR       (gcc + 0x185fc)
#define GCC_QUP1_SE5_CMD        (gcc + 0x18600)         /* bit 0: take the new configuration */
#define GCC_QUP1_SE5_CFG        (gcc + 0x18604)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP1_SE5_DFSR       (gcc + 0x18614)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP1_SE5_PERF_CFG(l) (gcc + 0x1861c + 4 * (l))
#define GCC_QUP1_SE5_PERF_M(l)  (gcc + 0x1865c + 4 * (l))
#define GCC_QUP1_SE5_PERF_N(l)  (gcc + 0x1869c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB);
	if (rd(GCC_QUP1_SE5_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP1_SE5_PERF_CFG(level)), rd(GCC_QUP1_SE5_PERF_M(level)),
			    rd(GCC_QUP1_SE5_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("goodix: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP1_SE5_CFG, (rd(GCC_QUP1_SE5_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP1_SE5_CMD, rd(GCC_QUP1_SE5_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP1_SE5_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP1_SE5_CMD) & 1) {
			printf("goodix: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP1_SE5_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_SE5);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP1_M_AHB_CBCR) | rd(GCC_QUP1_S_AHB_CBCR) | rd(GCC_QUP1_SE5_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("goodix: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR),
	    rd(GCC_QUP1_SE5_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x00a94000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("goodix: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

static unsigned bitbang_in = PIN_MISO, bitbang_out = PIN_MOSI;

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the Goodix chip ------------------------------------------------------------------------------- */

#define GOODIX_FW_VERSION       0x10014         /* 28 bytes */
#define GOODIX_IC_INFO          0x10070         /* a 16-bit length, then that many bytes */
#define GOODIX_MAX_READ         1024
#define GOODIX_MAX_TOUCHES      10
#define GOODIX_STATUS_TOUCH     0x80
#define GOODIX_STATUS_REQUEST   0x40
#define GOODIX_REQUEST_RESET    3

static uint8_t spi_tx[8 + GOODIX_MAX_READ], spi_rx[8 + GOODIX_MAX_READ];
static uint32_t touch_address;
static uint32_t max_x = 10800, max_y = 24000;   /* the device tree's; the chip may say otherwise */

static bool
goodix_read(uint32_t address, uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf1;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	spi_tx[5] = spi_tx[6] = spi_tx[7] = 0xff;
	memset(spi_tx + 8, 0, length);
	if (!spi_transfer(spi_tx, spi_rx, 8 + length)) {
		return false;
	}
	memcpy(data, spi_rx + 8, length);
	return true;
}

static bool
goodix_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf0;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	memcpy(spi_tx + 5, data, length);
	return spi_transfer(spi_tx, NULL, 5 + length);
}

/* The chip's blocks end in the 16-bit sum of their bytes; all zeros or all ones is no answer at all. */
static bool
goodix_checksum_ok(const uint8_t *data, uint32_t length)
{
	uint32_t sum = 0;
	bool blank = true;

	if (length < 2) {
		return false;
	}
	for (uint32_t i = 0; i < length - 2; i++) {
		sum += data[i];
		blank &= data[i] == 0 || data[i] == 0xff;
	}
	return !blank && (uint16_t)sum == (uint16_t)(data[length - 2] | data[length - 1] << 8);
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(2);
	gpio_set(PIN_RESET, true);
	sleep_ms(100);
}

/* Is the chip there and running its firmware? Finds where it keeps the touch report. */
static bool
goodix_probe(bool verbose)
{
	static uint8_t info[GOODIX_MAX_READ];
	uint8_t version[28];
	uint32_t length, offset;

	if (!goodix_read(GOODIX_FW_VERSION, version, sizeof(version))) {
		return false;
	}
	if (!goodix_checksum_ok(version, sizeof(version))) {
		if (verbose) {
			printf("goodix: no valid firmware version: %02x %02x %02x %02x %02x %02x %02x %02x ... %02x %02x\n",
			    version[0], version[1], version[2], version[3], version[4], version[5], version[6], version[7],
			    version[26], version[27]);
		}
		return false;
	}
	printf("goodix: chip \"%.6s\", firmware \"%.8s\" %02x.%02x.%02x.%02x, sensor %u\n", (const char *)version,
	    (const char *)version + 10, version[18], version[19], version[20], version[21], version[23]);

	if (!goodix_read(GOODIX_IC_INFO, info, 2)) {
		return false;
	}
	length = le16(info);
	if (length < 40 || length >= GOODIX_MAX_READ || !goodix_read(GOODIX_IC_INFO, info, length) ||
	    !goodix_checksum_ok(info, length)) {
		printf("goodix: bad chip description (length %u)\n", length);
		return false;
	}
	/* the length, the version (16 bytes), the features (10), four counts, then five lists of 16-bit numbers */
	offset = 2 + 16 + 10 + 4;
	for (int i = 0; i < 5; i++) {
		if (offset >= length) {
			return false;
		}
		offset += 1 + 2u * info[offset];
	}
	if (offset + 48 + 2 > length) {
		printf("goodix: chip description too short (%u, lists end at %u)\n", length, offset);
		return false;
	}
	touch_address = le32(info + offset + 44);
	if (offset + 122 + 20 + 2 <= length) {
		uint16_t x = le16(info + offset + 122 + 16), y = le16(info + offset + 122 + 18);

		if (x && y) {
			max_x = x;
			max_y = y;
		}
	}
	printf("goodix: touch report at %#x, coordinates up to %u x %u\n", touch_address, max_x, max_y);
	return touch_address != 0;
}

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[GOODIX_MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static void
goodix_report(const uint8_t *touches, uint32_t count)
{
	static unsigned shown;

	for (int i = 0; i < GOODIX_MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *t = touches + 8 * i;
		uint32_t id = t[0] >> 4, kind = t[0] & 0xf;
		uint32_t x = le16(t + 2), y = le16(t + 4);

		if (id >= GOODIX_MAX_TOUCHES || kind == 1 || kind == 3) {       /* 1 and 3: a stylus */
			continue;
		}
		if (shown < 40) {
			shown++;
			printf("goodix: finger %u at %u, %u (of %u x %u)\n", id, x, y, max_x, max_y);
		}
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		x = x >= max_x ? 65535 : x * 65535 / max_x;
		y = y >= max_y ? 65535 : y * 65535 / max_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		contact[id].seen = true;
		if (!contact[id].down) {
			redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* Read the report if there is one, and tell the chip it has been read. */
static void
goodix_poll(void)
{
	static uint8_t report[8 + 8 * GOODIX_MAX_TOUCHES + 2];
	static unsigned bad;
	const uint8_t zero = 0;
	const uint32_t first = 8 + 8 * 2 + 2;           /* the header and two fingers: all there is, most of the time */
	uint32_t count;

	seen.polls++;
	if (!goodix_read(touch_address, report, first) || report[0] == 0) {
		return;
	}
	seen.reports++;
	count = report[2] & 0xf;
	if ((report[0] & GOODIX_STATUS_TOUCH) && count > 2 && count <= GOODIX_MAX_TOUCHES &&
	    !goodix_read(touch_address + 8, report + 8, 8 * count + 2)) {
		return;
	}
	goodix_write(touch_address, &zero, 1);
	if (!goodix_checksum_ok(report, 8)) {
		seen.bad++;
		if (bad++ < 8) {
			printf("goodix: bad report header: %02x %02x %02x %02x %02x %02x %02x %02x\n", report[0], report[1],
			    report[2], report[3], report[4], report[5], report[6], report[7]);
		}
		return;
	}
	if (report[0] & GOODIX_STATUS_REQUEST) {
		if ((report[2] & 0xf) == GOODIX_REQUEST_RESET || report[2] == GOODIX_REQUEST_RESET) {
			printf("goodix: the chip asks to be reset\n");
			touch_reset();
		}
		return;
	}
	if (!(report[0] & GOODIX_STATUS_TOUCH)) {
		return;
	}
	if (count > GOODIX_MAX_TOUCHES || (count && !goodix_checksum_ok(report + 8, count * 8 + 2))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("goodix: bad report (%u touches)\n", count);
		}
		return;
	}
	goodix_report(report + 8, count);
}

/* ---- the FocalTech FT3680 chip --------------------------------------------------------------------- */

/*
 * So here's the deal with FocalTech: unlike Goodix which just boots itself from its own flash
 * and gets to work, this chip comes up totally brain-dead with nothing but a boot ROM. The host
 * (us) has to shove the whole firmware blob (~114 KB) into the chip's PRAM over SPI every single
 * cold boot, THEN tell it to go run that code. Only after the chip has the firmware actually
 * executing can we read touch events out of it.
 *
 * The whole SPI framing is lifted verbatim from sm8350-kernel-OKX drivers/input/touchscreen/
 * focaltech_3680/focaltech_spi.c. Every transfer looks like this:
 *
 *   TX[0]        = register address (writebuf[0] / cmd[0])
 *   TX[1]        = ctrl byte: 0xA0 for read (with CRC check), 0x00 for write (no CRC)
 *   TX[2..3]     = data length, big-endian
 *   TX[4..6]     = three dummy bytes (SPI_DUMMY_BYTE), give the chip time to think
 *   TX[7..]      = write data (reads leave this blank so the chip can drive MISO here)
 *   TX[last-1..] = CRC-16/Kermit of the data bytes, LE (reads only; writes have no CRC)
 *
 * SPI_HEADER_LENGTH = 4 + SPI_DUMMY_BYTE = 7 bytes.
 *
 * Status byte lives at RX[3]: bit 7 = chip busy, bit 5 = CRC error, 0 means we're good.
 * If the data we read back is 0xEF with a valid CRC, the chip's firmware isn't running --
 * that's its "no app in memory, go upload firmware or shut up" NACK pattern.
 */
#define FOCALTECH_READ_CTRL         0xa0		/* READ_CMD = 0x80 | DATA_CRC_EN(0x20) */
#define FOCALTECH_WRITE_CTRL        0x00		/* WRITE_CMD, no CRC on TX */
#define FOCALTECH_CHIP_ID_H         0xa3
#define FOCALTECH_CHIP_ID_L         0x9f
#define FOCALTECH_TOUCH_DATA        0x01		/* the touch report lives at this address */
#define FOCALTECH_MAX_TOUCHES       10
#define FOCALTECH_TOUCH_REPORT_LEN  (2 + 6 * FOCALTECH_MAX_TOUCHES)
#define FOCALTECH_MAX_TRANSFER      (7 + FOCALTECH_TOUCH_REPORT_LEN + 2)

static uint8_t focaltech_tx[FOCALTECH_MAX_TRANSFER], focaltech_rx[FOCALTECH_MAX_TRANSFER];

/* The chip's CRC flavor: Kermit (0x8408 poly reflected, init 0xffff). Byte-for-byte identical to
 * the vendor's rckit_crc16() so our math matches whatever the chip wants to see.
 */
static uint16_t
focaltech_crc(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static bool
focaltech_read(uint8_t address, uint8_t *data, uint32_t length)
{
	const uint32_t data_offset = 7;
	uint32_t transfer_length = data_offset + length + 2;

	if (length == 0 || transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		memset(focaltech_tx, 0, transfer_length);
		memset(focaltech_rx, 0, transfer_length);
		focaltech_tx[0] = address;		/* vendor: TX[0] = cmd[0] = reg address */
		focaltech_tx[1] = FOCALTECH_READ_CTRL;	/* vendor: TX[1] = ctrl = 0xA0 */
		focaltech_tx[2] = (uint8_t)(length >> 8);
		focaltech_tx[3] = (uint8_t)length;
		if (!spi_transfer(focaltech_tx, focaltech_rx, transfer_length)) {
			sleep_us(150);
			continue;
		}
		/* vendor: bit 7 of status (rx[3]) is BUSY, bit 5 is CRC error */
		if (!(focaltech_rx[3] & 0xa0)) {
			uint16_t expected = focaltech_crc(focaltech_rx + data_offset, length);
			uint16_t received = (uint16_t)(focaltech_rx[data_offset + length] |
			    focaltech_rx[data_offset + length + 1] << 8);
			if (expected == received) {
				memcpy(data, focaltech_rx + data_offset, length);
				sleep_us(150);		/* minimum CS-high time in the vendor driver */
				return true;
			}
		}
		sleep_us(150);
	}
	return false;
}

/*
 * Vendor fts_write: no CRC on TX. For a 1-byte "command" (length == 0) the frame is just the
 * four header bytes { addr, 0x00, 0x00, 0x00 } with no dummy and no data. For length > 0 the
 * three dummy bytes precede the data: { addr, 0x00, lenH, lenL, 0, 0, 0, data... }. The chip
 * drives the "status" byte on rx[3] -- bits 7 and 5 set mean busy or CRC error (vendor retries
 * SPI_RETRY_NUMBER = 3 times on failure before giving up).
 */
static bool
focaltech_write(uint8_t address, const uint8_t *data, uint32_t length)
{
	uint32_t transfer_length;

	if (length == 0) {
		transfer_length = 4;
	} else {
		transfer_length = 7 + length;
	}
	if (transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	memset(focaltech_tx, 0, transfer_length);
	focaltech_tx[0] = address;
	focaltech_tx[1] = FOCALTECH_WRITE_CTRL;
	focaltech_tx[2] = (uint8_t)(length >> 8);
	focaltech_tx[3] = (uint8_t)length;
	if (data && length > 0) {
		memcpy(focaltech_tx + 7, data, length);
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		if (spi_transfer(focaltech_tx, focaltech_rx, transfer_length) &&
		    !(focaltech_rx[3] & 0xa0)) {
			sleep_us(150);
			return true;
		}
		sleep_us(150);
	}
	return false;
}

/*
 * Vendor fts_fwupg_get_boot_state() reads the chip's "boot ID" via the FT boot-ROM protocol:
 *   TX = { 0x55, 0xAA }                 (FTS_CMD_START1, FTS_CMD_START2 -- enter boot)
 *   wait FTS_CMD_START_DELAY = 12 ms
 *   TX = { 0x90 }, RX = 2 bytes         (FTS_CMD_READ_ID, incell cmd length)
 * If the two RX bytes are the chip's boot ID (0x56 0x62 for FT3680), firmware is present and
 * valid in flash but has not yet been started; the host must issue FTS_CMD_FIRMWARE_START
 * (0x08) to make the boot ROM jump to it.
 */
#define FTS_CMD_START1              0x55
#define FTS_CMD_START2              0xaa
#define FTS_CMD_START_DELAY         12		/* ms, between enter-boot and first command */
#define FTS_CMD_READ_BOOT_ID        0x90

/* Boot-ROM commands for firmware (pramboot) upload, from focaltech_3658u/focaltech_flash.h. */
#define FTS_ROMBOOT_CMD_SET_PRAM_ADDR   0xad
#define FTS_ROMBOOT_CMD_WRITE           0xae
#define FTS_ROMBOOT_CMD_START_APP       0x08
#define FTS_FLASH_PACKET_LENGTH         32		/* bytes per PRAM write */

#include "redwood_focaltech_fw.h"

/*
 * Knock on the chip's boot ROM door and ask who it is. The handshake is:
 *   1. framed write { 0x55, 0xAA } -- this is really a "write 0xAA to register 0x55", the
 *      magic combo FocalTech decided means "yo, pay attention, host is here"
 *   2. chill for 12 ms (FTS_CMD_START_DELAY) -- less and the chip isn't ready, more and it
 *      may have already auto-booted past this point
 *   3. framed read of 2 bytes from register 0x90 -- the boot ROM answers with its ID
 * For FT3680 the expected answer is { 0x56, 0x62 }. Anything else means the chip is either
 * running firmware already or hung somewhere we don't want to be.
 */
static bool
focaltech_read_boot_id(uint8_t *high, uint8_t *low)
{
	uint8_t aa = FTS_CMD_START2;
	uint8_t out[2] = { 0 };
	bool ok;

	if (!focaltech_write(FTS_CMD_START1, &aa, 1)) {
		return false;
	}
	sleep_ms(12);			/* FTS_CMD_START_DELAY */
	ok = focaltech_read(FTS_CMD_READ_BOOT_ID, out, 2);
	*high = out[0];
	*low  = out[1];
	return ok;
}

/*
 * Poll the runtime chip-ID registers (0xA3 / 0x9F) until the firmware has had time to boot
 * and start answering with { 0x56, 0x62 }. Vendor's fts_wait_tp_to_valid gives up after 1 s;
 * we do the same: 5 attempts, 200 ms apart.
 */
static bool
focaltech_wait_valid(uint8_t *high, uint8_t *low)
{
	for (int attempt = 0; attempt < 5; attempt++) {
		if (focaltech_read(FOCALTECH_CHIP_ID_H, high, 1) &&
		    focaltech_read(FOCALTECH_CHIP_ID_L, low, 1) &&
		    *high == 0x56 && *low == 0x62) {
			printf("focaltech: FT3680 runtime ID valid after %d attempt(s)\n", attempt + 1);
			return true;
		}
		sleep_ms(200);			/* vendor INTERVAL_READ_REG */
	}
	return false;
}

/*
 * Reset-and-knock. The boot ROM has a tiny window after reset release where it listens --
 * blink and you miss it, which is why our old 200 ms wait never caught the chip in boot mode.
 * Vendor fts_get_ic_information tries 3 times with escalating settle delays (12, 20, 28 ms)
 * so we do the same. Returns true if we caught it: boot-ID register answers { 0x56, 0x62 }.
 */
static bool
focaltech_enter_boot_rom(void)
{
	uint8_t boot_high = 0, boot_low = 0;

	for (int cnt = 0; cnt < 3; cnt++) {
		gpio_set(PIN_RESET, false);
		sleep_ms(1);
		gpio_set(PIN_RESET, true);
		sleep_ms(FTS_CMD_START_DELAY + cnt * 8);
		if (focaltech_read_boot_id(&boot_high, &boot_low) &&
		    boot_high == 0x56 && boot_low == 0x62) {
			printf("focaltech: in boot ROM after reset %d\n", cnt);
			return true;
		}
		printf("focaltech: boot ID attempt %d: %02x %02x (not 56 62)\n", cnt, boot_high, boot_low);
	}
	return false;
}

/*
 * Here's the main event: shove the firmware blob into the chip's PRAM.
 *
 * The boot ROM can only speak this one tiny upload protocol -- SET_PRAM_ADDR (0xAD) tells it
 * where to write, then WRITE (0xAE) hands over the data. We chunk the 116 KB file into 32-byte
 * packets (FTS_FLASH_PACKET_LENGTH), so for FT3680 it's ~3640 packets per cold boot. Each
 * packet costs two framed writes = ~7280 SPI transactions. On bit-banged pins at full tilt
 * this takes ~700 ms wall time, which is honestly fine for a once-per-boot thing.
 *
 * No ECC check here (vendor does one afterwards via register 0xCC) -- our framed writes
 * already carry per-packet status that we validate inside focaltech_write(), and if any
 * packet actually corrupts, the firmware either fails to start or runtime ID stays wrong.
 * Either way we notice without the extra round-trip, and skipping the ECC pass saves another
 * ~100 ms. If the chip ever ships flaky, add it back.
 */
static bool
focaltech_write_pram(const uint8_t *buf, uint32_t len)
{
	uint8_t addr_bytes[3];

	for (uint32_t offset = 0; offset < len; offset += FTS_FLASH_PACKET_LENGTH) {
		uint32_t chunk = FTS_FLASH_PACKET_LENGTH;
		if (offset + chunk > len) {
			chunk = len - offset;
		}
		addr_bytes[0] = (uint8_t)(offset >> 16);
		addr_bytes[1] = (uint8_t)(offset >> 8);
		addr_bytes[2] = (uint8_t)offset;
		if (!focaltech_write(FTS_ROMBOOT_CMD_SET_PRAM_ADDR, addr_bytes, 3)) {
			printf("focaltech: PRAM address write failed at offset %u\n", offset);
			return false;
		}
		if (!focaltech_write(FTS_ROMBOOT_CMD_WRITE, buf + offset, chunk)) {
			printf("focaltech: PRAM data write failed at offset %u\n", offset);
			return false;
		}
	}
	return true;
}

/*
 * Tell the boot ROM to shut up and hand control over to the code we just uploaded. One byte
 * command (0x08), no payload. After this the chip starts executing from PRAM address 0 and
 * (if everything above went right) a few hundred ms later it's answering runtime register
 * reads like a normal functioning touchscreen.
 */
static bool
focaltech_start_app(void)
{
	uint8_t cmd = FTS_ROMBOOT_CMD_START_APP;

	return focaltech_write(cmd, NULL, 0);
}

/*
 * The whole dance, start to finish:
 *
 *   1. Maybe the chip is already running from a previous session (warm reboot, loader bounce,
 *      whatever) -- try reading runtime chip ID first, that's basically free. If it answers
 *      0x56 / 0x62, skip the upload entirely and get on with the touchscreen's actual job.
 *   2. Otherwise, hard reset and catch the chip in its boot ROM (the handshake window is
 *      narrow, see focaltech_enter_boot_rom).
 *   3. Blast the firmware blob into PRAM (focaltech_write_pram) and tell it to run
 *      (focaltech_start_app).
 *   4. Give the firmware 300 ms to actually come up and initialize whatever analog stuff it
 *      needs (sensor drive signals, calibration load, whatever), then poll runtime ID again.
 *
 * If any of this fails we log where and give up -- the caller falls back to Goodix probe path
 * which will also fail for a FocalTech board, and bring_up() prints "no touch screen" and the
 * driver thread just exits. iOS still boots thanks to the auto-unlock in inject_sb.c.
 */
static bool
focaltech_probe(bool verbose)
{
	uint8_t high = 0, low = 0;

	/* Step 1: maybe firmware is already running. */
	if (focaltech_wait_valid(&high, &low)) {
		return true;
	}

	/* Step 2: reset and catch the boot ROM. */
	if (!focaltech_enter_boot_rom()) {
		if (verbose) {
			printf("focaltech: chip not in boot ROM; last read runtime %02x %02x\n", high, low);
		}
		return false;
	}

	/* Step 3: upload firmware and start it. */
	printf("focaltech: downloading %u bytes of firmware to PRAM...\n", FOCALTECH_FW_LENGTH);
	if (!focaltech_write_pram(focaltech_fw, FOCALTECH_FW_LENGTH)) {
		printf("focaltech: PRAM download failed\n");
		return false;
	}
	printf("focaltech: PRAM download complete; starting application\n");
	if (!focaltech_start_app()) {
		printf("focaltech: start-app command failed\n");
		return false;
	}
	sleep_ms(300);			/* let the firmware spin up all its analog bits */

	/* Step 4: confirm firmware is up and answering. */
	if (focaltech_wait_valid(&high, &low)) {
		return true;
	}
	if (verbose) {
		printf("focaltech: firmware did not come up (last runtime read %02x %02x)\n", high, low);
	}
	return false;
}

/* ---- FocalTech touch reports --------------------------------------------------------------------- */

/*
 * Report layout for FT3680 (vendor focaltech_core.c, FTS_TOUCH_OFF_* offsets).
 *
 * The first two bytes are the header: a gesture byte (nonzero only when the chip is in
 * gesture mode, which we don't use) and a count byte that packs chip type in the upper
 * nibble and the number of fingers currently touching in the lower nibble.
 *
 * After that, each finger slot is six bytes:
 *
 *   [0] E_XH:  [7:6] event   (0 = finger down, 1 = finger up, 2 = still touching aka contact)
 *              [3:0] x bits 14..11
 *   [1] XL:    x bits 10..3
 *   [2] ID_YH: [7:4] finger id (0..9), [3:0] y bits 14..11
 *   [3] YL:    y bits 10..3
 *   [4] PRE:   [7:5] x bits 2..0, [4:2] y bits 2..0   (yes, both axes borrow bits from the
 *                                                      pressure byte -- the chip gives us
 *                                                      "super-high resolution" sub-pixel coords
 *                                                      and this is where the extra precision
 *                                                      lives)
 *   [5] AREA:  touch area / major axis (we don't use it)
 *
 * X and Y are 15-bit sub-pixel values. The physical panel is 1080 x 2400 but the chip reports
 * in 10x finer units -- 10800 x 24000 (same as Goodix on this same panel). We then rescale to
 * 0..65535 before shipping to iOS via /dev/redwood-touch.
 */
#define FOCALTECH_EVENT_DOWN        0
#define FOCALTECH_EVENT_UP          1
#define FOCALTECH_EVENT_CONTACT     2

static struct {
	bool down, seen;
	uint32_t x, y;
} focaltech_contact[FOCALTECH_MAX_TOUCHES];

static void
focaltech_contact_update(uint32_t id, uint32_t event, uint32_t x, uint32_t y,
    uint32_t max_report_x, uint32_t max_report_y)
{
	static unsigned shown;

	if (id >= FOCALTECH_MAX_TOUCHES || max_report_x == 0 || max_report_y == 0) {
		return;
	}
	if (event == FOCALTECH_EVENT_UP) {
		focaltech_contact[id].seen = true;
		if (focaltech_contact[id].down) {
			focaltech_contact[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, focaltech_contact[id].x, focaltech_contact[id].y);
		}
		return;
	}
	seen.touches++;
	switch (flip) {
	case 1: x = max_report_x - 1 - x; break;
	case 2: y = max_report_y - 1 - y; break;
	case 3: x = max_report_x - 1 - x; y = max_report_y - 1 - y; break;
	}
	x = (uint32_t)(((uint64_t)x * 65535) / (max_report_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (max_report_y - 1));
	seen.last_x = x;
	seen.last_y = y;
	focaltech_contact[id].seen = true;
	if (!focaltech_contact[id].down) {
		focaltech_contact[id].down = true;
		redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
	} else if (focaltech_contact[id].x != x || focaltech_contact[id].y != y) {
		redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
	}
	focaltech_contact[id].x = x;
	focaltech_contact[id].y = y;
	if (shown++ < 8) {
		printf("focaltech: finger %u %s at %u, %u\n", id,
		    event == FOCALTECH_EVENT_DOWN ? "down" : "move", x, y);
	}
}

/* Panel size in the FT3680 super-high-resolution reports (device tree x_max/y_max on redwood). */
static uint32_t focaltech_max_x = 10800, focaltech_max_y = 24000;

static void
focaltech_report(const uint8_t *report)
{
	uint32_t count = report[1] & 0x0f;

	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		focaltech_contact[i].seen = false;
	}
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		const uint8_t *touch = report + 2 + 6 * i;
		uint32_t id = (touch[2] >> 4) & 0x0f;
		uint32_t event, x, y;

		if (id >= FOCALTECH_MAX_TOUCHES) {
			break;			/* no more valid slots in this report */
		}
		event = (touch[0] >> 6) & 0x3;
		x = ((uint32_t)(touch[0] & 0x0f) << 11) | ((uint32_t)touch[1] << 3) |
		    ((uint32_t)touch[4] >> 5);
		y = ((uint32_t)(touch[2] & 0x0f) << 11) | ((uint32_t)touch[3] << 3) |
		    (((uint32_t)touch[4] & 0x1c) >> 2);
		focaltech_contact_update(id, event, x, y, focaltech_max_x, focaltech_max_y);
	}
	(void)count;
	/* Fingers not in this report are up (vendor driver uses a bitmap; we remember explicitly). */
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (!focaltech_contact[i].seen && focaltech_contact[i].down) {
			focaltech_contact[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, focaltech_contact[i].x, focaltech_contact[i].y);
		}
	}
}

static void
focaltech_poll(void)
{
	static uint8_t report[FOCALTECH_TOUCH_REPORT_LEN];
	static unsigned bad;

	seen.polls++;
	if (!focaltech_read(FOCALTECH_TOUCH_DATA, report, sizeof(report))) {
		if (bad++ < 8) {
			printf("focaltech: SPI read failed for touch report\n");
		}
		return;
	}
	seen.reports++;
	focaltech_report(report);
}

/*
 * Which touch chip is physically on this particular board. Xiaomi ships redwood with two
 * different touch controllers depending on which panel the factory happened to grab -- Goodix
 * GT9916 or FocalTech FT3680. They're on the same SPI pins but speak totally different
 * protocols, so we need to figure out which one before doing anything else.
 *
 * The vendor's sm8350-kernel-OKX uses two GPIO straps to tell them apart:
 *   GPIO96 reads high AND GPIO68 reads low  -->  FocalTech
 *   anything else                           -->  Goodix
 *
 * These pins are tied to specific pads on the panel connector and set by the panel itself, so
 * reading them tells us what's actually plugged in. We just copy the vendor's discriminator.
 */
enum touch_chip { TOUCH_GOODIX, TOUCH_FOCALTECH };
static enum touch_chip touch_chip;

static enum touch_chip
touch_chip_detect(void)
{
	gpio_configure(PIN_FOCALTECH_ID, 0, PULL_NONE, 2, false);
	gpio_configure(PIN_GOODIX_ID, 0, PULL_NONE, 2, false);
	if (gpio_get(PIN_GOODIX_ID) && !gpio_get(PIN_FOCALTECH_ID)) {
		return TOUCH_FOCALTECH;
	}
	return TOUCH_GOODIX;
}

/* The two per-chip dispatchers below just hand off to goodix_* or focaltech_* based on what
 * touch_chip_detect() figured out. Keeps bring_up() clean of per-vendor if-else noise. */

static bool
chip_probe(bool verbose)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		return focaltech_probe(verbose);
	}
	touch_reset();
	return goodix_probe(verbose);
}

static void
chip_poll(void)
{
	if (touch_chip == TOUCH_FOCALTECH) {
		focaltech_poll();
	} else {
		goodix_poll();
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("goodix: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins cs/clk/mosi/miso cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u, avdd %#x/%u, straps %u %u\n", when, rd(GCC_VOTE), rd(GCC_QUP1_M_AHB_CBCR),
	    rd(GCC_QUP1_S_AHB_CBCR), rd(GCC_QUP1_SE5_CBCR), rd(GCC_QUP1_SE5_CFG), rd(GCC_QUP1_SE5_DFSR), rd(GPIO_CFG(PIN_CS)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_RESET)),
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)),
	    gpio_get(68), gpio_get(96));
}

/* Power the chip up from nothing, in the vendor driver's order. */
static void
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	io_supply_on();
	sleep_ms(3);
	gpio_output(PIN_AVDD, true, PULL_NONE, 2);
	sleep_ms(15);
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

static const char *
chip_name(void)
{
	return touch_chip == TOUCH_FOCALTECH ? "focaltech" : "goodix";
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";

	show_state("as found");
	touch_chip = touch_chip_detect();
	printf("goodix: ID gpio96=%u gpio68=%u: selected %s\n", gpio_get(PIN_GOODIX_ID),
	    gpio_get(PIN_FOCALTECH_ID), chip_name());
	power_on();

	if (clocks_on()) {
		printf("goodix: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		problem = geni_spi_init(1000000);
	}
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		show_state("powered, on the serial engine");
		if (chip_probe(true)) {
			return true;
		}
		printf("goodix: %s did not answer through the serial engine; trying the pins by hand\n", chip_name());
	} else {
		printf("goodix: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (chip_probe(true)) {
			printf("goodix: %s answers on pins driven by hand (data in on gpio%u)\n", chip_name(), bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

static void
goodix_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */

	if (!bring_up()) {
		printf("goodix: no %s touch screen\n", chip_name());
		thread_terminate(current_thread());
		return;
	}
	printf("goodix: %s touch screen ready\n", chip_name());
	for (unsigned tick = 1;; tick++) {
		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT) || ++idle >= 10) {
			idle = 0;
			chip_poll();
		}
		if (tick % (60 * 125) == 0) {
			printf("%s: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s\n", chip_name(), seen.reports, seen.polls, seen.bad, seen.touches,
			    seen.last_x, seen.last_y, gpio_get(PIN_INT) ? "high" : "low");
		}
		sleep_ms(8);
	}
}

void
redwood_goodix_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(goodix_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
