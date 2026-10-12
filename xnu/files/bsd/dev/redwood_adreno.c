/*
 * The GPU of the POCO X5 Pro (redwood), an Adreno 642L, as far as it can be woken without its
 * firmware. Only with boot-arg adreno=N: every address below belongs to this phone.
 *
 * Nothing in this system uses the GPU yet, and how much of it a kernel under Qualcomm's
 * hypervisor may reach is not known: an access the hypervisor does not like ends the run, at
 * once or a quarter of a minute later, and often without a word. So this finds out in small
 * steps. adreno=N performs steps 1 to N and stops; a step says what it is about to touch before
 * it touches it and what it found after, reads come before writes, and a step that does not find
 * what it needs ends the sequence instead of going on blind.
 *
 *   1  reads   what feeds the GPU from outside: its clocks in the global clock controller, and
 *              the names RPMh knows its supplies by
 *   2  reads   the fuses that say which GPU this is
 *   3  reads   the GPU's own clock controller, as the boot loader left it
 *   4  writes  the "CX" power switch on: the half of the GPU block that is not the 3D core --
 *              the power management processor ("GMU"), the bus interface, the GPU's SMMU
 *   5  writes  the clocks of that half on
 *   6  reads   the first words from the GPU block itself: registers of the GMU (among them a
 *              counter that should be seen counting) and of the bus interface
 *   7  reads   the GMU's own channel to RPMh
 *   8  reads   the SMMU in front of the GPU: its ID registers, and which stream goes where
 *   9  reads   that SMMU's first translation context
 *  10  writes  two words into the GMU's memory and reads them back: the way its firmware goes
 *  11  asks    TrustZone three questions that pass no memory, the last of them whether it can
 *              authenticate a GPU "zap shader" -- without which the 3D core stays locked
 *
 * What is left after that needs files from the phone's Android side: a translation context for
 * the GMU, its firmware (a660_gmu.bin) loaded and started, a conversation with it (HFI) that
 * makes it power the 3D core; then the core's microcode (a660_sqe.fw) and the signed zap shader
 * (a660_zap) that only TrustZone can run to let the core out of secure mode. There is no register
 * that holds the chip ID (0x06030500, "qcom,chipid" in the device tree): the fuses of step 2 are
 * all the hardware says about itself, and the GMU's version is a property of its firmware.
 *
 * More boot-args:
 *   adreno_only=K      step K alone (what steps 6 to 10 need, they check for themselves: within
 *                      one session of the loader, power and clocks stay as an earlier run left them)
 *   adreno_skip=MASK   leave out the steps whose bits are set (bit 2 = step 2)
 *   adreno_pause=MS    how long the newest step waits after announcing a group of accesses
 *                      (default 2200). The loader, watching from another core (bootcpu=N), writes
 *                      the end of our log to storage every two seconds, and after a silent reset
 *                      that note is all there is.
 *   adreno_fine=1      the newest step waits like that before every single access
 *   adreno_watch=S     after the last step, say once a second for S seconds (default 20) that
 *                      the machine is still alive, and what was written last
 * The system does not go on starting until all of this is over, so that the end of the log is
 * ours; the touch screen driver's start would talk across it (leave touchscreen=1 out).
 *
 * Addresses are from the phone's own device tree ("dt:" below; the nodes qcom,kgsl-3d0@3d00000,
 * qcom,gmu@3d69000, clock-controller@3d90000, qcom,gdsc@3d9106c, kgsl-smmu@3da0000,
 * qfprom@780000), register layouts from Linux, where SC7280 is this SoC: drivers/clk/qcom/
 * {gcc,gpucc}-sc7280.c, gdsc.c, clk-rcg2.c, drivers/gpu/drm/msm/adreno/a6xx_gmu.c with
 * registers/adreno/a6xx{,_gmu}.xml, drivers/iommu/arm/arm-smmu/arm-smmu.h, drivers/soc/qcom/
 * cmd-db.c, drivers/firmware/qcom/qcom_scm{,-smc}.c. The phone's vendor kernel agrees on every
 * offset used here.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

static uint32_t last_step, skip_steps, pause_ms = 2200, fine, watch_seconds = 20;
static uint32_t step;                   /* the one being performed */
static uint32_t newest_step;            /* the highest one asked for: the one that has not been seen to work */
static char last_write[96] = "nothing written";

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

