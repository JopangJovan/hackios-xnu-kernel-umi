/*
 * Where the driver meets 802.11 itself: under Linux that is mac80211 and cfg80211, under FreeBSD
 * LinuxKPI's stand-in for them on top of net80211, and here it is ours -- the functions that
 * LinuxKPI's headers (net/mac80211.h, net/cfg80211.h) declare under linuxkpi_ names, as far as
 * the files of the driver that are built call them, and the beginning of a station: the layer
 * that asks the driver for things, in mac80211's order (FreeBSD's linux_80211.c shows it).
 *
 * What is here: the radio as the driver describes it when it registers; the regulatory domain
 * the driver hands over, applied to the radio's channels the way cfg80211 applies it; one
 * interface in station mode; a scan of every channel the domain allows, which only listens;
 * and what the radio then hears -- beacons and answers to others' probes --, kept by sender
 * and said when the scan is over. Nothing joins a network yet, and nothing is sent.
 *
 * On the phone, 13 October, this scans: 26 channels in 4.1 s, and the access points around are
 * heard with their channels and strengths (bsd/dev/redwood_wifi.c, step_driver(), has the
 * account). What is said of them names other people's networks: it is for the log of the run.
 */
#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include <lx_os.h>
#include "lx.h"

int linuxkpi_debug_80211;			/* (which of the headers' own remarks are printed: none) */

#define HEARD_MOST	160

/* A network, as its beacons describe it. */
struct lx_heard {
	uint8_t sender[ETH_ALEN];		/* the access point's address (BSSID) */
	uint8_t name[32], name_length;
	uint16_t frequency;			/* MHz, of the channel it was heard on at its strongest */
	uint8_t says_channel;			/* the channel it says it is on (0: it does not say) */
	int8_t signal;				/* dBm, the strongest */
	uint16_t capability;
	bool protected;				/* it announces WPA2 or later (an RSN element) */
	unsigned times;
};

/* A radio: what stands behind the driver's "hw" and "wiphy". There is one. */
struct lx_radio {
	void *memory;				/* all of this was allocated as one piece, which begins here */
	struct wiphy *wiphy;
	struct ieee80211_hw hw;
	const struct ieee80211_ops *ops;
	bool registered;
	bool kept;				/* the channels' flags and powers as the driver set them have been put by */
	int applying;				/* a regulatory domain is being applied to the channels right now */
	struct workqueue_struct *work;		/* for what the driver has "the stack" run for it */
	struct linuxkpi_ieee80211_regdomain *regd;
	struct ieee80211_vif *vif;		/* the one interface, while it is up */
	volatile int scan;			/* 0: none, 1: running, 2: over, 3: given up by the driver */
	unsigned frames, others;		/* received in all, and of them neither beacon nor probe response */
	unsigned heard_count, heard_lost;
	struct lx_heard heard[HEARD_MOST];
};

static struct lx_radio *the_radio;
static const uint8_t no_network[ETH_ALEN];	/* the address of the network an interface has joined, while it has joined none */

static struct lx_radio *
radio_of(struct ieee80211_hw *hw)
{
	return (__containerof(hw, struct lx_radio, hw));
}

/* ---- the radio, as the driver makes and registers it -------------------------------------------- */

struct ieee80211_hw *
linuxkpi_ieee80211_alloc_hw(size_t private, const struct ieee80211_ops *ops)
{
	size_t ours = roundup2(sizeof(struct lx_radio), 64);
	uint8_t *memory = kzalloc(64 + offsetof(struct wiphy, priv) + ours + private, GFP_KERNEL);
	struct lx_radio *radio;
	struct wiphy *wiphy;

	if (memory == NULL) {
		return (NULL);
	}
	wiphy = (struct wiphy *)roundup2((uintptr_t)memory, 64);
	radio = (struct lx_radio *)wiphy->priv;
	radio->memory = memory;
	radio->wiphy = wiphy;
	radio->ops = ops;
	radio->hw.wiphy = wiphy;
	radio->hw.priv = (uint8_t *)radio + ours;
	radio->work = alloc_ordered_workqueue("wifi-80211", 0);
	mutex_init(&wiphy->mtx);
	return (&radio->hw);
}

