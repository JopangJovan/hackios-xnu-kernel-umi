/* Xiaomi Mi 10 PCIe2 bootstrap for the SDX55 modem. Register values and PHY
 * data are from the merged UMI stock DT (qcom,pcie@1c10000) and Kona register
 * definitions. Same controller layout as PCIe0 (the Wi-Fi's), different base.
 * The PHY sequence is PCIe2 modem PHY specific (umi_pcie2_phy.h).
 */
#include <sys/umi_pcie.h>
#include <sys/umi_mhi.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.audio.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/codesign.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include <sys/ubc.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <os/atomic.h>
#include "umi_pcie2_phy.h"
#include "umi_sdx55_wan.h"
#include "umi_sdx55_wds.h"
#include "umi_sdx55_qmap.h"
#include "umi_sdx55_ims.h"
#include "umi_sdx55_voice_wire.h"
#include <sys/umi_sdx55_net.h>
#include <sys/umi_sdx55_cell_status.h>
#include "umi_sdx55_sbl1.inc"
#include "umi_sdx55_ddr.inc"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern unsigned int pmap_cache_attributes(ppnum_t);
extern void pmap_set_cache_attributes(ppnum_t, unsigned int);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
#include "umi_sdx55_cell_public.inc"
static struct umi_sdx55_radio_snapshot sdx55_radio = {
	.size = sizeof(sdx55_radio), .version = UMI_SDX55_RADIO_VERSION, .rsrp = INT32_MIN
};

static bool
sdx55_data_enabled(void)
{
	lck_mtx_lock(&cell_status_lock);
	bool enabled = sdx55_radio.enabled;
	lck_mtx_unlock(&cell_status_lock);
	return enabled;
}

static void
sdx55_data_state(enum umi_sdx55_data_state state)
{
	lck_mtx_lock(&cell_status_lock);
	if (state == UMI_DATA_UNAVAILABLE) {
		sdx55_radio.enabled = sdx55_radio.rat = 0;
		sdx55_radio.rsrp = INT32_MIN;
		bzero(sdx55_radio.plmn, sizeof(sdx55_radio.plmn));
		bzero(sdx55_radio.operator_name, sizeof(sdx55_radio.operator_name));
	}
	if (sdx55_radio.state == UMI_DATA_UNAVAILABLE && state == UMI_DATA_CONNECTING)
		sdx55_radio.enabled = 1;
	sdx55_radio.state = state;
	lck_mtx_unlock(&cell_status_lock);
	hakios_tl("UMI SDX55: mobile data state=%u\n", state);
}

extern void loader_return(void);
extern int umi_rpmh_wifi_power(void);
extern int umi_rpmh_cx_vote(unsigned corner);
extern uint64_t umi_camera_iomap_free(void);
extern lck_mtx_t hakios_smmu_lock;
extern kern_return_t thread_terminate(thread_t);
void umi_sdx55_init(void);

#define GCC  0x00100000u
#define GPIO 0x0f500000u /* Kona SOUTH: TLMM 0x0f000000 + tile 0x00500000. */
#define PARF 0x01c10000u
#define PHY  0x01c16000u
#define DBI  0x64000000u
#define ATU  0x64001000u
#define CONF 0x64100000u
#define BAR  0x64300000u
#define SDX55_IOVA 0x20000000ull
#define SDX55_DMA_SIZE 0x200000u
#define SDX55_SAT_IOVA (SDX55_IOVA + SDX55_DMA_SIZE)
#define SDX55_SAT_BYTES 0x51000u
#define SDX55_SAT_TABLE 0x4000u
#define SDX55_DMA_TABLE_BYTES 0x5000u
#define SDX55_SID  0x1d01u
#define SDX55_MSI_ADDRESS 0x17a10040ull
#define SDX55_MSI_DATA 864u /* Live UMI DT: PCIe2 SPI 832, GIC number = SPI + 32. */
#define SDX55_SMMU_FSR_FAULT 0xc00001feu
#define BIT(n) (1u << (n))
#define TRY(expr) do { error = (expr); if (error) goto fail; } while (0)

static uint32_t rd(struct umi_pcie *p, uint32_t addr)
{
	return p->io->read32(p->io->context, addr);
}
static void wr(struct umi_pcie *p, uint32_t addr, uint32_t value)
{
	p->io->write32(p->io->context, addr, value);
}
static void pause_us(struct umi_pcie *p, unsigned us)
{
	p->io->delay_us(p->io->context, us);
}
static void report(struct umi_pcie *p, const char *name, uint32_t value)
{
	if (p->io->report) p->io->report(p->io->context, name, value);
}
static int update(struct umi_pcie *p, uint32_t addr, uint32_t clear, uint32_t set)
{
	uint32_t value = rd(p, addr);
	if (value == UINT32_MAX) return EIO;
	wr(p, addr, (value & ~clear) | set);
	return 0;
}
static int wait_bits(struct umi_pcie *p, uint32_t addr, uint32_t mask,
    uint32_t wanted, unsigned timeout_us)
{
	uint32_t value = 0;
	for (unsigned us = 0; us < timeout_us; us += 10) {
		value = rd(p, addr);
		if (value == UINT32_MAX) return EIO;
		if ((value & mask) == wanted) return 0;
		pause_us(p, 10);
	}
	report(p, "poll timeout address", addr);
	report(p, "poll timeout value", value);
	return ETIMEDOUT;
}
static int gpio_config(struct umi_pcie *p, unsigned pin, uint32_t config)
{
	return update(p, GPIO + pin * 0x1000, 0x3ff, config);
}
static int gpio_output(struct umi_pcie *p, unsigned pin, bool high, uint32_t config)
{
	/* Set output latch before output-enable/mux to avoid a reset glitch. */
	int error = update(p, GPIO + pin * 0x1000 + 4, BIT(1), high ? BIT(1) : 0);
	if (error) return error;
	return gpio_config(p, pin, config | BIT(9));
}
static uint32_t gpio_level(struct umi_pcie *p, unsigned pin)
{
	return rd(p, GPIO + pin * 0x1000 + 4) & BIT(0);
}
static int sdx55_modem_handshake(struct umi_pcie *p)
{
	int error;
	/* UMI's qcom,ext-sdx55m node has no modem soft-reset signal.  Its active
	 * pinctrl state contains only this four-wire GPIO handshake. */
	report(p, "AP2MDM STATUS config before", rd(p, GPIO + 56 * 0x1000));
	report(p, "AP2MDM STATUS level before", gpio_level(p, 56));
	report(p, "MDM2AP STATUS config before", rd(p, GPIO + 3 * 0x1000));
	report(p, "MDM2AP STATUS level before", gpio_level(p, 3));
	report(p, "MDM2AP ERRFATAL level before", gpio_level(p, 1));
	TRY(gpio_config(p, 1, 3u << 6));       /* input, no bias, 8 mA */
	TRY(gpio_config(p, 3, 3u << 6));       /* input, no bias, 8 mA */
	TRY(gpio_output(p, 57, false, 7u << 6)); /* AP error-fatal low, 16 mA */
	/* Match the ESOC probe/power-on transition: AP offline first, then ready. */
	TRY(gpio_output(p, 56, false, 7u << 6));
	pause_us(p, 150000);
	TRY(gpio_output(p, 56, true, 7u << 6));  /* AP status high, 16 mA */
	report(p, "AP2MDM STATUS asserted", gpio_level(p, 56));
	/* Qualcomm treats MDM2AP_STATUS as a run-state indication after image
	 * transfer, not as a prerequisite for PCIe/BHI.  Record it without
	 * spending the bounded probe window waiting for mission mode. */
	report(p, "MDM2AP STATUS before BHI", gpio_level(p, 3));
	report(p, "MDM2AP ERRFATAL before BHI", gpio_level(p, 1));
	return 0;
fail:
	return error;
}
static int source_rate(struct umi_pcie *p, uint32_t cmd, uint32_t cfg)
{
	int error = update(p, cmd + 4, 0x371f, cfg);
	if (error) return error;
	error = update(p, cmd, 0, 1);
	return error ? error : wait_bits(p, cmd, 1, 0, 10000);
}
static int branch(struct umi_pcie *p, uint32_t offset)
{
	int error = update(p, GCC + offset, 0, 1);
	return error ? error : wait_bits(p, GCC + offset, BIT(31), 0, 10000);
}
static int clocks(struct umi_pcie *p)
{
	int error;
	/* PCIe2 GDSC 0x6004: clear collapse, wait power-up. */
	TRY(update(p, GCC + 0x6004, 7, 0));
	pause_us(p, 1);
	TRY(wait_bits(p, GCC + 0x6004, BIT(31), BIT(31), 10000));
	/* PCIe2 aux source (RCG2 0x6038). */
	TRY(source_rate(p, GCC + 0x6038, 1)); /* 19.2 MHz XO, divide by 1. */
	/* The live UMI DT supplies the modem reference and a 100 MHz PHY
	 * refgen clock. F(100 MHz, GPLL0, /6) encodes as SRC=1, HID=11. */
	report(p, "enabling PHY refgen source", GCC + 0x6f014);
	TRY(source_rate(p, GCC + 0x6f014, 0x10b));
	report(p, "enabling PCIe2 PHY refgen", GCC + 0x6f034);
	TRY(branch(p, 0x6f034));
	report(p, "enabling SDX55 modem reference", GCC + 0x8c00c);
	TRY(branch(p, 0x8c00c));
	report(p, "enabling shared PCIe TBU", GCC + 0x9000c);
	TRY(branch(p, 0x9000c));
	report(p, "PCIe2 reference clocks ready", 1);
	/* Clear AXI/AHB halt votes so the bus clocks can come up. */
	TRY(update(p, GCC + 0x601c, 0x6000, 0)); /* mstr_axi halt */
	TRY(update(p, GCC + 0x6014, 0x6002, 0)); /* slv_axi halt */
	TRY(update(p, GCC + 0x6024, BIT(1), 0)); /* cfg_ahb halt */
	TRY(update(p, GCC + 0x602c, 0x6000, 0)); /* pipe halt */
	/* Enable AXI/AHB clocks (bit 10-13); cfg_ahb gates PARF access. */
	TRY(update(p, GCC + 0x52010, 0, 0x3c00));
	static const uint32_t gates[] = {0x6010, 0x6014, 0x601c, 0x6024};
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
		TRY(wait_bits(p, GCC + gates[i], BIT(31), 0, 10000));
	/* Enable aux (bit 14) after its source is set. */
	TRY(update(p, GCC + 0x52010, 0, BIT(14)));
	TRY(wait_bits(p, GCC + 0x6028, BIT(31), 0, 10000));
	/* Enable PIPE (bit 15) last; halt_check is BRANCH_HALT_SKIP. */
	TRY(update(p, GCC + 0x52010, 0, BIT(15)));
	static const uint32_t resets[] = {0x6000};
	for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
		TRY(update(p, GCC + resets[i], 0, 1));
		pause_us(p, 1000);
		TRY(update(p, GCC + resets[i], 1, 0));
	}
	/* pci-msm.c resets the controller and PHY before pcie_phy_init() writes
	 * the DT sequence. Resetting after that sequence discards its state. */
	TRY(update(p, GCC + 0x1f01c, 0, 1));
	pause_us(p, 1000);
	TRY(update(p, GCC + 0x1f01c, 1, 0));
	pause_us(p, 1000);
	return 0;
fail:
	return error;
}

static int outbound(struct umi_pcie *p, unsigned index, uint32_t type,
    uint32_t start, uint32_t end, uint32_t target)
{
	uint32_t a = ATU + index * 0x200;
	report(p, "iATU control before", rd(p, a + 4));
	wr(p, a + 4, 0);
	wr(p, a, type);
	wr(p, a + 8, start);
	wr(p, a + 12, 0);
	wr(p, a + 16, end);
	wr(p, a + 20, target);
	wr(p, a + 24, 0);
	wr(p, a + 4, BIT(31));
	int error = wait_bits(p, a + 4, BIT(31), BIT(31), 1000);
	report(p, "iATU control after", rd(p, a + 4));
	return error;
}

static struct {
	struct umi_pcie *owner;
	uint32_t address, old_config, old_table[256];
	bool changed;
} umi_pcie2_dma_route;

static void
umi_pcie2_restore_dma_route(struct umi_pcie *p)
{
	if (!umi_pcie2_dma_route.changed || umi_pcie2_dma_route.owner != p) return;
	wr(p, PARF + 0x2c00, umi_pcie2_dma_route.old_config);
	for (unsigned i = 0; i < 256; i++)
		wr(p, PARF + 0x2000 + 4 * i,
		    umi_pcie2_dma_route.old_table[i]);
	report(p, "BDF-to-SID route restored", rd(p, umi_pcie2_dma_route.address));
	umi_pcie2_dma_route.changed = false;
	umi_pcie2_dma_route.owner = NULL;
}

void umi_pcie2_quiesce(struct umi_pcie *p)
{
	if (!p || !p->io) return;
	bool reset = false;
	if (p->dma_enabled) {
		/* PERST before endpoint teardown: after a successful BHI transfer the
		 * SDX55 internally resets and config/BAR/DBI reads can all deadlock.
		 * Reset physically stops DMA without depending on endpoint MMIO. */
		(void)gpio_output(p, 85, false, 1);
		pause_us(p, 1000);
		reset = true;
		p->dma_enabled = false;
		p->msi_cap = 0;
	} else if (p->msi_cap) {
		uint32_t cap = rd(p, CONF + p->msi_cap);
		if (cap != UINT32_MAX) wr(p, CONF + p->msi_cap, cap & ~BIT(16));
		p->msi_cap = 0;
	}
	if (!reset) {
		(void)gpio_output(p, 85, false, 1);
		pause_us(p, 1000); /* PERST propagation before SMMU restoration. */
	}
	umi_pcie2_restore_dma_route(p);
	(void)gpio_config(p, 86, 3); /* GPIO input, pull-up while quiesced. */
	p->enumerated = false;
}

int umi_pcie2_enumerate(struct umi_pcie *p, const struct umi_pcie_io *io)
{
	if (!p || !io || !io->read32 || !io->write32 || !io->power_on || !io->delay_us)
		return EINVAL;
	if (p->io) return EBUSY;
	p->io = io;
	int error;
	TRY(gpio_output(p, 85, false, 1));       /* PERST low */
	pause_us(p, 1000);
	report(p, "power_on skipped (WiFi-rail)", 1);
	TRY(gpio_config(p, 87, 3));              /* WAKE: GPIO, pull-up, 2 mA input. */
	TRY(gpio_config(p, 86, 3 | BIT(2)));     /* CLKREQ: pci_e2 mux function 1. */
	pause_us(p, 1000);
	TRY(clocks(p));
	report(p, "power and clocks ready", rd(p, GCC + 0x6004));
	TRY(sdx55_modem_handshake(p));
	wr(p, PARF + 0x1000, 4); /* Root complex. */
	TRY(update(p, PARF + 0x40, 1, 0));
	/* Match pci-msm.c and the live UMI DT: DBI offset 0, 64 MiB slave window. */
	wr(p, PARF + 0x350, 0);
	wr(p, PARF + 0x358, 0x04000000);
	wr(p, PARF, 0x365e);
	TRY(update(p, PARF + 0x174, 0, BIT(4)));
	wr(p, PARF + 0x22c, 0);
	TRY(update(p, PARF + 0x1a8, 0, BIT(31)));
	for (unsigned i = 0; i < sizeof(umi_pcie_phy) / sizeof(umi_pcie_phy[0]); i++) {
		wr(p, PHY + umi_pcie_phy[i].offset, umi_pcie_phy[i].value);
		if (umi_pcie_phy[i].delay_us) pause_us(p, umi_pcie_phy[i].delay_us);
	}
	pause_us(p, 1000);
	TRY(update(p, GCC + 0x52010, 0, BIT(15))); /* PIPE */
	/* Qualcomm reports this PHY ready only after status bit 6 clears. */
	TRY(wait_bits(p, PHY + 0xa14, BIT(6), 0, 100000));
	report(p, "PHY ready", rd(p, PHY + 0xa14));
	pause_us(p, 10000);
	TRY(gpio_output(p, 85, true, 1));         /* PERST high */
	pause_us(p, 1000);
	report(p, "DBI access begins", DBI);
	TRY(update(p, DBI + 0x80c, 0x1f00, 0x100));
	wr(p, DBI + 0x8a8, 0x20);
	TRY(update(p, DBI + 0x890, BIT(0), 0));
	TRY(update(p, DBI + 0x8bc, 0, BIT(0)));
	wr(p, DBI + 0x154, 0x55555555);
	TRY(update(p, DBI + 0x8bc, BIT(0), 0));
	TRY(update(p, DBI + 0xa0, 0xf, 2)); /* Gen2 x1 */
	uint32_t control = rd(p, DBI + 0x78);
	if (control == UINT32_MAX) { error = EIO; goto fail; }
	wr(p, DBI + 0x78, (control & 0xffff) & ~0x7000u);
	TRY(update(p, PARF + 0x1b0, 0, BIT(8)));
	TRY(wait_bits(p, DBI + 0xf28, BIT(10), BIT(10), 100000));
	TRY(wait_bits(p, DBI + 0x80, BIT(29), BIT(29), 100000));
	report(p, "link status", rd(p, DBI + 0x80));
	wr(p, DBI + 0x18, 0x00010100);
	wr(p, DBI + 0x20, 0x60306030);
	wr(p, DBI + 0xb40, 0x14);
	TRY(outbound(p, 0, 4, CONF, CONF + 0xfff, 0x01000000));
	uint32_t id = 0;
	for (unsigned i = 0; i < 1000; i++) {
		id = rd(p, CONF);
		if (id == 0x030617cb) break;   /* SDX55: 17cb:0306 */
		pause_us(p, 1000);
	}
	report(p, "SDX55 endpoint PCI ID", id);
	if (id != 0x030617cb) { error = ENODEV; goto fail; }
	uint32_t command = rd(p, CONF + 4), bar = rd(p, CONF + 0x10);
	if (command == UINT32_MAX || bar == UINT32_MAX || (bar & 1) ||
	    ((bar >> 1) & 3) == 1 || ((bar >> 1) & 3) == 3) {
		error = EIO; goto fail;
	}
	report(p, "SDX55 endpoint command", command);
	report(p, "SDX55 endpoint BAR0", bar);
	report(p, "SDX55 endpoint BAR1", rd(p, CONF + 0x14));
	report(p, "SDX55 endpoint BAR2", rd(p, CONF + 0x18));
	p->enumerated = true;
	return 0;
fail:
	p->failed = true;
	report(p, "enumeration failed errno", (uint32_t)error);
	umi_pcie2_quiesce(p);
	return error;
}

int umi_pcie2_enable_dma(struct umi_pcie *p)
{
	if (!p || !p->io || !p->enumerated || p->failed) return ENODEV;
	if (p->dma_enabled) return EALREADY;
	/* Kona hashes the two BDF bytes (bus 1, devfn 0) using CRC-8/0x07.
	 * SID table contains the low byte; PCIe2 supplies the wired 0x1d00 base. */
	uint8_t hash = 0;
	const uint8_t bdf[] = {1, 0};
	for (unsigned i = 0; i < sizeof(bdf); i++) {
		hash ^= bdf[i];
		for (unsigned b = 0; b < 8; b++) hash = (hash << 1) ^ ((hash & 0x80) ? 7 : 0);
	}
	uint32_t route = PARF + 0x2000 + 4 * hash;
	uint32_t wanted = 0x01000100;
	uint32_t old_config = rd(p, PARF + 0x2c00);
	report(p, "BDF-to-SID route address", route);
	if ((umi_pcie2_dma_route.changed && umi_pcie2_dma_route.owner != p) ||
	    old_config == UINT32_MAX)
		return EBUSY;
	umi_pcie2_dma_route.owner = p;
	umi_pcie2_dma_route.address = route;
	umi_pcie2_dma_route.old_config = old_config;
	unsigned stale = 0;
	for (unsigned i = 0; i < 256; i++) {
		uint32_t value = rd(p, PARF + 0x2000 + 4 * i);
		if (value == UINT32_MAX) return EIO;
		umi_pcie2_dma_route.old_table[i] = value;
		stale += value != 0;
	}
	report(p, "BDF-to-SID stale table entries", stale);
	umi_pcie2_dma_route.changed = true;
	/* Qualcomm's msm_pcie_config_sid() clears all 256 CRC8 buckets before
	 * inserting the BDF. PCIe2 SRAM contains nonzero power-on garbage, not
	 * pre-existing owners. Preserve it above so the bounded probe can undo
	 * even this controller-local initialization. */
	for (unsigned i = 0; i < 256; i++)
		wr(p, PARF + 0x2000 + 4 * i, 0);
	wr(p, route, wanted);
	if (rd(p, route) != wanted) {
		umi_pcie2_restore_dma_route(p);
		return EIO;
	}
	uint32_t command = rd(p, CONF + 4);
	if (command == UINT32_MAX || (command & 7) != 2) {
		umi_pcie2_restore_dma_route(p);
		return EIO;
	}
	/* SM8250 defaults to BDF-to-SID bypass. A table alone does not route
	 * requests through the owned context; explicitly enable translation. */
	report(p, "BDF-to-SID configuration before", rd(p, PARF + 0x2c00));
	int error = update(p, PARF + 0x2c00, 1, 0);
	if (error || (rd(p, PARF + 0x2c00) & 1)) {
		umi_pcie2_restore_dma_route(p);
		return EIO;
	}
	report(p, "BDF-to-SID configuration after", rd(p, PARF + 0x2c00));
	wr(p, DBI + 4, 6); /* Allow protected endpoint transactions upstream. */
	wr(p, CONF + 4, (command & 0xffff) | 4);
	p->dma_enabled = true; /* Quiesce must clear it even if readback fails. */
	if ((rd(p, CONF + 4) & 7) != 6) return EIO;
	report(p, "protected DMA stream", 0x1d01);
	return 0;
}

