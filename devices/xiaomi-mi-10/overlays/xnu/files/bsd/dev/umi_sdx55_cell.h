/* SDX55 QMI status and session selection on QRTR. Field layouts follow libqmi's
 * qmi-service-uim.json (0x002f) and qmi-service-nas.json (0x0024).
 * QRTR node, service port and local port are checked by the caller. */
#ifndef UMI_SDX55_CELL_H
#define UMI_SDX55_CELL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>

#define SDX55_UIM_GET_CARD_STATUS    0x002fu
#define SDX55_UIM_PROVISION          0x0038u
#define SDX55_UIM_DEACTIVATE         0x100u
#define SDX55_UIM_GET_CONFIGURATION  0x003au
#define SDX55_NAS_REGISTER_AUTO      0x0022u
#define SDX55_NAS_GET_SERVING_SYSTEM 0x0024u
#define SDX55_NAS_GET_PLMN_NAME      0x0044u

#define SDX55_NAS_BIND_SUBSCRIPTION 0x0045u
#define SDX55_WDS_BIND_SUBSCRIPTION 0x00afu
#define SDX55_NAS_GET_SYSTEM_INFO   0x004du
#define SDX55_NAS_GET_SIGNAL_INFO   0x004fu

struct sdx55_gw_app {
	uint8_t slot, card_state, app_type, app_state, app_index;
};

struct sdx55_uim_app {
	uint8_t type, state, upin_replaces, pin1_state;
	uint8_t aid_length, aid[32];
};

struct sdx55_cell_status {
	uint16_t service_error;
	uint8_t cards, present_cards, error_cards;
	/* AIDs stay in the modem worker; public status includes only counts and states. */
	struct {
		uint8_t card_state, applications, ready_gw, upin_state, card_error;
		struct sdx55_uim_app app[8];
	} slots[2];
	uint8_t auto_select, halt_subscription;
	/* Primary and secondary 3GPP provisioning sessions, in that order. */
	struct sdx55_gw_app gw[2];
	uint8_t nr_service, nr_true_service, nr_domain, nr_forbidden;
	uint8_t lte_service, lte_forbidden;
	uint8_t registration, ps_attach;
	uint32_t radio_mask, data_mask; /* bit indexed by the corresponding QMI enum */
	int16_t lte_rsrp, nr_rsrp;
	char plmn[7], lte_plmn[7], operator_name[64];
};

static inline uint16_t
sdx55_cell_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline int
sdx55_cell_empty_request(uint16_t message, uint16_t txn, uint8_t *out, size_t capacity)
{
	if (!out || capacity < 7) return 0;
	out[0] = 0; /* QMI_REQUEST */
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)message;
	out[4] = (uint8_t)(message >> 8);
	out[5] = out[6] = 0;
	return 7;
}

static inline int
sdx55_cell_request(uint16_t message, uint16_t txn, uint8_t *out, size_t capacity)
{
	if (message == SDX55_NAS_REGISTER_AUTO) {
		if (capacity < 15 || !sdx55_cell_empty_request(message, txn, out, capacity)) return 0;
		out[5] = 8;
		out[7] = 1; out[8] = 1; out[9] = 0; out[10] = 1; /* automatic */
		out[11] = 0x11; out[12] = 1; out[13] = 0; out[14] = 0; /* until power cycle */
		return 15;
	}
	if (message == SDX55_UIM_GET_CONFIGURATION) {
		if (capacity < 14 || !sdx55_cell_empty_request(message, txn, out, capacity)) return 0;
		out[5] = 7;
		out[7] = 0x10;
		out[8] = 4;
		out[9] = 0;
		out[10] = 5; /* automatic selection and halt-subscription publication */
		out[11] = out[12] = out[13] = 0;
		return 14;
	}
	if (message != SDX55_UIM_GET_CARD_STATUS && message != SDX55_NAS_GET_SERVING_SYSTEM &&
	    message != SDX55_NAS_GET_SYSTEM_INFO && message != SDX55_NAS_GET_SIGNAL_INFO)
		return 0;
	return sdx55_cell_empty_request(message, txn, out, capacity);
}

/* Qualcomm NAS 0x45 encodes the control-point subscription as a one-byte
 * enum starting at zero. WDS 0xaf uses a four-byte enum starting at one. */