struct ieee80211_hw *
linuxkpi_wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
	return (&((struct lx_radio *)wiphy->priv)->hw);
}

void
linuxkpi_set_ieee80211_dev(struct ieee80211_hw *hw)
{
}

int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
	return (0);
}

/* ieee80211_register_hw(): the driver has described its radio, and from here on may be asked for things. */
int
linuxkpi_ieee80211_ifattach(struct ieee80211_hw *hw)
{
	struct lx_radio *radio = radio_of(hw);
	char bands[96] = "";

	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		if (hw->wiphy->bands[band] != NULL) {
			snprintf(bands + strlen(bands), sizeof(bands) - strlen(bands), " %d in band %u,", hw->wiphy->bands[band]->n_channels,
			    band);
		}
	}
	printf("lx: the driver registers a radio: channels:%s (0: 2.4 GHz, 1: 5 GHz, 3: 6 GHz); %s; up to %d names and %u "
	    "bytes of extras in a scan\n", bands, is_zero_ether_addr(hw->wiphy->perm_addr) ? "IT HAS NO ADDRESS" :
	    "it has an address", hw->wiphy->max_scan_ssids, hw->wiphy->max_scan_ie_len);
	radio->registered = true;
	the_radio = radio;
	return (0);
}

/* The driver takes its radio back, and frees the channels next: nobody may be at them when this returns. */
void
linuxkpi_ieee80211_unregister_hw(struct ieee80211_hw *hw)
{
	struct lx_radio *radio = radio_of(hw);

	lx_os_lock();
	radio->registered = false;
	while (radio->applying != 0) {
		lx_os_sleep(&radio->applying, 5 * 1000000ull);
	}
	if (the_radio == radio) {
		the_radio = NULL;
	}
	lx_os_unlock();
}

void
linuxkpi_ieee80211_iffree(struct ieee80211_hw *hw)
{
	struct lx_radio *radio = radio_of(hw);

	if (radio->work != NULL) {
		destroy_workqueue(radio->work);
		radio->work = NULL;
	}
}

void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
	struct lx_radio *radio = (struct lx_radio *)wiphy->priv;

	kfree(radio->regd);
	kfree(radio->memory);
}

/* ---- the regulatory domain ------------------------------------------------------------------------ */

/*
 * What cfg80211 does with a domain (Linux: net/wireless/reg.c, handle_channel()): a channel
 * may be used if a rule covers its 20 MHz, and takes the rule's limits; otherwise it is
 * disabled. The driver reads the result off the channels when it tells the firmware which it
 * may use (reg.c: ath11k_reg_update_chan_list()), and so does the scan below.
 */
static void
regd_apply(struct lx_radio *radio)
{
	const struct linuxkpi_ieee80211_regdomain *regd = radio->regd;
	char said[160] = "";

	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = radio->wiphy->bands[band];
		int allowed = 0;

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			struct linuxkpi_ieee80211_channel *channel = &has->channels[i];
			const struct ieee80211_reg_rule *rule = NULL;

			if (!radio->kept) {
				channel->orig_flags = channel->flags;
				channel->orig_mpwr = channel->max_power;
			}
			for (int r = 0; r < regd->n_reg_rules && rule == NULL; r++) {
				if (regd->reg_rules[r].freq_range.start_freq_khz <= ((int)channel->center_freq - 10) * 1000 &&
				    regd->reg_rules[r].freq_range.end_freq_khz >= ((int)channel->center_freq + 10) * 1000) {
					rule = &regd->reg_rules[r];
				}
			}
			channel->flags = channel->orig_flags & ~(IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR);
			if (rule == NULL) {
				channel->flags |= IEEE80211_CHAN_DISABLED;
				continue;
			}
			if (rule->flags & NL80211_RRF_NO_IR) {
				channel->flags |= IEEE80211_CHAN_NO_IR;
			}
			if (rule->flags & NL80211_RRF_DFS) {
				channel->flags |= IEEE80211_CHAN_RADAR;
			}
			channel->max_reg_power = rule->power_rule.max_eirp / 100;	/* (hundredths of a dBm) */
			channel->max_antenna_gain = rule->power_rule.max_antenna_gain / 100;
			channel->max_power = channel->orig_mpwr != 0 && channel->orig_mpwr < channel->max_reg_power ?
			    channel->orig_mpwr : channel->max_reg_power;
			allowed++;
		}
		if (has != NULL) {
			snprintf(said + strlen(said), sizeof(said) - strlen(said), " %d of %d in band %u,", allowed, has->n_channels, band);
		}
	}
	radio->kept = true;
	printf("lx: the regulatory domain \"%c%c\" (%d rules) allows the radio's channels:%s\n", regd->alpha2[0], regd->alpha2[1],
	    regd->n_reg_rules, said);
}

