/*
 * The radio's bus, for as long as ath11k's own (ahb.c, pcic.c) is not built: what stands between
 * the driver's core and this phone's WCN6750. It does what ahb.c's probe does for this chip, in
 * its order, with what the files built so far need of it -- the chip's registers through the
 * three fixed windows, no interrupts (the copy engines are looked at instead) -- and it is where
 * the driver's first calls into the parts not yet built are given something to do.
 *
 * Where this stage ends: the driver's qmi.c has its exchange with the firmware, core.c goes on
 * to start the firmware in mission mode and has ce.c and hal.c set up the copy engines' rings,
 * and then waits for the firmware's first message over a copy engine (ath11k_htc_wait_target()
 * of htc.c, which is not built: the one here looks at the copy engines until a message comes,
 * says what it was, and then fails on purpose, so that core.c winds everything down again).
 *
 * The kernel's boot-arg wifi_lx says how far the radio's registers are touched: 1, not at all
 * (every write is said and dropped, every read is 0: the driver's own exchange and nothing
 * else); 2, for real.
 *
 * What the phone said, 13 October: all of the above happens, and the first message comes 9 ms
 * after the buffers are posted, on copy engine 2: HTC's "ready" (bsd/dev/redwood_wifi.c,
 * step_driver(), has the account). Looking at the rings once a millisecond was enough.
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/skbuff.h>

#include "core.h"
#include "ce.h"
#include "debug.h"
#include "hal.h"
#include "hif.h"
#include "htc.h"
#include "pcic.h"
#include "qmi.h"

#include <lx_os.h>
#include "lx.h"

static struct {
	unsigned how;				/* wifi_lx */
	bool closed;				/* the driver has stopped the bus: the registers are not touched again */
	unsigned reads, writes, said, refused;
	u32 last, last_value;			/* the register written last, and with what ... */
	unsigned again;				/* ... and how many writes to it in a row have not been said */
	bool waiting;				/* ath11k_htc_wait_target() is looking at the copy engines ... */
	unsigned same;				/* ... and has written a pointer what it held so many times */
	volatile int messages;			/* received over the copy engines */
	volatile int waited;			/* ath11k_htc_wait_target() has been reached, and is over */
} bus;

/* ---- what the driver says ---------------------------------------------------------------------- */

static void
driver_says(const char *kind, const char *format, va_list list)
{
	char line[320];

	vsnprintf(line, sizeof(line), format, list);
	printf("ath11k%s: %s", kind, line);
}

void
ath11k_info(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says("", format, list);
	va_end(list);
}

void
ath11k_warn(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" WARNS", format, list);
	va_end(list);
}

void
ath11k_err(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" ERROR", format, list);
	va_end(list);
}

void
__ath11k_dbg(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" (debug)", format, list);
	va_end(list);
}

void
ath11k_dbg_dump(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *msg, const char *prefix, const void *buf,
    size_t len)
{
}

/* ---- the chip's registers ---------------------------------------------------------------------- */

/*
 * As ahb.c reaches them (ath11k_ahb_get_window_start_wcn6750()): the first 512K of the block
 * the firmware names are the chip's first 512K; the second 512K show its data path's
 * registers, the third its copy engines'.
 */
static u32
window(struct ath11k_base *ab, u32 offset)
{
	if (offset < ATH11K_PCI_WINDOW_START) {
		return (offset);
	}
	if ((offset ^ HAL_SEQ_WCSS_UMAC_OFFSET) < ATH11K_PCI_WINDOW_RANGE_MASK) {
		return (ATH11K_PCI_WINDOW_START + (offset & ATH11K_PCI_WINDOW_RANGE_MASK));
	}
	if ((offset ^ HAL_SEQ_WCSS_UMAC_CE0_SRC_REG(ab)) < ATH11K_PCI_WINDOW_RANGE_MASK) {
		return (2 * ATH11K_PCI_WINDOW_START + (offset & ATH11K_PCI_WINDOW_RANGE_MASK));
	}
	printf("lx: the driver reaches for the chip's %#x, which no window shows\n", offset);
	return (0);
}

