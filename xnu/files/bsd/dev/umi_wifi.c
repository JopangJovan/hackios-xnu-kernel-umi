/* Xiaomi Mi 10 QCA6390 / PCIe0 bring-up. Addresses are from UMI's DTB.
 * umi_wifi_step=1 is a read-only survey of always-accessible GCC and TLMM.
 * It deliberately does not read the powered-down PCIe or radio aperture.
 * umi_wifi_step=2 explicitly powers and enumerates PCIe0, without enabling DMA.
 */
#include <kern/clock.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/umi_pcie.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hk_tl(const char *, ...) __printflike(1, 2);
extern void loader_return(void);
extern int umi_rpmh_wifi_power(void);
bool umi_wifi_init(void);

static uint32_t
umi_wifi_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value;
	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (base + offset) : "memory");
	return value;
}

static bool
umi_wifi_is_umi(void)
{
	DTEntry chosen;
	const void *board;
	unsigned size;

	return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "hackios-board", &board, &size) == kSuccess &&
	    size == 4 && memcmp(board, "umi", 4) == 0;
}

bool
umi_wifi_is_board(void)
{
	return umi_wifi_is_umi();
}

static bool
umi_wifi_survey(void)
{
	static const struct { const char *name; uint32_t offset; } clocks[] = {
		{ "pcie0_reset", 0x6b000 }, { "pcie0_gdsc", 0x6b004 },
		{ "pcie0_clock_votes", 0x52008 }, { "pcie0_slv_q2a", 0x6b010 },
		{ "pcie0_slv_axi", 0x6b014 }, { "pcie0_mstr_axi", 0x6b01c },
		{ "pcie0_cfg_ahb", 0x6b024 }, { "pcie0_aux", 0x6b028 },
		{ "pcie0_pipe", 0x6b02c }, { "pcie0_aux_cmd", 0x6b038 },
		{ "pcie0_aux_cfg", 0x6b03c }, { "pcie0_phy_reset", 0x6c01c },
		{ "pcie0_phy_refgen", 0x6f02c },
	};
	static const struct { const char *name; uint32_t pin; } pins[] = {
		{ "wlan_enable", 20 }, { "bluetooth_enable", 21 },
		{ "pcie0_perst", 79 }, { "pcie0_clkreq", 80 },
		{ "pcie0_wake", 81 }, { "wlan_sw_ctrl", 124 },
	};
	vm_offset_t gcc = ml_io_map(0x00100000, 0x80000);
	vm_offset_t gpio = ml_io_map(0x0f900000, 0x80000);
	if (!gcc || !gpio) {
		hk_tl("UMI Wi-Fi: GCC/TLMM mapping failed\n");
		return false;
	}
	hk_tl("UMI Wi-Fi: QCA6390 on PCIe0; read-only GCC/TLMM survey\n");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		hk_tl("UMI Wi-Fi: GCC %s (%#x) = %#x\n", clocks[i].name,
		    0x00100000 + clocks[i].offset, umi_wifi_read(gcc, clocks[i].offset));
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		hk_tl("UMI Wi-Fi: GPIO%u %s cfg=%#x io=%#x\n", pins[i].pin, pins[i].name,
		    umi_wifi_read(gpio, pins[i].pin * 0x1000),
		    umi_wifi_read(gpio, pins[i].pin * 0x1000 + 4));
	}
	return true;
}

static struct {
	uint32_t address, size;
	vm_offset_t mapping;
} umi_pcie_regions[] = {
	{ 0x00100000, 0xa0000, 0 }, /* GCC, including the PCIe TBU clock. */
	{ 0x0f900000, 0x80000, 0 }, /* Kona NORTH GPIO tile. */
	{ 0x01c00000, 0x3000, 0 },  /* PARF */
	{ 0x01c06000, 0x1000, 0 },  /* PHY */
	{ 0x60000000, 0x1000, 0 },  /* DBI + ELBI */
	{ 0x60001000, 0x1000, 0 },  /* unrolled outbound iATU */
	{ 0x60100000, 0x1000, 0 },  /* endpoint configuration */
	{ 0x60300000, 0x1000, 0 },  /* only QCA6390's direct MHI aperture */
};