static inline size_t
sdx55_cell_bind_request(uint16_t message, uint16_t txn, uint32_t subscription,
    uint8_t *out, size_t capacity)
{
	if ((message != SDX55_NAS_BIND_SUBSCRIPTION && message != SDX55_WDS_BIND_SUBSCRIPTION) ||
	    (subscription != 1 && subscription != 2)) return 0;
	unsigned width = message == SDX55_NAS_BIND_SUBSCRIPTION ? 1 : 4;
	if (capacity < 10 + width || !sdx55_cell_empty_request(message, txn, out, capacity)) return 0;
	out[5] = (uint8_t)(3 + width);
	out[7] = 1;
	out[8] = (uint8_t)width;
	out[9] = 0;
	out[10] = (uint8_t)(width == 1 ? subscription - 1 : subscription);
	for (unsigned i = 1; i < width; i++) out[10 + i] = 0;
	return 10 + width;
}

/* A successful return validates the complete TLV framing for the caller.
 * A modem error may omit all service-specific fields. */
static inline int
sdx55_cell_body(uint16_t message, uint16_t txn, const uint8_t *packet, size_t size,
    uint16_t *service_error)
{
	if (!packet || !service_error || size < 7 || packet[0] != 2 ||
	    sdx55_cell_le16(packet + 1) != txn || sdx55_cell_le16(packet + 3) != message ||
	    sdx55_cell_le16(packet + 5) != size - 7)
		return EINVAL;

	const uint8_t *result = NULL;
	for (size_t off = 7; off < size; ) {
		if (size - off < 3) return EINVAL;
		uint8_t tag = packet[off];
		size_t n = sdx55_cell_le16(packet + off + 1);
		off += 3;
		if (n > size - off) return EINVAL;
		if (tag == 2) {
			if (result || n != 4) return EINVAL;
			result = packet + off;
		}
		off += n;
	}
	if (!result) return EINVAL;
	uint16_t code = sdx55_cell_le16(result);
	uint16_t error = sdx55_cell_le16(result + 2);
	if (code > 1 || (code == 0) != (error == 0)) return EINVAL;
	*service_error = error;
	return code == 0 ? 0 : EIO;
}

static inline int
sdx55_cell_cards(const uint8_t *p, size_t n, struct sdx55_cell_status *out)
{
	if (n < 9) return EINVAL;
	/* Each 3GPP index encodes (logical slot << 8) | application. */
	uint16_t indices[] = {sdx55_cell_le16(p), sdx55_cell_le16(p + 4)};
	out->cards = p[8];
	p += 9;
	n -= 9;
	for (unsigned slot = 0; slot < out->cards; slot++) {
		if (n < 6 || p[0] > 2) return EINVAL;
		uint8_t state = p[0], apps = p[5];
		out->present_cards += state == 1;
		out->error_cards += state == 2;
		if (slot < 2) {
			out->slots[slot].card_state = state;
			out->slots[slot].upin_state = p[1];
			out->slots[slot].card_error = p[4];
			out->slots[slot].applications = apps;
		}
		p += 6;
		n -= 6;
		for (unsigned app = 0; app < apps; app++) {
			/* Six attributes, AID length + bytes, then seven PIN fields. */
			if (n < 14) return EINVAL;
			size_t app_size = 14u + p[6];
			if (app_size > n) return EINVAL;
			if (slot < 2 && app < sizeof(out->slots[0].app) / sizeof(out->slots[0].app[0])) {
				const uint8_t *pin = p + 7u + p[6];
				out->slots[slot].app[app].type = p[0];
				out->slots[slot].app[app].state = p[1];
				out->slots[slot].app[app].upin_replaces = pin[0];
				out->slots[slot].app[app].pin1_state = pin[1];
				struct sdx55_uim_app *a = &out->slots[slot].app[app];
				if (p[6] <= sizeof(a->aid)) {
					a->aid_length = p[6];
					for (unsigned i = 0; i < a->aid_length; i++) a->aid[i] = p[7 + i];
				}
			}
			if (slot < 2 && (p[0] == 1 || p[0] == 2) && p[1] == 7) out->slots[slot].ready_gw++;
			for (unsigned i = 0; i < 2; i++) {
				if (indices[i] == ((slot << 8) | app))
					out->gw[i] = (struct sdx55_gw_app){(uint8_t)slot, state, p[0], p[1], (uint8_t)app};
			}
			p += app_size;
			n -= app_size;
		}
	}
	for (unsigned i = 0; i < 2; i++)
		if (indices[i] != UINT16_MAX && out->gw[i].slot == UINT8_MAX) return EINVAL;
	return n ? EINVAL : 0;
}

/* A cold UMI modem can enumerate one USIM without an active GW session.
 * Use the stock slot-to-subscription mapping only for this unambiguous case. */
