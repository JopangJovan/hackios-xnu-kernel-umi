#ifndef UMI_SDX55_SMS_WIRE_H
#define UMI_SDX55_SMS_WIRE_H

#include "umi_sdx55_cell.h"
#include <sys/umi_sdx55_sms.h>

#define SDX55_WMS_SERVICE 5u
#define SDX55_WMS_PORT 0x430bu
#define SDX55_WMS_EVENT_REPORT 0x0001u
#define SDX55_WMS_RAW_SEND 0x0020u
#define SDX55_WMS_RAW_READ 0x0022u
#define SDX55_WMS_SET_ROUTES 0x0032u
#define SDX55_WMS_GET_ROUTES 0x0033u
#define SDX55_WMS_ACK 0x0037u
#define SDX55_WMS_PRIMARY 0x0045u
#define SDX55_WMS_BIND 0x004cu
#define SDX55_WMS_REGISTRATION 0x004au
#define SDX55_WMS_ROUTE_MAX 12u

struct sdx55_sms_route {
	uint8_t type, message_class, storage, action;
};

struct sdx55_sms_routes {
	uint8_t count, status_reports;
	bool has_status_reports;
	struct sdx55_sms_route entries[SDX55_WMS_ROUTE_MAX];
};

struct sdx55_sms_event {
	uint32_t transaction, index;
	uint16_t length;
	uint8_t storage, mode, format;
	bool transfer, ack, ims;
	uint8_t pdu[UMI_SMS_PDU_MAX];
};

static inline bool
sdx55_sms_submit_valid(const struct umi_sms_submission *submit)
{
	const uint8_t *p = submit->pdu;
	if (submit->length < 9 || submit->length > sizeof(submit->pdu) ||
	    p[0] || (p[1] != 1 && p[1] != 0x41) || !p[3] || p[3] > 20 ||
	    (p[4] != 0x81 && p[4] != 0x91)) return false;
	unsigned address = (p[3] + 1) / 2, header = 8 + address;
	if (submit->length <= header || p[header - 3]) return false;
	for (unsigned i = 0; i < p[3]; i++)
		if ((p[5 + i / 2] >> (4 * (i % 2)) & 0xf) > 9) return false;
	if ((p[3] & 1) && (p[4 + address] >> 4) != 0xf) return false;
	unsigned dcs = p[header - 2], udl = p[header - 1];
	unsigned bytes = submit->length - header;
	if (!udl || bytes > 140) return false;
	if (dcs == 0 && bytes != (udl * 7 + 7) / 8) return false;
	if (dcs == 8 && (bytes != udl || (udl & 1))) return false;
	if (dcs != 0 && dcs != 8) return false;
	return p[1] == 1 || (bytes > 6 && p[header] == 5 && p[header + 1] == 0 &&
	    p[header + 2] == 3 && p[header + 4] > 1 && p[header + 5] &&
	    p[header + 5] <= p[header + 4] && (dcs != 0 || udl > 7));
}

static inline size_t
sdx55_sms_send_request(const struct umi_sms_submission *submit, uint16_t txn,
    bool ims, uint8_t *out, size_t capacity)
{
	size_t tlv_size = 6 + submit->length + (ims ? 4 : 0);
	if (capacity < 7 + tlv_size || !sdx55_sms_submit_valid(submit) ||
	    !sdx55_cell_empty_request(SDX55_WMS_RAW_SEND, txn, out, capacity)) return 0;
	out[5] = tlv_size & 0xff;
	out[6] = tlv_size >> 8;
	uint8_t *tlv = out + 7;
	tlv[0] = 1;
	tlv[1] = 3 + submit->length;
	tlv[2] = 0;
	tlv[3] = 6; /* GSM/WCDMA point-to-point. */
	tlv[4] = submit->length;
	tlv[5] = 0;
	memcpy(tlv + 6, submit->pdu, submit->length);
	static const uint8_t ims_tlv[] = {0x13, 1, 0, 1};
	if (ims) memcpy(tlv + 6 + submit->length, ims_tlv, sizeof(ims_tlv));
	return 7 + tlv_size;
}

/* WMS Raw Read prepends the SMSC address to the TPDU, including a length
 * byte when no address is present. Transfer-route indications carry the TPDU. */
static inline int
sdx55_sms_stored_tpdu(const uint8_t *data, size_t size, struct sdx55_sms_event *event)
{
	if (size < 5 || data[1] != 6 || size - 4 != sdx55_cell_le16(data + 2)) return EINVAL;
	size_t offset = 5u + data[4];
	if (offset >= size || size - offset > sizeof(event->pdu)) return EINVAL;
	event->length = size - offset;
	memcpy(event->pdu, data + offset, event->length);
	return 0;
}

static inline uint32_t
sdx55_sms_le32(const uint8_t *p)
{
	return sdx55_cell_le16(p) | (uint32_t)sdx55_cell_le16(p + 2) << 16;
}

static inline void
sdx55_sms_put32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static inline int
sdx55_sms_routes_decode(const uint8_t *p, size_t n, struct sdx55_sms_routes *routes)
{
	if (n < 2 || sdx55_cell_le16(p) > SDX55_WMS_ROUTE_MAX) return EINVAL;
	unsigned count = sdx55_cell_le16(p);
	if (n != 2 + 4 * count) return EINVAL;
	routes->count = count;
	memcpy(routes->entries, p + 2, count * sizeof(routes->entries[0]));
	return 0;
}