/*
 * The registers answer while the firmware is in mission mode and at no other time: an access
 * outside it ends the kernel (bsd/dev/redwood_wifi.c, step 16). The driver asks for that mode
 * before it first touches them; once it has stopped the bus, on its way to asking for the mode
 * "off", nothing is let through any more.
 */
static bool
refused(u32 offset)
{
	if (!bus.closed) {
		return (false);
	}
	if (bus.refused++ < 8) {
		printf("lx: the chip's %#x is reached for after the bus was stopped: NOT DONE\n", offset);
	}
	return (true);
}

/*
 * The accesses are said one by one, up to a number of lines; of writes to one register in a row
 * (a ring's pointer, moved once for every buffer) the first three, and then how many followed.
 */
#define SAID_FIRST	600

static void
say_repeats(void)
{
	if (bus.again != 0) {
		printf("lx: ... and %u more to the chip's %#x, the last %#x\n", bus.again, bus.last, bus.last_value);
		bus.again = 0;
	}
}

static bool
said_write(u32 offset, u32 value)
{
	static struct {
		u32 offset, value;
	} pointers[16];				/* while the copy engines are looked at: what each pointer was moved to last */
	static unsigned row;

	if (bus.waiting) {
		for (unsigned i = 0; i < nitems(pointers); i++) {
			if (pointers[i].offset == offset || pointers[i].offset == 0) {
				bool moved = pointers[i].offset == 0 || pointers[i].value != value;

				pointers[i].offset = offset;
				pointers[i].value = value;
				bus.same += !moved;
				return (moved && bus.said < SAID_FIRST);
			}
		}
	}
	row = offset == bus.last ? row + 1 : 0;
	if (row >= 3) {
		bus.again++;
		bus.last_value = value;
		return (false);
	}
	say_repeats();
	bus.last = offset;
	return (bus.said < SAID_FIRST);
}

static u32
bus_read32(struct ath11k_base *ab, u32 offset)
{
	u32 value = 0;

	if (refused(offset)) {
		return (0);
	}
	bus.reads++;
	if (bus.how >= 2 && ab->mem != NULL) {
		value = ioread32((u8 *)ab->mem + window(ab, offset));
	}
	if (bus.said < SAID_FIRST) {
		say_repeats();
		bus.last = ~0u;
		bus.said++;
		printf("lx: the chip's %#x reads %#x%s\n", offset, value, bus.how >= 2 ? "" : " (NOT READ: wifi_lx=1)");
	}
	return (value);
}

static void
bus_write32(struct ath11k_base *ab, u32 offset, u32 value)
{
	if (refused(offset)) {
		return;
	}
	bus.writes++;
	if (said_write(offset, value)) {
		bus.said++;
		printf("lx: %s the chip's %#x (at +%#x of its block): %#x\n", bus.how >= 2 ? "about to write" : "NOT WRITTEN (wifi_lx=1):",
		    offset, window(ab, offset), value);
	}
	if (bus.how >= 2 && ab->mem != NULL) {
		__asm __volatile("dsb sy");	/* (what was written to a ring is in memory before the radio is told of it) */
		iowrite32(value, (u8 *)ab->mem + window(ab, offset));
	}
}

/* pcic.c: ath11k_pcic_start(), without the interrupts */
static int
bus_start(struct ath11k_base *ab)
{
	set_bit(ATH11K_FLAG_DEVICE_INIT_DONE, &ab->dev_flags);
	printf("lx: the driver starts the bus: buffers for the copy engines that receive (%u reads and %u writes of the chip's "
	    "registers so far; the configuration the firmware was sent named %u shadow registers)\n", bus.reads, bus.writes,
	    ab->qmi.ce_cfg.shadow_reg_v2_len);
	ath11k_ce_rx_post_buf(ab);
	return (0);
}