/* ---- register access ------------------------------------------------------------------------------- */

/*
 * One access is one plain load or store of a general register. Some of what is touched here (the
 * SMMU at least) is not the hardware but the hypervisor's imitation of it: the access traps and
 * is carried out on our behalf, which works for the simplest instruction forms only, and the
 * vendor's SMMU driver warns that this imitation "goes horribly wrong" for a store of the zero
 * register. So the compiler does not get to choose the instruction.
 */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
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
	if (step == newest_step && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

/* What a step is about to do, in a line. */
static void
announce(const char *what)
{
	printf("adreno: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step == newest_step) {
		printf("adreno: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adreno: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
	    b->phys + offset);
	hold(true);
	wr(b->base + offset, value);
}

/* Set bits of a register unless they are set. Returns what the register holds afterwards. */
static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

/* Look at a register that has been read before until the masked bits have the value, for so many milliseconds. */
static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/*
 * dt: clock-controller@100000 (qcom,yupik-gcc). The GPU's clocks there, by the names the GPU's
 * nodes give them: gcc_gpu_memnoc_gfx and gcc_gpu_snoc_dvm_gfx (its two bus connections),
 * "axi_clk" = gcc_ddrss_gpu_axi (memory), gcc_gpu_cfg_ahb (the registers of its clock controller);
 * and the two taps of the 600 MHz PLL that its clock controller takes as inputs. None of them is
 * among that node's protected-clocks, the ones the hypervisor keeps to itself. Offsets:
 * gcc-sc7280.c. The touch screen driver uses this controller too.
 */
static struct block gcc = { "gcc", 0x00100000, 0x80000 };
#define GCC_PLL_VOTE            0x52000         /* bit 15: gcc_gpu_gpll0_clk_src (600 MHz), bit 16: ..._div_clk_src (300 MHz) */
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)
#define GCC_GPU_CFG_AHB_CBCR    0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR 0x7100c
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x71018
#define GCC_DDRSS_GPU_AXI_CBCR  0x71154
#define CBCR_ENABLE             (1u << 0)       /* a clock's control register: on */
#define CBCR_OFF                (1u << 31)      /* ... and whether it is in fact off */

/* dt: reserved-memory cmd_db@80860000: the boot loader's list of what RPMh can switch (cmd-db.c) */
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

/*
 * Say where RPMh has a resource and, for a supply rail, which levels it knows: the GMU's firmware
 * is told the levels by their position in this list.
 */
static void
show_rpmh_resource(const struct cmd_db_header *db, const char *name)
{
	char id[8] = { 0 }, levels[160];

	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;
			int at = 0;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				count = 0;
			}
			levels[0] = 0;
			for (unsigned l = 0; l < count && l < 16 && at < (int)sizeof(levels) - 8; l++) {
				unsigned level = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && level == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
			}
			printf("adreno: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, entry[i].address, levels);
			return;
		}
	}
	printf("adreno: %u: RPMh has no \"%s\"\n", step, name);
}

static bool
step_outside(void)
{
	const struct cmd_db_header *db;

	announce("read the GPU's clocks in the global clock controller (registers the touch screen driver uses too)");
	get(&gcc, "pll votes", GCC_PLL_VOTE);
	get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	get(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR);
	get(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
	get(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);

	announce("look up the GPU's supply rails in the boot loader's list of RPMh resources (memory at 0x80860000)");
	db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);   /* memory, not registers */
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("adreno: %u: no list there (magic %02x %02x %02x %02x)\n", step, db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return true;                    /* nothing before step 10 depends on it */
	}
	show_rpmh_resource(db, "gfx.lvl");      /* the 3D core's own rail (dt: VDD_GFX_LEVEL, PM8350C S7) */
	show_rpmh_resource(db, "cx.lvl");       /* the rail of the CX half, shared with most of the SoC */
	show_rpmh_resource(db, "mx.lvl");       /* memories */
	return true;
}

