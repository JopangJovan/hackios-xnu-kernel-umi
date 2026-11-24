/* RAM-only UMI menu. Included after the USB and kernel handoff helpers. */

#define UMI_MENU_TIMEOUT_SECONDS 90
#define UMI_MENU_RUN_SECONDS    240
#define UMI_MENU_RESET_SECONDS  8
#define UMI_PMIC_CORE          0x0c440000u
#define UMI_PMIC_CONFIG        0x0c40a000u
#define UMI_PMIC_OBSERVER      0x0e600000u
#define UMI_PMIC_PON           0x0008u
#define UMI_PON_POWER          1u
#define UMI_PON_VOLUME_DOWN    2u

static struct {
	uint64_t deadline;
	unsigned apid;
	unsigned previous, stable, samples, armed;
	int ios;
} umi_menu;

static uint32_t
umi_menu_read32(uintptr_t addr)
{
	uint32_t value = *(volatile uint32_t *)addr;
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

/* The same PM8150 PON peripheral and read-only observer command as umi_keys.c. */
static unsigned
umi_menu_pon_apid(void)
{
	uint32_t version = umi_menu_read32(UMI_PMIC_CORE);
	uint32_t count = umi_menu_read32(UMI_PMIC_CORE + 4) & 0x7ffu;
	unsigned found = 512;

	if (version < 0x50000000u || version >= 0x70000000u || count == 0 || count > 512) {
		return found;
	}
	for (unsigned channel = 0; channel < count; channel++) {
		uint32_t map = umi_menu_read32(UMI_PMIC_CORE + 0x900u + 4u * channel);
		if (((map >> 8) & 0xfffu) == UMI_PMIC_PON) {
			found = channel;
			if ((umi_menu_read32(UMI_PMIC_CONFIG + 0x700u + 4u * channel) & 7u) == 0) {
				break;
			}
		}
	}
	return found;
}

static int
umi_menu_buttons(unsigned *buttons)
{
	if (umi_menu.apid == 512) {
		return 0;
	}
	uintptr_t base = UMI_PMIC_OBSERVER + 0x80u * umi_menu.apid;
	/* Extended read of PON's real-time status at offset 0x10. */
	*(volatile uint32_t *)base = (1u << 27) | (0x10u << 4);
	__asm__ volatile("dsb osh" ::: "memory");
	uint64_t start = READ_SYSREG(CNTVCT_EL0);
	uint64_t budget = READ_SYSREG(CNTFRQ_EL0) / 500;
	do {
		uint32_t status = umi_menu_read32(base + 8);
		if (status & 1u) {
			if (status & 0xeu) {
				return 0;
			}
			*buttons = umi_menu_read32(base + 0x18) & (UMI_PON_POWER | UMI_PON_VOLUME_DOWN);
			return 1;
		}
	} while (READ_SYSREG(CNTVCT_EL0) - start < budget);
	return 0;
}

static void
umi_menu_draw(void)
{
	print("\n  UMI temporary boot menu\n");
	print("  Volume Down: change   Power: confirm\n");
	print("   %s Android\n   %s iOS (RAM)\n", umi_menu.ios ? " " : ">", umi_menu.ios ? ">" : " ");
	print("  No confirmation: return to Android.\n\n");
}

static void
umi_menu_open(void)
{
	memset(&umi_menu, 0, sizeof(umi_menu));
	umi_menu.apid = umi_menu_pon_apid();
	umi_menu.deadline = READ_SYSREG(CNTVCT_EL0) + UMI_MENU_TIMEOUT_SECONDS * READ_SYSREG(CNTFRQ_EL0);
	screen_quiet = 0;
	umi_menu_draw();
	set_result("menu-ready", umi_menu.apid, 0);
	print("UMI RAM menu: PON observer APID %d\n", (uint64_t)umi_menu.apid);
	flush_log();
}

static void
umi_menu_select(int ios, int manual)
{
	umi_menu.deadline = 0;
	screen_quiet = 1;
	print("UMI RAM menu: %s selected (%s)\n", ios ? "iOS" : "Android", manual ? "power key" : "timeout");
	if (ios) {
		umi_menu_run_deadline = READ_SYSREG(CNTVCT_EL0) + UMI_MENU_RUN_SECONDS * READ_SYSREG(CNTFRQ_EL0);
		set_result("menu-ios-running", 0, 0);
		if (!run_kernel(0, 0)) {
			set_result("menu-ios-refused", 0, 0);
		}
		umi_menu_run_deadline = 0;
	} else {
		set_result(manual ? "menu-android-selected" : "menu-android-timeout", 0, 0);
	}
	print("UMI RAM menu: returning to stock Android\n");
	flush_log();
	reboot_after_kernel_deadline =
	    READ_SYSREG(CNTVCT_EL0) + UMI_MENU_RESET_SECONDS * READ_SYSREG(CNTFRQ_EL0);
}

static void
umi_menu_poll(void)
{
	unsigned buttons;
	if (READ_SYSREG(CNTVCT_EL0) >= umi_menu.deadline) {
		umi_menu_select(0, 0);
		return;
	}
	if (!umi_menu_buttons(&buttons)) {
		umi_menu.samples = 0;
		return;
	}
	if (!umi_menu.samples || umi_menu.previous != buttons) {
		umi_menu.previous = buttons;
		umi_menu.samples = 1;
		return;
	}
	/* Require a stable press and release; a key held from Fastboot has no effect. */
	if (!umi_menu.armed) {
		umi_menu.armed = buttons == 0;
	} else if (buttons != umi_menu.stable) {
		unsigned released = umi_menu.stable & ~buttons;
		umi_menu.stable = buttons;
		if (released & UMI_PON_POWER) {
			umi_menu_select(umi_menu.ios, 1);
		} else if (released & UMI_PON_VOLUME_DOWN) {
			umi_menu.ios = !umi_menu.ios;
			set_result(umi_menu.ios ? "menu-highlight-ios" : "menu-highlight-android", 0, 0);
			umi_menu_draw();
			flush_log();
		}
	}
}