/* pcic.c: ath11k_pcic_stop() */
static void
bus_stop(struct ath11k_base *ab)
{
	say_repeats();
	bus.closed = true;
	timer_delete_sync(&ab->rx_replenish_retry);
	ath11k_ce_cleanup_pipes(ab);
	printf("lx: the driver stops the bus (%u reads and %u writes of the chip's registers in all)\n", bus.reads, bus.writes);
}

static int
bus_power_up(struct ath11k_base *ab)
{
	return (-lx_os_rproc_boot());
}

static void
bus_power_down(struct ath11k_base *ab, bool is_suspend)
{
}

static void
bus_nothing(struct ath11k_base *ab)
{
}

static const struct ath11k_hif_ops bus_ops = {
	.read32 = bus_read32,
	.write32 = bus_write32,
	.irq_enable = bus_nothing,
	.irq_disable = bus_nothing,
	.start = bus_start,
	.stop = bus_stop,
	.power_up = bus_power_up,
	.power_down = bus_power_down,
	/* (no interrupt vectors are offered: the rings are then set up without any, see ce.c) */
};

/* ---- the first things the driver does with parts that are not built -------------------------------- */

static void
first_bytes(const char *what, struct sk_buff *skb)
{
	char text[3 * 32 + 1] = "";

	for (unsigned i = 0; i < skb->len && i < 32; i++) {
		snprintf(text + 3 * i, 4, " %02x", skb->data[i]);
	}
	printf("lx: A MESSAGE FROM THE FIRMWARE OVER A COPY ENGINE (%s): %u bytes, beginning%s\n", what, skb->len, text);
}

/* htc.c's: what a copy engine has received for the control and WMI endpoints */
void
ath11k_htc_rx_completion_handler(struct ath11k_base *ab, struct sk_buff *skb)
{
	if (bus.messages++ < 4) {
		first_bytes("for HTC", skb);
	}
	dev_kfree_skb_any(skb);
}

/* dp_rx.c's: ... for the data path */
void
ath11k_dp_htt_htc_t2h_msg_handler(struct ath11k_base *ab, struct sk_buff *skb)
{
	if (bus.messages++ < 4) {
		first_bytes("for the data path", skb);
	}
	dev_kfree_skb_any(skb);
}

void
ath11k_htc_tx_completion_handler(struct ath11k_base *ab, struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
}

/*
 * htc.c's: wait for the firmware's "ready" over copy engine 1. Here: look at every copy engine,
 * as its interrupt would have ath11k do, for five seconds or until something has come; then
 * fail, which is where this stage ends (see the top).
 */
int
ath11k_htc_wait_target(struct ath11k_htc *htc)
{
	struct ath11k_base *ab = container_of(htc, struct ath11k_base, htc);	/* (htc.c would have filled in htc->ab) */
	unsigned long long began = lx_os_nanoseconds(), limit = bus.how >= 2 ? 5000000000ull : 500000000ull;
	unsigned long long first = 0;

	say_repeats();
	printf("lx: the driver waits for the firmware's first message: looking at the %u copy engines%s\n", ab->hw_params.ce_count,
	    bus.how >= 2 ? "" : " (which were told of no ring, wifi_lx=1: none can come)");
	bus.waiting = true;
	/* (and for a little longer after the first, to see what follows it) */
	while (lx_os_nanoseconds() - began < limit && (first == 0 || lx_os_nanoseconds() - first < 200000000ull)) {
		for (unsigned i = 0; i < ab->hw_params.ce_count; i++) {
			ath11k_ce_per_engine_service(ab, i);
		}
		if (bus.messages != 0 && first == 0) {
			first = lx_os_nanoseconds();
			printf("lx: THE FIRST MESSAGE HAS COME, %llu ms after the buffers were posted\n", (first - began) / 1000000ull);
		}
		lx_pause_ms(1);
	}
	bus.waiting = false;
	printf("lx: %d message(s) from the firmware in %llu ms (%u reads and %u writes of the chip's registers in all, %u of "
	    "the writes a ring's pointer written what it held); the driver is told to give up here: htc.c is not built\n",
	    bus.messages, (lx_os_nanoseconds() - began) / 1000000ull, bus.reads, bus.writes, bus.same);
	bus.waited = 1;
	return (-ETIMEDOUT);
}