static inline unsigned
sdx55_uim_provision_slot(const struct sdx55_cell_status *c)
{
	if (c->present_cards != 1 || c->cards > 2 ||
	    c->gw[0].slot != UINT8_MAX || c->gw[1].slot != UINT8_MAX) return 0;
	for (unsigned slot = 0; slot < c->cards; slot++) {
		if (c->slots[slot].card_state != 1 || c->slots[slot].applications != 1) continue;
		const struct sdx55_uim_app *a = &c->slots[slot].app[0];
		/* Detected USIM, before PIN initialization. The modem enforces SIM PIN. */
		if (a->type == 2 && a->state == 1 && a->aid_length && !a->pin1_state &&
		    !c->slots[slot].upin_state) return slot + 1;
	}
	return 0;
}

static inline size_t
sdx55_uim_provision_request(uint16_t txn, unsigned argument, const struct sdx55_cell_status *c,
    uint8_t *out, size_t capacity)
{
	unsigned slot = argument & ~SDX55_UIM_DEACTIVATE;
	int activate = !(argument & SDX55_UIM_DEACTIVATE);
	if (slot < 1 || slot > 2 || (activate && sdx55_uim_provision_slot(c) != slot)) return 0;
	const struct sdx55_uim_app *a = &c->slots[slot - 1].app[0];
	unsigned len = activate ? 17u + a->aid_length : 12u;
	if (capacity < len || !sdx55_cell_empty_request(SDX55_UIM_PROVISION, txn, out, capacity)) return 0;
	out[5] = (uint8_t)(len - 7);
	out[7] = 1; out[8] = 2; out[9] = 0;
	out[10] = (uint8_t)((slot - 1) * 2); /* primary GW = 0, secondary GW = 2 */
	out[11] = (uint8_t)activate;
	if (activate) {
		out[12] = 0x10; out[13] = (uint8_t)(2 + a->aid_length); out[14] = 0;
		out[15] = (uint8_t)slot;
		out[16] = a->aid_length;
		for (unsigned i = 0; i < a->aid_length; i++) out[17 + i] = a->aid[i];
	}
	return len;
}

/* With no user-selected data line, accept one present card and exactly one
 * provisioned GW application. Distinguish an unready SIM from ambiguous DDS. */
static inline int
sdx55_cell_subscription(const struct sdx55_cell_status *c, uint8_t *subscription)
{
	if (!c->present_cards) return ENODEV;
	if (c->present_cards != 1) return EBUSY;
	unsigned found = 0;
	for (unsigned i = 0; i < 2; i++) {
		const struct sdx55_gw_app *a = &c->gw[i];
		if (a->slot == UINT8_MAX || a->card_state != 1) continue;
		if (found) return EBUSY;
		found = i + 1;
	}
	if (!found) return EAGAIN;
	const struct sdx55_gw_app *a = &c->gw[found - 1];
	if ((a->app_type != 1 && a->app_type != 2) || a->app_state != 7) return EAGAIN;
	*subscription = (uint8_t)found;
	return 0;
}

/* The first byte of either NAS list is its byte count. Ignore unknown IDs
 * outside the mask's range; an unknown radio must not alias a known RAT. */
static inline int
sdx55_cell_enum_mask(const uint8_t *p, size_t n, uint32_t *mask)
{
	if (n < 1 || p[0] != n - 1) return EINVAL;
	for (size_t i = 1; i < n; i++)
		if (p[i] < 32) *mask |= 1u << p[i];
	return 0;
}

static inline void
sdx55_cell_plmn(char out[7], unsigned mcc, unsigned mnc, unsigned digits)
{
	if (mcc > 999 || mnc > 999 || (digits == 2 && mnc > 99)) return;
	out[0] = '0' + mcc / 100;
	out[1] = '0' + mcc / 10 % 10;
	out[2] = '0' + mcc % 10;
	unsigned first = 3;
	if (digits == 3) out[first++] = '0' + mnc / 100;
	out[first++] = '0' + mnc / 10 % 10;
	out[first++] = '0' + mnc % 10;
	out[first] = 0;
}

static inline void
sdx55_cell_system_plmn(char out[7], const uint8_t *p)
{
	if (p[19] != 1) return;
	unsigned digits = p[25] == 0xff || !p[25] ? 5 : 6;
	for (unsigned i = 0; i < digits; i++)
		if (p[20+i] < '0' || p[20+i] > '9') return;
	for (unsigned i = 0; i < digits; i++) out[i] = p[20+i];
}