/*
 * The driver's domain for its radio (which it has from the firmware). It keeps its own copy and
 * may free it. The driver calls this from work of its own, one call at a time, and may be
 * waiting for that work to end while it holds the radio's lock: so that lock is not taken here.
 * A domain that comes when the radio is no longer registered is not applied (the firmware sends
 * one whenever the country changes, also while the driver is on its way down).
 */
int
linuxkpi_regulatory_set_wiphy_regd_sync(struct wiphy *wiphy, struct linuxkpi_ieee80211_regdomain *regd)
{
	struct lx_radio *radio = (struct lx_radio *)wiphy->priv;
	struct linuxkpi_ieee80211_regdomain *copy, *old;
	size_t size;

	if (regd == NULL) {
		return (-EINVAL);
	}
	size = sizeof(*regd) + regd->n_reg_rules * sizeof(regd->reg_rules[0]);
	copy = kmalloc(size, GFP_KERNEL);
	if (copy == NULL) {
		return (-ENOMEM);
	}
	memcpy(copy, regd, size);
	lx_os_lock();
	if (!radio->registered) {
		lx_os_unlock();
		kfree(copy);
		return (-ENODEV);
	}
	radio->applying++;
	lx_os_unlock();
	old = radio->regd;
	radio->regd = copy;
	wiphy->regd = copy;
	regd_apply(radio);
	lx_os_lock();
	radio->applying--;
	lx_os_wakeup(&radio->applying);
	lx_os_unlock();
	kfree(old);
	return (0);
}

/* IEEE 802.11-2020, Annex E: a channel's centre, in MHz */
uint32_t
linuxkpi_ieee80211_channel_to_frequency(uint32_t channel, enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return (channel == 14 ? 2484 : 2407 + 5 * channel);
	case NL80211_BAND_5GHZ:
		return (channel >= 182 && channel <= 196 ? 4000 + 5 * channel : 5000 + 5 * channel);
	case NL80211_BAND_6GHZ:
		return (channel == 2 ? 5935 : 5950 + 5 * channel);
	default:
		return (0);
	}
}

static unsigned
channel_of(unsigned frequency)
{
	return (frequency == 2484 ? 14 : frequency < 2484 ? (frequency - 2407) / 5 : frequency < 5950 ? (frequency - 5000) / 5 :
	       (frequency - 5950) / 5);
}

/* The channel of that frequency among those the radio was registered with. */
struct linuxkpi_ieee80211_channel *
linuxkpi_ieee80211_get_channel(struct wiphy *wiphy, uint32_t frequency)
{
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			if (has->channels[i].center_freq == frequency) {
				return (&has->channels[i]);
			}
		}
	}
	return (NULL);
}

/* ---- what the driver has the layer above do for it ------------------------------------------------- */

void
linuxkpi_ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work)
{
	queue_work(radio_of(hw)->work, work);
}

void
linuxkpi_ieee80211_queue_delayed_work(struct ieee80211_hw *hw, struct delayed_work *work, int delay)
{
	queue_delayed_work(radio_of(hw)->work, work, delay);
}

