/* Xiaomi Mi 10 QCA6390 / PCIe0 bring-up. Addresses are from UMI's DTB.
 * umi_wifi_step=1 is a read-only survey of always-accessible GCC and TLMM.
 * It deliberately does not read the powered-down PCIe or radio aperture.
 */
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void redwood_tl(const char *, ...) __printflike(1, 2);
extern void loader_return(void);
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
	vm_offset_t gpio = ml_io_map(0x0f500000, 0x80000);
	if (!gcc || !gpio) {
		redwood_tl("UMI Wi-Fi: GCC/TLMM mapping failed\n");
		return false;
	}
	redwood_tl("UMI Wi-Fi: QCA6390 on PCIe0; read-only GCC/TLMM survey\n");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		redwood_tl("UMI Wi-Fi: GCC %s (%#x) = %#x\n", clocks[i].name,
		    0x00100000 + clocks[i].offset, umi_wifi_read(gcc, clocks[i].offset));
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		redwood_tl("UMI Wi-Fi: GPIO%u %s cfg=%#x io=%#x\n", pins[i].pin, pins[i].name,
		    umi_wifi_read(gpio, pins[i].pin * 0x1000),
		    umi_wifi_read(gpio, pins[i].pin * 0x1000 + 4));
	}
	return true;
}

/* Return true for UMI even when disabled: never enter Redwood's WPSS path. */
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
		redwood_tl("UMI Wi-Fi: survey %s; returning to loader\n", umi_wifi_survey() ? "passed" : "failed");
		loader_return();
	} else if (step || (PE_parse_boot_argn("wifi", &enabled, sizeof(enabled)) && enabled)) {
		redwood_tl("UMI Wi-Fi: QCA6390 requires the UMI PCIe/MHI driver; diagnostic: umi_wifi_step=1 umi_wifi_return=1\n");
	}
	return true;
}