/* ---- step 2: the fuses ------------------------------------------------------------------------------- */

/*
 * dt: qfprom@780000 with gpu_speed_bin@1e9 (two bytes, 8 bits from bit 5) and gpu_gaming_bin@1f5
 * (bit 5). Linux has the same cell 0x4000 further on, in the error-corrected copy of the fuses
 * (sc7280.dtsi: efuse@784000), and that is the copy it reads under this hypervisor on sibling
 * boards; so that one first.
 */
static struct block qfprom = { "qfprom", 0x00780000, 0x7000 };
#define QFPROM_CORRECTED        0x4000
#define QFPROM_GPU_SPEED_BIN    0x1e9
#define QFPROM_GPU_GAMING_BIN   0x1f5

static uint32_t
get8(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step == newest_step) {
		printf("adreno: %u: about to read %s %s at %#x (a byte)\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd8(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static bool
step_fuses(void)
{
	uint32_t bin, gaming;

	announce("read the GPU's fuses in the error-corrected copy (0x784000), which Linux reads");
	bin = get8(&qfprom, "speed bin, low", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN);
	bin |= get8(&qfprom, "speed bin, high", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN + 1) << 8;
	bin = bin >> 5 & 0xff;
	gaming = get8(&qfprom, "gaming bin", QFPROM_CORRECTED + QFPROM_GPU_GAMING_BIN) >> 5 & 1;
	/* dt: qcom,gpu-models: 0xac and 0xbe are sold as Adreno 643, anything else is the 642L */
	printf("adreno: %u: speed bin %u: an Adreno %s; gaming bin %u\n", step, bin, bin == 0xac || bin == 0xbe ? "643" : "642L",
	    gaming);

	announce("read the same fuses where the phone's device tree has them (0x780000)");
	bin = get8(&qfprom, "raw speed bin, low", QFPROM_GPU_SPEED_BIN);
	bin |= get8(&qfprom, "raw speed bin, high", QFPROM_GPU_SPEED_BIN + 1) << 8;
	printf("adreno: %u: raw speed bin %u\n", step, bin >> 5 & 0xff);
	return true;
}

/* ---- step 3: the GPU's clock controller ----------------------------------------------------------------- */

/*
 * dt: clock-controller@3d90000 (qcom,yupik-gpucc), 0x9000 long; offsets: gpucc-sc7280.c. Its
 * registers answer only while gcc_gpu_cfg_ahb runs, which Linux turns on for good when it
 * starts (gcc-sc7280.c, the end of the probe function).
 */
static struct block gpucc = { "gpucc", 0x03d90000, 0x9000 };
#define GPUCC_PLL0_MODE         0x0000          /* the 3D core's PLL: bit 31 locked, bit 0 output on; +4: times 19.2 MHz */
#define GPUCC_PLL1_MODE         0x0100          /* a 500 MHz PLL, an alternative source for the GMU's clock */
#define GPUCC_CXO_AON_CBCR      0x1004
#define GPUCC_GX_SW_RESET       0x1008          /* dt: gpu_gx_sw_reset */
#define GPUCC_GX_GDSCR          0x100c          /* dt: gpu_gx_gdsc, the 3D core's power switch: the GMU's to operate */
#define GPUCC_GX_GMU_CBCR       0x1064
#define GPUCC_CX_GDSCR          0x106c          /* dt: gpu_cx_gdsc */
#define GPUCC_AHB_CBCR          0x1078          /* gpu_cc_ahb_clk: the register bus inside the GPU block */
#define GPUCC_CX_GMU_CBCR       0x1098          /* gpu_cc_cx_gmu_clk: the GMU */
#define GPUCC_CXO_CBCR          0x109c          /* gpu_cc_cxo_clk: the crystal, for the GMU's timers */
#define GPUCC_GMU_RCGR          0x1120          /* gpu_cc_gmu_clk_src: +0 command, +4 source and divider */
#define GPUCC_CB_CBCR           0x1170          /* gpu_cc_cb_clk: Linux keeps it on always */
#define GPUCC_HUB_AON_CBCR      0x1178          /* gpu_cc_hub_aon_clk */
#define GPUCC_HUB_RCGR          0x117c          /* gpu_cc_hub_clk_src */
#define GPUCC_HUB_CX_INT_CBCR   0x1204          /* gpu_cc_hub_cx_int_clk */
#define GPUCC_CX_HW_CTRL        0x1540          /* dt: gpu_cx_hw_ctrl: bit 31 is where the vendor's driver sees CX come on */
#define GPUCC_GX_DOMAIN_MISC    0x158c          /* dt: gpu_gx_domain_addr */
#define GPUCC_SMMU_VOTE_CBCR    0x5000          /* gpu_cc_hlos1_vote_gpu_smmu_clk: our vote for the SMMU's clock */

#define GDSC_SW_COLLAPSE        (1u << 0)       /* a power switch's control register (gdsc.c): off, by software */
#define GDSC_HW_CONTROL         (1u << 1)       /* something else than software operates it */
#define GDSC_POWER_ON           (1u << 31)

static bool
step_gpucc(void)
{
	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
	if ((set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE) & CBCR_OFF) &&
	    !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
		printf("adreno: %u: it stays off (%#x): the GPU's clock controller would not answer\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}

	announce("read the GPU's clock controller (0x3d90000) as it is: the first access to the GPU's part of the SoC");
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 multiplier", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
	get(&gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
	get(&gpucc, "hub_clk_src command", GPUCC_HUB_RCGR);
	get(&gpucc, "hub_clk_src source", GPUCC_HUB_RCGR + 4);
	get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
	get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&gpucc, "cxo", GPUCC_CXO_CBCR);
	get(&gpucc, "cb", GPUCC_CB_CBCR);
	get(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR);
	get(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR);
	get(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR);
	return true;
}

/* ---- step 4: CX power --------------------------------------------------------------------------------- */

/* Not to touch the GPU's clock controller without the clock of its registers: where step 3 has not been. */
static bool
gpucc_answers(void)
{
	need(&gcc);
	need(&gpucc);
	if (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
		printf("adreno: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x): step 3\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}
	return true;
}

static bool
cx_is_on(void)
{
	return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}

/*
 * The switch hangs on the rail "cx.lvl", which is up for as long as the SoC runs at all; the
 * vendor's driver (gdsc-regulator.c) clears the collapse bit and waits for bit 31 of the register
 * the device tree calls hw-ctrl-addr, Linux for bit 31 of the control register itself.
 */
static bool
step_cx_power(void)
{
	uint32_t gdscr;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cx_gdsc): clear the collapse bit of its power switch");
	gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (cx_is_on()) {
		printf("adreno: %u: CX is on already\n", step);
		return true;
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("adreno: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (!cx_is_on()) {
		printf("adreno: %u: CX does not come on\n", step);
		return false;
	}
	printf("adreno: %u: CX is on\n", step);
	return true;
}

/* ---- step 5: clocks ------------------------------------------------------------------------------------ */

#define RCG_UPDATE              (1u << 0)       /* command register (clk-rcg2.c): take the new source and divider */
#define RCG_SOURCE_MASK         0x371fu         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define RCG_GPLL0_DIV(twice)    (6u << 8 | ((twice) - 1))       /* source 6: the 300 MHz tap; twice the divider wanted */

/* Turn a clock on and see it run. Not every clock's status can be believed (halt_check in Linux): `must` says which. */
static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
	if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("adreno: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; its status bit is not one to rely on, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("adreno: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/*
 * The clocks the device tree gives the GMU and the SMMU, at the rates both the vendor's driver
 * and Linux bring the GMU up with: 200 MHz for the GMU, 150 MHz for the "hub" (a6xx_gmu_resume).
 * Both come from the 300 MHz tap of the global 600 MHz PLL, which runs anyway: no PLL of the
 * GPU's own has to be started.
 */
static bool
step_clocks(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("adreno: %u: CX is off: step 4\n", step);
		return false;
	}
	announce("in the global clock controller: ask for the 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
	set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	if (!clock_on(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, true) ||
	    !clock_on(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false)) {
		return false;
	}

	announce("in the GPU's clock controller: the GMU's clock to 200 MHz, the hub's to 150 MHz");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3)) ||       /* 300 MHz / 1.5 */
	    !clock_source("hub_clk_src", GPUCC_HUB_RCGR, RCG_GPLL0_DIV(4))) {       /* 300 MHz / 2 */
		return false;
	}

	announce("in the GPU's clock controller: the clocks of the GMU and of the SMMU on");
	return clock_on(&gpucc, "cb", GPUCC_CB_CBCR, false) &&
	       clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) &&
	       clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false) &&
	       clock_on(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR, true) &&
	       clock_on(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR, true) &&
	       clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true) &&
	       clock_on(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR, false);
}