/* libqmi NAS Get PLMN Name: PLMN, MNC PCS digit, always return a name. */
static inline size_t
sdx55_cell_plmn_request(uint16_t txn, uint32_t plmn, uint8_t *out, size_t capacity)
{
	if (capacity < 22 || !sdx55_cell_empty_request(SDX55_NAS_GET_PLMN_NAME, txn, out, capacity)) return 0;
	uint8_t tlvs[] = {1,4,0,plmn, (plmn>>8)&3, plmn>>10, (plmn>>18)&3,
	    0x11,1,0,(plmn>>20)&1, 0x12,1,0,1};
	out[5] = sizeof(tlvs);
	for (unsigned i = 0; i < sizeof(tlvs); i++) out[7+i] = tlvs[i];
	return 7 + sizeof(tlvs);
}

static inline bool
sdx55_cell_name(char name[64], unsigned encoding, unsigned spare, const uint8_t *data, size_t size)
{
	if (!size || (encoding == 4 && (size & 1)) || (encoding == 9 && spare > 7)) return false;
	unsigned used = 0;
	if (encoding == 0 || encoding == 1) {
		if (size >= 64) return false;
		for (; used < size && data[used]; used++) name[used] = data[used];
	} else if (encoding == 4) {
		for (size_t i = 0; i < size; i += 2) {
			unsigned c = sdx55_cell_le16(data+i);
			if (!c) break;
			if ((c >= 0xd800 && c <= 0xdfff) || used + 3 >= 64) return false;
			if (c >= 0x800) name[used++] = 0xe0 | c >> 12;
			if (c >= 0x80) name[used++] = (c >= 0x800 ? 0x80 : 0xc0) | ((c >> 6) & 0x3f);
			name[used++] = c < 0x80 ? c : 0x80 | (c & 0x3f);
		}
	} else if (encoding == 9) {
		/* Decode ASCII-compatible GSM-7 names. Use the numeric PLMN if
		 * a name needs characters outside this subset. */
		size_t bits = size * 8 - spare;
		for (size_t bit = 0; bit + 7 <= bits; bit += 7) {
			unsigned c = data[bit/8] >> (bit%8);
			if (bit%8 > 1) c |= (unsigned)data[bit/8+1] << (8-bit%8);
			c &= 0x7f;
			if (!c && !spare && bit + 14 > bits) break;
			if (used == 63 || !((c >= ' ' && c <= '?') || (c >= 'A' && c <= 'Z') ||
			    (c >= 'a' && c <= 'z'))) return false;
			name[used++] = c;
		}
	}
	name[used] = 0;
	return used != 0;
}

static inline void
sdx55_cell_eons(char name[64], const uint8_t *data, size_t size)
{
	if (size < 2 || data[1] > size - 2) return;
	size_t offset = 2 + data[1]; /* Skip the SIM provider name, which can differ when roaming. */
	char candidate[64];
	for (unsigned i = 0; i < 2; i++) { /* Short and then long serving network name. */
		if (size - offset < 4 || data[offset+3] > size-offset-4) return;
		size_t length = data[offset+3];
		if (sdx55_cell_name(candidate, data[offset], data[offset+2], data+offset+4, length))
			for (unsigned j = 0; j < sizeof(candidate); j++) {
				name[j] = candidate[j];
				if (!candidate[j]) break;
			}
		offset += 4 + length;
	}
}

static inline int
sdx55_cell_decode(uint16_t message, uint16_t txn, const uint8_t *packet, size_t size,
    struct sdx55_cell_status *out)
{
	if (!out || (message != SDX55_UIM_GET_CONFIGURATION && message != SDX55_UIM_GET_CARD_STATUS &&
	    message != SDX55_NAS_GET_SERVING_SYSTEM && message != SDX55_NAS_GET_SYSTEM_INFO &&
	    message != SDX55_NAS_GET_SIGNAL_INFO && message != SDX55_NAS_GET_PLMN_NAME))
		return EINVAL;
	struct sdx55_cell_status decoded = {
		.auto_select = UINT8_MAX, .halt_subscription = UINT8_MAX,
		.slots = {{.card_state = UINT8_MAX}, {.card_state = UINT8_MAX}},
		.gw = {{UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX},
		       {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX}},
		.nr_service = UINT8_MAX, .nr_true_service = UINT8_MAX, .nr_domain = UINT8_MAX,
		.lte_service = UINT8_MAX,
		.lte_rsrp = INT16_MIN, .nr_rsrp = INT16_MIN,
	};
	int error = sdx55_cell_body(message, txn, packet, size, &decoded.service_error);
	if (error) {
		if (error == EIO) *out = decoded;
		return error;
	}