int umi_pcie2_setup_mhi_msi(struct umi_pcie *p, uint64_t address, uint32_t data)
{
	if (!p || !p->io || !p->enumerated || p->failed) return ENODEV;
	if (p->dma_enabled || p->msi_cap) return EBUSY;
	if ((address & 3) || data > 0xffff || (data & 31)) return EINVAL;
	uint32_t cap = rd(p, CONF + 0x34) & 0xff;
	uint64_t seen = 0;
	for (unsigned i = 0; i < 48 && cap; i++) {
		if (cap < 0x40 || cap > 0xfc || (cap & 3) || (seen & (1ull << (cap / 4)))) return EINVAL;
		seen |= 1ull << (cap / 4);
		uint32_t header = rd(p, CONF + cap);
		if (header == UINT32_MAX) return EIO;
		if ((header & 0xff) == 5) {
			uint32_t control = header >> 16;
			bool wide = (control & 0x80) != 0;
			if (((control >> 1) & 7) < 5 || !(control & 0x100)) return ENOTSUP;
			if (!wide && address > UINT32_MAX) return ERANGE;
			uint32_t msg = cap + (wide ? 12 : 8), mask = msg + 4;
			if (mask > 0xfc) return EINVAL;
			wr(p, CONF + cap, header & ~BIT(16));
			wr(p, CONF + mask, UINT32_MAX);
			wr(p, CONF + cap + 4, (uint32_t)address);
			if (wide) wr(p, CONF + cap + 8, (uint32_t)(address >> 32));
			wr(p, CONF + msg, data);
			wr(p, CONF + mask, 0xfffffff8); /* UMI MHI owns vectors 0, 1 and 2. */
			wr(p, CONF + cap, (header & ~(0x71u << 16)) | (0x51u << 16));
			p->msi_cap = cap;
			if (!(rd(p, CONF + cap) & BIT(16))) return EIO;
			report(p, "MHI MSI address low", (uint32_t)address);
			report(p, "MHI MSI data", data);
			return 0;
		}
		cap = (header >> 8) & 0xff;
	}
	return ENODEV;
}


/* ---- SDX55 modem PCIe2 diagnostic probe ---------------------------------------- */

static struct {
	uint32_t address, size;
	vm_offset_t mapping;
} umi_sdx55_regions[] = {
	{ 0x00100000, 0xa0000, 0 }, /* GCC, whole tile like PCIe0 (GDSC/clocks/TBU/refgen) */
	{ 0x0011f000, 0x1000, 0 }, /* GCC PCIe2 PHY BCR 0x11f01c */
	{ 0x0f501000, 0x3000, 0 }, /* GPIO1/GPIO3 MDM2AP ERRFATAL/STATUS */
	{ 0x0f538000, 0x2000, 0 }, /* GPIO56/GPIO57 AP2MDM STATUS/ERRFATAL */
	{ 0x0f555000, 0x3000, 0 }, /* GPIO85-87 PCIe2 pins in SOUTH tile */
	{ 0x01c10000, 0x3000, 0 },  /* PARF */
	{ 0x01c16000, 0x1000, 0 },  /* PHY */
	{ 0x64000000, 0x1000, 0 },  /* DBI + ELBI */
	{ 0x64001000, 0x1000, 0 },  /* unrolled outbound iATU */
	{ 0x64100000, 0x1000, 0 },  /* endpoint configuration */
	{ 0x64300000, 0x1000, 0 },  /* SDX55 MHI BAR; map only after link-up */
};

static vm_offset_t
umi_sdx55_register(uint32_t address)
{
	if (address & 3) return 0;
	for (unsigned i = 0; i < sizeof(umi_sdx55_regions) / sizeof(umi_sdx55_regions[0]); i++) {
		if (!umi_sdx55_regions[i].mapping) continue;
		uint32_t offset = address - umi_sdx55_regions[i].address;
		if (offset <= umi_sdx55_regions[i].size - 4)
			return umi_sdx55_regions[i].mapping + offset;
	}
	hakios_tl("UMI SDX55: unmapped register %#x\n", address);
	return 0;
}

static uint32_t
umi_sdx55_read(void *context, uint32_t address)
{
	(void)context;
	vm_offset_t reg = umi_sdx55_register(address);
	if (!reg) return UINT32_MAX;
	uint32_t value = *(volatile uint32_t *)reg;
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
umi_sdx55_write(void *context, uint32_t address, uint32_t value)
{
	(void)context;
	vm_offset_t reg = umi_sdx55_register(address);
	if (!reg) return;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)reg = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static int
umi_sdx55_power(void *context)
{
	(void)context;
	/* Isolated CX corner vote test. */
	return umi_rpmh_cx_vote(7);
}

static void
umi_sdx55_delay(void *context, unsigned us)
{
	(void)context;
	delay(us);
}

static void
umi_sdx55_report(void *context, const char *stage, uint32_t value)
{
	(void)context;
	hakios_tl("UMI SDX55: %s = %#x\n", stage, value);
}

static const struct umi_pcie_io umi_sdx55_io = {
	.read32 = umi_sdx55_read,
	.write32 = umi_sdx55_write,
	.power_on = umi_sdx55_power,
	.delay_us = umi_sdx55_delay,
	.report = umi_sdx55_report,
};
static struct umi_pcie umi_sdx55_host;

static uint32_t
umi_sdx55_direct_read(vm_offset_t base, unsigned offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
umi_sdx55_direct_write(vm_offset_t base, unsigned offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
umi_sdx55_clean(vm_offset_t address, vm_size_t size)
{
	for (vm_offset_t end = address + size; address < end; address += 64)
		__asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
umi_sdx55_invalidate(vm_offset_t address, vm_size_t size)
{
	for (vm_offset_t end = address + size; address < end; address += 64)
		__asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static const unsigned umi_sdx55_cbregs[] = {
	0, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x38, 0x3c
};

static struct {
	vm_offset_t smmu, gr1, cb, cb_physical, memory, tables;
	vm_size_t memory_size;
	unsigned smr, context;
	uint32_t old_smr, old_s2cr, old_cbar, old_cba2r;
	uint32_t old_cb[sizeof(umi_sdx55_cbregs) / sizeof(umi_sdx55_cbregs[0])];
	bool context_changed, route_installed;
} umi_sdx55_dma;

static bool
sdx55_voice_boot_enabled(void)
{
	uint32_t voice = 0;
	return PE_parse_boot_argn("sdx55_voice", &voice, sizeof(voice)) && voice == 1;
}

static bool
sdx55_voice_diag_enabled(void)
{
	uint32_t diag = 0;
	return PE_parse_boot_argn("sdx55_voice_diag", &diag, sizeof(diag)) && diag == 1;
}

static bool
umi_sdx55_smmu_sync(void)
{
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x610,
	    umi_sdx55_dma.context);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x7f0, 1);
	for (unsigned us = 0; us < 1000; us++) {
		if (!(umi_sdx55_direct_read(umi_sdx55_dma.cb, 0x7f4) & 1))
			return true;
		delay(1);
	}
	return false;
}

/* hakios_smmu_lock must be held.  This owns exactly one previously free SMR
 * and one disabled, fault-free context bank; it never changes global SMMU
 * policy.  The first proof maps one private guard page but does not expose it
 * to endpoint DMA because PCI command Bus Master remains clear. */
static int
umi_sdx55_smmu_install(void)
{
	if (!umi_sdx55_dma.smmu)
		umi_sdx55_dma.smmu = ml_io_map(0x15000000, 0x1000);
	if (!umi_sdx55_dma.smmu) return ENOMEM;
	uint32_t scr0 = umi_sdx55_direct_read(umi_sdx55_dma.smmu, 0);
	uint32_t idr0 = umi_sdx55_direct_read(umi_sdx55_dma.smmu, 0x20);
	uint32_t idr1 = umi_sdx55_direct_read(umi_sdx55_dma.smmu, 0x24);
	unsigned page = (idr1 >> 31) ? 0x10000 : 0x1000;
	unsigned pages = 2u << ((idr1 >> 28) & 7);
	unsigned matches = idr0 & 0xff, contexts = idr1 & 0xff;
	if (!(idr0 & BIT(27)) || (scr0 & BIT(3)) || !matches || matches > 128 ||
	    !contexts || contexts > 128 || contexts > pages ||
	    2ull * page * pages != 0x100000)
		return ENODEV;
	if (!umi_sdx55_dma.gr1)
		umi_sdx55_dma.gr1 = ml_io_map(0x15000000 + page, 0x1000);
	if (!umi_sdx55_dma.gr1) return ENOMEM;

	bool used[128] = { false };
	unsigned free_match = ~0u;
	for (unsigned i = 0; i < matches; i++) {
		uint32_t smr = umi_sdx55_direct_read(umi_sdx55_dma.smmu,
		    0x800 + 4 * i);
		uint32_t s2cr = umi_sdx55_direct_read(umi_sdx55_dma.smmu,
		    0xc00 + 4 * i);
		if (smr & BIT(31)) {
			if ((s2cr & 0xff) < 128) used[s2cr & 0xff] = true;
			uint32_t sid = smr & 0x7fff;
			uint32_t mask = (smr >> 16) & 0x7fff;
			if (!((sid ^ SDX55_SID) & ~mask)) return EBUSY;
		} else if (free_match == ~0u) {
			free_match = i;
		}
	}
	if (free_match == ~0u) return EBUSY;

	unsigned cb;
	for (cb = 0; cb < contexts; cb++) {
		if (used[cb]) continue;
		vm_offset_t physical = 0x15000000 +
		    (uint64_t)(pages + cb) * page;
		vm_offset_t mapped = umi_sdx55_dma.cb &&
		    umi_sdx55_dma.cb_physical == physical ? umi_sdx55_dma.cb :
		    ml_io_map(physical, 0x1000);
		if (!mapped) return ENOMEM;
		if (!(umi_sdx55_direct_read(mapped, 0) & 1) &&
		    !(umi_sdx55_direct_read(mapped, 0x58) & SDX55_SMMU_FSR_FAULT)) {
			umi_sdx55_dma.cb = mapped;
			umi_sdx55_dma.cb_physical = physical;
			break;
		}
	}
	if (cb == contexts) return EBUSY;

	umi_sdx55_dma.smr = free_match;
	umi_sdx55_dma.context = cb;
	umi_sdx55_dma.old_smr = umi_sdx55_direct_read(umi_sdx55_dma.smmu,
	    0x800 + 4 * free_match);
	umi_sdx55_dma.old_s2cr = umi_sdx55_direct_read(umi_sdx55_dma.smmu,
	    0xc00 + 4 * free_match);
	umi_sdx55_dma.old_cbar = umi_sdx55_direct_read(umi_sdx55_dma.gr1, 4 * cb);
	umi_sdx55_dma.old_cba2r = umi_sdx55_direct_read(umi_sdx55_dma.gr1,
	    0x800 + 4 * cb);
	for (unsigned i = 0; i < sizeof(umi_sdx55_cbregs) /
	    sizeof(umi_sdx55_cbregs[0]); i++)
		umi_sdx55_dma.old_cb[i] = umi_sdx55_direct_read(umi_sdx55_dma.cb,
		    umi_sdx55_cbregs[i]);

	if (!umi_sdx55_dma.memory || !umi_sdx55_dma.memory_size ||
	    umi_sdx55_dma.memory_size > 0x200000 ||
	    (umi_sdx55_dma.memory_size & 0xfff))
		return EINVAL;
	memset((void *)umi_sdx55_dma.tables, 0, SDX55_DMA_TABLE_BYTES);
	uint64_t *l1 = (uint64_t *)umi_sdx55_dma.tables;
	uint64_t *l2 = (uint64_t *)(umi_sdx55_dma.tables + 0x1000);
	uint64_t *l3 = (uint64_t *)(umi_sdx55_dma.tables + 0x2000);
	uint64_t *msi_l3 = (uint64_t *)(umi_sdx55_dma.tables + 0x3000);
	l1[(SDX55_IOVA >> 30) & 511] =
	    kvtophys((vm_offset_t)l2) | 3;
	l2[(SDX55_IOVA >> 21) & 511] =
	    kvtophys((vm_offset_t)l3) | 3;
	l2[(SDX55_MSI_ADDRESS >> 21) & 511] =
	    kvtophys((vm_offset_t)msi_l3) | 3;
	if (umi_sdx55_dma.memory_size == SDX55_DMA_SIZE && sdx55_voice_boot_enabled())
		l2[(SDX55_SAT_IOVA >> 21) & 511] = kvtophys(umi_sdx55_dma.tables + SDX55_SAT_TABLE) | 3;
	for (unsigned page_index = 0;
	    page_index < umi_sdx55_dma.memory_size / 0x1000; page_index++)
		l3[((SDX55_IOVA >> 12) + page_index) & 511] =
		    kvtophys(umi_sdx55_dma.memory + 0x1000ull * page_index) |
		    3ull | BIT(6) | (2ull << 8) | BIT(10) | BIT(11);
	/* PCIe2's live-DT MSI doorbell is a GIC device register.  It needs a
	 * device-memory, execute-never mapping in the same protected SID context. */
	msi_l3[(SDX55_MSI_ADDRESS >> 12) & 511] =
	    (SDX55_MSI_ADDRESS & ~0xfffull) | 3ull | BIT(6) | (2ull << 2) |
	    (2ull << 8) | BIT(10) | BIT(11) | (1ull << 53) | (1ull << 54);
	umi_sdx55_clean(umi_sdx55_dma.tables, SDX55_DMA_TABLE_BYTES);
	umi_sdx55_clean(umi_sdx55_dma.memory, umi_sdx55_dma.memory_size);

	umi_sdx55_dma.context_changed = true;
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0, 0);
	umi_sdx55_direct_write(umi_sdx55_dma.gr1, 0x800 + 4 * cb, 1);
	umi_sdx55_direct_write(umi_sdx55_dma.gr1, 4 * cb,
	    BIT(16) | 3u << 8 | 15u << 12);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x10,
	    1u | BIT(4) | 7u << 15);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x30,
	    25u | BIT(8) | BIT(10) | 3u << 12 | BIT(23));
	uint64_t ttbr = kvtophys(umi_sdx55_dma.tables) | (uint64_t)cb << 48;
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x20, (uint32_t)ttbr);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x24, (uint32_t)(ttbr >> 32));
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x28, 0);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x2c, 0);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x38, 0xf404ff44);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x3c, 0);
	if ((umi_sdx55_direct_read(umi_sdx55_dma.gr1, 4 * cb) & 0x30000) !=
	    BIT(16) || !(umi_sdx55_direct_read(umi_sdx55_dma.gr1,
	    0x800 + 4 * cb) & 1) ||
	    umi_sdx55_direct_read(umi_sdx55_dma.cb, 0x20) != (uint32_t)ttbr ||
	    umi_sdx55_direct_read(umi_sdx55_dma.cb, 0x24) !=
	    (uint32_t)(ttbr >> 32))
		return EIO;
	if (!umi_sdx55_smmu_sync()) return ETIMEDOUT;
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0,
	    1u | BIT(1) | BIT(2) | BIT(5) | BIT(8) | BIT(12));
	if (!(umi_sdx55_direct_read(umi_sdx55_dma.cb, 0) & 1)) return EIO;

	uint32_t route = cb | 3u << 8;
	uint32_t wanted = BIT(31) | SDX55_SID;
	/* Route first, then make the match valid.  Bus Master is still disabled. */
	umi_sdx55_direct_write(umi_sdx55_dma.smmu, 0xc00 + 4 * free_match,
	    route);
	umi_sdx55_dma.route_installed = true;
	umi_sdx55_direct_write(umi_sdx55_dma.smmu, 0x800 + 4 * free_match,
	    wanted);
	uint32_t smr_back = umi_sdx55_direct_read(umi_sdx55_dma.smmu,
	    0x800 + 4 * free_match);
	uint32_t s2cr_back = umi_sdx55_direct_read(umi_sdx55_dma.smmu,
	    0xc00 + 4 * free_match);
	hakios_tl("UMI SDX55: SID 0x1d01 routed by SMR%u to CB%u "
	    "SMR=%#x S2CR=%#x IOVA=%#llx\n", free_match, cb, smr_back,
	    s2cr_back, SDX55_IOVA);
	if (smr_back != wanted || (s2cr_back & 0x300ff) != cb) return EIO;
	return 0;
}

/* CPU-owned WP and modem-owned RP share a cache line in each 44-byte MHI
 * context. Cleaning a cached WP can overwrite a concurrent endpoint RP.
 * Change the wired MHI pages through pmap so all CPU aliases agree. */
static unsigned umi_sdx55_saved_attr[SDX55_DMA_SIZE / 4096];
static vm_offset_t umi_sdx55_shared_start;
static unsigned umi_sdx55_shared_pages;

static void
umi_sdx55_share_mhi_pages(vm_size_t offset)
{
	umi_sdx55_shared_start = umi_sdx55_dma.memory + offset;
	for (; offset < umi_sdx55_dma.memory_size; offset += PAGE_SIZE) {
		ppnum_t pn = (ppnum_t)atop(kvtophys(umi_sdx55_dma.memory + offset));
		umi_sdx55_saved_attr[umi_sdx55_shared_pages++] =
		    pmap_cache_attributes(pn) & VM_WIMG_MASK;
		pmap_set_cache_attributes(pn, VM_WIMG_WCOMB);
	}
}

static void
umi_sdx55_unshare_mhi_pages(void)
{
	while (umi_sdx55_shared_pages) {
		unsigned i = --umi_sdx55_shared_pages;
		ppnum_t pn = (ppnum_t)atop(kvtophys(umi_sdx55_shared_start + (vm_offset_t)i * PAGE_SIZE));
		pmap_set_cache_attributes(pn, umi_sdx55_saved_attr[i]);
	}
}

/* hakios_smmu_lock must be held.  Roll back even a partially installed
 * context so a bounded probe cannot leave stale page tables behind. */
static void
umi_sdx55_smmu_restore(void)
{
	if (!umi_sdx55_dma.context_changed) return;
	umi_sdx55_unshare_mhi_pages();
	if (umi_sdx55_dma.route_installed) {
		umi_sdx55_direct_write(umi_sdx55_dma.smmu,
		    0x800 + 4 * umi_sdx55_dma.smr, umi_sdx55_dma.old_smr);
		umi_sdx55_direct_write(umi_sdx55_dma.smmu,
		    0xc00 + 4 * umi_sdx55_dma.smr, umi_sdx55_dma.old_s2cr);
		umi_sdx55_dma.route_installed = false;
	}
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0, 0);
	if (!umi_sdx55_smmu_sync())
		hakios_tl("UMI SDX55: SMMU rollback TLB sync timed out\n");
	uint32_t fault = umi_sdx55_direct_read(umi_sdx55_dma.cb, 0x58) &
	    SDX55_SMMU_FSR_FAULT;
	if (fault) umi_sdx55_direct_write(umi_sdx55_dma.cb, 0x58, fault);
	for (unsigned i = 1; i < sizeof(umi_sdx55_cbregs) /
	    sizeof(umi_sdx55_cbregs[0]); i++)
		umi_sdx55_direct_write(umi_sdx55_dma.cb, umi_sdx55_cbregs[i],
		    umi_sdx55_dma.old_cb[i]);
	umi_sdx55_direct_write(umi_sdx55_dma.gr1,
	    4 * umi_sdx55_dma.context, umi_sdx55_dma.old_cbar);
	umi_sdx55_direct_write(umi_sdx55_dma.gr1,
	    0x800 + 4 * umi_sdx55_dma.context, umi_sdx55_dma.old_cba2r);
	umi_sdx55_direct_write(umi_sdx55_dma.cb, 0, umi_sdx55_dma.old_cb[0]);
	umi_sdx55_dma.context_changed = false;
	hakios_tl("UMI SDX55: SMMU context restored, fault=%#x\n", fault);
}