/* What every later step stands on: CX powered and its clocks running. */
static bool
cx_ready(void)
{
	uint32_t gmu_clock, ahb_clock;

	if (!gpucc_answers()) {
		return false;
	}
	gmu_clock = rd(gpucc.base + GPUCC_CX_GMU_CBCR);
	ahb_clock = rd(gpucc.base + GPUCC_AHB_CBCR);
	if (cx_is_on() && (gmu_clock & (CBCR_ENABLE | CBCR_OFF)) == CBCR_ENABLE && (ahb_clock & CBCR_ENABLE)) {
		return true;
	}
	printf("adreno: %u: needs CX on (it is %s) and its clocks running (cx_gmu %#x, ahb %#x): steps 4 and 5\n", step,
	    cx_is_on() ? "on" : "off", gmu_clock, ahb_clock);
	return false;
}

/* ---- step 6: the first words from the GPU block ------------------------------------------------------- */

/*
 * dt: qcom,gmu "kgsl_gmu_reg" at 0x3d6a000, 0x34000 long. Offsets in a6xx_gmu.xml are in words.
 */
static struct block gmu = { "gmu", 0x03d6a000, 0x34000 };
#define GMU_WORD(n)             (4u * (n))
#define GMU_DTCM                GMU_WORD(0x1c00)        /* the processor's 16K of data memory (its code: 0x0c00) */
#define GMU_DTCM_VERSIONS       GMU_WORD(0x1c00 + 0xff8) /* five words where loaded firmware carries its versions */
#define GMU_CM3_SYSRESET        GMU_WORD(0x5000)        /* 1: the processor is held in reset */
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x501c)        /* 0xbabeface once firmware has started */
#define GMU_CM3_CFG             GMU_WORD(0x502d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)        /* counts the 19.2 MHz crystal */
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x50d0)     /* bit 6: the 3D core has no power, bit 7: no clock */
#define GMU_RPMH_POWER_STATE    GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x5181)
#define GMU_GENERAL_7           GMU_WORD(0x51cc)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x9305)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x9308)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x9313)