/* (flags: all interfaces, or those that are up; here the one there is is up for as long as it is there) */
void
linuxkpi_ieee80211_iterate_interfaces(struct ieee80211_hw *hw, enum ieee80211_iface_iter flags,
    void (*function)(void *, uint8_t *, struct ieee80211_vif *), void *argument)
{
	struct ieee80211_vif *vif = radio_of(hw)->vif;

	if (vif != NULL) {
		function(argument, vif->addr, vif);
	}
}

/* (no network has been joined: there are no stations and no channel is held) */
void
linuxkpi_ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw, void (*function)(void *, struct ieee80211_sta *), void *argument)
{
}

void
linuxkpi_ieee80211_iterate_chan_contexts(struct ieee80211_hw *hw,
    void (*function)(struct ieee80211_hw *, struct ieee80211_chanctx_conf *, void *), void *argument)
{
}

struct ieee80211_sta *
linuxkpi_ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *address)
{
	return (NULL);
}

struct ieee80211_sta *
linuxkpi_ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const uint8_t *address, const uint8_t *own)
{
	return (NULL);
}

struct cfg80211_bss *
linuxkpi_cfg80211_get_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel, const uint8_t *bssid,
    const uint8_t *ssid, size_t ssid_length, enum ieee80211_bss_type type, enum ieee80211_privacy privacy)
{
	return (NULL);
}

void
linuxkpi_cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss)
{
}

/* (nothing is queued for sending: there is nothing to wake, and nothing sent to account for) */
void
linuxkpi_ieee80211_wake_queues(struct ieee80211_hw *hw)
{
}

void
linuxkpi_ieee80211_stop_queues(struct ieee80211_hw *hw)
{
}

void
linuxkpi_ieee80211_handle_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *queue)
{
}

void
linuxkpi_ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
}

void
linuxkpi_ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb, int status)
{
	dev_kfree_skb_any(skb);
}

void
linuxkpi_ieee80211_beacon_loss(struct ieee80211_vif *vif)
{
	printf("lx: the driver reports beacons lost\n");
}

void
linuxkpi_ieee80211_connection_loss(struct ieee80211_vif *vif)
{
	printf("lx: the driver reports the connection lost\n");
}

/* (the driver's recovery after its firmware has died, which nothing here goes through) */
void
linuxkpi_ieee80211_restart_hw(struct ieee80211_hw *hw)
{
}

/* ---- what the radio hears -------------------------------------------------------------------------- */

/*
 * A beacon or an answer to a probe (IEEE 802.11-2020, 9.3.3): after the 24 bytes every
 * management frame begins with -- the sender's network is the third address -- a time stamp, the
 * beacon interval, the capabilities, and then elements, of which the name (0), the channel the
 * sender says it is on (3) and whether it announces RSN (48) are taken.
 */
static void
heard(struct lx_radio *radio, const uint8_t *frame, unsigned length, unsigned frequency, int signal)
{
	struct lx_heard *network = NULL;
	const uint8_t *element = frame + 36, *end = frame + length;

	lx_os_lock();
	for (unsigned i = 0; i < radio->heard_count && network == NULL; i++) {
		if (memcmp(radio->heard[i].sender, frame + 16, ETH_ALEN) == 0) {
			network = &radio->heard[i];
		}
	}
	if (network == NULL) {
		if (radio->heard_count == HEARD_MOST) {
			radio->heard_lost++;
			lx_os_unlock();
			return;
		}
		network = &radio->heard[radio->heard_count++];
		bzero(network, sizeof(*network));
		memcpy(network->sender, frame + 16, ETH_ALEN);
		network->signal = -128;
	}
	network->times++;
	network->capability = (uint16_t)(frame[34] | frame[35] << 8);
	if (signal > network->signal) {
		network->signal = (int8_t)signal;
		network->frequency = (uint16_t)frequency;
	}
	for (; element + 2 <= end && element + 2 + element[1] <= end; element += 2 + element[1]) {
		if (element[0] == 0 && element[1] <= sizeof(network->name) && (element[1] != 0 || network->name_length == 0)) {
			network->name_length = element[1];
			memcpy(network->name, element + 2, element[1]);
		} else if (element[0] == 3 && element[1] == 1) {
			network->says_channel = element[2];
		} else if (element[0] == 48) {
			network->protected = true;
		}
	}
	lx_os_unlock();
}