/* ---- the device tree, of which there is none ------------------------------------------------------ */

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

bool
of_machine_is_compatible(const char *compatible)
{
	return (false);
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	return (-ENODEV);
}

/* (mac80211's, named in core.c's recovery, which nothing here reaches) */
void
linuxkpi_ieee80211_restart_hw(struct ieee80211_hw *hw)
{
}

void
linuxkpi_ieee80211_stop_queues(struct ieee80211_hw *hw)
{
}

/* ---- the entry ------------------------------------------------------------------------------------ */

int
lx_start(void)
{
	static struct device device;
	static struct device_node node;
	struct ath11k_base *ab;
	unsigned long long began;
	int ret;

	bus.how = lx_os_option("wifi_lx");
	if (lx_base_init() != 0 || lx_pool_init() != 0) {
		printf("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	ath11k_debug_mask = ATH11K_DBG_QMI | ATH11K_DBG_BOOT | ATH11K_DBG_AHB;
	device.of_node = &node;
	device.kobj.name = "wifi";

	/* ahb.c: ath11k_ahb_probe(), for this chip */
	ab = ath11k_core_alloc(&device, 0, ATH11K_BUS_AHB);
	if (ab == NULL) {
		return (ENOMEM);
	}
	ab->hif.ops = &bus_ops;
	ab->hw_rev = ATH11K_HW_WCN6750_HW10;
	ab->fw_mode = ATH11K_FIRMWARE_MODE_NORMAL;
	if ((ret = ath11k_core_pre_init(ab)) != 0 || (ret = ath11k_hal_srng_init(ab)) != 0 || (ret = ath11k_ce_alloc_pipes(ab)) != 0) {
		printf("lx: the driver does not get as far as its exchange with the firmware: %d\n", ret);
		return (-ret);
	}
	ab->qmi.ce_cfg.tgt_ce_len = ab->hw_params.target_ce_count;
	ab->qmi.ce_cfg.tgt_ce = ab->hw_params.target_ce_config;
	ab->qmi.ce_cfg.svc_to_ce_map_len = ab->hw_params.svc_to_ce_map_len;
	ab->qmi.ce_cfg.svc_to_ce_map = ab->hw_params.svc_to_ce_map;
	ab->qmi.service_ins_id = ab->hw_params.qmi_service_ins_id;
	printf("lx: the driver (\"%s\") is set up; its exchange with the firmware begins (registers: %s)\n", ab->hw_params.name,
	    bus.how >= 2 ? "for real" : "NOT TOUCHED, wifi_lx=1");
	if ((ret = ath11k_core_init(ab)) != 0) {
		printf("lx: the driver's start fails: %d\n", ret);
		return (-ret);
	}

	/* the rest happens on the driver's own threads: until it has waited for the first message, or has failed */
	began = lx_os_nanoseconds();
	while (!bus.waited && !test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags) && lx_os_nanoseconds() - began < 45000000000ull) {
		lx_pause_ms(10);
	}
	lx_pause_ms(600);		/* (for core.c to have wound down: the mode "off") */
	printf("lx: over after %llu ms: %s; the driver's flags: %#lx\n", (lx_os_nanoseconds() - began) / 1000000ull, bus.messages ?
	    "A MESSAGE CAME OVER A COPY ENGINE" : bus.waited ? "the driver got as far as waiting for the first message" :
	    "THE DRIVER DID NOT GET AS FAR AS WAITING FOR THE FIRST MESSAGE", ab->dev_flags);
	ath11k_qmi_deinit_service(ab);
	/* as far as this stage can go: a message with the registers for real, the wait without them */
	return (bus.messages ? 0 : !bus.waited ? EIO : bus.how >= 2 ? ETIMEDOUT : 0);
}