/*
 * dt: qcom,kgsl-3d0 "kgsl_3d0_reg_memory" at 0x3d00000 is the 3D core, and without power (the GMU
 * gives it) not to be touched -- except its bus interface ("GBIF", words 0x3c00 on in a6xx.xml),
 * which is in the CX half: the vendor's driver writes GBIF_HALT before it starts the GMU.
 * "cx_misc" at 0x3d9e000 is a page of odds and ends in the CX half.
 */
static struct block gbif = { "gbif", 0x03d00000 + 4 * 0x3c00, 0x1000 };
#define GBIF_SCACHE_CNTL0       (4 * 0x01)
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)
static struct block cx_misc = { "cx_misc", 0x03d9e000, 0x1000 };
#define CX_MISC_SYSTEM_CACHE_CNTL_0 (4 * 1)
#define CX_MISC_SYSTEM_CACHE_CNTL_1 (4 * 2)

static bool
step_first_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz;

	if (!cx_ready()) {
		return false;
	}
	announce("read registers of the GMU (0x3d6a000): the first words from the GPU block itself");
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "general_7", GMU_GENERAL_7);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	printf("adreno: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
	    "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz (firmware may start it): see the rest");
	for (uint32_t i = 0; i < 5; i++) {
		get(&gmu, "dtcm, where firmware has its versions", GMU_DTCM_VERSIONS + 4 * i);
	}

	announce("read the GPU's bus interface (0x3d0f000) and the CX odds and ends (0x3d9e000)");
	get(&gbif, "scache_cntl0", GBIF_SCACHE_CNTL0);
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&cx_misc, "system_cache_cntl_0", CX_MISC_SYSTEM_CACHE_CNTL_0);
	get(&cx_misc, "system_cache_cntl_1", CX_MISC_SYSTEM_CACHE_CNTL_1);
	return true;
}