static vm_offset_t
umi_pcie_register(uint32_t address)
{
	if (address & 3) return 0;
	for (unsigned i = 0; i < sizeof(umi_pcie_regions) / sizeof(umi_pcie_regions[0]); i++) {
		uint32_t offset = address - umi_pcie_regions[i].address;
		if (offset <= umi_pcie_regions[i].size - 4)
			return umi_pcie_regions[i].mapping + offset;
	}
	hk_tl("UMI PCIe: unmapped register %#x\n", address);
	return 0;
}

static uint32_t
umi_pcie_read(void *context, uint32_t address)
{
	(void)context;
	vm_offset_t reg = umi_pcie_register(address);
	if (!reg) return UINT32_MAX;
	uint32_t value = *(volatile uint32_t *)reg;
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
umi_pcie_write(void *context, uint32_t address, uint32_t value)
{
	(void)context;
	vm_offset_t reg = umi_pcie_register(address);
	if (!reg) return;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)reg = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static int
umi_pcie_power(void *context)
{
	(void)context;
	return umi_rpmh_wifi_power();
}

static void
umi_pcie_delay(void *context, unsigned us)
{
	(void)context;
	delay(us);
}

static void
umi_pcie_report(void *context, const char *stage, uint32_t value)
{
	(void)context;
	hk_tl("UMI PCIe: %s = %#x\n", stage, value);
}

static int
umi_wifi_pcie_diagnostic(void)
{
	for (unsigned i = 0; i < sizeof(umi_pcie_regions) / sizeof(umi_pcie_regions[0]); i++) {
		umi_pcie_regions[i].mapping = ml_io_map(umi_pcie_regions[i].address, umi_pcie_regions[i].size);
		if (!umi_pcie_regions[i].mapping) return ENOMEM;
	}
	const struct umi_pcie_io io = {
		.read32 = umi_pcie_read, .write32 = umi_pcie_write,
		.power_on = umi_pcie_power, .delay_us = umi_pcie_delay, .report = umi_pcie_report,
	};
	struct umi_pcie host = {0};
	hk_tl("UMI Wi-Fi: PCIe enumeration diagnostic; DMA stays disabled\n");
	int error = umi_pcie_enumerate(&host, &io);
	if (!error) {
		umi_pcie_report(NULL, "MHICFG", umi_pcie_read(NULL, 0x60300010));
		umi_pcie_report(NULL, "BHI offset", umi_pcie_read(NULL, 0x60300028));
		umi_pcie_report(NULL, "BHIE offset", umi_pcie_read(NULL, 0x6030002c));
	}
	umi_pcie_quiesce(&host);
	return error;
}

/* Return true for UMI even when disabled: never enter HK's WPSS path. */
bool
umi_wifi_init(void)
{
	uint32_t step = 0, back = 0, enabled = 0;
	if (!umi_wifi_is_umi()) {
		return false;
	}
	PE_parse_boot_argn("umi_wifi_step", &step, sizeof(step));
	PE_parse_boot_argn("umi_wifi_return", &back, sizeof(back));
	if (step == 1 && back == 1) {
		hk_tl("UMI Wi-Fi: survey %s; returning to loader\n", umi_wifi_survey() ? "passed" : "failed");
		loader_return();
	} else if (step == 2 && back == 1) {
		int error = umi_wifi_pcie_diagnostic();
		hk_tl("UMI Wi-Fi: PCIe diagnostic errno=%d; endpoint reset; returning to loader\n", error);
		loader_return();
	} else if (step || (PE_parse_boot_argn("wifi", &enabled, sizeof(enabled)) && enabled)) {
		hk_tl("UMI Wi-Fi: QCA6390 requires the UMI PCIe/MHI driver; diagnostic: umi_wifi_step=1 umi_wifi_return=1\n");
	}
	return true;
}