static int
umi_sdx55_smmu_proof(void)
{
	if (!umi_sdx55_dma.memory && kmem_alloc(kernel_map, &umi_sdx55_dma.memory,
	    0x1000, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
		return ENOMEM;
	umi_sdx55_dma.memory_size = 0x1000;
	memset((void *)umi_sdx55_dma.memory, 0xa5, umi_sdx55_dma.memory_size);
	if (!umi_sdx55_dma.tables && kmem_alloc(kernel_map, &umi_sdx55_dma.tables,
	    SDX55_DMA_TABLE_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
		return ENOMEM;
	lck_mtx_lock(&hakios_smmu_lock);
	int error = umi_sdx55_smmu_install();
	umi_sdx55_smmu_restore();
	lck_mtx_unlock(&hakios_smmu_lock);
	hakios_tl("UMI SDX55: SMMU translation-only proof errno=%d; "
	    "Bus Master remains disabled\n", error);
	return error;
}

static void
umi_sdx55_bhi_release(struct umi_pcie *p)
{
	/* Device reset and Bus Master removal must precede SMMU rollback. */
	umi_pcie2_quiesce(p);
	lck_mtx_lock(&hakios_smmu_lock);
	umi_sdx55_smmu_restore();
	lck_mtx_unlock(&hakios_smmu_lock);
}

static uint32_t umi_sdx55_bhi_address;

static int
umi_sdx55_bhi_boot_inner(struct umi_pcie *p, bool reserve_mhi)
{
	vm_size_t image_size = round_page(umi_sdx55_sbl1_size);
	vm_size_t dma_size = reserve_mhi ? SDX55_DMA_SIZE : image_size;
	if (image_size > SDX55_DMA_SIZE) return E2BIG;
	if (umi_sdx55_dma.memory) return EBUSY;
	if (kmem_alloc(kernel_map, &umi_sdx55_dma.memory, dma_size,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
		return ENOMEM;
	if (!umi_sdx55_dma.tables && kmem_alloc(kernel_map, &umi_sdx55_dma.tables,
	    SDX55_DMA_TABLE_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
		return ENOMEM;
	umi_sdx55_dma.memory_size = dma_size;
	memcpy((void *)umi_sdx55_dma.memory, umi_sdx55_sbl1,
	    umi_sdx55_sbl1_size);

	int error = umi_pcie2_setup_mhi_msi(p, SDX55_MSI_ADDRESS,
	    SDX55_MSI_DATA);
	if (error) goto teardown;
	lck_mtx_lock(&hakios_smmu_lock);
	error = umi_sdx55_smmu_install();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (error) goto teardown;
	error = umi_pcie2_enable_dma(p);
	if (error) goto teardown;

	uint32_t offset = rd(p, BAR + 0x28);
	if ((offset & 3) || offset < 0x100 || offset > 0xfc0) {
		error = EINVAL;
		goto teardown;
	}
	uint32_t bhi = BAR + offset;
	umi_sdx55_bhi_address = bhi;
	uint32_t session = (uint32_t)mach_absolute_time() & 0x3fffffffu;
	if (!session) session = 1;
	report(p, "BHI image address low", (uint32_t)SDX55_IOVA);
	report(p, "BHI image address high", (uint32_t)(SDX55_IOVA >> 32));
	report(p, "BHI image size", (uint32_t)umi_sdx55_sbl1_size);
	wr(p, bhi + 0x20, 0); /* polling mode, no BHI interrupt vector */
	/* Match Qualcomm's BHI programming order exactly: status is cleared
	 * before the latched 64-bit address, size, and 30-bit session doorbell. */
	wr(p, bhi + 0x2c, 0);
	wr(p, bhi + 0x0c, (uint32_t)(SDX55_IOVA >> 32));
	wr(p, bhi + 0x08, (uint32_t)SDX55_IOVA);
	wr(p, bhi + 0x10, (uint32_t)umi_sdx55_sbl1_size);
	report(p, "BHI session", session);
	wr(p, bhi + 0x18, session);
	error = wait_bits(p, bhi + 0x2c, 3u << 30, 2u << 30, 10000000);
	if (error) {
		report(p, "BHI status", rd(p, bhi + 0x2c));
		report(p, "BHI error code", rd(p, bhi + 0x30));
		report(p, "BHI error debug 1", rd(p, bhi + 0x34));
		report(p, "BHI error debug 2", rd(p, bhi + 0x38));
		report(p, "BHI error debug 3", rd(p, bhi + 0x3c));
	} else {
		report(p, "BHI status", 2u << 30);
		/* Never issue endpoint MMIO during the SBL transition.  The caller
		 * either tears down immediately (step 3) or transfers the persistent
		 * host and SMMU ownership to the asynchronous observer (step 4). */
		hakios_tl("UMI SDX55: BHI accepted SBL1; endpoint MMIO stopped\n");
		return 0;
	}

teardown:
	umi_sdx55_bhi_release(p);
	return error;
}

static int
umi_sdx55_bhi_boot(struct umi_pcie *p)
{
	int error = umi_sdx55_bhi_boot_inner(p, false);
	if (!error) umi_sdx55_bhi_release(p);
	hakios_tl("UMI SDX55: bounded BHI SBL1 boot errno=%d\n", error);
	return error;
}

static struct umi_mhi umi_sdx55_mhi;
static struct umi_mhi_io umi_sdx55_mhi_io;
static vm_size_t umi_sdx55_mhi_cursor;

static uint32_t
umi_sdx55_mhi_read(void *context, uint32_t offset)
{
	struct umi_pcie *p = context;
	return offset <= 0xffc && !(offset & 3) ? rd(p, BAR + offset) : UINT32_MAX;
}

static void
umi_sdx55_mhi_write(void *context, uint32_t offset, uint32_t value)
{
	struct umi_pcie *p = context;
	if (offset <= 0xffc && !(offset & 3)) wr(p, BAR + offset, value);
}

static int
umi_sdx55_mhi_allocate_aligned(size_t size, vm_size_t alignment, struct umi_mhi_dma *dma)
{
	vm_size_t rounded = round_page(size);
	vm_size_t offset = (umi_sdx55_mhi_cursor + alignment - 1) & ~(alignment - 1);
	if (!rounded || offset > umi_sdx55_dma.memory_size ||
	    rounded > umi_sdx55_dma.memory_size - offset)
		return ENOMEM;
	dma->bytes = (void *)(umi_sdx55_dma.memory + offset);
	dma->address = SDX55_IOVA + offset;
	dma->size = rounded;
	umi_sdx55_mhi_cursor = offset + rounded;
	return 0;
}

static int
umi_sdx55_mhi_allocate(void *context, size_t size, struct umi_mhi_dma *dma)
{
	(void)context;
	return umi_sdx55_mhi_allocate_aligned(size, PAGE_SIZE, dma);
}

static void
umi_sdx55_mhi_sync(void *context, const struct umi_mhi_dma *dma)
{
	(void)context;
	/* The apps-SMMU page tables are coherent, but the SDX55 PCIe endpoint does
	 * not snoop dirty CPU cache lines in this protected aperture.  The initial
	 * SBL worked because smmu_install() cleaned the complete buffer after the
	 * image copy; contexts are written later, after EE=SBL, and need the same
	 * publication before MHI reads them. */
	if (umi_sdx55_shared_pages) __asm__ volatile("dsb oshst" ::: "memory");
	else umi_sdx55_clean((vm_offset_t)dma->bytes, dma->size);
}

static void
umi_sdx55_mhi_sync_for_cpu(void *context, const struct umi_mhi_dma *dma)
{
	(void)context;
	if (umi_sdx55_shared_pages) __asm__ volatile("dsb osh" ::: "memory");
	else umi_sdx55_invalidate((vm_offset_t)dma->bytes, dma->size);
}

static void
umi_sdx55_mhi_report(void *context, const char *stage, uint32_t value)
{
	report(context, stage, value);
}

static void
umi_sdx55_mhi_delay(void *context, unsigned us)
{
	pause_us(context, us);
}

#include "umi_sdx55_modem_diag.inc"

extern const uint8_t *hakios_image_file(const char *, const char *, uint64_t *, int *);

static int
umi_sdx55_serve_boot_images(struct umi_mhi *host)
{
	static const struct { uint32_t id; const char *name; } images[] = {
		{ 41, "devcfg.mbn" }, { 33, "hyp.mbn" }, { 25, "tz.mbn" },
		{ 23, "aop.mbn" }, { 29, "acdb.mbn" }, { 6, "apps.mbn" },
		{ 36, "multi_image_qti.mbn" }, { 37, "multi_image.mbn" },
		{ 38, "xbl_cfg.elf" }, { 42, "sec.elf" },
		{ 16, "mdm1m9kefs1.bin" }, { 17, "mdm1m9kefs2.bin" },
		{ 20, "mdm1m9kefs3.bin" }, { 8, "qdsp6sw.mbn" },
	};
	for (unsigned round = 0; round < 32; round++) {
		/* next_image validated and retained this READ_DATA packet. */
		uint32_t id = host->sahara_rx_words[2];
		const char *name = NULL;
		for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
			if (images[i].id == id) name = images[i].name;
		if (!name) {
			hakios_tl("UMI SDX55: pending Sahara image ID=%u\n", id);
			return ENOTSUP;
		}
		uint64_t size = 0;
		int disk = -1;
		const uint8_t *bytes = hakios_image_file("sdx55m", name, &size, &disk);
		if (!bytes) return ENOENT;
		int error = umi_mhi_sahara_serve_image(host, id, bytes, size, 5000);
		hakios_tl("UMI SDX55: Sahara %s ID=%u errno=%d disk=md%d\n", name, id, error, disk);
		if (error) return error;
		if (host->sahara_complete) {
			uint32_t ee = 0;
			for (unsigned ms = 0; ms < 5000; ms++) {
				error = umi_mhi_execution_environment(host, &ee);
				if (error || ee == 2) break;
				host->io->delay_us(host->io->context, 1000);
			}
			hakios_tl("UMI SDX55: post-Sahara EE=%u errno=%d\n", ee, error);
			if (!error && ee == 2) host->amss_sent = true;
			return error ? error : (ee == 2 ? 0 : ETIMEDOUT);
		}
		error = umi_mhi_sahara_next_image(host, 5000);
		if (error) return error;
	}
	return E2BIG;
}

static uint32_t umi_sdx55_loc_port, umi_sdx55_loc_node;
static unsigned umi_sdx55_qrtr_frames;
static uint32_t umi_sdx55_peer;

#define SDX55_PDM_PORT 0x4302u
static struct {
	uint32_t packet[32];
	size_t size;
} umi_sdx55_pdm_queue[8];
static unsigned umi_sdx55_pdm_count;

#define SDX55_LOC_PORT 0x4300u
#define SDX55_DMS_PORT 0x4301u
static uint32_t umi_sdx55_dms_node, umi_sdx55_dms_port;
enum { SDX55_DMS_GET_MODE = 0x2d, SDX55_DMS_SET_MODE = 0x2e, SDX55_DMS_BIND = 0x54,
	SDX55_DMS_ONLINE = 0, SDX55_DMS_LOW_POWER = 1, SDX55_DMS_OFFLINE = 3, SDX55_DMS_SHUTTING_DOWN = 5,
	SDX55_DMS_NOT_READY = 52, SDX55_DMS_INVALID_TRANSITION = 60 };
static struct {
	uint16_t txn, message, reason, service_error;
	uint8_t mode;
	bool restricted, replied;
	int error;
} umi_sdx55_dms;
static struct {
	uint16_t message, txn, orbit_part;
	unsigned sv_reports, max_sv, nmea_reports, gsv_reports, max_gsv, max_bds_gsv;
	unsigned max_tracked, max_bds_tracked;
	uint32_t peak_snr_bits;
	bool replied, fix, control_ind;
	uint32_t control_status, lock;
	bool mask_valid;
	uint64_t mask;
	int error;
} umi_sdx55_loc;

#define SDX55_CELL_PORT 0x4303u
static void sdx55_voice_announce(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void sdx55_voice_receive(uint32_t, uint32_t, uint32_t, const uint8_t *, size_t);
static void sdx55_ims_receive(uint32_t, uint32_t, uint32_t, const uint8_t *, size_t);
/* UMI netmgr / ds_get_epid: PCIe iface 4 for rmnet_mhi0, separate from netdev GET_EPID (0). */
#define SDX55_PCIE_DATA_IFACE 4u
#define SDX55_CELL_MUX 1u
enum { CELL_UIM, CELL_NAS, CELL_WDS, CELL_WDA };
struct sdx55_cell_endpoint {
	uint32_t service;
	uint16_t message;
	uint32_t port, node, generation;
};
static struct sdx55_cell_endpoint sdx55_services[] = {
	{11, SDX55_UIM_GET_CARD_STATUS, 0, 0, 0},
	{3, SDX55_NAS_GET_SERVING_SYSTEM, 0, 0, 0},
	{1, SDX55_WDS_GET_STATUS, 0, 0, 0},
	{26, SDX55_WDA_GET_FORMAT, 0, 0, 0},
};
static struct {
	const struct sdx55_cell_endpoint *service;
	uint32_t node, port;
	uint16_t txn, message, service_error;
	bool done;
	int error;
	struct sdx55_wds_result wds;
	struct sdx55_cell_status cell;
	struct sdx55_wan_status wan;
} sdx55_query;

struct sdx55_data_routes {
	struct sdx55_cell_endpoint wds, wda;
};

/* The modem can reannounce a service on the same node and port after a
 * restart. Bound clients, packet handles and saved link formats expire. */
static bool
sdx55_cell_endpoint_current(const struct sdx55_cell_endpoint *expected)
{
	for (unsigned i = 0; i < sizeof(sdx55_services)/sizeof(sdx55_services[0]); i++) {
		const struct sdx55_cell_endpoint *live = &sdx55_services[i];
		if (live->service == expected->service)
			return live->port && live->port == expected->port &&
			    live->node == expected->node && live->generation == expected->generation;
	}
	return false;
}

static bool
sdx55_data_routes_current(const struct sdx55_data_routes *routes)
{
	return sdx55_cell_endpoint_current(&routes->wds) && sdx55_cell_endpoint_current(&routes->wda);
}

static void
sdx55_cell_announce(uint32_t type, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	if (instance != 1 || !port || !node || node == UINT32_MAX ||
	    (umi_sdx55_dms_node && node != umi_sdx55_dms_node)) return;
	for (unsigned i = 0; i < sizeof(sdx55_services)/sizeof(sdx55_services[0]); i++) {
		struct sdx55_cell_endpoint *ep = &sdx55_services[i];
		if (ep->service != service) continue;
		if (type == 4 && (ep->port != port || ep->node != node)) {
			ep->port = port;
			ep->node = node;
		} else if (type == 5 && ep->port == port && ep->node == node) {
			ep->port = 0;
		} else {
			return;
		}
		ep->generation++;
		sdx55_status_lost(service);
		if (sdx55_query.service && sdx55_query.service->service == service) {
			sdx55_query.error = ENXIO;
			sdx55_query.done = true;
		}
		return;
	}
}

static void
sdx55_cell_reply(uint32_t node, uint32_t port, const uint8_t *p, size_t size)
{
	const struct sdx55_cell_endpoint *ep = sdx55_query.service;
	if (!ep || sdx55_query.done || node != sdx55_query.node || port != sdx55_query.port ||
	    size < 7 || p[0] != QMI_RESPONSE || sdx55_cell_le16(p + 1) != sdx55_query.txn ||
	    sdx55_cell_le16(p + 3) != sdx55_query.message) return;
	uint16_t message = sdx55_query.message;
	int error;
	if ((ep->service == 3 && (message == SDX55_NAS_BIND_SUBSCRIPTION || message == SDX55_NAS_REGISTER_AUTO)) ||
	    (ep->service == 1 && message == SDX55_WDS_BIND_SUBSCRIPTION) ||
	    (ep->service == 11 && message == SDX55_UIM_PROVISION)) {
		error = sdx55_cell_body(message, sdx55_query.txn, p, size, &sdx55_query.service_error);
	} else if (ep->service == 26 && message == SDX55_WDA_SET_FORMAT) {
		error = sdx55_wda_set_ack(sdx55_query.txn, p, size, &sdx55_query.service_error);
	} else if (ep->service == 1 && message != SDX55_WDS_GET_STATUS) {
		error = sdx55_wds_decode(message, sdx55_query.txn, p, size, &sdx55_query.wds);
		sdx55_query.service_error = sdx55_query.wds.service_error;
	} else if (ep->service == 26 || ep->service == 1) {
		error = sdx55_wan_decode(message, sdx55_query.txn, p, size, &sdx55_query.wan);
		sdx55_query.service_error = sdx55_query.wan.service_error;
	} else {
		error = sdx55_cell_decode(message, sdx55_query.txn, p, size, &sdx55_query.cell);
		sdx55_query.service_error = sdx55_query.cell.service_error;
	}
	sdx55_query.error = error;
	sdx55_query.done = true;
}

/* The two small startup WRQs use the same option negotiation as stock. */
#define SDX55_TFTP_SERVICE_PORT 0x4103u
#define SDX55_TFTP_TRANSFER_PORT 0x4203u
static struct {
	uint32_t node, port;
	uint16_t ack;
	bool pending, temporary;
	uint8_t reply[96];
	size_t reply_len;
	size_t size;
} umi_sdx55_check;


#define SDX55_TFTP_ERROR_PORT 0x4205u
static struct {
	const uint8_t *file;
	uint64_t size;
	uint32_t node, port, local, offset, end, block_size, timeout_ms;
	uint16_t sent, acknowledged;
	uint8_t window_size;
	bool pending, oack, last_data, tsize, seek, rsize, blksize, timeout, window;
} umi_sdx55_config_transfer;
static struct {
	const char *path, *name;
	const uint8_t *bytes;
	uint64_t size;
} umi_sdx55_configs[] = {
	{ "/readonly/firmware/image/modem_pr/so/514_0_0.mbn", "514_0_0.mbn", NULL, 0 },
	{ "/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig", "mbn_hw.dig", NULL, 0 },
	{ "/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_sw/mbn_sw.dig", "mbn_sw.dig", NULL, 0 },
};
static struct { bool exists; size_t size; uint8_t bytes[512]; } umi_sdx55_temp;
static struct { uint32_t node, port; } umi_sdx55_missing;
static struct { uint32_t node, port, local; } umi_sdx55_credit[32];
static unsigned umi_sdx55_credits;

static bool
sdx55_tftp_next_option(const uint8_t **cursor, size_t *remaining, uint32_t *number)
{
	const uint8_t *options = *cursor;
	const uint8_t *key_end = memchr(options, 0, *remaining);
	if (!key_end || key_end == options) return false;
	size_t used = (size_t)(key_end - options) + 1;
	const uint8_t *value = options + used;
	const uint8_t *end = memchr(value, 0, *remaining - used);
	if (!end || end == value) return false;
	*number = 0;
	for (const uint8_t *c = value; c < end; c++) {
		if (*c < '0' || *c > '9' || *number > (UINT32_MAX - (*c - '0')) / 10) return false;
		*number = *number * 10 + (*c - '0');
	}
	*remaining -= (size_t)(end + 1 - options);
	*cursor = end + 1;
	return true;
}

/* Only the RAM-backed mcfg.tmp may be unlinked. Unknown options are omitted. */
static int
sdx55_tftp_write_options(const uint8_t *options, size_t length, bool temporary,
    uint8_t *out, size_t capacity, bool *unlink)
{
	if (capacity < 4) return -1;
	size_t n = 2;
	*unlink = false;
	out[0] = 0;
	while (length) {
		const char *key = (const char *)options;
		uint32_t value;
		if (!sdx55_tftp_next_option(&options, &length, &value)) return -1;
		if (!strcmp(key, "blksize") && value >= 8) value = MIN(value, 7680u);
		else if (!strcmp(key, "wsize") && value) value = MIN(value, 10u);
		else if (!strcmp(key, "timeoutms") && value && value <= 255000) { /* echo */ }
		else if (temporary && !strcmp(key, "unlink") && value == 0) *unlink = true;
		else continue;
		int chars = snprintf((char *)out + n, capacity - n, "%s%c%u", key, 0, value);
		if (chars < 0 || (size_t)chars >= capacity - n) return -1;
		n += (size_t)chars + 1;
	}
	out[1] = n > 2 ? 6 : 4;
	if (n == 2) { out[2] = out[3] = 0; n = 4; }
	return (int)n;
}

static void
umi_sdx55_tftp_read(uint32_t node, uint32_t port, const uint8_t *options, size_t length)
{
	uint32_t offset = 0, limit = 0, block_size = 512, timeout_ms = 1000;
	bool blksize = false, timeout = false, window = false;
	uint8_t window_size = 1;
	bool tsize = false, seek = false, rsize = false;
	while (length) {
		const uint8_t *name = options;
		uint32_t number;
		if (!sdx55_tftp_next_option(&options, &length, &number)) return;
		if (!strcmp((const char *)name, "blksize")) { blksize = true; block_size = MIN(number, 7680); }
		if (!strcmp((const char *)name, "timeoutms")) { timeout = true; timeout_ms = number; }
		if (!strcmp((const char *)name, "wsize")) {
			if (!number) return;
			window = true;
			window_size = MIN(number, 10u);
		}
		if (!strcmp((const char *)name, "tsize")) tsize = true;
		if (!strcmp((const char *)name, "seek")) { seek = true; offset = number; }
		if (!strcmp((const char *)name, "rsize")) { rsize = true; limit = number; }
	}
	if (block_size < 8 || !umi_sdx55_config_transfer.file || offset > umi_sdx55_config_transfer.size) return;
	/* Distinct requests use separate transfer IDs, as on the UMI server. */
	if (umi_sdx55_config_transfer.node != node || umi_sdx55_config_transfer.port != port)
		umi_sdx55_config_transfer.local = 0x4400 + ((umi_sdx55_config_transfer.local + 1) & 255);
	umi_sdx55_config_transfer.node = node;
	umi_sdx55_config_transfer.port = port;
	umi_sdx55_config_transfer.offset = offset;
	umi_sdx55_config_transfer.end = umi_sdx55_config_transfer.size;
	if (limit && limit < umi_sdx55_config_transfer.end - offset) umi_sdx55_config_transfer.end = offset + limit;
	umi_sdx55_config_transfer.block_size = block_size;
	umi_sdx55_config_transfer.blksize = blksize;
	umi_sdx55_config_transfer.timeout = timeout;
	umi_sdx55_config_transfer.timeout_ms = timeout_ms;
	umi_sdx55_config_transfer.window = window;
	umi_sdx55_config_transfer.window_size = window_size;
	umi_sdx55_config_transfer.sent = umi_sdx55_config_transfer.acknowledged = 0;
	umi_sdx55_config_transfer.last_data = false;
	umi_sdx55_config_transfer.oack = tsize || seek || rsize || blksize || timeout || window;
	hakios_tl("UMI SDX55: TFTP config offset=%u limit=%u block=%u size_query=%u bytes=%u window=%u\n",
	    offset, limit, block_size, tsize, (unsigned)umi_sdx55_config_transfer.size, window_size);
	umi_sdx55_config_transfer.tsize = tsize;
	umi_sdx55_config_transfer.seek = seek;
	umi_sdx55_config_transfer.rsize = rsize;
	umi_sdx55_config_transfer.pending = true;
}

/* A cumulative ACK releases the next window. Update state before the next
 * MHI send, which can dispatch another QRTR packet during TX completion. */
static void
umi_sdx55_tftp_ack(uint32_t node, uint32_t port, uint32_t local, unsigned block)
{
	typeof(umi_sdx55_config_transfer) *t = &umi_sdx55_config_transfer;
	if (node != t->node || port != t->port || local != t->local) return;
	if (t->oack) {
		if (block != 0) return;
		t->oack = false;
	} else {
		if (block <= t->acknowledged || block > t->sent) return;
		t->acknowledged = block;
		if (t->last_data) {
			if (block == t->sent)
				hakios_tl("UMI SDX55: TFTP read acknowledged offset=%u bytes=%u blocks=%u\n",
				    t->offset, t->end - t->offset, block);
			return;
		}
	}
	t->pending = true;
}

static int
umi_sdx55_tftp_flush(struct umi_mhi *host)
{
	if (!umi_sdx55_config_transfer.pending && !umi_sdx55_missing.port) return 0;
	static uint32_t packet[1930];
	memset(packet, 0, sizeof(packet));
	packet[0] = packet[1] = packet[2] = 1;
	packet[3] = umi_sdx55_config_transfer.local;
	uint8_t *out = (uint8_t *)(packet + 8);
	size_t n = 2;
	if (umi_sdx55_missing.port) {
		packet[3] = SDX55_TFTP_ERROR_PORT;
		packet[6] = umi_sdx55_missing.node;
		packet[7] = umi_sdx55_missing.port;
		umi_sdx55_missing.port = 0;
		static const char missing[] = "\0\5\0\1File not found";
		memcpy(out, missing, sizeof(missing));
		n = sizeof(missing);
	} else {
		packet[6] = umi_sdx55_config_transfer.node;
		packet[7] = umi_sdx55_config_transfer.port;
		umi_sdx55_config_transfer.pending = false;
		typeof(umi_sdx55_config_transfer) *t = &umi_sdx55_config_transfer;
		if (t->oack) {
			out[1] = 6;
			if (umi_sdx55_config_transfer.blksize)
				n += snprintf((char *)out + n, 48, "blksize%c%u", 0, umi_sdx55_config_transfer.block_size) + 1;
			if (umi_sdx55_config_transfer.timeout)
				n += snprintf((char *)out + n, 48, "timeoutms%c%u", 0, umi_sdx55_config_transfer.timeout_ms) + 1;
			if (umi_sdx55_config_transfer.tsize)
				n += snprintf((char *)out + n, 48, "tsize%c%u", 0, (unsigned)umi_sdx55_config_transfer.size) + 1;
			if (umi_sdx55_config_transfer.seek)
				n += snprintf((char *)out + n, 48, "seek%c%u", 0, umi_sdx55_config_transfer.offset) + 1;
			if (t->window)
				n += snprintf((char *)out + n, 48, "wsize%c%u", 0, t->window_size) + 1;
			if (umi_sdx55_config_transfer.rsize)
				n += snprintf((char *)out + n, 48, "rsize%c%u", 0,
				    umi_sdx55_config_transfer.end - umi_sdx55_config_transfer.offset) + 1;
		} else {
			unsigned block = ++t->sent;
			uint64_t pos = umi_sdx55_config_transfer.offset + (uint64_t)(block - 1) * umi_sdx55_config_transfer.block_size;
			if (pos > umi_sdx55_config_transfer.end) return 0;
			unsigned len = MIN(umi_sdx55_config_transfer.block_size, umi_sdx55_config_transfer.end - pos);
			out[1] = 3;
			out[2] = block >> 8;
			out[3] = block;
			memcpy(out + 4, umi_sdx55_config_transfer.file + pos, len);
			n = 4 + len;
			t->last_data = len < t->block_size;
			t->pending = !t->last_data && (unsigned)(t->sent - t->acknowledged) < t->window_size;
		}
	}
	packet[5] = n;
	return umi_mhi_send_qrtr_packet(host, packet, 32 + ((n + 3) & ~3u), 1000, UINT16_MAX);
}

/* Stock pd-mapper lookups made by SDX55 during this bounded boot. The
 * locator describes the installed domains independently of service uptime. */
static int
sdx55_pdm_response(const uint8_t *qmi, size_t length, void *out, size_t capacity)
{
	struct lookup { char name[65]; uint32_t offset; } req = {0};
	struct entry { char name[65]; uint32_t instance; uint8_t valid; uint32_t data; };
	struct reply {
		struct qmi_response_type_v01 result;
		uint16_t total, revision;
		uint32_t count;
		struct entry entry;
	} res = {0};
	static const struct qmi_elem_info request[] = {
		{QMI_STRING, 65, 1, NO_ARRAY, 1, offsetof(struct lookup, name), NULL},
		{QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x10, offsetof(struct lookup, offset), NULL},
		{.data_type = QMI_EOTI},
	};
	static const struct qmi_elem_info domain[] = {
		{QMI_STRING, 65, 1, NO_ARRAY, 0, offsetof(struct entry, name), NULL},
		{QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0, offsetof(struct entry, instance), NULL},
		{QMI_UNSIGNED_1_BYTE, 1, 1, NO_ARRAY, 0, offsetof(struct entry, valid), NULL},
		{QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0, offsetof(struct entry, data), NULL},
		{.data_type = QMI_EOTI},
	};
	static const struct qmi_elem_info response[] = {
		{QMI_STRUCT, 1, sizeof(res.result), NO_ARRAY, 2, 0, qmi_response_type_v01_ei},
		{QMI_UNSIGNED_2_BYTE, 1, 2, NO_ARRAY, 0x10, offsetof(struct reply, total), NULL},
		{QMI_UNSIGNED_2_BYTE, 1, 2, NO_ARRAY, 0x11, offsetof(struct reply, revision), NULL},
		{QMI_DATA_LEN, 1, 1, NO_ARRAY, 0x12, offsetof(struct reply, count), NULL},
		{QMI_STRUCT, 1, sizeof(struct entry), VAR_LEN_ARRAY, 0x12, offsetof(struct reply, entry), domain},
		{.data_type = QMI_EOTI},
	};
	if (length < 7 || qmi[0] != QMI_REQUEST) return -EINVAL;
	uint16_t txn = qmi[1] | qmi[2] << 8, msg = qmi[3] | qmi[4] << 8;
	if (msg == 0x21 && hakios_qmi_decode(qmi, length, request, &req) >= 0 && req.name[0]) {
		const char *name = NULL;
		if (!strcmp(req.name, "avs/audio")) {
			name = "msm/adsp/audio_pd";
			res.entry.instance = 74;
		} else if (!strcmp(req.name, "gps/gps_service")) {
			name = "mdm/modem/root_pd";
			res.entry.instance = 165;
		}
		if (name || !strcmp(req.name, "tms/pddump_disabled")) {
			res.revision = 1;
			res.total = name != NULL;
			if (req.offset < res.total) {
				memcpy(res.entry.name, name, strlen(name) + 1);
				res.count = 1;
			}
			hakios_tl("UMI SDX55: PD lookup %s domains=%u offset=%u\n", req.name, res.total, req.offset);
			return hakios_qmi_encode(QMI_RESPONSE, msg, txn, response, &res, out, capacity);
		}
		hakios_tl("UMI SDX55: PD lookup unsupported %s\n", req.name);
	}
	struct qmi_elem_info result[] = {response[0], {.data_type = QMI_EOTI}};
	res.result.result = 1;
	res.result.error = QMI_ERR_NOT_SUPPORTED_V01;
	return hakios_qmi_encode(QMI_RESPONSE, msg, txn, result, &res, out, capacity);
}

static uint32_t
sdx55_le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A GSV sentence reports the per-constellation SV count in its third field. */
static int
sdx55_nmea_gsv(const uint8_t *s, size_t n)
{
	if (n < 11 || s[0] != '$' || memcmp(s + 3, "GSV,", 4)) return -1;
	size_t i = 7;
	for (unsigned field = 0; field < 2; field++) {
		size_t first = i;
		while (i < n && s[i] >= '0' && s[i] <= '9') i++;
		if (i == first || i >= n || s[i++] != ',') return -1;
	}
	size_t first = i;
	unsigned count = 0;
	while (i < n && s[i] >= '0' && s[i] <= '9') {
		if (i - first == 3) return -1;
		count = count * 10 + s[i++] - '0';
	}
	if (i == first || i >= n || (s[i] != ',' && s[i] != '*')) return -1;
	return (int)count;
}

struct sdx55_sv_quality {
	unsigned tracked, signal, bds_listed, bds_tracked;
	uint32_t peak_bits;
};

/* LOC expandedSvList appends a single GLONASS frequency byte to each
 * 28-byte svInfo. Both lists start with a one-byte entry count. */
static bool
sdx55_sv_list(const uint8_t *p, size_t n, bool expanded, bool log_details,
    struct sdx55_sv_quality *q)
{
	memset(q, 0, sizeof(*q));
	size_t stride = expanded ? 29u : 28u;
	if (!n || p[0] > (expanded ? 176u : 80u) || n != 1u + stride * p[0]) return false;
	unsigned count = *p++;
	for (unsigned i = 0; i < count; i++, p += stride) {
		uint32_t valid = sdx55_le32(p);
		bool bds = (valid & 1) && sdx55_le32(p + 4) == 6;
		bool tracked = (valid & 8) && sdx55_le32(p + 11) == 3;
		q->tracked += tracked;
		q->bds_listed += bds;
		q->bds_tracked += bds && tracked;
		/* Ordered IEEE-754 bits suffice for a positive SNR up to 100 dB-Hz. */
		uint32_t bits = sdx55_le32(p + 24);
		bool signal = (valid & 0x80) && bits && bits <= 0x42c80000u;
		if (signal) {
			q->signal++;
			q->peak_bits = MAX(q->peak_bits, bits);
		}
		if (log_details && (tracked || signal)) {
			int system = valid & 1 ? (int)sdx55_le32(p + 4) : -1;
			int svid = valid & 2 ? p[8] | p[9] << 8 : -1;
			/* nav bits 0 and 1 mean ephemeris and almanac available. */
			int nav = valid & 0x10 ? p[15] : -1;
			hakios_tl("UMI SDX55: LOC SV system=%d svid=%d tracked=%u nav=%d snr_bits=%#x\n",
			    system, svid, tracked ? 1u : 0u, nav, signal ? bits : 0);
		}
	}
	return true;
}

static void
sdx55_sv_report(const uint8_t *body, size_t len)
{
	const uint8_t *basic = NULL, *expanded = NULL;
	size_t basic_len = 0, expanded_len = 0;
	while (len) {
		if (len < 3) return;
		size_t n = body[1] | body[2] << 8;
		if (n > len - 3) return;
		if (body[0] == 0x10) {
			if (basic) return;
			basic = body + 3;
			basic_len = n;
		} else if (body[0] == 0x11) {
			if (expanded) return;
			expanded = body + 3;
			expanded_len = n;
		}
		body += 3 + n;
		len -= 3 + n;
	}
	/* The native HAL prefers the expanded array when available. Do not
	 * double-count the same observation if both arrays were supplied. */
	const uint8_t *list = expanded ? expanded : basic;
	struct sdx55_sv_quality q;
	unsigned report = umi_sdx55_loc.sv_reports + 1;
	bool sample = report == 1 || report % 10 == 0;
	if (!list || !sdx55_sv_list(list, expanded ? expanded_len : basic_len,
	    expanded != NULL, sample, &q)) return;
	umi_sdx55_loc.sv_reports = report;
	umi_sdx55_loc.max_sv = MAX(umi_sdx55_loc.max_sv, list[0]);
	umi_sdx55_loc.max_tracked = MAX(umi_sdx55_loc.max_tracked, q.tracked);
	umi_sdx55_loc.max_bds_tracked = MAX(umi_sdx55_loc.max_bds_tracked, q.bds_tracked);
	umi_sdx55_loc.peak_snr_bits = MAX(umi_sdx55_loc.peak_snr_bits, q.peak_bits);
	if (sample) {
		hakios_tl("UMI SDX55: LOC SV report=%u satellites=%u max=%u expanded=%u\n",
		    report, (unsigned)list[0], umi_sdx55_loc.max_sv, expanded != NULL);
		hakios_tl("UMI SDX55: LOC tracked=%u signal=%u peak_bits=%#x BDS_listed=%u BDS_tracked=%u\n",
		    q.tracked, q.signal, q.peak_bits, q.bds_listed, q.bds_tracked);
	}
}

static void
sdx55_loc_event(uint16_t message, const uint8_t *body, size_t len)
{
	if (message == 0x25) {
		sdx55_sv_report(body, len);
		return;
	}
	const uint8_t *value = NULL, *expanded = NULL;
	size_t size = 0, expanded_size = 0;
	int session = -1;
	while (len) {
		if (len < 3) return;
		size_t n = body[1] | body[2] << 8;
		if (n > len - 3) return;
		if (body[0] == 1) {
			if (value) return;
			value = body + 3;
			size = n;
		} else if (message == 0x26 && body[0] == 0x10) {
			if (expanded) return;
			expanded = body + 3;
			expanded_size = n;
		} else if (message == 0x2c && body[0] == 0x10 && n == 1) {
			session = body[3];
		}
		body += 3 + n;
		len -= 3 + n;
	}
	if (!value) return;
	if (message != 0x26) {
		if (size == 4)
			hakios_tl("UMI SDX55: LOC %s state=%u session_id=%d\n",
			    message == 0x2b ? "engine" : "fix-session", sdx55_le32(value), session);
		return;
	}
	/* An expanded NMEA sentence accompanies an empty mandatory field. */
	if (expanded) {
		if (size || expanded_size > 4095) return;
		value = expanded;
		size = expanded_size;
	}
	if (size < 6 || value[0] != '$') return;
	umi_sdx55_loc.nmea_reports++;
	int count = sdx55_nmea_gsv(value, size);
	if (count < 0) return;
	umi_sdx55_loc.gsv_reports++;
	umi_sdx55_loc.max_gsv = MAX(umi_sdx55_loc.max_gsv, (unsigned)count);
	if (!memcmp(value + 1, "GB", 2) || !memcmp(value + 1, "BD", 2))
		umi_sdx55_loc.max_bds_gsv = MAX(umi_sdx55_loc.max_bds_gsv, (unsigned)count);
}

#define SDX55_LOC_INJECT_XTRA_DATA 0xa7u
#define SDX55_ORBIT_CHUNK 1024u  /* LOC v2 maximum partData length */
#define SDX55_ORBIT_MAX (256u * 1024u)  /* bounded diagnostic image */

static int
sdx55_encode_orbit_part(uint16_t txn, const uint8_t *data, size_t size,
    unsigned part, void *out, size_t capacity)
{
	if (!data || !size || size > SDX55_ORBIT_MAX) return -EINVAL;
	unsigned parts = (unsigned)((size + SDX55_ORBIT_CHUNK - 1) / SDX55_ORBIT_CHUNK);
	if (!part || part > parts) return -EINVAL;
	struct orbit_request {
		uint32_t size;
		uint16_t parts, part;
		uint32_t count;
		uint8_t bytes[SDX55_ORBIT_CHUNK];
	};
	static const struct qmi_elem_info fields[] = {
		{ QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 1, offsetof(struct orbit_request, size), NULL },
		{ QMI_UNSIGNED_2_BYTE, 1, 2, NO_ARRAY, 2, offsetof(struct orbit_request, parts), NULL },
		{ QMI_UNSIGNED_2_BYTE, 1, 2, NO_ARRAY, 3, offsetof(struct orbit_request, part), NULL },
		{ QMI_DATA_LEN, 1, 2, NO_ARRAY, 4, offsetof(struct orbit_request, count), NULL },
		{ QMI_UNSIGNED_1_BYTE, SDX55_ORBIT_CHUNK, 1, VAR_LEN_ARRAY, 4, offsetof(struct orbit_request, bytes), NULL },
		{ .data_type = QMI_EOTI },
	};
	size_t offset = (part - 1) * SDX55_ORBIT_CHUNK;
	struct orbit_request req = { .size = (uint32_t)size, .parts = parts, .part = part };
	req.count = size - offset < SDX55_ORBIT_CHUNK ? (uint32_t)(size - offset) : SDX55_ORBIT_CHUNK;
	memcpy(req.bytes, data + offset, req.count);
	return hakios_qmi_encode(QMI_REQUEST, SDX55_LOC_INJECT_XTRA_DATA, txn, fields, &req, out, capacity);
}

/* Only publish a status after validating the complete indication. */
static bool
sdx55_orbit_ack(const uint8_t *body, size_t length, unsigned part, uint32_t *status)
{
	bool seen_status = false, seen_part = false;
	uint32_t value = 0;
	while (length) {
		if (length < 3) return false;
		unsigned field = body[0], n = body[1] | body[2] << 8;
		if (n > length - 3) return false;
		if (field == 1) {
			if (n != 4 || seen_status) return false;
			value = sdx55_le32(body + 3);
			seen_status = true;
		} else if (field == 0x10) {
			if (n != 2 || seen_part || (unsigned)(body[3] | body[4] << 8) != part) return false;
			seen_part = true;
		}
		body += 3 + n;
		length -= 3 + n;
	}
	if (!seen_status) return false;
	*status = value;
	return true;
}

static bool
sdx55_loc_has_control_ind(uint16_t message)
{
	return message == SDX55_LOC_INJECT_XTRA_DATA || message == 0x37 || message == 0x38 || message == 0x3a || message == 0x3b ||
	    message == 0x4a || message == 0xba || message == 0xbf;
}

/* ADSP protection-domain service routing on the modem worker. */
static int sdx55_voice_audio_prepare(struct umi_mhi *);

static struct {
	bool enabled, pending;
	uint64_t advertised;
	struct hakios_audio_pd_packet request;
} sdx55_audio_pd;

#include "umi_sdx55_mhi_adsp.inc"

static bool
sdx55_audio_pd_rx(uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
    bool confirm, const uint8_t *payload, uint32_t length)
{
	if (!sdx55_audio_pd.advertised ||
	    sdx55_audio_pd.advertised != ((uint64_t)dst_node << 32 | dst_port)) return false;
	if ((src_node != umi_sdx55_dms_node && src_node != umi_sdx55_peer) || !src_node || !src_port || sdx55_audio_pd.pending ||
	    length > sizeof(sdx55_audio_pd.request.data)) return true;
	if (type == 1) {
		if (length < 7 || payload[0] != QMI_REQUEST || sdx55_cell_le16(payload + 5) != length - 7) return true;
		unsigned msg = sdx55_cell_le16(payload + 3);
		if (msg != 0x20 && msg != 0x23) return true; /* register listener, acknowledge state */
	} else if (type != 7 || length != 20) return true; /* flow-control resume */
	sdx55_audio_pd.request = (struct hakios_audio_pd_packet){
		type, src_node, src_port, dst_node, dst_port, confirm, length, {0}
	};
	memcpy(sdx55_audio_pd.request.data, payload, length);
	sdx55_audio_pd.pending = true;
	return true;
}

static int
sdx55_audio_pd_poll(struct umi_mhi *host)
{
	if (!sdx55_audio_pd.enabled || !umi_sdx55_peer || !umi_sdx55_dms_node) return 0;
	uint64_t endpoint = hakios_adsp_audio_pd_endpoint();
	if (sdx55_audio_pd.advertised != endpoint) {
		bool added = !sdx55_audio_pd.advertised;
		uint64_t which = added ? endpoint : sdx55_audio_pd.advertised;
		uint32_t type = added ? 4 : 5;
		uint32_t packet[] = {1, type, 1, 0xfffffffe, 0, 20, umi_sdx55_peer, 0xfffffffe,
		    type, HAKIOS_AUDIO_PD_SERVICE, HAKIOS_AUDIO_PD_INSTANCE, (uint32_t)(which >> 32), (uint32_t)which};
		sdx55_audio_pd.advertised = added ? endpoint : 0;
		int error = umi_mhi_send_qrtr_packet(host, packet, sizeof(packet), 1000, UINT16_MAX);
		if (!error && umi_sdx55_dms_node != umi_sdx55_peer) {
			/* The next hop consumes its control packets locally. The modem's
			 * service registry needs its own copy of the ADSP announcement. */
			packet[6] = umi_sdx55_dms_node;
			error = umi_mhi_send_qrtr_packet(host, packet, sizeof(packet), 1000, UINT16_MAX);
		}
		if (error) return error;
		hakios_tl("UMI SDX55: voice audio PD %s node=%u port=%u modem=%u\n",
		    added ? "announced" : "withdrawn", packet[11], packet[12], umi_sdx55_dms_node);
	}
	if (sdx55_audio_pd.pending) {
		struct hakios_audio_pd_packet request = sdx55_audio_pd.request;
		sdx55_audio_pd.pending = false;
		int error = hakios_adsp_audio_pd_send(&request);
		hakios_tl("UMI SDX55: voice audio PD to DSP type=%u msg=%#x errno=%d\n",
		    request.type, request.type == 1 ? sdx55_cell_le16(request.data + 3) : 0, error);
	}
	struct hakios_audio_pd_packet reply;
	if (!hakios_adsp_audio_pd_receive(&reply) ||
	    (reply.to_node != umi_sdx55_dms_node && reply.to_node != umi_sdx55_peer)) return 0;
	uint16_t msg = reply.type == 1 && reply.length >= 7 ? sdx55_cell_le16(reply.data + 3) : 0;
	if (msg == 0x20 || msg == 0x22) {
		unsigned state_tag = msg == 0x20 ? 0x10 : 1;
		for (unsigned at = 7; at + 3 <= reply.length;) {
			unsigned count = sdx55_cell_le16(reply.data + at + 1);
			if (count > reply.length - at - 3) break;
			if (reply.data[at] == state_tag && count == 4) {
				uint32_t state = sdx55_le32(reply.data + at + 3);
				hakios_tl("UMI SDX55: voice audio PD state=%#x\n", state);
				if (state == 0x1fffffffu) {
					/* The modem can open its voice transport when it sees
					 * Audio PD UP. Publish the mailbox before that point. */
					int setup = sdx55_voice_audio_prepare(host);
					hakios_tl("UMI SDX55: voice mailbox before audio PD UP errno=%d\n", setup);
				}
				break;
			}
			at += count + 3;
		}
	}
	uint32_t packet[8 + sizeof(reply.data) / 4] = {
	    1, reply.type, reply.from_node, reply.from_port, reply.confirm, reply.length, reply.to_node, reply.to_port
	};
	memcpy(packet + 8, reply.data, reply.length);
	int error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((reply.length + 3) & ~3u), 1000, UINT16_MAX);
	hakios_tl("UMI SDX55: voice audio PD to modem type=%u msg=%#x errno=%d\n", reply.type, msg, error);
	return error;
}

static void
umi_sdx55_qrtr_packet(void *context, const void *data, size_t size)
{
	(void)context;
	const uint8_t *bytes = data;
	uint32_t type, length, dst_node, dst_port, src_node, src_port;
	size_t header;
	++umi_sdx55_qrtr_frames;
	if (size >= 16 && bytes[0] == 3) {
		type = bytes[1];
		src_port = bytes[10] | (uint32_t)bytes[11] << 8;
		src_node = bytes[8] | (uint32_t)bytes[9] << 8;
		dst_node = bytes[12] | (uint32_t)bytes[13] << 8;
		dst_port = bytes[14] | (uint32_t)bytes[15] << 8;
		length = sdx55_le32(bytes + 4);
		header = 16u + bytes[3];
	} else if (size >= 32 && sdx55_le32(bytes) == 1) {
		type = sdx55_le32(bytes + 4);
		src_port = sdx55_le32(bytes + 12);
		src_node = sdx55_le32(bytes + 8);
		dst_node = sdx55_le32(bytes + 24);
		dst_port = sdx55_le32(bytes + 28);
		length = sdx55_le32(bytes + 20);
		header = 32;
	} else return;
	if (header > size || length > size - header) return;
	const uint8_t *payload = bytes + header;
	bool confirm = bytes[0] == 3 ? (bytes[2] & 1) != 0 : sdx55_le32(bytes + 16) != 0;
	if (sdx55_audio_pd_rx(type, src_node, src_port, dst_node, dst_port, confirm, payload, length)) return;
	if (type == 1 && confirm && umi_sdx55_credits < 32) {
		unsigned i = umi_sdx55_credits++;
		umi_sdx55_credit[i].node = src_node;
		umi_sdx55_credit[i].port = src_port;
		umi_sdx55_credit[i].local = dst_port;
	}
	if (type == 1 && dst_port == SDX55_CELL_PORT) {
		sdx55_cell_reply(src_node, src_port, payload, length);
		return;
	}
	if (type == 1 && (dst_port == SDX55_VOICE_PORT || dst_port == SDX55_IMSA_PORT || dst_port == SDX55_IMSS_PORT)) {
		sdx55_voice_receive(dst_port, src_node, src_port, payload, length);
		return;
	}
	if (type == 1 && (dst_port == SDX55_IMSDCM_PORT || dst_port == SDX55_IMS_WDS_PORT)) {
		sdx55_ims_receive(dst_port, src_node, src_port, payload, length);
		return;
	}
	if (type == 1 && dst_port == SDX55_PDM_PORT) {
		if (umi_sdx55_pdm_count >= sizeof(umi_sdx55_pdm_queue)/sizeof(umi_sdx55_pdm_queue[0])) return;
		uint32_t *packet = umi_sdx55_pdm_queue[umi_sdx55_pdm_count].packet;
		memset(packet, 0, sizeof(umi_sdx55_pdm_queue[0].packet));
		int n = sdx55_pdm_response(payload, length, packet + 8, sizeof(umi_sdx55_pdm_queue[0].packet) - 32);
		if (n < 0) return;
		packet[0] = packet[1] = packet[2] = 1;
		packet[3] = SDX55_PDM_PORT;
		packet[5] = (uint32_t)n;
		packet[6] = src_node;
		packet[7] = src_port;
		umi_sdx55_pdm_queue[umi_sdx55_pdm_count++].size = 32 + ((n + 3) & ~3u);
		return;
	}
	if (type == 1 && dst_port == SDX55_DMS_PORT && src_node == umi_sdx55_dms_node &&
	    src_port == umi_sdx55_dms_port && length >= 7 && payload[0] == QMI_RESPONSE &&
	    (payload[1] | payload[2] << 8) == umi_sdx55_dms.txn &&
	    (payload[3] | payload[4] << 8) == umi_sdx55_dms.message &&
	    (uint32_t)(payload[5] | payload[6] << 8) == length - 7) {
		uint16_t service_error = UINT16_MAX;
		umi_sdx55_dms.error = hakios_loc_result(payload, length, &service_error);
		umi_sdx55_dms.service_error = service_error;
		umi_sdx55_dms.replied = true;
		hakios_tl("UMI SDX55: DMS reply msg=%#x errno=%d service_error=%u\n",
		    umi_sdx55_dms.message, umi_sdx55_dms.error, service_error);
		if (umi_sdx55_dms.error || umi_sdx55_dms.message != SDX55_DMS_GET_MODE) return;
		size_t off = 7;
		while (off + 3 <= length) {
			unsigned field = payload[off], n = payload[off + 1] | payload[off + 2] << 8;
			off += 3;
			if (n > length - off) break;
			if (field == 1 && n == 1) umi_sdx55_dms.mode = payload[off];
			if (field == 0x10 && n == 2) umi_sdx55_dms.reason = payload[off] | payload[off+1] << 8;
			if (field == 0x11 && n == 1) umi_sdx55_dms.restricted = payload[off] != 0;
			off += n;
		}
		if (off != length || umi_sdx55_dms.mode == UINT8_MAX) umi_sdx55_dms.error = EINVAL;
		hakios_tl("UMI SDX55: DMS operating mode=%u offline_reason=%#x restricted=%u\n",
		    umi_sdx55_dms.mode, umi_sdx55_dms.reason, umi_sdx55_dms.restricted);
		return;
	}
	if (type == 1 && dst_port == SDX55_LOC_PORT &&
	    src_node == umi_sdx55_loc_node && src_port == umi_sdx55_loc_port && length >= 7 &&
	    (uint32_t)(payload[5] | payload[6] << 8) == length - 7) {
		uint16_t message = payload[3] | payload[4] << 8;
		uint16_t txn = payload[1] | payload[2] << 8;
		if (payload[0] == QMI_RESPONSE && message == umi_sdx55_loc.message &&
		    txn == umi_sdx55_loc.txn) {
			uint16_t service_error = UINT16_MAX;
			umi_sdx55_loc.error = hakios_loc_result(payload, length, &service_error);
			umi_sdx55_loc.replied = true;
			hakios_tl("UMI SDX55: LOC reply msg=%#x txn=%u errno=%d service_error=%u\n",
			    message, txn, umi_sdx55_loc.error, service_error);
		} else if (payload[0] == QMI_INDICATION && message == SDX55_LOC_INJECT_XTRA_DATA && message == umi_sdx55_loc.message) {
			if (sdx55_orbit_ack(payload + 7, length - 7, umi_sdx55_loc.orbit_part,
			    &umi_sdx55_loc.control_status)) umi_sdx55_loc.control_ind = true;
		} else if (payload[0] == QMI_INDICATION && message == umi_sdx55_loc.message &&
		    sdx55_loc_has_control_ind(message)) {
			for (size_t off = 7; off + 3 <= length; ) {
				unsigned field = payload[off], n = payload[off+1] | payload[off+2] << 8;
				off += 3;
				if (n > length - off) return;
				hakios_tl("UMI SDX55: LOC control field=%#x bytes=%u\n", field, n);
				if (message == 0x37 && field == 0x10 && n == 10) {
					uint64_t start = sdx55_le32(payload + off) | (uint64_t)sdx55_le32(payload + off + 4) << 32;
					unsigned hours = payload[off + 8] | payload[off + 9] << 8;
					hakios_tl("UMI SDX55: predicted orbits start_utc=%llu duration_hours=%u\n", start, hours);
				}
				if (message == 0xbf && n == 4 && field >= 0x10 && field <= 0x15)
					hakios_tl("UMI SDX55: LOC constellation field=%#x config=%u\n", field, sdx55_le32(payload + off));
				if (n == 4 && field == 1) umi_sdx55_loc.control_status = sdx55_le32(payload + off);
				if (n == 8 && field == 0x12 && message == 0x3b) {
					umi_sdx55_loc.mask = sdx55_le32(payload + off) |
					    (uint64_t)sdx55_le32(payload + off + 4) << 32;
					umi_sdx55_loc.mask_valid = true;
					hakios_tl("UMI SDX55: LOC engine mask=%#llx\n", umi_sdx55_loc.mask);
				}
				if (n == 4 && field == 0x10 && message == 0x3b)
					umi_sdx55_loc.lock = sdx55_le32(payload + off);
				off += n;
			}
			umi_sdx55_loc.control_ind = true;
			hakios_tl("UMI SDX55: LOC engine msg=%#x status=%u lock=%u\n",
			    message, umi_sdx55_loc.control_status, umi_sdx55_loc.lock);
		} else if (payload[0] == QMI_INDICATION &&
		    (message == 0x25 || message == 0x26 || message == 0x2b || message == 0x2c)) {
			sdx55_loc_event(message, payload + 7, length - 7);
		} else if (payload[0] == QMI_INDICATION && message == 0x24) {
			if (hakios_loc_indication(payload, length)) umi_sdx55_loc.fix = true;
		}
		return;
	}
	if (type == 2 && length == 20 && sdx55_le32(payload) == 2 && src_node && src_node != UINT32_MAX) {
		umi_sdx55_peer = src_node;
		hakios_tl("UMI SDX55: QRTR HELLO peer=%u\n", src_node);
	}
	if (type == 1 && dst_port == SDX55_TFTP_SERVICE_PORT && length >= 4) {
		unsigned opcode = (unsigned)payload[0] << 8 | payload[1];
		if (opcode == 1 || opcode == 2) {
			size_t n = 0;
			while (n + 2 < length && n < 160 &&
			    payload[2 + n] >= 32 && payload[2 + n] < 127) n++;
			hakios_tl("UMI SDX55: TFTP op=%u path=%.*s\n",
			    opcode, (int)n, (const char *)payload + 2);
			if (2 + n + sizeof("\0octet") > length ||
			    memcmp(payload + 2 + n, "\0octet", sizeof("\0octet"))) return;
			const char *name = (const char *)payload + 2;
			const uint8_t *options = payload + 2 + n + sizeof("\0octet");
			size_t options_length = length - (size_t)(options - payload);
			bool temporary = !strcmp(name, "/readwrite/mcfg.tmp");
			if (opcode == 1) {
				const uint8_t *file = NULL;
				uint64_t size = 0;
				for (unsigned i = 0; i < sizeof(umi_sdx55_configs)/sizeof(umi_sdx55_configs[0]); i++) {
					if (strcmp(umi_sdx55_configs[i].path, name)) continue;
					file = umi_sdx55_configs[i].bytes;
					size = umi_sdx55_configs[i].size;
					break;
				}
				if (temporary && umi_sdx55_temp.exists) {
					file = umi_sdx55_temp.bytes;
					size = umi_sdx55_temp.size;
				}
				if (file) {
					umi_sdx55_config_transfer.file = file;
					umi_sdx55_config_transfer.size = size;
					umi_sdx55_tftp_read(src_node, src_port, options, options_length);
				} else {
					umi_sdx55_missing.node = src_node;
					umi_sdx55_missing.port = src_port;
				}
			}
			if (opcode == 2 && (temporary || !strcmp(name, "/readwrite/server_check.txt"))) {
				bool unlink = false;
				int len = sdx55_tftp_write_options(options, options_length, temporary,
				    umi_sdx55_check.reply, sizeof(umi_sdx55_check.reply), &unlink);
				if (len < 0) return;
				umi_sdx55_check.reply_len = len;
				if (unlink) memset(&umi_sdx55_temp, 0, sizeof(umi_sdx55_temp));
				umi_sdx55_check.temporary = temporary && !unlink;
				umi_sdx55_check.node = src_node;
				umi_sdx55_check.port = src_port;
				umi_sdx55_check.ack = 0;
				umi_sdx55_check.pending = true;
			}
		}
		return;
	}
	if (type == 1 && dst_port == umi_sdx55_config_transfer.local && length >= 4 &&
	    payload[0] == 0 && payload[1] == 5)
		hakios_tl("UMI SDX55: TFTP config end code=%u detail=%.*s\n",
		    payload[2] << 8 | payload[3], (int)MIN(64, length-4), (const char *)payload + 4);
	if (type == 1 && dst_port == umi_sdx55_config_transfer.local && length == 4 &&
	    payload[0] == 0 && payload[1] == 4) {
		umi_sdx55_tftp_ack(src_node, src_port, dst_port, (unsigned)payload[2] << 8 | payload[3]);
		return;
	}
	if (type == 1 && dst_port == SDX55_TFTP_TRANSFER_PORT &&
	    src_node == umi_sdx55_check.node && src_port == umi_sdx55_check.port &&
	    length >= 4 && !memcmp(payload, "\0\3\0\1", 4)) {
		if (length - 4 > sizeof(umi_sdx55_temp.bytes)) {
			static const char full[] = "\0\5\0\3File too large";
			memcpy(umi_sdx55_check.reply, full, sizeof(full));
			umi_sdx55_check.reply_len = sizeof(full);
			umi_sdx55_check.pending = true;
			return;
		}
		umi_sdx55_check.size = length - 4;
		memcpy(umi_sdx55_check.reply, "\0\4\0\1", 4);
		umi_sdx55_check.reply_len = 4;
		if (umi_sdx55_check.temporary) {
			memcpy(umi_sdx55_temp.bytes, payload + 4, length - 4);
			umi_sdx55_temp.size = length - 4;
			umi_sdx55_temp.exists = true;
		}
		umi_sdx55_check.ack = 1;
		umi_sdx55_check.pending = true;
		return;
	}
	if ((type != 4 && type != 5) || length != 20 || sdx55_le32(payload) != type) {
		if (umi_sdx55_qrtr_frames <= 4)
			hakios_tl("UMI SDX55: QRTR control type=%u length=%u\n", type, length);
		return;
	}
	uint32_t service = sdx55_le32(payload + 4), instance = sdx55_le32(payload + 8);
	uint32_t node = sdx55_le32(payload + 12), port = sdx55_le32(payload + 16);
	hakios_tl("UMI SDX55: QRTR type=%u service=%u instance/version=%#x node=%u port=%u\n",
	    type, service, instance, node, port);
	sdx55_cell_announce(type, service, instance, node, port);
	sdx55_voice_announce(type, service, instance, node, port);
	if (service == 2 && instance == 1 && port && (type == 4 ||
	    (umi_sdx55_dms_node == node && umi_sdx55_dms_port == port))) {
		umi_sdx55_dms_node = node;
		umi_sdx55_dms_port = type == 4 ? port : 0;
	}
	if (service != 16 || instance != 2 || !port) return;
	if (type == 4) {
		umi_sdx55_loc_node = node;
		umi_sdx55_loc_port = port;
		hakios_tl("UMI SDX55: LOC v2 instance 0 node=%u port=%u\n", node, port);
	} else if (node == umi_sdx55_loc_node && port == umi_sdx55_loc_port) {
		umi_sdx55_loc_port = 0;
	}
}

/* Release QRTR flow-control credits outside the RX callback. */
static int
umi_sdx55_poll_loc(struct umi_mhi *host, unsigned ms)
{
	unsigned received;
	int error = umi_mhi_poll_ipcr(host, ms, &received);
	if (!error && umi_sdx55_check.pending) {
		uint16_t block = umi_sdx55_check.ack;
		size_t n = umi_sdx55_check.reply_len;
		uint32_t packet[8 + sizeof(umi_sdx55_check.reply) / 4] = {1, 1, 1,
		    SDX55_TFTP_TRANSFER_PORT, 0, (uint32_t)n, umi_sdx55_check.node, umi_sdx55_check.port};
		memcpy(packet + 8, umi_sdx55_check.reply, n);
		unsigned opcode = umi_sdx55_check.reply[1];
		umi_sdx55_check.pending = false;
		error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((n + 3u) & ~3u), 1000, UINT16_MAX);
		hakios_tl("UMI SDX55: TFTP write reply opcode=%u block=%u bytes=%u errno=%d\n",
		    opcode, block, (unsigned)umi_sdx55_check.size, error);
	}
	if (!error) error = umi_sdx55_tftp_flush(host);
	if (!error && umi_sdx55_pdm_count) {
		/* TX also services RX, so release the queued slot only after copying it. */
		typeof(umi_sdx55_pdm_queue[0]) reply = umi_sdx55_pdm_queue[--umi_sdx55_pdm_count];
		error = umi_mhi_send_qrtr_packet(host, reply.packet, reply.size, 1000, UINT16_MAX);
	}
	if (!error && sdx55_audio_pd.enabled) sdx55_sat_poll(host);
	if (!error) error = sdx55_audio_pd_poll(host);
	if (error || !umi_sdx55_credits) return error;
	unsigned i = --umi_sdx55_credits;
	uint32_t local = umi_sdx55_credit[i].local;
	uint32_t resume[] = {1, 7, 1, local, 0, 20,
	    umi_sdx55_credit[i].node, umi_sdx55_credit[i].port, 7, 1, local, 0, 0};
	return umi_mhi_send_qrtr_packet(host, resume, sizeof(resume), 1000, UINT16_MAX);
}

static int
umi_sdx55_dms_command(struct umi_mhi *host, uint16_t message, uint8_t mode)
{
	uint32_t packet[12] = {1, 1, 1, SDX55_DMS_PORT, 0, 7, umi_sdx55_dms_node, umi_sdx55_dms_port};
	uint8_t *qmi = (uint8_t *)(packet + 8);
	uint16_t txn = ++umi_sdx55_dms.txn;
	qmi[1] = txn;
	qmi[2] = txn >> 8;
	qmi[3] = message;
	if (message == SDX55_DMS_SET_MODE || message == SDX55_DMS_BIND) {
		/* BIND uses a 32-bit subscription enum, SET_MODE a single byte. */
		qmi[7] = 1;
		qmi[8] = message == SDX55_DMS_BIND ? 4 : 1;
		qmi[5] = 3 + qmi[8];
		qmi[10] = mode;
		packet[5] += qmi[5];
	}
	umi_sdx55_dms.service_error = UINT16_MAX;
	umi_sdx55_dms.message = message;
	umi_sdx55_dms.mode = UINT8_MAX;
	umi_sdx55_dms.reason = 0;
	umi_sdx55_dms.restricted = umi_sdx55_dms.replied = false;
	int error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((packet[5] + 3) & ~3u), 1000, UINT16_MAX);
	for (unsigned ms = 0; !error && !umi_sdx55_dms.replied && ms < 5000; ms += 10)
		error = umi_sdx55_poll_loc(host, 10);
	if (error || !umi_sdx55_dms.replied) return error ? error : ETIMEDOUT;
	return umi_sdx55_dms.error;
}

static int
umi_sdx55_dms_change_mode(struct umi_mhi *host, uint8_t mode)
{
	int error = umi_sdx55_dms_command(host, SDX55_DMS_SET_MODE, mode);
	for (unsigned i = 0; !error && i < 20; i++) {
		error = umi_sdx55_dms_command(host, SDX55_DMS_GET_MODE, 0);
		if (!error && umi_sdx55_dms.mode == mode) return 0;
		if (!error) error = umi_sdx55_poll_loc(host, 100);
	}
	return error ? error : ETIMEDOUT;
}

/* Stock UMI publishes LOC before its radio becomes ready. Continue serving
 * modem startup traffic while DMS explicitly reports DEVICE_NOT_READY. */
static int
umi_sdx55_dms_online(struct umi_mhi *host, int *restore_mode)
{
	int error = umi_sdx55_dms_command(host, SDX55_DMS_BIND, 1);
	if (error) return error;
	bool skip_low_power = false;
	int unsupported_from = -1;
	for (unsigned i = 0; i <= 12; i++) {
		error = umi_sdx55_dms_command(host, SDX55_DMS_GET_MODE, 0);
		if (error || umi_sdx55_dms.mode == SDX55_DMS_ONLINE) return error;
		if (umi_sdx55_dms.reason || umi_sdx55_dms.restricted) return EBUSY;
		uint8_t previous = umi_sdx55_dms.mode;
		if (previous != SDX55_DMS_OFFLINE && previous != SDX55_DMS_LOW_POWER &&
		    previous != SDX55_DMS_SHUTTING_DOWN) return EBUSY;
		if (previous != unsupported_from) {
			/* On stock cold boot DMS also reports 5 until qcrild requests LOW_POWER. */
			bool startup = previous == SDX55_DMS_SHUTTING_DOWN;
			uint8_t target = startup || (previous == SDX55_DMS_OFFLINE && !skip_low_power) ?
			    SDX55_DMS_LOW_POWER : SDX55_DMS_ONLINE;
			bool first_change = *restore_mode < 0;
			/* A transient boot indication is not a shutdown request to replay at cleanup. */
			if (first_change) *restore_mode = startup ? SDX55_DMS_LOW_POWER : previous;
			error = umi_sdx55_dms_change_mode(host, target);
			if (!error) {
				if (target == SDX55_DMS_ONLINE) return 0;
				continue;
			}
			if (umi_sdx55_dms.message != SDX55_DMS_SET_MODE || !umi_sdx55_dms.replied)
				return error;
			if (first_change) *restore_mode = -1;
			if (umi_sdx55_dms.service_error == SDX55_DMS_INVALID_TRANSITION) {
				/* OFFLINE can reject the stock LOW_POWER path even while
				 * ONLINE returns the transient DEVICE_NOT_READY result. */
				if (target == SDX55_DMS_ONLINE) unsupported_from = previous;
				else if (!startup) skip_low_power = true;
			} else if (umi_sdx55_dms.service_error != SDX55_DMS_NOT_READY) {
				return error;
			}
		}
		if (i == 12) break;
		for (unsigned n = 0; n < 40; n++) {
			error = umi_sdx55_poll_loc(host, 100);
			if (error) return error;
		}
	}
	return ETIMEDOUT;
}

static int
umi_sdx55_loc_exchange(struct umi_mhi *host, uint16_t message, uint32_t *packet, int length)
{
	if (length < 0) return -length;
	packet[0] = packet[1] = packet[2] = 1;
	packet[3] = SDX55_LOC_PORT;
	packet[4] = 0;
	packet[5] = length;
	packet[6] = umi_sdx55_loc_node;
	packet[7] = umi_sdx55_loc_port;
	umi_sdx55_loc.message = message;
	umi_sdx55_loc.replied = false;
	umi_sdx55_loc.control_ind = false;
	umi_sdx55_loc.control_status = UINT32_MAX;
	umi_sdx55_loc.error = 0;
	bool control = sdx55_loc_has_control_ind(message);
	int error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((length + 3) & ~3),
	    1000, UINT16_MAX);
	for (unsigned ms = 0; !error && ms < 5000; ms += 10) {
		if (umi_sdx55_loc.replied &&
		    (umi_sdx55_loc.error || !control || umi_sdx55_loc.control_ind)) break;
		error = umi_sdx55_poll_loc(host, 10);
	}
	if (error || !umi_sdx55_loc.replied) return error ? error : ETIMEDOUT;
	if (umi_sdx55_loc.error) return umi_sdx55_loc.error;
	if (control && !umi_sdx55_loc.control_ind) return ETIMEDOUT;
	return control && umi_sdx55_loc.control_status ? EIO : 0;
}

static int
umi_sdx55_loc_command(struct umi_mhi *host, uint16_t message, uint32_t lock, const uint64_t *mask)
{
	uint32_t packet[32] = {0};
	uint16_t txn = ++umi_sdx55_loc.txn;
	int length = hakios_loc_request(message, txn, packet + 8, sizeof(packet) - 32, lock, mask);
	return umi_sdx55_loc_exchange(host, message, packet, length);
}

/* A probe can supply a fresh file on the supplementary RAM disk. The modem
 * validates the orbit data. Leave the normal acquisition path independent. */
static int
umi_sdx55_inject_orbits(struct umi_mhi *host)
{
	int disk = -1;
	uint64_t size = 0, deadline;
	const uint8_t *data = hakios_image_file("sdx55m", "xtra3Mgrbeji.bin", &size, &disk);
	if (!data || disk != 1) return ENOENT;
	if (!size || size > SDX55_ORBIT_MAX) return EINVAL;
	unsigned parts = (unsigned)((size + SDX55_ORBIT_CHUNK - 1) / SDX55_ORBIT_CHUNK);
	uint32_t packet[(32 + 32 + SDX55_ORBIT_CHUNK) / 4];
	clock_interval_to_deadline(30, NSEC_PER_SEC, &deadline);
	for (unsigned part = 1; part <= parts; part++) {
		if (mach_absolute_time() >= deadline) return ETIMEDOUT;
		memset(packet, 0, sizeof(packet));
		umi_sdx55_loc.orbit_part = part;
		int length = sdx55_encode_orbit_part(++umi_sdx55_loc.txn, data, size, part,
		    packet + 8, sizeof(packet) - 32);
		int error = umi_sdx55_loc_exchange(host, SDX55_LOC_INJECT_XTRA_DATA, packet, length);
		if (error || part == parts) {
			hakios_tl("UMI SDX55: LOC orbit part=%u/%u bytes=%llu status=%u errno=%d\n",
			    part, parts, size, umi_sdx55_loc.control_status, error);
		}
		if (error) return error;
	}
	return 0;
}

static int
umi_sdx55_restore_mode(struct umi_mhi *host, int previous)
{
	if (previous < 0 || host->poisoned) return 0;
	/* Even a lost SET response may have taken effect. Read back before restoring. */
	int error = umi_sdx55_dms_command(host, SDX55_DMS_GET_MODE, 0);
	if (!error && umi_sdx55_dms.mode != previous)
		error = umi_sdx55_dms_change_mode(host, (uint8_t)previous);
	hakios_tl("UMI SDX55: DMS restore mode=%d errno=%d\n", previous, error);
	return error;
}

static int
umi_sdx55_loc_session(struct umi_mhi *host)
{
	uint32_t seconds = 150, interactive = 0;
	PE_parse_boot_argn("sdx55_fix_secs", &seconds, sizeof(seconds));
	PE_parse_boot_argn("sdx55_interactive", &interactive, sizeof(interactive));
	if (!seconds || seconds > 600) return EINVAL;
	memset(&umi_sdx55_loc, 0, sizeof(umi_sdx55_loc));
	int error = umi_sdx55_loc_command(host, 0x21, 0, NULL);
	if (!error) error = umi_sdx55_loc_command(host, 0xba, 0, NULL);
	if (!error) error = umi_sdx55_loc_command(host, 0x3b, 0, NULL);
	if (error) return error;
	int config_error = umi_sdx55_loc_command(host, 0xbf, 0, NULL);
	hakios_tl("UMI SDX55: LOC get constellation config errno=%d\n", config_error);
	uint32_t saved = umi_sdx55_loc.lock;
	bool has_mask = umi_sdx55_loc.mask_valid;
	uint64_t saved_mask = umi_sdx55_loc.mask;
	/* Android with location enabled reports mask 2 on this modem. Keep
	 * its non-local bits as read and allow the locally initiated session. */
	uint64_t active_mask = saved_mask & ~1ull;
	if (!has_mask && (saved < 1 || saved > 4)) return EINVAL;
	bool change = has_mask ? active_mask != saved_mask : saved == 2 || saved == 4;
	if (change) error = umi_sdx55_loc_command(host, 0x3a,
	    has_mask ? 3 : (saved == 2 ? 1 : 3), has_mask ? &active_mask : NULL);
	int restore_mode = -1;
	if (!error && umi_sdx55_dms_port && umi_sdx55_dms_node == umi_sdx55_loc_node) {
		int mode_error = umi_sdx55_dms_online(host, &restore_mode);
		hakios_tl("UMI SDX55: DMS request online errno=%d restore_mode=%d\n", mode_error, restore_mode);
		if (mode_error && (host->poisoned || restore_mode >= 0)) error = mode_error;
	}
	if (!error) {
		int validity_error = umi_sdx55_loc_command(host, 0x37, 0, NULL);
		hakios_tl("UMI SDX55: LOC get predicted orbit validity errno=%d\n", validity_error);
	}
	uint32_t xtra = 0;
	bool orbits_transferred = false;
	if (!error && !host->poisoned && PE_parse_boot_argn("sdx55_xtra", &xtra, sizeof(xtra)) && xtra == 1) {
		int orbit_error = umi_sdx55_inject_orbits(host);
		hakios_tl("UMI SDX55: LOC inject predicted orbits errno=%d\n", orbit_error);
		if (!orbit_error) {
			orbits_transferred = true;
			int verify = umi_sdx55_loc_command(host, 0x37, 0, NULL);
			hakios_tl("UMI SDX55: LOC orbit validity after injection errno=%d\n", verify);
		}
	}
	if (!error && host->poisoned) error = EIO;
	if (!error) {
		uint32_t standalone = 0;
		PE_parse_boot_argn("sdx55_standalone", &standalone, sizeof(standalone));
		/* Mode 2 matches the native UMI HAL. Mode 4 runs without MS-based assistance. */
		uint32_t mode = standalone ? 4 : 2;
		error = umi_sdx55_loc_command(host, 0x4a, mode, NULL);
		hakios_tl("UMI SDX55: LOC operation mode=%u errno=%d\n", mode, error);
	}
	bool stop_session = false;
	if (!error) {
		error = umi_sdx55_loc_command(host, 0x22, 0, NULL);
		/* A lost START reply may still leave our periodic session active. */
		stop_session = !error || error == ETIMEDOUT;
	}
	if (!error) {
		int time_error = umi_sdx55_loc_command(host, 0x38, 0, NULL);
		hakios_tl("UMI SDX55: LOC UTC assistance errno=%d\n", time_error);
	}
	struct umi_pcie *pcie = host->io->context;
	uint32_t last_ltssm = UINT32_MAX, last_signals = UINT32_MAX;
	/* A manual app test keeps the periodic stream until its acquisition window ends. */
	for (unsigned ms = 0; !error && (interactive == 1 || !umi_sdx55_loc.fix) && ms < seconds * 1000; ms += 50) {
		if (ms % 1000 == 0) {
			/* Observe host-side registers only, including after the last QRTR RX. */
			uint32_t ltssm = rd(pcie, PARF + 0x1b0);
			uint32_t running = gpio_level(pcie, 3), fatal = gpio_level(pcie, 1);
			uint32_t perst = gpio_level(pcie, 85), ap_status = gpio_level(pcie, 56);
			uint32_t signals = running | fatal << 1 | perst << 2 | ap_status << 3;
			if (ltssm != last_ltssm || signals != last_signals || ms % 30000 == 0)
				hakios_tl("UMI SDX55: LOC link ms=%u LTSSM=%#x MDM_STATUS=%u ERRFATAL=%u PERST=%u AP_STATUS=%u frames=%u\n",
				    ms, ltssm, running, fatal, perst, ap_status, umi_sdx55_qrtr_frames);
			last_ltssm = ltssm;
			last_signals = signals;
			if (fatal || !running) {
				hakios_tl("UMI SDX55: modem unavailable, ending mission DMA before reset\n");
				host->poisoned = true;
				error = ENXIO;
				break;
			}
		}
		error = umi_sdx55_poll_loc(host, 50);
	}
	if (stop_session && !host->poisoned) {
		int stop = umi_sdx55_loc_command(host, 0x23, 0, NULL);
		hakios_tl("UMI SDX55: LOC stop session errno=%d\n", stop);
		if (!error && stop) error = stop;
	}
	if (orbits_transferred && !host->poisoned) {
		int verify = umi_sdx55_loc_command(host, 0x37, 0, NULL);
		hakios_tl("UMI SDX55: LOC orbit validity after acquisition errno=%d\n", verify);
	}
	/* The desktop's console can wrap the loader's live log between host polls. */
	printf("UMI SDX55: LOC peak tracked=%u BDS_tracked=%u snr_bits=%#x reports=%u fix=%u\n",
	    umi_sdx55_loc.max_tracked, umi_sdx55_loc.max_bds_tracked,
	    umi_sdx55_loc.peak_snr_bits, umi_sdx55_loc.sv_reports, umi_sdx55_loc.fix);
	if (!error && !umi_sdx55_loc.fix) error = ETIMEDOUT;
	int mode_error = umi_sdx55_restore_mode(host, restore_mode);
	if (mode_error) error = mode_error;
	if (change && !host->poisoned) {
		int restore = umi_sdx55_loc_command(host, 0x3a, has_mask ? 3 : saved, has_mask ? &saved_mask : NULL);
		hakios_tl("UMI SDX55: LOC restore engine lock=%u mask=%#llx errno=%d\n", saved, saved_mask, restore);
		if (restore) return restore;
	}
	return error;
}

static bool
sdx55_cell_discovered(void)
{
	if (!umi_sdx55_dms_node || !umi_sdx55_dms_port) return false;
	for (unsigned i = 0; i < sizeof(sdx55_services)/sizeof(sdx55_services[0]); i++)
		if (!sdx55_services[i].port || sdx55_services[i].node != umi_sdx55_dms_node) return false;
	return true;
}

/* Only the mission worker services MHI. The network stack hands raw IPv4
 * to a bounded queue and this worker drains it alongside modem RX and EFS. */
static struct {
	const struct sdx55_data_routes *routes;
	bool network_ready, flow_off;
	unsigned head, count, tx, rx, dropped;
	int error;
	uint8_t ack[8][20], packet[1508];
} sdx55_packets;

static void
sdx55_receive_packets(void *context, const void *buffer, size_t size)
{
	(void)context;
	const uint8_t *p = buffer;
	if (!sdx55_packets.routes || !sdx55_data_routes_current(sdx55_packets.routes)) return;
	while (size) {
		struct sdx55_qmap_frame frame;
		size_t used = sdx55_qmap_next(SDX55_CELL_MUX, p, size, &frame);
		if (!used) { sdx55_packets.dropped++; return; }
		if (frame.kind == SDX55_QMAP_IPV4) {
			if (sdx55_packets.network_ready && umi_sdx55_net_receive(frame.ip, frame.ip_length)) sdx55_packets.rx++;
			else sdx55_packets.dropped++;
		} else if (frame.kind == SDX55_QMAP_FLOW_DISABLE || frame.kind == SDX55_QMAP_FLOW_ENABLE) {
			unsigned slots = sizeof(sdx55_packets.ack)/sizeof(sdx55_packets.ack[0]);
			if (sdx55_packets.count == slots) { sdx55_packets.error = EOVERFLOW; return; }
			unsigned slot = (sdx55_packets.head + sdx55_packets.count++) % slots;
			memcpy(sdx55_packets.ack[slot], frame.ack, sizeof(frame.ack));
			sdx55_packets.flow_off = frame.kind == SDX55_QMAP_FLOW_DISABLE;
		}
		p += used;
		size -= used;
	}
}

/* Called by the worker after an MHI operation returns. ACK sends can service
 * RX themselves. Pop before sending and bound the work in each pass. */
static int
sdx55_flush_flow_acks(struct umi_mhi *host)
{
	if (!sdx55_packets.routes) return 0;
	unsigned slots = sizeof(sdx55_packets.ack)/sizeof(sdx55_packets.ack[0]);
	for (unsigned sent = 0; ; sent++) {
		if (!sdx55_data_routes_current(sdx55_packets.routes)) return ENXIO;
		if (sdx55_packets.error) return sdx55_packets.error;
		if (!sdx55_packets.count || sent == slots) return 0;
		uint8_t ack[sizeof(sdx55_packets.ack[0])];
		memcpy(ack, sdx55_packets.ack[sdx55_packets.head], sizeof(ack));
		sdx55_packets.head = (sdx55_packets.head + 1) % slots;
		sdx55_packets.count--;
		int error = umi_mhi_send_cell_data(host, ack, sizeof(ack), 1000);
		if (error) return error;
	}
}

static int
sdx55_cell_command(struct umi_mhi *host, const struct sdx55_cell_endpoint *ep,
    uint16_t message, uint32_t argument, const struct sdx55_wan_status *format)
{
	if (!sdx55_cell_endpoint_current(ep)) return ENXIO;
	uint32_t packet[32] = {1, 1, 1, SDX55_CELL_PORT, 0, 0, ep->node, ep->port};
	uint8_t *qmi = (uint8_t *)(packet + 8);
	uint16_t txn = sdx55_query.txn + 1;
	size_t cap = sizeof(packet) - 32, len;
	if (ep->service == 11 && message == SDX55_UIM_PROVISION)
		len = sdx55_uim_provision_request(txn, argument, &sdx55_query.cell, qmi, cap);
	else if ((ep->service == 3 && message == SDX55_NAS_BIND_SUBSCRIPTION) ||
	    (ep->service == 1 && message == SDX55_WDS_BIND_SUBSCRIPTION))
		len = sdx55_cell_bind_request(message, txn, argument, qmi, cap);
	else if (ep->service == 3 && message == SDX55_NAS_GET_PLMN_NAME)
		len = sdx55_cell_plmn_request(txn, argument, qmi, cap);
	else if (ep->service == 26 && message == SDX55_WDA_SET_FORMAT)
		len = sdx55_wda_set_pcie_format_request(txn, SDX55_PCIE_DATA_IFACE, format, qmi, cap);
	else if (ep->service == 26)
		len = sdx55_wda_pcie_format_request(txn, SDX55_PCIE_DATA_IFACE, qmi, cap);
	else if (ep->service == 1 && message != SDX55_WDS_GET_STATUS)
		len = sdx55_wds_request(message, txn, SDX55_PCIE_DATA_IFACE, SDX55_CELL_MUX, argument, qmi, cap);
	else if (ep->service == 1)
		len = sdx55_wan_request(message, txn, qmi, cap);
	else
		len = sdx55_cell_request(message, txn, qmi, cap);
	sdx55_query = (typeof(sdx55_query)){ .service = ep, .node = ep->node,
	    .port = ep->port, .txn = txn, .message = message };
	packet[5] = (uint32_t)len;
	int error = len ? umi_mhi_send_qrtr_packet(host, packet, 32 + ((len + 3u) & ~3u),
	    1000, UINT16_MAX) : EINVAL;
	unsigned timeout = ep->service == 1 && message == SDX55_WDS_START ? 60000 : 5000;
	if (!error) error = sdx55_flush_flow_acks(host);
	for (unsigned ms = 0; !error && !sdx55_query.done && ms < timeout; ms += 10) {
		error = umi_sdx55_poll_loc(host, 10);
		if (!error) error = sdx55_flush_flow_acks(host);
	}
	sdx55_query.service = NULL;
	if (!error) error = sdx55_query.done ? sdx55_query.error : ETIMEDOUT;
	sdx55_status_result(ep->service, message, argument, error, &sdx55_query.cell, &sdx55_query.wan);
	hakios_tl("UMI SDX55: cellular QMI service=%u message=%#x errno=%d service_error=%u\n",
	    ep->service, message, error, sdx55_query.service_error);
	return error;
}

static int
sdx55_read_cell_service(struct umi_mhi *host, const struct sdx55_cell_endpoint *ep)
{
	int error = sdx55_cell_command(host, ep, ep->message, 0, NULL);
	const struct sdx55_cell_status *c = &sdx55_query.cell;
	const struct sdx55_wan_status *w = &sdx55_query.wan;
	if (error) return error;
	switch (ep->message) {
	case SDX55_UIM_GET_CARD_STATUS:
		hakios_tl("UMI SDX55: UIM slots=%u present=%u primary_slot=%d app_state=%u secondary_slot=%d app_state=%u\n",
		    c->cards, c->present_cards, c->gw[0].slot == UINT8_MAX ? -1 : (int)c->gw[0].slot, c->gw[0].app_state,
		    c->gw[1].slot == UINT8_MAX ? -1 : (int)c->gw[1].slot, c->gw[1].app_state);
		hakios_tl("UMI SDX55: UIM slot0 state=%u apps=%u ready_gw=%u slot1 state=%u apps=%u ready_gw=%u\n",
		    c->slots[0].card_state, c->slots[0].applications, c->slots[0].ready_gw,
		    c->slots[1].card_state, c->slots[1].applications, c->slots[1].ready_gw);
		break;
	case SDX55_NAS_GET_SERVING_SYSTEM:
		hakios_tl("UMI SDX55: NAS registration=%u PS_attach=%u radio_mask=%#x data_mask=%#x\n",
		    c->registration, c->ps_attach, c->radio_mask, c->data_mask);
		break;
	case SDX55_WDS_GET_STATUS:
		hakios_tl("UMI SDX55: WDS packet service status=%u\n", w->connection_status);
		break;
	case SDX55_WDA_GET_FORMAT:
		hakios_tl("UMI SDX55: WDA iface=%u link=%#x UL=%#x DL=%#x DL_bytes=%#x\n",
		    SDX55_PCIE_DATA_IFACE, w->link_layer, w->uplink, w->downlink, w->dl_max_size);
		break;
	}
	return 0;
}

static void
sdx55_refresh_radio(struct umi_mhi *host, const struct sdx55_cell_endpoint *nas)
{
	struct sdx55_cell_status serving = {0}, system = {.nr_service = UINT8_MAX, .lte_service = UINT8_MAX};
	if (!sdx55_cell_command(host, nas, SDX55_NAS_GET_SERVING_SYSTEM, 0, NULL))
		serving = sdx55_query.cell;
	if (!sdx55_cell_command(host, nas, SDX55_NAS_GET_SYSTEM_INFO, 0, NULL))
		system = sdx55_query.cell;
	bool nr = (system.nr_service == 2 && !system.nr_forbidden) ||
	    (serving.registration == 1 && (serving.radio_mask & (1u << 12)));
	bool lte = (system.lte_service == 2 && !system.lte_forbidden) ||
	    (serving.registration == 1 && (serving.radio_mask & (1u << 8)));
	unsigned rat = nr ? 5 : lte ? 4 : 0;
	int32_t rsrp = INT32_MIN;
	if (rat && !sdx55_cell_command(host, nas, SDX55_NAS_GET_SIGNAL_INFO, 0, NULL)) {
		int16_t sample = rat == 5 ? sdx55_query.cell.nr_rsrp : sdx55_query.cell.lte_rsrp;
		if (sample >= -156 && sample <= -31) rsrp = sample;
	}
	const char *plmn = serving.plmn;
	if (rat == 4 && system.lte_plmn[0]) plmn = system.lte_plmn;
	if (rat == 5 && system.plmn[0]) plmn = system.plmn;
	else if (rat == 5 && !plmn[0] && lte) plmn = system.lte_plmn;
	char name[sizeof(sdx55_radio.operator_name)] = {0};
	if (rat && plmn[0]) {
		if (!strcmp(plmn, serving.plmn)) strlcpy(name, serving.operator_name, sizeof(name));
		if (!name[0] && !strcmp(plmn, sdx55_radio.plmn)) strlcpy(name, sdx55_radio.operator_name, sizeof(name));
		if (!name[0] && strcmp(plmn, sdx55_radio.plmn)) {
			uint32_t mcc = (plmn[0]-'0')*100 + (plmn[1]-'0')*10 + (plmn[2]-'0');
			uint32_t mnc = (plmn[3]-'0')*10 + (plmn[4]-'0');
			if (plmn[5]) mnc = mnc*10 + (plmn[5]-'0');
			uint32_t argument = mcc | (mnc << 10) | ((plmn[5] != 0) << 20);
			if (!sdx55_cell_command(host, nas, SDX55_NAS_GET_PLMN_NAME, argument, NULL))
				strlcpy(name, sdx55_query.cell.operator_name, sizeof(name));
		}
	}
	lck_mtx_lock(&cell_status_lock);
	bool changed = sdx55_radio.rat != rat || sdx55_radio.rsrp != rsrp ||
	    strcmp(sdx55_radio.plmn, rat ? plmn : "") || strcmp(sdx55_radio.operator_name, rat ? name : "");
	sdx55_radio.rat = rat;
	sdx55_radio.rsrp = rsrp;
	strlcpy(sdx55_radio.plmn, rat ? plmn : "", sizeof(sdx55_radio.plmn));
	strlcpy(sdx55_radio.operator_name, rat ? name : "", sizeof(sdx55_radio.operator_name));
	lck_mtx_unlock(&cell_status_lock);
	if (changed) hakios_tl("UMI SDX55: radio generation=%u RSRP=%d PLMN=%s description=%u (NAS NR=%u LTE=%u legacy=%u)\n",
	    rat, rsrp, rat ? plmn : "", rat && name[0], system.nr_service, system.lte_service, serving.registration);
}

#include "umi_sdx55_ims.inc"
#include "umi_sdx55_voice_audio.inc"
#include "umi_sdx55_voice.inc"

static int
sdx55_move_packets(struct umi_mhi *host, uint64_t deadline, const struct sdx55_data_routes *routes,
    const struct sdx55_cell_endpoint *nas)
{
	uint64_t check_status;
	clock_interval_to_deadline(10, NSEC_PER_SEC, &check_status);
	struct umi_pcie *p = host->io->context;
	int error = 0;
	sdx55_packets.network_ready = true;
	while (!error && sdx55_data_enabled() && mach_absolute_time() < deadline) {
		if (!gpio_level(p, 3) || gpio_level(p, 1)) { host->poisoned = true; error = ENXIO; break; }
		error = umi_sdx55_poll_loc(host, 5);
		if (!error) error = sdx55_flush_flow_acks(host);
		if (error) break;
		sdx55_voice_tick(host);
		if (mach_absolute_time() >= check_status) {
			error = sdx55_read_cell_service(host, &routes->wds);
			if (!error && sdx55_query.wan.connection_status != 2) error = ENETDOWN;
			if (!error && sdx55_data_enabled()) sdx55_refresh_radio(host, nas);
			clock_interval_to_deadline(10, NSEC_PER_SEC, &check_status);
			continue;
		}
		if (!sdx55_packets.count && !sdx55_packets.flow_off) {
			uint8_t *tx = sdx55_packets.packet;
			size_t len = umi_sdx55_net_dequeue(tx + 8, sizeof(sdx55_packets.packet) - 8);
			if (!len) continue;
			len = sdx55_qmap_encode(SDX55_CELL_MUX, tx + 8, len, tx, sizeof(sdx55_packets.packet));
			if (!len) { sdx55_packets.dropped++; continue; }
			error = umi_mhi_send_cell_data(host, tx, len, 1000);
			if (!error) sdx55_packets.tx++;
		}
	}
	sdx55_packets.network_ready = false;
	hakios_tl("UMI SDX55: en1 QMAP IPv4 TX=%u RX=%u dropped=%u errno=%d\n",
	    sdx55_packets.tx, sdx55_packets.rx, sdx55_packets.dropped, error);
	return error;
}

static bool
sdx55_radio_initializing(uint16_t service_error)
{
	return service_error == 74 || service_error == SDX55_DMS_NOT_READY;
}

static int
sdx55_wait_registration(struct umi_mhi *host, const struct sdx55_data_routes *routes, unsigned *provisioned)
{
	const struct sdx55_cell_endpoint nas = sdx55_services[CELL_NAS];
	uint8_t bound = 0, bound_slot = UINT8_MAX, bound_app = UINT8_MAX, bound_type = UINT8_MAX;
	bool still_empty = false, saw_card = false, requested_registration = false;
	unsigned samples = 0;
	struct sdx55_cell_status last_card_status = {0};
	uint64_t deadline;
	clock_interval_to_deadline(60, NSEC_PER_SEC, &deadline);
	do {
		if (!sdx55_data_routes_current(routes) || !sdx55_cell_endpoint_current(&nas)) return ENXIO;
		uint8_t subscription = 0;
		int error = sdx55_read_cell_service(host, &sdx55_services[CELL_UIM]);
		struct sdx55_gw_app app = {0};
		still_empty = false;
		if (!error) {
			const struct sdx55_cell_status *c = &sdx55_query.cell;
			samples++;
			last_card_status = *c;
			still_empty = c->cards && !c->present_cards && !c->error_cards;
			saw_card |= c->present_cards || c->error_cards;
			error = sdx55_cell_subscription(c, &subscription);
			if (!error) app = c->gw[subscription - 1];
			if (error == EAGAIN && !bound && !*provisioned) {
				unsigned slot = sdx55_uim_provision_slot(c);
				if (slot) {
					int start = sdx55_cell_command(host, &sdx55_services[CELL_UIM],
					    SDX55_UIM_PROVISION, slot, NULL);
					/* Clean up even if the modem acted and its reply was lost. */
					if (!start || start == ETIMEDOUT) *provisioned = slot;
					hakios_tl("UMI SDX55: UIM activate slot=%u errno=%d\n", slot, start);
					if (start && start != ETIMEDOUT) return start;
					clock_interval_to_deadline(60, NSEC_PER_SEC, &deadline);
				}
			}
			if (bound && (error || bound != subscription || bound_slot != app.slot ||
			    bound_app != app.app_index || bound_type != app.app_type)) {
				hakios_tl("UMI SDX55: bound UIM application lost/changed subscription=%u slot=%u app=%u\n",
				    bound, bound_slot, bound_app);
				return ENXIO;
			}
			/* Early boot may report all slots empty while UIM enumerates the card. */
			if (error == ENODEV) error = EAGAIN;
		}
		if (!error && !bound) {
			error = sdx55_cell_command(host, &nas, SDX55_NAS_BIND_SUBSCRIPTION, subscription, NULL);
			if (!error) error = sdx55_cell_command(host, &routes->wds, SDX55_WDS_BIND_SUBSCRIPTION, subscription, NULL);
			if (!error) {
				bound = subscription;
				bound_slot = app.slot;
				bound_app = app.app_index;
				bound_type = app.app_type;
				/* Allow a full registration window even for a late-enumerating SIM. */
				clock_interval_to_deadline(60, NSEC_PER_SEC, &deadline);
				hakios_tl("UMI SDX55: cellular NAS/WDS client subscription=%u\n", bound);
			}
		}
		if (!error) {
			error = sdx55_read_cell_service(host, &nas);
			if (!error && sdx55_query.cell.registration == 1 && sdx55_query.cell.ps_attach == 1) return 0;
			if (!error) {
				error = sdx55_cell_command(host, &nas, SDX55_NAS_GET_SYSTEM_INFO, 0, NULL);
				if (!error) {
					const struct sdx55_cell_status *c = &sdx55_query.cell;
					hakios_tl("UMI SDX55: NAS NR service=%u true=%u domain=%u forbidden=%u\n",
					    c->nr_service, c->nr_true_service, c->nr_domain, c->nr_forbidden);
					if (sdx55_cell_nr_packet_service(c)) return 0;
				}
			}
			if (!error && *provisioned && !requested_registration) {
				requested_registration = true;
				int start = sdx55_cell_command(host, &nas, SDX55_NAS_REGISTER_AUTO, 0, NULL);
				hakios_tl("UMI SDX55: NAS automatic registration errno=%d\n", start);
			}
		}
		if (error && error != EAGAIN && !(error == EIO && sdx55_radio_initializing(sdx55_query.service_error))) return error;
		error = umi_sdx55_poll_loc(host, 1000);
		if (error) return error;
	} while (mach_absolute_time() < deadline);
	hakios_tl("UMI SDX55: cellular registration timeout UIM_samples=%u last_empty=%u card_or_error_seen=%u bound=%u\n",
	    samples, still_empty, saw_card, bound);
	if (!bound && last_card_status.present_cards) {
		for (unsigned slot = 0; slot < 2 && slot < last_card_status.cards; slot++) {
			const typeof(last_card_status.slots[0]) *s = &last_card_status.slots[slot];
			hakios_tl("UMI SDX55: UIM logical_slot=%u card_state=%u card_error=%u UPIN_state=%u\n",
			    slot, s->card_state, s->card_error, s->upin_state);
			for (unsigned i = 0; i < s->applications && i < sizeof(s->app)/sizeof(s->app[0]); i++)
				hakios_tl("UMI SDX55: UIM logical_slot=%u app=%u type=%u state=%u UPIN_replaces=%u PIN1_state=%u\n",
				    slot, i, s->app[i].type, s->app[i].state, s->app[i].upin_replaces, s->app[i].pin1_state);
		}
		int config = sdx55_cell_command(host, &sdx55_services[CELL_UIM], SDX55_UIM_GET_CONFIGURATION, 0, NULL);
		if (!config) hakios_tl("UMI SDX55: UIM auto_selection=%u halt_subscription=%u\n",
		    sdx55_query.cell.auto_select, sdx55_query.cell.halt_subscription);
	}
	return still_empty && !saw_card ? ENODEV : ETIMEDOUT;
}

static int
sdx55_program_format(struct umi_mhi *host, const struct sdx55_cell_endpoint *wda,
    const struct sdx55_wan_status *wanted)
{
	int error = sdx55_cell_command(host, wda, SDX55_WDA_SET_FORMAT, 0, wanted);
	if (host->poisoned) return error ? error : EIO;
	int get = sdx55_read_cell_service(host, wda);
	if (get) return get;
	if (sdx55_wda_format_equal(&sdx55_query.wan, wanted)) return 0;
	return error ? error : EPROTO;
}

static int
sdx55_close_cell_client(struct umi_mhi *host)
{
	/* Qualcomm's QRTR unbind announces the local client, via the link peer. */
	uint32_t packet[] = {1, 6, 1, SDX55_CELL_PORT, 0, 20, umi_sdx55_peer, 0xfffffffe,
	    6, 1, SDX55_CELL_PORT, 0, 0};
	return umi_mhi_send_qrtr_packet(host, packet, sizeof(packet), 1000, UINT16_MAX);
}

static int
sdx55_data_bearer(struct umi_mhi *host, uint64_t deadline, const struct sdx55_data_routes *routes,
    const struct sdx55_cell_endpoint *nas)
{
	const struct sdx55_cell_endpoint *wds = &routes->wds;
	int error = sdx55_read_cell_service(host, wds);
	if (error) return error;
	if (sdx55_query.wan.connection_status != 1) return EBUSY;
	if (!sdx55_data_enabled()) return 0;
	sdx55_data_state(UMI_DATA_CONNECTING);
	/* Flow control can precede the START reply and IP configuration. */
	memset(&sdx55_packets, 0, sizeof(sdx55_packets));
	sdx55_packets.routes = routes;
	uint32_t handle = 0;
	error = sdx55_cell_command(host, wds, SDX55_WDS_START, 0, NULL);
	if (!error) handle = sdx55_query.wds.handle;
	else if (sdx55_query.wds.has_call_end || sdx55_query.wds.has_verbose) {
		const struct sdx55_wds_result *v = &sdx55_query.wds;
		hakios_tl("UMI SDX55: WDS call end=%d verbose_type=%d reason=%d\n",
		    v->has_call_end ? (int)v->call_end : -1,
		    v->has_verbose ? (int)v->verbose_type : -1, (int)v->verbose_reason);
	}
	if (!error && !sdx55_data_routes_current(routes)) error = ENXIO;
	if (!error && sdx55_data_enabled()) {
		error = sdx55_cell_command(host, wds, SDX55_WDS_SETTINGS, 0, NULL);
		if (!error && !sdx55_data_routes_current(routes)) error = ENXIO;
		if (!error && sdx55_data_enabled()) {
			const struct sdx55_wds_result *v = &sdx55_query.wds;
			error = umi_sdx55_net_up(v->address, v->netmask, v->gateway, v->dns1, v->dns2, v->mtu);
			if (!error) {
				sdx55_data_state(UMI_DATA_ON);
				error = sdx55_move_packets(host, deadline, routes, nas);
				umi_sdx55_net_down();
			}
		}
	}
	sdx55_packets.routes = NULL;
	if (handle && !host->poisoned) {
		sdx55_data_state(UMI_DATA_DISCONNECTING);
		int stop = sdx55_cell_endpoint_current(wds) ?
		    sdx55_cell_command(host, wds, SDX55_WDS_STOP, handle, NULL) : ENXIO;
		if (stop) error = stop;
	}
	if (!error) sdx55_data_state(UMI_DATA_OFF);
	return error;
}

static int
sdx55_run_data_call(struct umi_mhi *host, unsigned seconds)
{
	const struct sdx55_data_routes routes = {
		.wds = sdx55_services[CELL_WDS], .wda = sdx55_services[CELL_WDA]
	};
	const struct sdx55_cell_endpoint *wds = &routes.wds;
	int error = sdx55_read_cell_service(host, wds);
	if (error) return error;
	if (sdx55_query.wan.connection_status != 1) return EBUSY;
	error = sdx55_read_cell_service(host, &routes.wda);
	if (error) return error;
	struct sdx55_wan_status saved = sdx55_query.wan;
	if (!sdx55_wda_format_supported(&saved)) {
		hakios_tl("UMI SDX55: cannot restore WDA format, option=%#x\n", saved.unsupported_option);
		return ENOTSUP;
	}
	struct sdx55_wan_status wanted = sdx55_wda_stock_format();
	const struct sdx55_cell_endpoint nas = sdx55_services[CELL_NAS];
	unsigned provisioned = 0;
	sdx55_data_state(UMI_DATA_CONNECTING);
	int ready = sdx55_wait_registration(host, &routes, &provisioned);
	if (!ready && !sdx55_data_routes_current(&routes)) ready = ENXIO;
	if (ready) hakios_tl("UMI SDX55: data call gated before WDS START errno=%d\n", ready);
	error = ready;
	/* Recheck the selected WDS subscription before changing its data path. */
	if (!error) error = sdx55_read_cell_service(host, wds);
	if (!error && sdx55_query.wan.connection_status != 1) error = EBUSY;
	bool change = !error && !sdx55_wda_format_equal(&saved, &wanted);
	if (change) error = sdx55_program_format(host, &routes.wda, &wanted);
	if (!error) error = sdx55_cell_command(host, wds, SDX55_WDS_BIND_MUX, 0, NULL);
	if (!error) error = sdx55_cell_command(host, wds, SDX55_WDS_SET_FAMILY, 0, NULL);
	if (!error && !sdx55_data_routes_current(&routes)) error = ENXIO;
	uint64_t deadline, refresh = 0;
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	if (!error) sdx55_voice_begin();
	while (!error && mach_absolute_time() < deadline) {
		if (!sdx55_data_routes_current(&routes) || !sdx55_cell_endpoint_current(&nas)) {
			error = ENXIO;
			break;
		}
		if (mach_absolute_time() >= refresh) {
			sdx55_refresh_radio(host, &nas);
			clock_interval_to_deadline(10, NSEC_PER_SEC, &refresh);
		}
		if (sdx55_data_enabled())
			error = sdx55_data_bearer(host, deadline, &routes, &nas);
		else {
			if (sdx55_radio.state != UMI_DATA_OFF) sdx55_data_state(UMI_DATA_OFF);
			sdx55_voice_tick(host);
			error = umi_sdx55_poll_loc(host, 100);
		}
	}
	sdx55_voice_finish(host);
	/* A lost START reply can leave a modem-owned session with an unknown
	 * handle. Closing this client's QRTR port releases its resources. */
	if (!host->poisoned) {
		int close = sdx55_close_cell_client(host);
		if (close) error = close;
	}
	if (change && !host->poisoned) {
		/* SET may have taken effect even if its response was lost. */
		int restore = sdx55_cell_endpoint_current(&routes.wda) ?
		    sdx55_program_format(host, &routes.wda, &saved) : ENXIO;
		hakios_tl("UMI SDX55: WDA restore errno=%d\n", restore);
		if (restore) error = restore;
	}
	if (provisioned && !host->poisoned) {
		int restore = sdx55_cell_command(host, &sdx55_services[CELL_UIM],
		    SDX55_UIM_PROVISION, provisioned | SDX55_UIM_DEACTIVATE, NULL);
		hakios_tl("UMI SDX55: UIM deactivate slot=%u errno=%d\n", provisioned, restore);
		if (restore) error = restore;
	}
	return error;
}

/* Query the default clients exactly as found. No DMS mode change, UIM
 * provisioning, subscription binding, WDA change or WDS call is needed. */
static int
sdx55_observe_sim(struct umi_mhi *host)
{
	const uint16_t messages[] = {SDX55_UIM_GET_CARD_STATUS, SDX55_NAS_GET_SERVING_SYSTEM,
	    SDX55_NAS_GET_SYSTEM_INFO, SDX55_WDS_GET_STATUS};
	const unsigned services[] = {CELL_UIM, CELL_NAS, CELL_NAS, CELL_WDS};
	struct sdx55_cell_endpoint endpoints[4];
	for (unsigned i = 0; i < 4; i++) endpoints[i] = sdx55_services[services[i]];
	uint64_t deadline;
	/* An individual status query can take another 5 s if it was already sent. */
	clock_interval_to_deadline(40, NSEC_PER_SEC, &deadline);
	bool seen_uim = false, seen_card = false;
	while (!host->poisoned && mach_absolute_time() < deadline) {
		bool ready = false;
		for (unsigned i = 0; i < 4; i++) {
			if (host->poisoned) return EIO;
			if (mach_absolute_time() >= deadline) break;
			int error = sdx55_cell_command(host, &endpoints[i], messages[i], 0, NULL);
			if (error == ENXIO || host->poisoned) return error ? error : EIO;
			if (!error && services[i] == CELL_UIM) {
				const struct sdx55_cell_status *c = &sdx55_query.cell;
				seen_uim = true;
				seen_card |= c->present_cards != 0 || c->error_cards != 0;
				for (unsigned slot = 0; slot < c->cards && slot < 2; slot++)
					ready |= c->slots[slot].card_state == 1 && c->slots[slot].ready_gw != 0;
			}
		}
		if (ready) return 0;
		if (mach_absolute_time() >= deadline) break;
		int error = umi_sdx55_poll_loc(host, 2000);
		if (error) return error;
	}
	if (host->poisoned) return EIO;
	return seen_uim && !seen_card ? ENODEV : ETIMEDOUT;
}

static int
umi_sdx55_cell_session(struct umi_mhi *host)
{
	uint32_t rings = 0, call = 0, seconds = 120, sim_status = 0;
	PE_parse_boot_argn("sdx55_data_rings", &rings, sizeof(rings));
	PE_parse_boot_argn("sdx55_data_call", &call, sizeof(call));
	PE_parse_boot_argn("sdx55_data_secs", &seconds, sizeof(seconds));
	PE_parse_boot_argn("sdx55_sim_status", &sim_status, sizeof(sim_status));
	if (sim_status) {
		if (sim_status != 1 || rings || call) return EINVAL;
		return sdx55_observe_sim(host);
	}
	if (call && (call != 1 || rings != 1 || !seconds || seconds > 600)) return EINVAL;
	memset(&sdx55_packets, 0, sizeof(sdx55_packets));
	int restore_mode = -1;
	int first_error;
	uint64_t deadline;
	clock_interval_to_deadline(45, NSEC_PER_SEC, &deadline);
	do {
		first_error = umi_sdx55_dms_online(host, &restore_mode);
		if (first_error != EIO || restore_mode >= 0 || host->poisoned ||
		    !sdx55_radio_initializing(umi_sdx55_dms.service_error)) break;
		first_error = umi_sdx55_poll_loc(host, 500);
		if (first_error) break;
		first_error = EAGAIN;
	} while (mach_absolute_time() < deadline);
	hakios_tl("UMI SDX55: cellular online errno=%d restore_mode=%d\n", first_error, restore_mode);
	for (unsigned i = 0; !host->poisoned && i < sizeof(sdx55_services)/sizeof(sdx55_services[0]); i++) {
		int query_error = sdx55_read_cell_service(host, &sdx55_services[i]);
		/* The data path repeats UIM/NAS during its registration window. */
		if (call == 1 && (i == CELL_UIM || i == CELL_NAS) && query_error == EIO &&
		    sdx55_radio_initializing(sdx55_query.service_error)) continue;
		if (!first_error && query_error) first_error = query_error;
	}
	if (!host->poisoned && rings == 1) {
		int ring_error = umi_mhi_probe_cell_data(host);
		hakios_tl("UMI SDX55: cellular IP_HW0 probe errno=%d\n", ring_error);
		if (!first_error && ring_error) first_error = ring_error;
	}
	if (!first_error && sdx55_audio_pd.enabled && sdx55_voice_diag_enabled()) {
		int diag_error = sdx55_modem_diag_start(host);
		hakios_tl("UMI SDX55: voice modem DIAG start errno=%d DMA used=%#lx\n", diag_error,
		    (unsigned long)umi_sdx55_mhi_cursor);
	}
	if (!first_error && call == 1) {
		first_error = sdx55_run_data_call(host, seconds);
		sdx55_data_state(UMI_DATA_UNAVAILABLE);
	}
	int restore = umi_sdx55_restore_mode(host, restore_mode);
	return restore ? restore : first_error;
}

static int
umi_sdx55_run_mission_clients(struct umi_mhi *host, bool cellular, bool location)
{
	int cell_error = 0, loc_error = 0;
	if (cellular) {
		sdx55_status_session(true);
		cell_error = sdx55_cell_discovered() ? umi_sdx55_cell_session(host) : ETIMEDOUT;
		sdx55_status_session(false);
		hakios_tl("UMI SDX55: cellular session errno=%d\n", cell_error);
	}
	if (location && !host->poisoned) {
		loc_error = umi_sdx55_loc_port ? 0 : ETIMEDOUT;
		hakios_tl("UMI SDX55: LOC discovery errno=%d frames=%u\n", loc_error, umi_sdx55_qrtr_frames);
		if (!loc_error) {
			loc_error = umi_sdx55_loc_session(host);
			hakios_tl("UMI SDX55: LOC session errno=%d fix=%u SV_reports=%u max_SV=%u\n", loc_error, umi_sdx55_loc.fix,
			    umi_sdx55_loc.sv_reports, umi_sdx55_loc.max_sv);
			hakios_tl("UMI SDX55: LOC NMEA=%u GSV=%u max_in_view=%u max_BDS_in_view=%u\n",
			    umi_sdx55_loc.nmea_reports, umi_sdx55_loc.gsv_reports,
			    umi_sdx55_loc.max_gsv, umi_sdx55_loc.max_bds_gsv);
		}
	}
	return loc_error ? loc_error : cell_error;
}

static int
umi_sdx55_mission_session(struct umi_mhi *host, bool open_efs, bool cellular)
{
	if (cellular && !open_efs) return EINVAL;
	uint32_t fix_seconds;
	bool location = !cellular || PE_parse_boot_argn("sdx55_fix_secs", &fix_seconds, sizeof(fix_seconds));
	memset(&sdx55_query, 0, sizeof(sdx55_query));
	for (unsigned i = 0; i < sizeof(sdx55_services)/sizeof(sdx55_services[0]); i++)
		sdx55_services[i].port = sdx55_services[i].node = 0;
	umi_sdx55_pdm_count = 0;
	bzero(&sdx55_audio_pd, sizeof(sdx55_audio_pd));
	bzero(&sdx55_sat, sizeof(sdx55_sat));
	sdx55_audio_pd.enabled = cellular && sdx55_voice_boot_enabled();
	umi_sdx55_dms_node = umi_sdx55_dms_port = 0;
	umi_sdx55_peer = umi_sdx55_loc_port = umi_sdx55_loc_node = umi_sdx55_qrtr_frames = 0;
	struct umi_pcie *pcie = host->io->context;
	memset(&umi_sdx55_check, 0, sizeof(umi_sdx55_check));
	memset(&umi_sdx55_config_transfer, 0, sizeof(umi_sdx55_config_transfer));
	umi_sdx55_missing.port = umi_sdx55_credits = 0;
	memset(&umi_sdx55_temp, 0, sizeof(umi_sdx55_temp));
	for (unsigned i = 0; i < sizeof(umi_sdx55_configs)/sizeof(umi_sdx55_configs[0]); i++) {
		umi_sdx55_configs[i].bytes = hakios_image_file("sdx55m", umi_sdx55_configs[i].name,
		    &umi_sdx55_configs[i].size, NULL);
		if (!umi_sdx55_configs[i].bytes) return ENOENT;
	}
	/* Stock mdm_helper waits for this ESOC run-state signal after ks exits. */
	unsigned elapsed = 0;
	while (elapsed < 10000 && !gpio_level(pcie, 3) && !gpio_level(pcie, 1)) {
		host->io->delay_us(pcie, 1000);
		elapsed++;
	}
	uint32_t running = gpio_level(pcie, 3), fatal = gpio_level(pcie, 1);
	hakios_tl("UMI SDX55: mission MDM2AP_STATUS=%u ERRFATAL=%u after %u ms\n",
	    running, fatal, elapsed);
	if (fatal || !running) return fatal ? EIO : ETIMEDOUT;
	/* Qualcomm prevents L1 around mission MMIO; keep the bounded probe
	 * in L0 and restore both root-port controls before releasing it. */
	uint32_t link_ctl = rd(pcie, DBI + 0x80), pm_ctl = rd(pcie, PARF + 0x20);
	if (link_ctl == UINT32_MAX || pm_ctl == UINT32_MAX) return EIO;
	report(pcie, "mission LTSSM before L0 vote", rd(pcie, PARF + 0x1b0));
	wr(pcie, DBI + 0x80, (link_ctl & 0xffff) & ~BIT(1));
	wr(pcie, PARF + 0x20, pm_ctl | BIT(5));
	int error = wait_bits(pcie, PARF + 0x1b0, 0x3f, 0x11, 100000);
	if (!error) error = umi_mhi_start_ipcr(host);
	hakios_tl("UMI SDX55: mission IPCR start errno=%d\n", error);
	uint8_t *shadow = NULL;
	if (!error && open_efs) {
		shadow = kalloc_data(UMI_EFS_SHADOW_BYTES, Z_WAITOK | Z_ZERO);
		error = shadow ? umi_mhi_probe_efs(host, shadow) : ENOMEM;
		hakios_tl("UMI SDX55: mission EFS start errno=%d\n", error);
	}
	if (!error) error = umi_mhi_send_qrtr_hello(host);
	/* The downstream router rewrites a broadcast to the link peer on the wire. */
	for (unsigned ms = 0; !error && !umi_sdx55_peer && ms < 1000; ms++) {
		unsigned received;
		error = umi_mhi_poll_ipcr(host, 1, &received);
	}
	if (!error && !umi_sdx55_peer) error = ETIMEDOUT;
	if (!error) {
		uint32_t announce[] = {1, 4, 1, 0xfffffffe, 0, 20, umi_sdx55_peer,
		    0xfffffffe, 4, 4096, 3, 1, SDX55_TFTP_SERVICE_PORT};
		error = umi_mhi_send_qrtr_packet(host, announce, sizeof(announce), 1000, UINT16_MAX);
	}
	if (!error) {
		uint32_t announce[] = {1, 4, 1, 0xfffffffe, 0, 20, umi_sdx55_peer,
		    0xfffffffe, 4, 64, 0x101, 1, SDX55_PDM_PORT};
		error = umi_mhi_send_qrtr_packet(host, announce, sizeof(announce), 1000, UINT16_MAX);
	}
	if (!error && cellular) sdx55_ims_dcm_announce(host);
	for (unsigned i = 0; !error && i < 12000; i++) {
		if ((!cellular || sdx55_cell_discovered()) && (!location || umi_sdx55_loc_port != 0)) break;
		/* A HELLO asks the peer name server to send its current services.
		 * Some boots only deliver the early PDC advertisement burst. */
		if (i == 2000 || i == 6000) {
			uint32_t hello[] = {1, 2, 1, 0xfffffffe, 0, 20,
			    umi_sdx55_peer, 0xfffffffe, 2, 0, 0, 0, 0};
			error = umi_mhi_send_qrtr_packet(host, hello, sizeof(hello), 1000, UINT16_MAX);
			hakios_tl("UMI SDX55: QRTR refresh peer=%u errno=%d\n", umi_sdx55_peer, error);
		}
		if (!error) error = umi_sdx55_poll_loc(host, 10);
	}
	if (!error) error = umi_sdx55_run_mission_clients(host, cellular, location);
	if (shadow) {
		host->efs_shadow = NULL;
		kfree_data(shadow, UMI_EFS_SHADOW_BYTES);
	}
	wr(pcie, PARF + 0x20, pm_ctl);
	wr(pcie, DBI + 0x80, link_ctl & 0xffff);
	return error;
}

static int
umi_sdx55_prepare_mhi(struct umi_pcie *p, bool probe_sahara, bool handshake,
    bool serve_ddr, bool next_image, unsigned step)
{
	memset(&umi_sdx55_mhi, 0, sizeof(umi_sdx55_mhi));
	memset(&umi_sdx55_mhi_io, 0, sizeof(umi_sdx55_mhi_io));
	umi_sdx55_mhi_cursor = round_page(umi_sdx55_sbl1_size);
	if (step >= 12) umi_sdx55_share_mhi_pages(umi_sdx55_mhi_cursor);
	umi_sdx55_mhi_io.context = p;
	umi_sdx55_mhi_io.bar_size = 0x1000;
	umi_sdx55_mhi_io.dma_start = SDX55_IOVA;
	umi_sdx55_mhi_io.dma_end = SDX55_IOVA + SDX55_DMA_SIZE;
	if (sdx55_voice_boot_enabled()) umi_sdx55_mhi_io.dma_end = SDX55_SAT_IOVA + SDX55_SAT_BYTES;
	umi_sdx55_mhi_io.profile = UMI_MHI_PROFILE_SDX55;
	umi_sdx55_mhi_io.read32 = umi_sdx55_mhi_read;
	umi_sdx55_mhi_io.write32 = umi_sdx55_mhi_write;
	umi_sdx55_mhi_io.allocate = umi_sdx55_mhi_allocate;
	umi_sdx55_mhi_io.sync_for_device = umi_sdx55_mhi_sync;
	umi_sdx55_mhi_io.sync_for_cpu = umi_sdx55_mhi_sync_for_cpu;
	umi_sdx55_mhi_io.delay_us = umi_sdx55_mhi_delay;
	umi_sdx55_mhi_io.report = umi_sdx55_mhi_report;
	umi_sdx55_mhi_io.qrtr_packet = umi_sdx55_qrtr_packet;
	umi_sdx55_mhi_io.cell_rx = sdx55_receive_packets;
	umi_sdx55_mhi_io.diag_rx = sdx55_modem_diag_receive;
	report(p, "SBL MHI length", rd(p, BAR));
	report(p, "SBL MHI version", rd(p, BAR + 8));
	report(p, "SBL MHI channel doorbells", rd(p, BAR + 0x18));
	report(p, "SBL MHI event doorbells", rd(p, BAR + 0x20));
	report(p, "SBL MHI BHI offset", rd(p, BAR + 0x28));
	report(p, "SBL MHI BHIE offset", rd(p, BAR + 0x2c));
	int error = umi_mhi_probe(&umi_sdx55_mhi, &umi_sdx55_mhi_io);
	if (!error) {
		/* The bounded BHI path already transferred this exact embedded SBL1. */
		umi_sdx55_mhi.sbl_sent = true;
		error = umi_mhi_prepare_ready(&umi_sdx55_mhi);
	}
	if (!error && probe_sahara) {
		error = umi_mhi_probe_sahara(&umi_sdx55_mhi, 5000);
		hakios_tl("UMI SDX55: bounded Sahara first-request probe errno=%d\n",
		    error);
	}
	if (!error && handshake) {
		error = umi_mhi_sahara_handshake(&umi_sdx55_mhi, 5000);
		hakios_tl("UMI SDX55: bounded Sahara handshake errno=%d\n", error);
	}
	if (!error && serve_ddr) {
		error = umi_mhi_sahara_serve_image(&umi_sdx55_mhi, 34,
		    umi_sdx55_ddr, sizeof(umi_sdx55_ddr), 5000);
		hakios_tl("UMI SDX55: bounded Sahara DDR image errno=%d\n", error);
	}
	if (!error && next_image) {
		error = umi_mhi_sahara_next_image(&umi_sdx55_mhi, 5000);
		hakios_tl("UMI SDX55: next Sahara image request errno=%d\n", error);
	}
	if (!error && step >= 10) {
		error = umi_mhi_sahara_serve_image(&umi_sdx55_mhi, 40,
		    umi_sdx55_msadp, sizeof(umi_sdx55_msadp), 5000);
		hakios_tl("UMI SDX55: bounded Sahara APDP image errno=%d\n", error);
		if (!error) error = umi_mhi_sahara_next_image(&umi_sdx55_mhi, 5000);
		hakios_tl("UMI SDX55: post-APDP Sahara request errno=%d\n", error);
	}
	if (!error && step >= 11) error = umi_sdx55_serve_boot_images(&umi_sdx55_mhi);
	if (!error && step >= 12) {
		uint32_t observe_efs = 0;
		PE_parse_boot_argn("sdx55_efs", &observe_efs, sizeof(observe_efs));
		uint32_t cellular = 0;
		PE_parse_boot_argn("sdx55_cell", &cellular, sizeof(cellular));
		error = umi_sdx55_mission_session(&umi_sdx55_mhi, observe_efs != 0, cellular == 1);
	}
	if (error && umi_sdx55_dma.cb)
		report(p, "MHI failure SMMU FSR",
		    umi_sdx55_direct_read(umi_sdx55_dma.cb, 0x58));
	hakios_tl("UMI SDX55: MHI READY/M0 proof errno=%d, DMA used=%lu/%u\n",
	    error, (unsigned long)umi_sdx55_mhi_cursor, SDX55_DMA_SIZE);
	return error;
}

static struct {
	struct umi_pcie *host;
	bool return_to_loader, pending, worker_active, final_observation;
	bool prepare_mhi, probe_sahara, handshake, serve_ddr, next_image;
	unsigned step;
	unsigned observations;
} umi_sdx55_sbl_work;

static void
umi_sdx55_sbl_worker(void *parameter, __unused wait_result_t result)
{
	struct umi_pcie *p = parameter;
	int error = 0;
	bool retain = false;
	unsigned observation = ++umi_sdx55_sbl_work.observations;

	hakios_tl("UMI SDX55: SBL observation %u userland-triggered worker entered\n",
	    observation);
	uint32_t link = rd(p, DBI + 0x80);
	report(p, "SBL observation local link", link);
	if (link == UINT32_MAX || !(link & BIT(29))) {
		error = ETIMEDOUT;
	} else {
		uint32_t endpoint = rd(p, CONF);
		report(p, "SBL observation endpoint ID", endpoint);
		if (endpoint != 0x030617cb) {
			error = ENODEV;
		} else {
			/* Revalidate and idempotently reapply BAR0 decoding after the BHI
			 * reset boundary, without resetting the modem or PHY again.  Current
			 * hardware runs retained the assignment, but SBL may invalidate it. */
			uint32_t command = rd(p, CONF + 4);
			uint32_t bar = rd(p, CONF + 0x10);
			report(p, "SBL observation endpoint command before BAR restore",
			    command);
			report(p, "SBL observation endpoint BAR0 before restore", bar);
			if (command == UINT32_MAX || bar == UINT32_MAX || (bar & 1)) {
				error = EIO;
			} else {
				wr(p, CONF + 4, ((command & 0xffff) & ~7u) | BIT(10));
				wr(p, CONF + 0x10, BAR | (bar & 15));
				wr(p, CONF + 0x14, 0);
				error = outbound(p, 1, 0, BAR, BAR + 0xfff, BAR);
				if (!error) {
					wr(p, DBI + 4, 6);
					wr(p, CONF + 4,
					    ((command & 0xffff) & ~7u) | 6 | BIT(10));
					report(p, "SBL observation endpoint BAR0 restored",
					    rd(p, CONF + 0x10));
					uint32_t offset = rd(p, BAR + 0x28);
					report(p, "SBL observation BHI offset", offset);
					if ((offset & 3) || offset < 0x100 ||
					    offset > 0xfc0) {
						error = EIO;
					} else {
						umi_sdx55_bhi_address = BAR + offset;
					}
				}
			}
			uint32_t ee = error ? UINT32_MAX :
			    rd(p, umi_sdx55_bhi_address + 0x28);
			report(p, "SBL observation execution environment", ee);
			if (!error && ee != 1) {
				uint32_t status = rd(p, umi_sdx55_bhi_address + 0x2c);
				uint32_t bhi_error = rd(p, umi_sdx55_bhi_address + 0x30);
				report(p, "SBL observation BHI status", status);
				report(p, "SBL observation BHI error code", bhi_error);
				for (unsigned i = 0; i < 3; i++)
					report(p, "SBL observation BHI error debug",
					    rd(p, umi_sdx55_bhi_address + 0x34 + 4 * i));
				retain = ee == 0 &&
				    (status & (3u << 30)) == (2u << 30) &&
				    bhi_error == 0 &&
				    !umi_sdx55_sbl_work.final_observation;
				if (!retain) error = ETIMEDOUT;
			}
			if (!error && ee == 1 && umi_sdx55_sbl_work.prepare_mhi)
				error = umi_sdx55_prepare_mhi(p,
				    umi_sdx55_sbl_work.probe_sahara,
				    umi_sdx55_sbl_work.handshake, umi_sdx55_sbl_work.serve_ddr,
				    umi_sdx55_sbl_work.next_image, umi_sdx55_sbl_work.step);
		}
	}
	if (retain) {
		umi_sdx55_sbl_work.worker_active = false;
		hakios_tl("UMI SDX55: SBL still PBL after observation %u; protected ownership retained\n",
		    observation);
		thread_terminate(current_thread());
		return;
	}

	umi_pcie2_quiesce(p);
	lck_mtx_lock(&hakios_smmu_lock);
	umi_sdx55_smmu_restore();
	lck_mtx_unlock(&hakios_smmu_lock);
	umi_sdx55_sbl_work.pending = false;
	umi_sdx55_sbl_work.worker_active = false;
	hakios_tl("UMI SDX55: asynchronous SBL observation errno=%d\n", error);
	if (umi_sdx55_sbl_work.return_to_loader) loader_return();
	thread_terminate(current_thread());
}

static int
umi_sdx55_observe_sysctl SYSCTL_HANDLER_ARGS
{
	int value = umi_sdx55_sbl_work.pending ? 1 : 0;
	int error = SYSCTL_OUT(req, &value, sizeof(value));
	if (error || !req->newptr) return error;
	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error) return error;
	if (value != 1 && value != 2) return EINVAL;
	if (!umi_sdx55_sbl_work.pending || umi_sdx55_sbl_work.worker_active ||
	    !umi_sdx55_sbl_work.host)
		return ENXIO;
	umi_sdx55_sbl_work.final_observation = value == 2;
	umi_sdx55_sbl_work.worker_active = true;
	thread_t thread;
	if (kernel_thread_start(umi_sdx55_sbl_worker,
	    umi_sdx55_sbl_work.host, &thread) != KERN_SUCCESS) {
		umi_sdx55_sbl_work.worker_active = false;
		umi_sdx55_sbl_work.pending = false;
		umi_sdx55_bhi_release(umi_sdx55_sbl_work.host);
		return EAGAIN;
	}
	thread_deallocate(thread);
	hakios_tl("UMI SDX55: SBL observation worker triggered by userland\n");
	return 0;
}

SYSCTL_DECL(_kern_hakios);
static int
sdx55_cell_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	struct umi_sdx55_cell_snapshot state;
	lck_mtx_lock(&cell_status_lock);
	state = cell_view;
	lck_mtx_unlock(&cell_status_lock);
	return SYSCTL_OUT(req, &state, sizeof(state));
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_cell_status,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sdx55_cell_sysctl, "S,umi_sdx55_cell_snapshot", "Read-only QMI cellular observations");

static int
sdx55_radio_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	lck_mtx_lock(&cell_status_lock);
	struct umi_sdx55_radio_snapshot state = sdx55_radio;
	lck_mtx_unlock(&cell_status_lock);
	return SYSCTL_OUT(req, &state, sizeof(state));
}

static int
sdx55_data_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = sdx55_data_enabled();
	int error = SYSCTL_OUT(req, &value, sizeof(value));
	if (error || !req->newptr) return error;
	if (kauth_cred_getuid(kauth_cred_get()) != 0) {
		const char *identity = cs_identity_get(req->p);
		if (!identity || (strcmp(identity, "com.apple.Preferences") && strcmp(identity, "com.apple.springboard")) ||
		    !csproc_get_platform_binary(req->p))
			return EPERM;
	}
	if (req->newlen != sizeof(value)) return EINVAL;
	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error || (value != 0 && value != 1)) return error ? error : EINVAL;
	lck_mtx_lock(&cell_status_lock);
	if (sdx55_radio.state == UMI_DATA_UNAVAILABLE) error = ENXIO;
	else sdx55_radio.enabled = value;
	lck_mtx_unlock(&cell_status_lock);
	if (!error) hakios_tl("UMI SDX55: mobile data requested=%d\n", value);
	return error;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_radio_status,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sdx55_radio_sysctl, "S,umi_sdx55_radio_snapshot", "Serving radio and mobile data state");

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_mobile_data,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0,
    sdx55_data_sysctl, "I", "Request an SDX55 packet-data connection");

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_voice,
    CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0,
    sdx55_voice_sysctl, "S,umi_voice_snapshot", "SDX55 outbound voice test");

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_voice_route,
    CTLTYPE_STRUCT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0,
    sdx55_voice_route_sysctl, "S,umi_voice_route_status", "SDX55 in-call audio output");

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_voice_mute,
    CTLTYPE_STRUCT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0,
    sdx55_voice_mute_sysctl, "S,umi_voice_mute_status", "SDX55 in-call microphone mute");