/* A frame the radio has received (the driver has its description behind it: where and how strong). */
void
linuxkpi_ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb, struct ieee80211_sta *sta, struct napi_struct *napi,
    struct list_head *list)
{
	struct lx_radio *radio = radio_of(hw);
	const struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);

	radio->frames++;
	/* (the first byte: 0x80 a beacon, 0x50 an answer to a probe) */
	if (skb->len >= 36 && (skb->data[0] == 0x80 || skb->data[0] == 0x50)) {
		heard(radio, skb->data, skb->len, status->freq, status->signal);
	} else {
		radio->others++;
	}
	dev_kfree_skb_any(skb);
}

/* The driver's word that the scan it was asked for is over. */
void
linuxkpi_ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info)
{
	struct lx_radio *radio = radio_of(hw);

	lx_os_lock();
	radio->scan = info != NULL && info->aborted ? 3 : 2;
	lx_os_wakeup(&radio->scan);
	lx_os_unlock();
}

/* What was heard, the strongest first. Names and addresses of other people's networks: for the log of the run only. */
static void
say_heard(struct lx_radio *radio)
{
	unsigned on_channel[200] = { 0 }, low = 0, high = 0, open = 0, nameless = 0;
	int strongest = -128, weakest = 127;
	char line[400] = "";
	bool said[HEARD_MOST] = { false };

	for (unsigned n = 0; n < radio->heard_count; n++) {
		const struct lx_heard *network = NULL;
		char name[33];
		unsigned which = 0, channel;

		for (unsigned i = 0; i < radio->heard_count; i++) {
			if (!said[i] && (network == NULL || radio->heard[i].signal > network->signal)) {
				network = &radio->heard[i];
				which = i;
			}
		}
		said[which] = true;
		for (unsigned i = 0; i < network->name_length; i++) {
			name[i] = network->name[i] >= 0x20 && network->name[i] < 0x7f ? (char)network->name[i] : '?';
		}
		name[network->name_length] = 0;
		channel = channel_of(network->frequency);
		if (channel < nitems(on_channel)) {
			on_channel[channel]++;
		}
		*(network->frequency < 4000 ? &low : &high) += 1;
		open += !(network->capability & 0x10);
		nameless += network->name_length == 0 || network->name[0] == 0;
		strongest = network->signal > strongest ? network->signal : strongest;
		weakest = network->signal < weakest ? network->signal : weakest;
		printf("lx: heard: channel %3u (%u MHz) %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s, %u times  \"%s\"\n", channel,
		    network->frequency, network->signal, network->sender[0], network->sender[1], network->sender[2], network->sender[3],
		    network->sender[4], network->sender[5], !(network->capability & 0x10) ? "open" : network->protected ? "RSN" :
		    "protected", network->times, name);
	}
	for (unsigned channel = 0; channel < nitems(on_channel); channel++) {
		if (on_channel[channel] != 0) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " %u on %u,", on_channel[channel], channel);
		}
	}
	printf("lx: THE SCAN HEARD %u NETWORKS (access points): %u at 2.4 GHz, %u at 5 GHz; by channel:%s the strongest at %d dBm, "
	    "the weakest at %d; %u open, %u without a name; %u frames in all, %u of them neither beacon nor answer to a probe%s\n",
	    radio->heard_count, low, high, line, radio->heard_count ? strongest : 0, radio->heard_count ? weakest : 0, open, nameless,
	    radio->frames, radio->others, radio->heard_lost ? "; MORE WERE HEARD THAN THERE WAS ROOM FOR" : "");
}

/* ---- the station ------------------------------------------------------------------------------------ */

/*
 * What mac80211 does when an interface in station mode is brought up and asked to scan, as far
 * as the driver is concerned: the radio is started, the interface added to it, and a scan
 * requested of every channel the regulatory domain allows, with no name to ask for -- which
 * makes it a scan that only listens (mac.c: ath11k_mac_op_hw_scan()). When the driver says the
 * scan is over, what was heard is said, and the interface and the radio are taken down again.
 * 0, or an errno.
 */
