/*
 * Where the driver meets 802.11 itself: under Linux that is mac80211 and cfg80211, under FreeBSD
 * LinuxKPI's stand-in for them on top of net80211, and here it is ours -- the functions that
 * LinuxKPI's headers (net/mac80211.h, net/cfg80211.h) declare under linuxkpi_ names, as far as
 * the files of the driver that are built call them. So far that is wmi.c, which hands what the
 * firmware has received and what it has sent to the layer above; there is no such layer yet, and
 * frames end here.
 */
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include "lx.h"

int linuxkpi_debug_80211;			/* (which of the headers' own remarks are printed: none) */

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

/* (no station is known yet: nothing has joined a network) */
struct ieee80211_sta *
linuxkpi_ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const uint8_t *address, const uint8_t *own)
{
	return (NULL);
}

/* A frame the radio has received. */
void
linuxkpi_ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb, struct ieee80211_sta *sta, struct napi_struct *napi,
    struct list_head *list)
{
	dev_kfree_skb_any(skb);
}

/* What became of a frame the driver was given to send. */
void
linuxkpi_ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
}

/* (the driver's recovery after its firmware has died, which nothing here goes through) */
void
linuxkpi_ieee80211_restart_hw(struct ieee80211_hw *hw)
{
}

void
linuxkpi_ieee80211_stop_queues(struct ieee80211_hw *hw)
{
}