SYSCTL_PROC(_kern_hakios, OID_AUTO, sdx55_observe,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    umi_sdx55_observe_sysctl, "I",
    "trigger the armed SDX55 SBL observation after userland delay");

static int
umi_sdx55_arm_sbl_observation(struct umi_pcie *p, bool return_to_loader,
    bool prepare_mhi, bool probe_sahara, bool handshake, bool serve_ddr,
    bool next_image, unsigned step)
{
	if (umi_sdx55_sbl_work.pending || umi_sdx55_sbl_work.worker_active)
		return EBUSY;
	int error = umi_sdx55_bhi_boot_inner(p, prepare_mhi);
	if (error) {
		hakios_tl("UMI SDX55: asynchronous SBL observation errno=%d\n", error);
		return error;
	}
	umi_sdx55_sbl_work.host = p;
	umi_sdx55_sbl_work.return_to_loader = return_to_loader;
	umi_sdx55_sbl_work.pending = true;
	umi_sdx55_sbl_work.final_observation = false;
	umi_sdx55_sbl_work.prepare_mhi = prepare_mhi;
	umi_sdx55_sbl_work.probe_sahara = probe_sahara;
	umi_sdx55_sbl_work.handshake = handshake;
	umi_sdx55_sbl_work.serve_ddr = serve_ddr;
	umi_sdx55_sbl_work.next_image = next_image;
	umi_sdx55_sbl_work.step = step;
	umi_sdx55_sbl_work.observations = 0;
	hakios_tl("UMI SDX55: SBL observation armed for userland trigger\n");
	return 0;
}