int
lx_station_scan(void)
{
	struct lx_radio *radio = the_radio;
	struct ieee80211_scan_request *request;
	struct ieee80211_vif *vif;
	struct ieee80211_hw *hw;
	uint8_t *memory;
	unsigned long long began;
	int channels = 0, ret;

	if (radio == NULL || !radio->registered) {
		printf("lx: no radio is registered: nothing to scan with\n");
		return (ENXIO);
	}
	hw = &radio->hw;
	if (is_zero_ether_addr(hw->wiphy->perm_addr)) {
		printf("lx: THE RADIO HAS NO ADDRESS (the kernel was given none, and the firmware has none): no interface is made\n");
		return (EADDRNOTAVAIL);
	}
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		channels += hw->wiphy->bands[band] != NULL ? hw->wiphy->bands[band]->n_channels : 0;
	}
	memory = kzalloc(64 + sizeof(*vif) + hw->vif_data_size, GFP_KERNEL);
	request = kzalloc(sizeof(*request) + channels * sizeof(request->req.channels[0]), GFP_KERNEL);
	if (memory == NULL || request == NULL) {
		return (ENOMEM);
	}
	vif = (struct ieee80211_vif *)roundup2((uintptr_t)memory, 64);
	vif->type = NL80211_IFTYPE_STATION;
	memcpy(vif->addr, hw->wiphy->perm_addr, ETH_ALEN);
	memcpy(vif->bss_conf.addr, hw->wiphy->perm_addr, ETH_ALEN);
	vif->bss_conf.vif = vif;
	vif->bss_conf.bssid = no_network;	/* (the driver compares every beacon's sender with it) */
	vif->link_conf[0] = &vif->bss_conf;

	mutex_lock(&hw->wiphy->mtx);
	ret = radio->ops->start(hw);
	printf("lx: the radio is started: %d\n", ret);
	if (ret != 0) {
		mutex_unlock(&hw->wiphy->mtx);
		return (-ret);
	}
	ret = radio->ops->add_interface(hw, vif);
	printf("lx: an interface in station mode is added to it: %d\n", ret);
	if (ret != 0) {
		radio->ops->stop(hw, false);
		mutex_unlock(&hw->wiphy->mtx);
		return (-ret);
	}
	radio->vif = vif;

	request->req.wiphy = hw->wiphy;
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = hw->wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			if (!(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
				request->req.channels[request->req.n_channels++] = &has->channels[i];
			}
		}
	}
	radio->scan = 1;
	radio->frames = radio->others = radio->heard_count = radio->heard_lost = 0;
	began = lx_os_nanoseconds();
	ret = radio->ops->hw_scan(hw, vif, request);
	mutex_unlock(&hw->wiphy->mtx);
	printf("lx: THE DRIVER IS ASKED TO SCAN %d channels, listening only: %d\n", request->req.n_channels, ret);
	if (ret == 0) {
		lx_os_lock();
		while (radio->scan == 1 && lx_os_nanoseconds() - began < 40000000000ull) {
			lx_os_sleep(&radio->scan, 100 * 1000000ull);
		}
		lx_os_unlock();
		printf("lx: %llu ms later the scan is %s\n", (lx_os_nanoseconds() - began) / 1000000ull, radio->scan == 2 ? "OVER" :
		    radio->scan == 3 ? "GIVEN UP by the driver" : "NOT OVER: the driver has not said so");
		say_heard(radio);
	}

	mutex_lock(&hw->wiphy->mtx);
	if (ret == 0 && radio->scan == 1 && radio->ops->cancel_hw_scan != NULL) {
		radio->ops->cancel_hw_scan(hw, vif);
	}
	radio->ops->remove_interface(hw, vif);
	radio->vif = NULL;
	radio->ops->stop(hw, false);
	mutex_unlock(&hw->wiphy->mtx);
	printf("lx: the interface is removed and the radio stopped\n");
	kfree(request);
	kfree(memory);
	return (ret != 0 ? -ret : radio->scan == 2 ? 0 : ETIMEDOUT);
}
