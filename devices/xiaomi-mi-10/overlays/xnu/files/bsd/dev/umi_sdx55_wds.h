/* SDX55 WDS client messages. QMI WDS fields and IDs are specified by
 * libqmi's qmi-service-wds.json (Start/Stop Network, Get Current Settings,
 * Set IP Family, Bind Mux Data Port). All messages use one owned QRTR port. */
#ifndef UMI_SDX55_WDS_H
#define UMI_SDX55_WDS_H

#include "umi_sdx55_cell.h"

#define SDX55_WDS_START      0x0020u
#define SDX55_WDS_STOP       0x0021u
#define SDX55_WDS_SETTINGS   0x002du
#define SDX55_WDS_SET_FAMILY 0x004du
#define SDX55_WDS_BIND_MUX   0x00a2u

struct sdx55_wds_result {
	uint16_t service_error, call_end, verbose_type;
	int16_t verbose_reason;
	uint8_t has_call_end, has_verbose;
	uint32_t handle;
	/* Host-order IPv4 values, as accepted by hakios_cell_session().
	 * Optional settings default to zero when absent. */
	uint32_t address, gateway, netmask, dns1, dns2, mtu;
};

static inline void
sdx55_wds_put32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static inline uint32_t
sdx55_wds_get32(const uint8_t *p)
{
	return (uint32_t)sdx55_cell_le16(p) | ((uint32_t)sdx55_cell_le16(p + 2) << 16);
}

/* Leave APN, profile index, credentials and autoconnect at their modem defaults.
 * STOP accepts only the handle returned by this client's successful START. */
static inline int
sdx55_wds_request(uint16_t msg, uint16_t txn, uint32_t endpoint, uint8_t mux,
    uint32_t owned_handle, uint8_t *out, size_t capacity)
{
	uint8_t body[15] = {0};
	size_t length;
	switch (msg) {
	case SDX55_WDS_BIND_MUX:
		if (!mux || mux == UINT8_MAX) return 0;
		body[0] = 0x10;
		body[1] = 8;
		body[3] = 3; /* PCIe */
		sdx55_wds_put32(body + 7, endpoint);
		body[11] = 0x11;
		body[12] = 1;
		body[14] = mux;
		length = 15;
		break;
	case SDX55_WDS_SET_FAMILY:
	case SDX55_WDS_START:
		if (msg == SDX55_WDS_START && owned_handle) return 0;
		body[0] = msg == SDX55_WDS_SET_FAMILY ? 1 : 0x19;
		body[1] = 1;
		body[3] = 4; /* IPv4 */
		length = 4;
		break;
	case SDX55_WDS_SETTINGS:
		body[0] = 0x10;
		body[1] = 4;
		sdx55_wds_put32(body + 3, 0x0000a310u); /* DNS, address, gateway, MTU, IP family */
		length = 7;
		break;
	case SDX55_WDS_STOP:
		if (!owned_handle || owned_handle == UINT32_MAX) return 0;
		body[0] = 1;
		body[1] = 4;
		sdx55_wds_put32(body + 3, owned_handle);
		length = 7;
		break;
	default:
		return 0;
	}
	if (capacity < 7 + length || !sdx55_cell_empty_request(msg, txn, out, capacity)) return 0;
	out[5] = (uint8_t)length;
	for (size_t i = 0; i < length; i++) out[7 + i] = body[i];
	return (int)(7 + length);
}

static inline int
sdx55_wds_decode(uint16_t msg, uint16_t txn, const uint8_t *packet, size_t size,
    struct sdx55_wds_result *out)
{
	if (!out) return EINVAL;
	switch (msg) {
	case SDX55_WDS_BIND_MUX:
	case SDX55_WDS_SET_FAMILY:
	case SDX55_WDS_START:
	case SDX55_WDS_SETTINGS:
	case SDX55_WDS_STOP:
		break;
	default:
		return EINVAL;
	}
	struct sdx55_wds_result reply = {0};
	int error = sdx55_cell_body(msg, txn, packet, size, &reply.service_error);
	if (error) {
		if (error != EIO) return error;
		if (msg == SDX55_WDS_START && reply.service_error == 14) {
			/* CALL_FAILED can carry carrier/modem causes; retain numeric fields only. */
			for (size_t off = 7; off < size; ) {
				size_t n = sdx55_cell_le16(packet + off + 1);
				const uint8_t *p = packet + off + 3;
				if (packet[off] == 0x10) {
					if (reply.has_call_end || n != 2) return EINVAL;
					reply.has_call_end = 1;
					reply.call_end = sdx55_cell_le16(p);
				} else if (packet[off] == 0x11) {
					if (reply.has_verbose || n != 4) return EINVAL;
					reply.has_verbose = 1;
					reply.verbose_type = sdx55_cell_le16(p);
					reply.verbose_reason = (int16_t)sdx55_cell_le16(p + 2);
				}
				off += 3 + n;
			}
		}
		*out = reply;
		return error;
	}

	uint64_t seen = 0;
	for (size_t off = 7; off < size; ) {
		uint8_t tag = packet[off];
		size_t n = sdx55_cell_le16(packet + off + 1);
		const uint8_t *p = packet + off + 3;
		off += 3 + n;
		uint32_t *field = NULL;
		if (msg == SDX55_WDS_START && tag == 1) {
			field = &reply.handle;
		} else if (msg == SDX55_WDS_SETTINGS) {
			switch (tag) {
			case 0x15: field = &reply.dns1; break;
			case 0x16: field = &reply.dns2; break;
			case 0x1e: field = &reply.address; break;
			case 0x20: field = &reply.gateway; break;
			case 0x21: field = &reply.netmask; break;
			case 0x29: field = &reply.mtu; break;
			case 0x2b: /* optional IP family */
				if (n != 1 || (seen & (1ull << tag))) return EINVAL;
				if (*p != 4) return EAFNOSUPPORT;
				seen |= 1ull << tag;
				continue;
			default: continue;
			}
		} else {
			continue;
		}
		if (n != 4 || (seen & (1ull << tag))) return EINVAL;
		seen |= 1ull << tag;
		*field = sdx55_wds_get32(p);
	}
	if (msg == SDX55_WDS_START && (!reply.handle || reply.handle == UINT32_MAX)) return EINVAL;
	if (msg == SDX55_WDS_SETTINGS && !reply.address) return EADDRNOTAVAIL;
	*out = reply;
	return 0;
}

#endif