static void
umi_sdx55_smmu_survey(void)
{
	vm_offset_t smmu = ml_io_map(0x15000000, 0x1000);
	if (!smmu) {
		hakios_tl("UMI SDX55: apps-SMMU survey mapping failed\n");
		return;
	}
	uint32_t scr0 = umi_sdx55_direct_read(smmu, 0);
	uint32_t idr0 = umi_sdx55_direct_read(smmu, 0x20);
	uint32_t idr1 = umi_sdx55_direct_read(smmu, 0x24);
	unsigned matches = idr0 & 0xff, free_matches = 0, covering = 0;
	hakios_tl("UMI SDX55: apps-SMMU IDR0=%#x IDR1=%#x SCR0=%#x\n",
	    idr0, idr1, scr0);
	if (matches > 128) matches = 128;
	for (unsigned i = 0; i < matches; i++) {
		uint32_t smr = umi_sdx55_direct_read(smmu, 0x800 + 4 * i);
		uint32_t s2cr = umi_sdx55_direct_read(smmu, 0xc00 + 4 * i);
		if (!(smr & BIT(31))) {
			free_matches++;
			continue;
		}
		uint32_t sid = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (!((sid ^ 0x1d01) & ~mask)) {
			covering++;
			hakios_tl("UMI SDX55: apps-SMMU SMR%u covers SID 0x1d01: "
			    "SMR=%#x S2CR=%#x\n", i, smr, s2cr);
		}
	}
	hakios_tl("UMI SDX55: apps-SMMU free matches=%u contexts=%u "
	    "existing SID routes=%u\n", free_matches, idr1 & 0xff, covering);
}