	int found_status = 0, found_data = 0, found_nr = 0, found_domain = 0;
	unsigned mcc = 0, mnc = 0, pcs_mcc = 0, pcs_mnc = 0, pcs_digits = 0;
	for (size_t off = 7; off < size; ) {
		uint8_t tag = packet[off];
		size_t n = sdx55_cell_le16(packet + off + 1);
		const uint8_t *p = packet + off + 3;
		off += n + 3;
		if (message == SDX55_UIM_GET_CONFIGURATION && (tag == 0x10 || tag == 0x12)) {
			uint8_t *field = tag == 0x10 ? &decoded.auto_select : &decoded.halt_subscription;
			if (n != 1 || *p > 1 || *field != UINT8_MAX) return EINVAL;
			*field = *p;
		} else if (message == SDX55_UIM_GET_CARD_STATUS && tag == 0x10) {
			if (found_status++) return EINVAL;
			if (sdx55_cell_cards(p, n, &decoded)) return EINVAL;
		} else if (message == SDX55_NAS_GET_SYSTEM_INFO && tag == 0x4a) {
			if (found_nr++ || n != 3) return EINVAL;
			decoded.nr_service = p[0];
			decoded.nr_true_service = p[1];
		} else if (message == SDX55_NAS_GET_SYSTEM_INFO && tag == 0x4b) {
			/* Fixed libqmi NR5G System Info through TAC (29 bytes). */
			if (found_domain++ || n < 29 || p[0] > 1 || p[6] > 1) return EINVAL;
			if (p[0]) decoded.nr_domain = p[1];
			if (p[6]) decoded.nr_forbidden = p[7];
			sdx55_cell_system_plmn(decoded.plmn, p);
		} else if (message == SDX55_NAS_GET_SYSTEM_INFO && tag == 0x14 && n == 3) {
			decoded.lte_service = p[0];
		} else if (message == SDX55_NAS_GET_SYSTEM_INFO && tag == 0x19 && n >= 29) {
			if (p[6] == 1) decoded.lte_forbidden = p[7];
			sdx55_cell_system_plmn(decoded.lte_plmn, p);
		} else if (message == SDX55_NAS_GET_SERVING_SYSTEM && tag == 1) {
			if (found_status++ || n < 5 || p[0] > 4 || p[1] > 2 || p[2] > 2) return EINVAL;
			decoded.registration = p[0];
			decoded.ps_attach = p[2];
			if (sdx55_cell_enum_mask(p + 4, n - 4, &decoded.radio_mask)) return EINVAL;
		} else if (message == SDX55_NAS_GET_SERVING_SYSTEM && tag == 0x11) {
			if (found_data++ || sdx55_cell_enum_mask(p, n, &decoded.data_mask)) return EINVAL;
		} else if (message == SDX55_NAS_GET_SERVING_SYSTEM && tag == 0x12 && n >= 5 && p[4] == n-5) {
			mcc = sdx55_cell_le16(p);
			mnc = sdx55_cell_le16(p+2);
			if (p[4] < sizeof(decoded.operator_name))
				for (unsigned i = 0; i < p[4]; i++) decoded.operator_name[i] = p[5+i];
		} else if (message == SDX55_NAS_GET_SERVING_SYSTEM && tag == 0x27 && n == 5 && p[4] <= 1) {
			pcs_mcc = sdx55_cell_le16(p);
			pcs_mnc = sdx55_cell_le16(p+2);
			pcs_digits = 2 + p[4];
		} else if (message == SDX55_NAS_GET_SIGNAL_INFO && tag == 0x14 && n == 6) {
			decoded.lte_rsrp = (int16_t)sdx55_cell_le16(p+2);
		} else if (message == SDX55_NAS_GET_SIGNAL_INFO && tag == 0x17 && n == 4) {
			decoded.nr_rsrp = (int16_t)sdx55_cell_le16(p);
		} else if (message == SDX55_NAS_GET_PLMN_NAME && tag == 0x10) {
			sdx55_cell_eons(decoded.operator_name, p, n);
		}
	}
	if ((message == SDX55_UIM_GET_CARD_STATUS || message == SDX55_NAS_GET_SERVING_SYSTEM) && !found_status) return EINVAL;
	if (mcc) sdx55_cell_plmn(decoded.plmn, mcc, mnc,
	    mcc == pcs_mcc && mnc == pcs_mnc && pcs_digits ? pcs_digits : mnc > 99 ? 3 : 2);
	*out = decoded;
	return 0;
}

static inline int
sdx55_cell_nr_packet_service(const struct sdx55_cell_status *c)
{
	return c->nr_service == 2 && !c->nr_forbidden && (c->nr_domain == 2 || c->nr_domain == 3);
}

#endif