/* ---- step 7: the GMU's channel to RPMh ----------------------------------------------------------------- */

/*
 * dt: qcom,kgsl-3d0 "rscc" at 0x3de0000: the same kind of command slots through which this
 * kernel asks RPMh for the touch screen's supply, but the GMU's own; through them its firmware
 * raises the 3D core's rail. Offsets in words: a6xx_gmu.xml, domain A6XX_RSCC.
 */
static struct block rscc = { "rscc", 0x03de0000, 0x1000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0     (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)

static bool
step_rscc(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the GMU's RPMh channel (0x3de0000)");
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	return true;
}

/* ---- steps 8 and 9: the SMMU --------------------------------------------------------------------------- */

/*
 * dt: kgsl-smmu@3da0000 (qcom,qsmmu-v500: an Arm MMU-500), 0x20000 long, with "qcom,skip-init":
 * the vendor's driver leaves its stream table as it finds it. Its streams (dt: qcom,kgsl-iommu
 * and qcom,gmu, "iommus"): 0 the GPU, 1 its low-priority twin, 2 secure content, 5 the GMU.
 * The global registers here are the ones the hypervisor imitates; offsets: arm-smmu.h.
 */
static struct block smmu = { "smmu", 0x03da0000, 0x20000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page; [17:16] kind of context */
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;

/* The layout of the SMMU's register space from its ID registers; false if it is not what the device tree says. */
static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("adreno: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page != smmu.size) {
		printf("adreno: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	return true;
}

static bool
step_smmu(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x3da0000): global registers, which the hypervisor answers for");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);

	announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader and hypervisor left them");
	for (uint32_t i = 0; i < smmu_matches && i < 16; i++) {
		get(&smmu, "smr", SMMU_SMR(i));
		get(&smmu, "s2cr", SMMU_S2CR(i));
	}
	for (uint32_t i = 0; i < smmu_contexts && i < 16; i++) {
		get(&smmu, "cbar", smmu_page + SMMU_CBAR(i));
	}
	return true;
}

/*
 * Only context 0, the one both the vendor's driver and Linux give the GPU itself: of the others
 * at least one belongs to TrustZone (secure content), and not all can be expected to answer.
 */
static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!cx_ready()) {
		return false;
	}
	announce("read translation context 0 of the SMMU: registers of the hardware itself, not the hypervisor's");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- step 10: the GMU's memory -------------------------------------------------------------------------- */

/*
 * Firmware is loaded by writing it through these registers into the processor's memory while
 * the processor is held in reset (a6xx_gmu_fw_load). Two words of data memory go there and back,
 * and what was there is put back.
 */
static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2];

	if (!cx_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory (0x3d71000), read them back");
	if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == 0xbabeface) {
		printf("adreno: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("adreno: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- step 11: TrustZone -------------------------------------------------------------------------------- */

/*
 * The 3D core comes out of reset in "secure" mode, and on a phone only TrustZone lets it out: it
 * does when handed the vendor's signed zap shader to authenticate (a6xx_gpu.c: a6xx_hw_init;
 * qcom_scm.c: qcom_scm_pas_*; the GPU is image 13). Whether it will do that for this kernel
 * decides whether there can be a GPU at all. Three questions that pass no memory show whether it
 * listens: the one Linux asks before anything else (is the call "is this call available"
 * available), whether the calls that authenticate an image are, and whether image 13 is one it
 * authenticates. A call is an SMC, which the hypervisor passes on; convention: qcom_scm-smc.c.
 */
#define SCM_FAST                (1ull << 31)            /* answered at once; without it a call may be preempted */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */
#define PAS_GPU                 13

/* One call with one argument. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t argument, uint64_t *answer)
{
	uint64_t session = 0;

	printf("adreno: %u: about to ask TrustZone %s: smc %#llx, argument %#llx\n", step, what, function, argument);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx)", step, function, argument);
	hold(true);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = SCM_ONE_VALUE;
		register uint64_t x2 __asm__("x2") = argument;
		register uint64_t x3 __asm__("x3") = 0;
		register uint64_t x4 __asm__("x4") = 0;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adreno: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adreno: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static bool
step_trustzone(void)
{
	uint64_t answer = 0, how = SCM_FAST | SCM_64 | SCM_SIP;

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", how | SCM_IS_CALL_AVAILABLE, SCM_SIP | SCM_IS_CALL_AVAILABLE, &answer) != 0 ||
	    answer != 1) {
		how = SCM_FAST | SCM_SIP;       /* the same with 32-bit arguments, Linux's second try */
		if (scm("the same, the 32-bit way", how | SCM_IS_CALL_AVAILABLE, SCM_SIP | SCM_IS_CALL_AVAILABLE, &answer) != 0 ||
		    answer != 1) {
			printf("adreno: %u: TrustZone does not answer the way it answers Linux\n", step);
			return false;
		}
	}

	announce("ask TrustZone whether it has the calls that authenticate a firmware image, and whether the GPU's is one");
	scm("for the call that takes an image's signature", how | SCM_IS_CALL_AVAILABLE, SCM_SIP | SCM_PAS_INIT_IMAGE, &answer);
	scm("for the call that authenticates and starts an image", how | SCM_IS_CALL_AVAILABLE, SCM_SIP | SCM_PAS_AUTH_AND_RESET,
	    &answer);
	if (scm("whether the GPU's zap shader is an image it authenticates", how | SCM_PAS_IS_SUPPORTED, PAS_GPU, &answer) != 0) {
		/* Linux makes this one as a call that may be preempted */
		if (scm("the same, as a slow call", (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, PAS_GPU, &answer) != 0) {
			printf("adreno: %u: TrustZone does not say\n", step);
			return false;
		}
	}
	printf("adreno: %u: TrustZone %s\n", step, answer ? "CAN AUTHENTICATE A GPU ZAP SHADER" :
	    "says it does not authenticate a GPU zap shader");
	return answer != 0;
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks, step_first_words, step_rscc, step_smmu,
	step_smmu_context, step_gmu_memory, step_trustzone,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/*
 * Called while the system starts, before there is a root file system, and the start waits: for
 * as long as the steps and the watch after them last, nothing else writes to the log, so that
 * its end -- all that the loader's notes keep -- is about what is being done here.
 */
void
redwood_adreno_init(void)
{
	uint32_t only = 0, done = 0;

	PE_parse_boot_argn("adreno", &last_step, sizeof(last_step));
	PE_parse_boot_argn("adreno_skip", &skip_steps, sizeof(skip_steps));
	if (PE_parse_boot_argn("adreno_only", &only, sizeof(only)) && only > 0 && only <= STEPS) {
		last_step = only;
		skip_steps = ~(1u << only);
	}
	if (last_step == 0) {
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	PE_parse_boot_argn("adreno_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("adreno_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("adreno_watch", &watch_seconds, sizeof(watch_seconds));
	for (newest_step = last_step; newest_step > 0 && (skip_steps & 1u << newest_step); newest_step--) {
	}

	printf("adreno: steps up to %u, without those in mask %#x; step %u is the new one and waits %u ms %s\n", last_step,
	    skip_steps, newest_step, pause_ms, fine ? "before every access" : "after each announcement");
	for (step = 1; step <= last_step; step++) {
		if (skip_steps & 1u << step) {
			continue;
		}
		if (!steps[step - 1]()) {
			printf("adreno: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adreno: step %u: done\n", step);
		done = step;
	}
	/*
	 * Punishment for a write may come late (a reset some 17 seconds after it has been seen):
	 * keep the end of the log saying where things stand.
	 */
	for (uint32_t second = 1; second <= watch_seconds; second++) {
		sleep_ms(1000);
		printf("adreno: alive %u s after step %u; %s\n", second, done, last_write);
	}
	printf("adreno: over; the system goes on starting\n");
}