/* sdx55_step=1 sdx55_return=1: power and enumerate PCIe2, report, return. */
void
umi_sdx55_init(void)
{
	uint32_t step = 0, back = 0;
	PE_parse_boot_argn("sdx55_step", &step, sizeof(step));
	PE_parse_boot_argn("sdx55_return", &back, sizeof(back));
	hakios_tl("UMI SDX55: probe entered, step=%u back=%u\n", step, back);
	if (step < 1) return;

	printf("UMI SDX55: io_submap free = %llu bytes\n", umi_camera_iomap_free());
	for (unsigned i = 0; i < sizeof(umi_sdx55_regions) / sizeof(umi_sdx55_regions[0]); i++) {
		if (umi_sdx55_regions[i].address == BAR) continue;
		hakios_tl("UMI SDX55: mapping region[%u] %#x (%u bytes)\n", i, umi_sdx55_regions[i].address, umi_sdx55_regions[i].size);
		umi_sdx55_regions[i].mapping = ml_io_map(umi_sdx55_regions[i].address, umi_sdx55_regions[i].size);
		if (!umi_sdx55_regions[i].mapping) {
			hakios_tl("UMI SDX55: cannot map region[%u] %#x\n", i, umi_sdx55_regions[i].address);
			if (back) loader_return();
			return;
		}
	}
	hakios_tl("UMI SDX55: all regions mapped\n");
	printf("UMI SDX55: regions[0].mapping = %p\n", (void *)(uintptr_t)umi_sdx55_regions[0].mapping);
	printf("UMI SDX55: direct deref regions[0].mapping = %#x\n",
	    umi_sdx55_regions[0].mapping ? *(volatile uint32_t *)umi_sdx55_regions[0].mapping : 0);
	hakios_tl("UMI SDX55: calling enumerate\n");
	int error = umi_pcie2_enumerate(&umi_sdx55_host, &umi_sdx55_io);
	hakios_tl("UMI SDX55: enumerate returned errno=%d\n", error);
	if (!error) {
		unsigned bar_index = (unsigned)(sizeof(umi_sdx55_regions) /
		    sizeof(umi_sdx55_regions[0]) - 1);
		umi_sdx55_regions[bar_index].mapping = ml_io_map(BAR, 0x1000);
		if (!umi_sdx55_regions[bar_index].mapping) {
			hakios_tl("UMI SDX55: BAR mapping failed\n");
			error = ENOMEM;
		} else {
			uint32_t command = rd(&umi_sdx55_host, CONF + 4);
			uint32_t bar = rd(&umi_sdx55_host, CONF + 0x10);
			/* Assign the same 4 KiB window Android uses.  Memory decode is
			 * permitted for AP reads, while endpoint bus mastering stays off. */
			wr(&umi_sdx55_host, CONF + 4, ((command & 0xffff) & ~7u) | BIT(10));
			wr(&umi_sdx55_host, CONF + 0x10, BAR | (bar & 15));
			wr(&umi_sdx55_host, CONF + 0x14, 0); /* BAR0 is a 64-bit memory BAR. */
			error = outbound(&umi_sdx55_host, 1, 0, BAR, BAR + 0xfff, BAR);
			if (!error) {
				wr(&umi_sdx55_host, DBI + 4, 2);
				wr(&umi_sdx55_host, CONF + 4,
				    ((command & 0xffff) & ~7u) | 2 | BIT(10));
				report(&umi_sdx55_host, "MHI endpoint command (DMA disabled)",
				    rd(&umi_sdx55_host, CONF + 4));
				report(&umi_sdx55_host, "MHI endpoint BAR0", rd(&umi_sdx55_host, CONF + 0x10));
				report(&umi_sdx55_host, "MHI register length", rd(&umi_sdx55_host, BAR));
				report(&umi_sdx55_host, "MHI version (DMA disabled)", rd(&umi_sdx55_host, BAR + 8));
				report(&umi_sdx55_host, "MHI configuration", rd(&umi_sdx55_host, BAR + 0x10));
				report(&umi_sdx55_host, "MHI control", rd(&umi_sdx55_host, BAR + 0x38));
				report(&umi_sdx55_host, "BHI offset", rd(&umi_sdx55_host, BAR + 0x28));
				report(&umi_sdx55_host, "BHIE offset", rd(&umi_sdx55_host, BAR + 0x2c));
				report(&umi_sdx55_host, "BHI major version", rd(&umi_sdx55_host, BAR + 0x104));
				report(&umi_sdx55_host, "BHI execution environment", rd(&umi_sdx55_host, BAR + 0x128));
			}
			hakios_tl("UMI SDX55: MHI read-only probe errno=%d\n", error);
		}
	}
	umi_sdx55_smmu_survey();
	if (!error && step == 2) error = umi_sdx55_smmu_proof();
	if (!error && step == 3) error = umi_sdx55_bhi_boot(&umi_sdx55_host);
	if (!error && (step == 4 || step == 5 || step == 6 || (step >= 7 && step <= 12))) {
		error = umi_sdx55_arm_sbl_observation(&umi_sdx55_host, back != 0,
		    step >= 5, step >= 6, step >= 7, step >= 8, step >= 9, step);
		if (!error) return;
	}
	umi_pcie2_quiesce(&umi_sdx55_host);
	if (back) loader_return();
}