static inline struct sdx55_sms_routes
sdx55_sms_receive_routes(const struct sdx55_sms_routes *saved)
{
	struct sdx55_sms_routes routes = *saved;
	for (unsigned i = 0; i < routes.count; i++) {
		struct sdx55_sms_route *r = &routes.entries[i];
		/* Class 2 and CDMA retain the modem's configured routes. */
		if (r->type || r->message_class == 2 || r->message_class > 4) continue;
		r->storage = 0xff;
		r->action = 2; /* Transfer to host and wait for its ACK. */
	}
	return routes;
}

static inline size_t
sdx55_sms_request(uint16_t message, uint16_t txn, unsigned argument,
    const struct sdx55_sms_routes *routes, const struct sdx55_sms_event *event,
    uint8_t *out, size_t capacity)
{
	if (capacity < 7 + 5 + 4 * SDX55_WMS_ROUTE_MAX ||
	    !sdx55_cell_empty_request(message, txn, out, capacity)) return 0;
	uint8_t *tlv = out + 7;
	memset(tlv, 0, capacity - 7);
	size_t n = 0;
	switch (message) {
	case SDX55_WMS_BIND:
		if (argument < 1 || argument > 2) return 0;
		tlv[0] = 1;
		tlv[1] = 1;
		/* NAS publishes slots as 1/2. WMS uses primary/secondary indices 0/1. */
		tlv[3] = argument - 1;
		n = 4;
		break;
	case SDX55_WMS_EVENT_REPORT:
		tlv[0] = 0x10;
		tlv[1] = 1;
		tlv[3] = argument != 0;
		n = 4;
		break;
	case SDX55_WMS_PRIMARY:
		tlv[0] = 0x01;
		tlv[1] = 1;
		tlv[3] = argument != 0;
		n = 4;
		break;
	case SDX55_WMS_SET_ROUTES:
		if (!routes || routes->count > SDX55_WMS_ROUTE_MAX ||
		    (routes->has_status_reports && capacity < 7 + 9 + 4 * routes->count)) return 0;
		tlv[0] = 1;
		tlv[1] = 2 + 4 * routes->count;
		tlv[3] = routes->count;
		memcpy(tlv + 5, routes->entries, 4 * routes->count);
		n = 5 + 4 * routes->count;
		if (routes->has_status_reports) {
			tlv[n] = 0x10;
			tlv[n + 1] = 1;
			tlv[n + 3] = routes->status_reports;
			n += 4;
		}
		break;
	case SDX55_WMS_RAW_READ:
		if (!event) return 0;
		tlv[0] = 1;
		tlv[1] = 5;
		tlv[3] = event->storage;
		sdx55_sms_put32(tlv + 4, event->index);
		tlv[8] = 0x10;
		tlv[9] = 1;
		tlv[11] = 1; /* 3GPP */
		n = 12;
		break;
	case SDX55_WMS_ACK:
		if (!event) return 0;
		tlv[0] = 1;
		tlv[1] = 6;
		sdx55_sms_put32(tlv + 3, event->transaction);
		tlv[7] = 1; /* 3GPP */
		tlv[8] = argument == 1;
		tlv[9] = 0x12;
		tlv[10] = 1;
		tlv[12] = event->ims;
		n = 13;
		if (argument != 1) {
			tlv[13] = 0x11;
			tlv[14] = 2;
			tlv[16] = argument == 2 ? 0x16 : 0x6f; /* RP: memory capacity or protocol error */
			tlv[17] = argument == 2 ? 0xd3 : 0xff; /* TP: memory capacity or unspecified error */
			n = 18;
		}
		break;
	case SDX55_WMS_GET_ROUTES:
	case SDX55_WMS_REGISTRATION:
		break;
	default:
		return 0;
	}
	out[5] = n;
	return 7 + n;
}

static inline int
sdx55_sms_event_decode(const uint8_t *packet, size_t size, struct sdx55_sms_event *event)
{
	if (size < 7 || packet[0] != 4 || sdx55_cell_le16(packet + 3) != SDX55_WMS_EVENT_REPORT ||
	    sdx55_cell_le16(packet + 5) != size - 7) return EINVAL;
	*event = (struct sdx55_sms_event){.storage = 0xff, .mode = 1};
	unsigned seen = 0;
	for (size_t offset = 7; offset < size;) {
		if (size - offset < 3) return EINVAL;
		unsigned tag = packet[offset], n = sdx55_cell_le16(packet + offset + 1);
		offset += 3;
		if (n > size - offset) return EINVAL;
		const uint8_t *p = packet + offset;
		offset += n;
		if (tag < 0x10 || tag > 0x16) continue;
		unsigned bit = 1u << (tag - 0x10);
		if (seen & bit) return EINVAL;
		seen |= bit;
		switch (tag) {
		case 0x10:
			if (n != 5) return EINVAL;
			event->storage = p[0];
			event->index = sdx55_sms_le32(p + 1);
			break;
		case 0x11:
			if (n < 8 || n - 8 > sizeof(event->pdu) || n - 8 != sdx55_cell_le16(p + 6) ||
			    p[0] > 1) return EINVAL;
			event->transfer = true;
			event->ack = p[0] == 0;
			event->transaction = sdx55_sms_le32(p + 1);
			event->format = p[5];
			event->length = n - 8;
			memcpy(event->pdu, p + 8, event->length);
			break;
		case 0x12:
			if (n != 1) return EINVAL;
			event->mode = p[0];
			break;
		case 0x16:
			if (n != 1 || p[0] > 1) return EINVAL;
			event->ims = p[0];
			break;
		}
	}
	if (event->mode != 1) return ENOTSUP;
	if (event->transfer) return event->format == 6 && event->length ? 0 : ENOTSUP;
	/* UIM notifications can be SIM provisioning messages. Leave them on UIM. */
	return event->storage == 1 ? 0 : ENOTSUP;
}

#endif
